// mcp-retriage — re-score the `severity` frontmatter of every OPEN PinWright
// issue-board ticket against a fixed impact×reach rubric, as a bounded one-pass
// background Workflow, so the mcp-fix-workflow picker (which ranks OPEN tickets by
// severity and works the top band first) works the genuinely-most-important
// tickets first.
//
// One bounded sweep: read all OPEN, score them in parallel against one shared
// rubric, write back ONLY the tickets whose severity changed (one append-only
// `#N-retriage` history line each), commit the exact changed paths, stop. NOT an
// infinite supervised loop — it returns on its own and re-running is safe
// (idempotent: an unchanged board produces zero writes).
//
// Sandbox: standard JS only (no fs/shell/Math.random/Date). Every effect is an
// agent(). The script owns sequencing + the idempotence invariant; agents own all
// file I/O, time (Get-Date), and git. Batching is deterministic (contiguous index
// slices) so no randomness is needed.

export const meta = {
    name: 'mcp-retriage',
    description: 'Re-score the severity of every OPEN PinWright issue-board ticket against a fixed impact×reach rubric, writing back only the changed tickets (append-only history) and committing the exact paths, so the fix-workflow picker works the most important tickets first.',
    phases: [
        { title: 'Read' },
        { title: 'Score' },
        { title: 'Write' },
        { title: 'Commit' },
    ],
};

const opts = (args && typeof args === 'object') ? args
    : (typeof args === 'string' && args.trim()
        ? (() => { try { return JSON.parse(args); } catch (e) { return {}; } })()
        : {});

// Board paths are relative to the plugin directory (<host project>/Plugins/PinWright), so the
// default is a sibling of the host-project checkout: a clone of PinWright/pinwright-board.
const BOARD = opts.boardPath || '../../../.pinwright-board';
const REPO = opts.boardRepo || '../../../.pinwright-board';
const MAX = (typeof opts.maxTickets === 'number' && opts.maxTickets > 0) ? opts.maxTickets : 0;   // 0 = all OPEN
const FANOUT = (typeof opts.scoreFanout === 'number' && opts.scoreFanout > 0) ? opts.scoreFanout : 5;
const TTL = (typeof opts.claimTtlHours === 'number' && opts.claimTtlHours > 0) ? opts.claimTtlHours : 4;
const DRY = !!opts.dryRun;
// push defaults TRUE (cadence use publishes to origin so fuzz hosts pull the
// re-ratings); the first dogfood run passes push:false to commit locally only.
const PUSH = (opts.push === undefined) ? true : !!opts.push;
// Identity for claim-skip: only a FRESH foreign lease blocks a ticket. Derive a
// fuzzN id if launched on a host, else a neutral 'retriage'.
const hostStr = (opts.host && opts.host.projectPath) || opts.projectPath || '';
const hostMatch = String(hostStr).match(/fuzz\d+/i);
const HOST_ID = opts.hostId || (hostMatch ? hostMatch[0] : 'retriage');

// Severity ladder (high → low) for the up/down tally and ordering.
const RANK = { Critical: 4, High: 3, Medium: 2, Low: 1 };

// The canonical rubric — interpolated VERBATIM into every Score prompt so all
// fan-out agents classify against identical text (no per-agent drift). Mirrors
// .pinwright-board README.md "Severity Levels".
const RUBRIC = `SEVERITY RUBRIC (impact class × reach → one of Critical | High | Medium | Low):
Impact class:
- Critical: editor crash, or a write that corrupts or loses asset data.
- High: silent false-success, or silent wrong / stale / hardcoded data on a normal path (the caller trusts a result that is a lie and builds on it).
- High or Medium: hard blocker with no workaround (a stub, a missing verb, or rejecting valid input) so a reasonable task is impossible — pick High if the blocked task is common, Medium if niche.
- Medium: soft blocker — doable only via a documented workaround, a source dive, or many extra calls; OR a readback omits a field and forces a fallback.
- Low: pure friction — docs, discoverability, naming, a response spill that only forces a Read, or cosmetic.
Reach modifier: if the affected method runs in almost every session, bump UP one level; if it is a rare edge path, bump DOWN one. A Low-impact gap on an every-session method outranks a High-impact gap on a method nobody hits.`;

// Retry an agent() that died on a TRANSIENT failure (runtime returns null after
// its own API retries). A returned object passes straight through.
async function agentOrRetry(make, tries = 3) {
    let r = null;
    for (let i = 0; i < tries; i++) {
        r = await make();
        if (r) return r;
        if (i + 1 < tries) log(`agent died (transient); retrying (attempt ${i + 2}/${tries})`);
    }
    return r;
}

// Split a list into n contiguous index slices — deterministic (no Math.random),
// reproducible across reruns because the Read agent returns tickets in glob order.
function splitContiguous(arr, n) {
    const out = [];
    const size = Math.ceil(arr.length / n) || 1;
    for (let i = 0; i < arr.length; i += size) out.push(arr.slice(i, i + size));
    return out;
}

const readSchema = {
    type: 'object', required: ['status', 'now', 'open'],
    properties: {
        status: { type: 'string', enum: ['OK', 'FATAL'] },
        now: { type: 'string' },                       // ISO-8601 from Get-Date -Format o (single sample)
        open: {
            type: 'array', items: {
                type: 'object', required: ['id', 'path', 'currentSeverity'],
                properties: {
                    id: { type: 'string' },
                    path: { type: 'string' },
                    currentSeverity: { type: 'string' },   // Critical|High|Medium|Low (or '' if unset)
                    category: { type: 'string' },          // bug|feature|ergonomic
                    claimBlocked: { type: 'boolean' },     // fresh foreign lease (computed by the agent)
                },
            },
        },
        reason: { type: 'string' },
    },
};

const scoreSchema = {
    type: 'object', required: ['scores'],
    properties: {
        scores: {
            type: 'array', items: {
                type: 'object', required: ['id', 'proposed', 'reason'],
                properties: {
                    id: { type: 'string' },
                    proposed: { type: 'string', enum: ['Critical', 'High', 'Medium', 'Low'] },
                    reason: { type: 'string' },            // one clause: impact class + reach, becomes the history note
                },
            },
        },
    },
};

const writeSchema = {
    type: 'object', required: ['status', 'written'],
    properties: {
        status: { type: 'string', enum: ['OK', 'FATAL'] },
        written: { type: 'array', items: { type: 'string' } },   // exact paths actually edited
        skipped: { type: 'array', items: { type: 'string' } },   // ids skipped (status no longer OPEN at write time)
        reason: { type: 'string' },
        excerpt: { type: 'string' },
    },
};

const commitSchema = {
    type: 'object', required: ['status'],
    properties: {
        status: { type: 'string', enum: ['OK', 'FATAL'] },
        committed: { type: 'boolean' },
        pushed: { type: 'boolean' },
        newTip: { type: 'string' },
        reason: { type: 'string' },
        excerpt: { type: 'string' },
        note: { type: 'string' },
    },
};

function readPrompt() {
    return `Read the PinWright issue board to find every OPEN ticket for re-triage. Run PowerShell. READ-ONLY — edit nothing.

1. NOW: run \`Get-Date -Format o\` ONCE and return it as 'now' (the single time sample for this pass).
2. ENUMERATE: glob \`${BOARD}\\*.md\`, EXCLUDE \`README.md\`. For each file read its YAML frontmatter and parse: id, status, severity, category, claimedBy, claimedAt.
3. FILTER: keep only \`status: OPEN\`.
4. CLAIM CHECK: for each OPEN ticket, set claimBlocked = true IFF it has a non-empty \`claimedBy\` that is NOT "${HOST_ID}" AND its \`claimedAt\` is FRESH — i.e. (now - claimedAt) < ${TTL} hours (compute the delta in PowerShell from the two timestamps). A stale (>= ${TTL}h) or own claim is claimBlocked = false. No claim field at all is claimBlocked = false.
5. Return {status:'OK', now:'<iso>', open:[{id, path (absolute), currentSeverity, category, claimBlocked}, ...]}. If the board folder does not exist or no .md files are found, return {status:'FATAL', reason:'<what>'}.`;
}

function scorePrompt(batch) {
    const list = batch.map((t) => `  - ${t.id} (current: ${t.currentSeverity || 'unset'}, category: ${t.category || '?'}) @ ${t.path}`).join('\n');
    return `You are re-scoring the severity of a batch of OPEN PinWright MCP issue-board tickets. These describe defects/gaps in an Unreal Editor MCP plugin driven by an AUTONOMOUS AI AGENT — "impact" means impact on that agent. For EACH ticket, Read its .md body in full, then assign the correct severity strictly by the rubric below. READ-ONLY — edit NOTHING.

${RUBRIC}

Tickets to score (read each body before deciding):
${list}

For each ticket return {id, proposed, reason} where:
- proposed ∈ Critical | High | Medium | Low — the severity the rubric dictates (may equal the current value; score on the merits, do not anchor on the current value).
- reason — ONE short clause naming the impact class + reach that drove the score (e.g. "silent stale readback on a common verify path" or "docs/discoverability friction, rare path"). This becomes the ticket's history note, so keep it factual and terse (no period needed).

Return {scores:[{id, proposed, reason}, ...]} covering every ticket in the batch. Do not invent ids; score only the ones listed.`;
}

function writePrompt(changed) {
    const list = changed.map((c) => `  - ${c.path}  (${c.old} → ${c.new}) reason: ${c.reason}`).join('\n');
    return `Apply severity re-ratings to these OPEN PinWright board tickets. Run by editing each file (Read then Edit — do NOT rewrite the whole file via shell). Board history format and rules live in ${BOARD}\\README.md.

For EACH ticket below:
1. RE-CHECK: Read the file. If its frontmatter \`status\` is NO LONGER \`OPEN\` (a fix host may have flipped it since the board was read), SKIP it — make no edit — and add its id to 'skipped'. Otherwise proceed.
2. SEVERITY: change ONLY the frontmatter \`severity:\` line from the old value to the new value. Touch no other frontmatter field; never change \`status\`.
3. HISTORY: find the current maximum \`#N\` in the \`## History\` section and append exactly ONE new line at the true end of that section, in the board's documented format (note the separator is " — ", an em dash, matching every existing entry):
   - \`#<N+1>-retriage\` \`OPEN\` triage — <old>→<new>: <reason>
   Use the per-ticket <old>, <new>, and <reason> given below. Never edit or delete any prior history line.

Tickets:
${list}

Return {status:'OK', written:[<absolute paths actually edited>], skipped:[<ids skipped because no longer OPEN>]}. If a file cannot be edited (parse problem, missing History section), return {status:'FATAL', reason, excerpt} and stop.`;
}

function commitPrompt(written, up, down) {
    const pushSteps = PUSH
        ? `4. Integrate origin: git -C "${REPO}" pull --rebase origin master. On a history-line conflict (a fuzz host appended to the same ticket), reconcile KEEPING BOTH sides (keep their line AND the new retriage line), strip markers, git -C "${REPO}" add the resolved exact paths, git -C "${REPO}" rebase --continue. If a conflict is genuinely unreconcilable, return {status:'FATAL', reason, excerpt} and do NOT push (never force-push master).
5. Push: git -C "${REPO}" push origin master. If rejected because origin advanced, repeat step 4 then push again.
6. Return {status:'OK', committed:true, pushed:true, newTip:'<git -C "${REPO}" rev-parse origin/master>'}.`
        : `4. Do NOT pull/rebase and do NOT push — this run commits locally only for review. Return {status:'OK', committed:true, pushed:false, newTip:'<git -C "${REPO}" rev-parse HEAD>'}.`;
    return `Commit the retriage severity edits in ${REPO}. Run PowerShell. The working tree may also contain UNRELATED uncommitted board changes — DO NOT touch or stage those.

1. Stage ONLY these exact re-rated files, each individually (git -C "${REPO}" add "<path>"). NEVER git add -A / -a / . — exact paths only:
${written.map((p) => `   ${p}`).join('\n')}
2. Sanity: git -C "${REPO}" status --porcelain --untracked-files=no — confirm the staged set is exactly the files above and nothing else; if anything else is staged, unstage it (git -C "${REPO}" restore --staged "<path>") so only the retriage files are in the commit.
3. Commit: git -C "${REPO}" commit -m "retriage: re-score ${written.length} OPEN tickets (${up} up, ${down} down)".
${pushSteps}`;
}

// Terminal payload for a hard failure (supervisor: stop, surface, do not relaunch).
function fatal(where, info) {
    const i = info || {};
    log(`FATAL (${where}): ${i.reason || 'unrecoverable'}`);
    return {
        stop_reason: 'fatal',
        phase: where,
        reason: i.reason || 'unrecoverable error',
        excerpt: i.excerpt || null,
        counts,
        changed,
    };
}

// --- State (referenced by fatal()) ---
const counts = { scored: 0, changedUp: 0, changedDown: 0, unchanged: 0, skippedClaimed: 0, written: 0 };
let changed = [];

// --- Read ---
phase('Read');
const pre = await agentOrRetry(() => agent(readPrompt(), { schema: readSchema, label: 'read-board', phase: 'Read' }));
if (!pre) return { stop_reason: 'agent_died', note: 'read agent died after retries — relaunch resumes (idempotent)', counts };
if (pre.status === 'FATAL' || !Array.isArray(pre.open)) return fatal('Read', { reason: pre.reason || 'board unreadable' });

const now = pre.now;
const eligible = pre.open.filter((t) => !t.claimBlocked);
counts.skippedClaimed = pre.open.length - eligible.length;
let work = MAX ? eligible.slice(0, MAX) : eligible;
log(`OPEN: ${pre.open.length}; eligible after claim-skip: ${eligible.length}; scoring: ${work.length}` + (DRY ? ' (dryRun)' : ''));
if (work.length === 0) return { stop_reason: 'done', dryRun: DRY, now, counts, changed: [], committed: false, pushed: false };

// --- Score (parallel fan-out, deterministic batches) ---
phase('Score');
const batches = splitContiguous(work, FANOUT);
const scored = await parallel(batches.map((batch, k) =>
    () => agentOrRetry(() => agent(scorePrompt(batch), { schema: scoreSchema, label: `score#${k + 1}`, phase: 'Score' }))));
const scoreById = {};
for (const r of scored) {
    if (!r || !Array.isArray(r.scores)) { log('a score batch died — its tickets go unscored this pass (re-run to cover them)'); continue; }
    for (const s of r.scores) scoreById[s.id] = s;
}
counts.scored = Object.keys(scoreById).length;

// --- Decide (script only) ---
for (const t of work) {
    const s = scoreById[t.id];
    if (!s) continue;                                  // batch died; skip (re-run covers it)
    if (s.proposed === t.currentSeverity) { counts.unchanged++; continue; }
    const up = (RANK[s.proposed] || 0) > (RANK[t.currentSeverity] || 0);
    if (up) counts.changedUp++; else counts.changedDown++;
    changed.push({ id: t.id, path: t.path, old: t.currentSeverity || 'unset', new: s.proposed, reason: s.reason });
}
log(`changed: ${changed.length} (${counts.changedUp} up, ${counts.changedDown} down); unchanged: ${counts.unchanged}`);

if (changed.length === 0) return { stop_reason: 'done', dryRun: DRY, now, counts, changed: [], committed: false, pushed: false, note: 'board already converged — nothing to write' };
if (DRY) { log('dryRun: writing nothing'); return { stop_reason: 'done', dryRun: true, now, counts, changed, committed: false, pushed: false }; }

// --- Write (single serial agent; #N is per-file so distinct files have no race) ---
phase('Write');
const w = await agentOrRetry(() => agent(writePrompt(changed), { schema: writeSchema, label: 'write', phase: 'Write' }));
if (!w) return { stop_reason: 'agent_died', note: 'write agent died — partial board re-converges on re-run (applied tickets now read unchanged)', counts, changed };
if (w.status === 'FATAL') return fatal('Write', w);
const written = Array.isArray(w.written) ? w.written : [];
counts.written = written.length;
if (Array.isArray(w.skipped) && w.skipped.length) log(`write skipped ${w.skipped.length} ticket(s) no longer OPEN: ${w.skipped.join(', ')}`);
if (written.length === 0) return { stop_reason: 'done', dryRun: false, now, counts, changed, committed: false, pushed: false, note: 'nothing written (all changed tickets left OPEN-state)' };

// --- Commit (exact paths; push only when PUSH) ---
phase('Commit');
const cm = await agentOrRetry(() => agent(commitPrompt(written, counts.changedUp, counts.changedDown), { schema: commitSchema, label: 'commit', phase: 'Commit' }));
if (!cm) return { stop_reason: 'agent_died', note: 'commit agent died after write — re-run will see the edits already applied (unchanged) and just commit', counts, changed, written };
if (cm.status === 'FATAL') return fatal('Commit', cm);

log(`done: ${counts.written} written, committed=${!!cm.committed}, pushed=${!!cm.pushed}`);
return {
    stop_reason: 'done',
    dryRun: false,
    now,
    counts,
    changed,
    committed: !!cm.committed,
    pushed: !!cm.pushed,
    newTip: cm.newTip || null,
};
