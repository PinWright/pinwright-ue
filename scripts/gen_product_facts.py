#!/usr/bin/env python3
"""Emits product-facts.json at the plugin repo root: the single source of truth for every
number published about this plugin (website, docs, Fab listing). Every figure is read from
a build artifact or a source file that already owns it - nothing here is typed by hand -
because hand-maintained copies are what produced the published "1,300+ operations" claim
against a real figure of 1,169.

Also writes dist/fab-listing.md: the Fab listing lives in Epic's seller portal and cannot
be generated into, so the marketing sentences are emitted paste-ready with the correct
numbers already substituted.

Python 3 standard library only, so one code path runs on Windows and Linux:
    python3 scripts/gen_product_facts.py            (Linux)
    py scripts\\gen_product_facts.py                 (Windows)
"""
import argparse
import json
import os
import re
import sys
from collections import OrderedDict
from pathlib import Path


def fail(message):
    sys.exit('ERROR: ' + message)


def read_text(path):
    # Explicit UTF-8 (a BOM is dropped) and no newline translation, so a CRLF checkout keeps
    # its CRLF when the README is rewritten.
    with open(path, encoding='utf-8-sig', newline='') as f:
        return f.read()


def newline_of(path):
    # Scripted edits keep a file's existing newline style (see .gitattributes); a new file is LF.
    return '\r\n' if os.path.exists(path) and '\r\n' in read_text(path) else '\n'


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def to_ps_json(value, column=0):
    # Reproduces Windows PowerShell 5.1 ConvertTo-Json layout, which product-facts.json has
    # always been committed in: members four columns past the opening bracket, two spaces after
    # each colon, and a nested container indented from the column its bracket opens at.
    pad = ' ' * (column + 4)
    if isinstance(value, dict):
        members = []
        for key, item in value.items():
            prefix = pad + json.dumps(key) + ':  '
            members.append(prefix + to_ps_json(item, len(prefix)))
        return '{\n' + ',\n'.join(members) + '\n' + ' ' * column + '}'
    if isinstance(value, list):
        return '[\n' + ',\n'.join(pad + to_ps_json(item, column + 4) for item in value) + '\n' + ' ' * column + ']'
    return json.dumps(value)


def rounded_floor(value):
    # Rounded marketing strings are LOWER bounds and must never overstate. The value is
    # floored to the largest bucket multiple STRICTLY below it, so an exact-multiple figure
    # steps down a bucket instead of publishing itself as a "+" claim:
    #   1169 -> 1,100+   3500 -> 3,400+   66 -> 60+
    # Bucket is 100 from three digits up, 10 below that (66 has no non-zero hundreds floor).
    if value < 2:
        fail('Cannot produce a rounded lower-bound string for value %d.' % value)
    bucket = 100 if value >= 100 else 10
    return '{:,}+'.format((value - 1) // bucket * bucket)


def _openers_minus_closers(text):
    return len(re.sub(r'[^({\[]', '', text)) - len(re.sub(r'[^)}\]]', '', text))


def short_description(text):
    # One README-sized line from a wiki description, never reworded: the first sentence with
    # link and emphasis markup stripped. A sentence over 120 characters is cut, all within 110
    # and outside brackets, at its first ':' ';' or ' - ' break, else at its last comma,
    # else at a word boundary; a cut ends with a period.
    t = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', text).replace('`', '').replace('**', '')
    t = re.sub('\\s*[\u2013\u2014]\\s*', ' - ', t)
    t = re.sub(r'\*([A-Za-z][^*]*?[A-Za-z])\*', r'\1', t)
    t = re.sub(r'\s+', ' ', t).strip()
    # A sentence ends at . ! or ? before whitespace; ellipses and e.g./i.e. do not end one.
    sentence = re.match(r'^.*?(?<!\.)(?<!\be\.g)(?<!\bi\.e)(?<!\bvs)(?<!\betc)[.!?](?!\.)(?=\s|$)', t)
    if sentence:
        t = sentence.group(0)
    # ponytail: lengths count code points where .NET counted UTF-16 units; they differ only
    # for characters outside the BMP, which no wiki description carries.
    if len(t) <= 120:
        return t

    head = t[:110]
    breaks = [m.start() for m in re.finditer(r'(?<!:):(?!:)|;| - ', head)]
    commas = [m.start() for m in re.finditer(',', head)][::-1]
    for at in breaks + commas:
        clause = head[:at].rstrip()
        if len(clause) >= 15 and _openers_minus_closers(clause) == 0:
            return clause + '.'
    cut = re.sub(r'\s+\S*$', '', head)
    if _openers_minus_closers(cut) > 0:
        cut = cut[:max(cut.rfind(c) for c in '({[')]
    # Case-insensitive, as PowerShell's -replace was.
    cut = re.sub(r'(\s+(a|an|and|or|the|to|of|for|from|with|in|on|at|by|as|into|over|through|via|than|that|which))+\s*$',
                 '', cut, flags=re.IGNORECASE)
    return cut.rstrip(',;: -') + '.'


def wiki_operations(wiki_dir, slug):
    # Method rows ("- `name` - description") from the "## Methods" sections of <slug>.md and
    # its sub-namespace pages <slug>.<sub>.md; method and guide pages carry no such section.
    ops = OrderedDict()
    pages = sorted(p for p in os.listdir(wiki_dir)
                   if p.endswith('.md') and (p[:-3] == slug or p[:-3].startswith(slug + '.'))
                   and os.path.isfile(os.path.join(wiki_dir, p)))
    for page in pages:
        section = re.search(r'(?ms)^## Methods\r?\n(.*?)(?=^## |\Z)', read_text(os.path.join(wiki_dir, page)))
        if not section:
            continue
        for row in re.finditer('(?m)^- `([^`]+)` \u2014 (.*?)\r?$', section.group(1)):
            ops.setdefault(row.group(1), row.group(2))
    return ops


def format_cell(text):
    # Table-cell / <summary> safe: angle brackets would parse as HTML, | splits the row,
    # * would start emphasis (backticks are already stripped, so nothing protects them).
    return text.replace('<', '&lt;').replace('>', '&gt;').replace('|', '\\|').replace('*', '\\*')


def agent_tool_count(path):
    # Members of `enum class EAgentTool` - the agents the in-editor setup screen can
    # configure one-click. The generated clientsOneClick list is asserted against this so a
    # client added or removed in code cannot silently diverge from the published list.
    if not os.path.exists(path):
        fail('Agent configurator header is missing: ' + path)
    match = re.search(r'enum\s+class\s+EAgentTool\s*(?::\s*\w+\s*)?\{(?P<body>[^}]*)\}', read_text(path))
    if not match:
        fail("Could not locate 'enum class EAgentTool' in " + path)
    body = re.sub(r'//[^\r\n]*', ' ', match.group('body'))
    return len(re.findall(r'\b[A-Za-z_]\w*\b', body))


def assert_registry_is_complete(registry):
    # registry.json is written from the LIVE registry at editor launch, and the integration
    # sub-modules only register their handlers when the owning engine plugin is enabled in
    # the host project (see the startup line "PinWright integrations: loaded=[...]
    # skipped=[...]"). An editor run in a project with, say, PCG disabled emits a total below
    # the true shipping figure - silently, and low enough to shift the floor-rounded string
    # (1169 -> "1,100+" but 1061 -> "1,000+"). Refuse to build facts from a partial registry.
    #
    # slug -> (minimum method count that proves the contributing sub-module registered, plugin,
    # module). "ui" is shared: PinWrightCommonUI adds 4 methods on top of the main module's 6,
    # so presence of the namespace alone proves nothing and the count has to be checked.
    expected = [
        ('geometry', 85, 'GeometryScripting', 'PinWrightGeometry'),
        ('pcg', 14, 'PCG', 'PinWrightPCG'),
        ('chooser', 6, 'Chooser', 'PinWrightChooser'),
        ('pose_search', 3, 'PoseSearch', 'PinWrightPoseSearch'),
        ('ui', 10, 'CommonUI', 'PinWrightCommonUI'),
    ]
    by_slug = {str(ns['slug']): int(ns['methods']) for ns in registry['namespaces']}
    problems = []
    for slug, minimum, plugin, module in expected:
        if slug not in by_slug:
            problems.append("%s (missing entirely; plugin '%s', module '%s')" % (slug, plugin, module))
        elif by_slug[slug] < minimum:
            problems.append("%s (%d methods, expected at least %d; plugin '%s', module '%s')"
                            % (slug, by_slug[slug], minimum, plugin, module))
    if problems:
        fail('registry.json is incomplete - these integration namespaces are absent or short: '
             + '; '.join(problems)
             + '. The editor that wrote it ran in a host project with those engine plugins disabled, so the operation count is below the true shipping figure. '
             + "Enable every integration plugin in the host project (check the startup line 'PinWright integrations: loaded=[...] skipped=[...]'), relaunch the editor once, and re-run this script.")


def count_tests(source_dir):
    # Same definition as the ripgrep one-liner documented at docs/test-organization.md -
    # matching LINES of IMPLEMENT_*_AUTOMATION_TEST / BEGIN_DEFINE_SPEC in .cpp/.h under
    # Source/**/Private/Tests/ - done natively so a release script does not depend on ripgrep
    # being installed. Case-insensitive, as Select-String was in the PowerShell original.
    pattern = re.compile('IMPLEMENT_.*AUTOMATION_TEST|BEGIN_DEFINE_SPEC', re.IGNORECASE)
    tests = 0
    for root, _, files in os.walk(source_dir):
        if '/private/tests/' not in root.replace('\\', '/').lower() + '/':
            continue
        for name in files:
            if os.path.splitext(name)[1].lower() in ('.cpp', '.h'):
                with open(os.path.join(root, name), encoding='utf-8', errors='replace', newline='') as f:
                    tests += sum(1 for line in re.split(r'\r\n|\r|\n', f.read()) if pattern.search(line))
    return tests


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--plugin-root', '-PluginRoot', default=str(Path(__file__).resolve().parent.parent))
    # Registry manifest written by the editor at launch (WikiDiskGenerator). Defaults to the
    # host project's default wiki output directory; pass this when WikiOutputDirectory moved.
    parser.add_argument('--registry-json', '-RegistryJson', default='')
    args = parser.parse_args()

    plugin_root = os.path.abspath(args.plugin_root)
    if not os.path.exists(os.path.join(plugin_root, 'PinWright.uplugin')):
        fail('Plugin root does not contain PinWright.uplugin: ' + plugin_root)

    # --- operations / namespaces: the live registry, never a source grep ----------------
    registry_json = args.registry_json.strip()
    if not registry_json:
        # Default WikiOutputDirectory is <HostProject>/Saved/PinWright/wiki; the plugin sits
        # two levels below the host project root.
        registry_json = os.path.join(os.path.dirname(os.path.dirname(plugin_root)), 'Saved', 'PinWright', 'wiki', 'registry.json')
    if not os.path.exists(registry_json):
        fail('Registry manifest not found: %s\n' % registry_json
             + "It is written from the live handler registry when the editor starts. Launch the host project's editor once (with every integration engine plugin enabled), then re-run this script. "
             + "Pass --registry-json if the project's WikiOutputDirectory setting moved the wiki output elsewhere. "
             + 'Counting the source tree instead is what produced the wrong published figures, so there is deliberately no fallback.')

    registry = json.loads(read_text(registry_json))
    operations = int(registry.get('operations') or 0)
    namespace_count = int(registry.get('namespaceCount') or 0)
    namespaces = registry.get('namespaces') or []
    if operations <= 0 or namespace_count <= 0:
        fail('Registry manifest has no usable counts (operations=%d, namespaceCount=%d): %s'
             % (operations, namespace_count, registry_json))
    if len(namespaces) != namespace_count:
        fail('Registry manifest is inconsistent: namespaceCount=%d but the namespaces array holds %d entries.'
             % (namespace_count, len(namespaces)))
    assert_registry_is_complete(registry)

    # --- version: the descriptor ----------------------------------------------------------
    version = str(json.loads(read_text(os.path.join(plugin_root, 'PinWright.uplugin'))).get('VersionName') or '')
    if not version.strip():
        fail('PinWright.uplugin has no VersionName.')

    # --- UE versions: the engine range the packaging loop is run against ------------------
    # One list, verified by running scripts/package-prebuilt.ps1 -EngineRoot C:\UE_<v> per
    # entry plus the version matrix; widen or narrow it here when that loop changes.
    ue_versions = ['5.3', '5.4', '5.5', '5.6', '5.7', '5.8']
    if len(ue_versions) < 2:
        fail('The UE version list must hold at least two versions, found %d.' % len(ue_versions))
    ue_range = ue_versions[0] + '-' + ue_versions[-1]

    # --- tests: the count published at docs/test-organization.md -------------------------
    tests = count_tests(os.path.join(plugin_root, 'Source'))
    if tests <= 0:
        fail('Test count came back as 0 - the registration grep found nothing under Source/**/Private/Tests/.')

    # --- MCP clients ----------------------------------------------------------------------
    # One-click clients are the ones the in-editor setup screen writes configs for; the list
    # is asserted against EAgentTool so code and copy cannot drift apart.
    clients_one_click = ['Claude Code', 'Codex CLI', 'Cursor', 'Gemini CLI', 'VS Code Copilot']
    tool_count = agent_tool_count(os.path.join(plugin_root, 'Source', 'PinWright', 'Private', 'Setup', 'AgentMcpConfigurator.h'))
    if tool_count != len(clients_one_click):
        fail('enum class EAgentTool has %d member(s) but this script publishes %d one-click client(s): %s. '
             % (tool_count, len(clients_one_click), ', '.join(clients_one_click))
             + 'A client was added or removed in code without updating the published list - fix clients_one_click in this script.')
    # Documentation-only: no setup-screen code backs these, users hand-edit the config.
    clients_manual = ['Windsurf', 'Cline']

    operations_rounded = rounded_floor(operations)
    namespaces_rounded = rounded_floor(namespace_count)
    tests_rounded = rounded_floor(tests)

    facts = OrderedDict([
        ('version', version),
        ('ueRange', ue_range),
        ('ueVersions', ue_versions),
        ('operations', operations),
        ('operationsRounded', operations_rounded),
        ('namespaceCount', namespace_count),
        ('namespacesRounded', namespaces_rounded),
        ('namespaces', [OrderedDict([('slug', str(ns['slug'])), ('tier', str(ns.get('tier') or '')), ('methods', int(ns['methods']))])
                        for ns in namespaces]),
        ('tests', tests),
        ('testsRounded', tests_rounded),
        ('clientsOneClick', clients_one_click),
        ('clientsManual', clients_manual),
    ])

    facts_path = os.path.join(plugin_root, 'product-facts.json')
    nl = newline_of(facts_path)
    write_text(facts_path, (to_ps_json(facts) + '\n').replace('\n', nl))

    listing_path = os.path.join(plugin_root, 'dist', 'fab-listing.md')
    listing = f"""# Fab listing copy

Generated by scripts/gen_product_facts.py from product-facts.json. The Fab listing lives in
Epic's seller portal and cannot be written to programmatically - paste these blocks by hand,
and regenerate this file whenever product-facts.json changes.

## Short description

Drive the Unreal Editor from your AI coding assistant over MCP: {operations_rounded} editor
operations across {namespaces_rounded} namespaces, on Unreal Engine {ue_range}.

## Long description

PinWright puts an MCP server inside the Unreal Editor, so your AI coding assistant can drive
the editor directly instead of guessing at your project from source files. It exposes
{operations_rounded} editor operations across {namespaces_rounded} namespaces - actors, levels,
Blueprints, materials, Niagara, Sequencer, UMG, physics, networking and more - through a
single `call` tool with an on-disk wiki the assistant reads to discover them.

One-click setup for {', '.join(clients_one_click)}. {' and '.join(clients_manual)} connect with a
short manual config entry.

Works on Unreal Engine {ue_range}, editor-only, Windows and Linux. Covered by {tests_rounded}
automated tests run against every supported engine version.

## Technical details

- Plugin version: {version}
- Supported engine versions: {', '.join(ue_versions)}
- Editor operations: {operations_rounded}
- Namespaces: {namespaces_rounded}
- Automated tests: {tests_rounded}
- One-click MCP client setup: {', '.join(clients_one_click)}
- Manual MCP client setup: {', '.join(clients_manual)}
"""
    write_text(listing_path, listing.replace('\n', newline_of(listing_path)))

    # --- README namespace list: regenerated between marker comments -----------------------
    # Area groups and their order mirror the website's src/_data/namespaces.js so README and
    # site read the same; a registered namespace missing here lands in "Other" with a warning.
    readme_groups = OrderedDict([
        ('Project & assets', ['asset', 'blueprint', 'data_table', 'chooser', 'input', 'texture', 'geometry', 'model']),
        ('Inspect, debug & data', ['property', 'container', 'object', 'static_mesh', 'recorder', 'insights', 'performance']),
        ('Editor & system', ['editor', 'system', 'python', 'source_control', 'localization', 'misc', 'pipeline']),
        ('UI & widgets', ['widget', 'ui', 'drive']),
        ('Audio', ['audio']),
        ('Scene, level & world', ['actor', 'level', 'world_partition', 'volume', 'spline', 'foliage', 'landscape', 'water', 'environment', 'navigation', 'spatial']),
        ('Rendering & look', ['material', 'niagara', 'lighting', 'post_process', 'rendering', 'render', 'image', 'camera', 'effect', 'mrq']),
        ('Animation & rigging', ['animation', 'anim', 'controlrig', 'skeleton', 'pose_search', 'physics', 'sequencer']),
        ('AI & gameplay', ['ai', 'behavior_tree', 'eqs', 'state_tree', 'gas', 'gameplay_tags', 'character', 'game_framework', 'game_features', 'interaction', 'session', 'networking', 'vehicle', 'pcg']),
    ])
    # Internal-tier namespaces (plumbing, not for direct use) are left out of the list but are
    # counted in product-facts.json, so the list names them to keep the two totals reconcilable.
    listed = [ns for ns in namespaces if ns.get('tier') != 'internal']
    internal = [ns for ns in namespaces if ns.get('tier') == 'internal']
    grouped = {slug for members in readme_groups.values() for slug in members}
    other = [str(ns['slug']) for ns in listed if ns['slug'] not in grouped]
    if other:
        print('WARNING: Not in the README/website area groups, listed under Other: ' + ', '.join(other), file=sys.stderr)
        readme_groups['Other'] = other

    def noun(count):
        return 'operation' if count == 1 else 'operations'

    wiki_dir = os.path.dirname(registry_json)
    listed_operations = 0
    body = []
    for group, slugs in readme_groups.items():
        members = [ns for slug in slugs for ns in listed if ns['slug'] == slug]
        if not members:
            continue
        body += ['**%s**' % group, '']
        for ns in members:
            slug = str(ns['slug'])
            ops = wiki_operations(wiki_dir, slug)
            if len(ops) != int(ns['methods']):
                fail("Wiki lists %d method(s) for '%s' but registry.json registers %s; the wiki in %s is stale. Relaunch the editor to regenerate it."
                     % (len(ops), slug, ns['methods'], wiki_dir))
            listed_operations += len(ops)
            page = read_text(os.path.join(wiki_dir, slug + '.md'))
            lede = re.search(r'(?ms)^Stability:[^\n]*\n\s*\n([^#\s].*?)(?:\r?\n\s*\r?\n|\Z)', page)
            summary = short_description(lede.group(1) if lede else '').rstrip('.').replace('<', '&lt;').replace('>', '&gt;')
            if summary:
                summary = ': ' + summary
            maturity = 'Core' if ns.get('tier') == 'core' else 'Unclassified'
            body += ['<details>',
                     '<summary><code>%s</code>%s (%d %s, %s)</summary>' % (slug, summary, len(ops), noun(len(ops)), maturity),
                     '',
                     '| Operation | What it does |',
                     '| --- | --- |']
            for name in sorted(ops):
                body.append('| `%s` | %s |' % (name, format_cell(short_description(ops[name]))))
            body += ['', '</details>', '']

    block = ['<!-- namespaces:begin -->',
             '<details>',
             '<summary><strong>All %d public namespaces (%d operations)</strong></summary>' % (len(listed), listed_operations),
             '',
             'Every operation is documented in the in-editor wiki; the agent reads `call("<namespace>")` for any of these.',
             '']
    if internal:
        internal_names = ', '.join('`%s` (%s %s)' % (ns['slug'], ns['methods'], noun(int(ns['methods']))) for ns in internal)
        block += ['Not listed: internal plumbing namespaces, not intended for direct use - %s.' % internal_names, '']
    block += body + ['</details>', '<!-- namespaces:end -->']

    readme_path = os.path.join(plugin_root, 'README.md')
    readme = read_text(readme_path)
    begin = readme.find('<!-- namespaces:begin -->')
    end = readme.find('<!-- namespaces:end -->')
    if begin < 0 or end < begin:
        fail("README.md needs a '<!-- namespaces:begin -->' line followed by a '<!-- namespaces:end -->' line to hold the generated namespace list: " + readme_path)
    end += len('<!-- namespaces:end -->')
    readme = readme[:begin] + newline_of(readme_path).join(block) + readme[end:]

    # The overview paragraph quotes the full totals; it was hand-typed and went stale, so it
    # is rewritten here and a reworded sentence fails the run rather than keeping an old figure.
    facts_sentence = r'[\d,]+ operations across \d+ namespaces, developed against [\d,]+ automated tests'
    if not re.search(facts_sentence, readme):
        fail("README.md no longer contains the overview sentence '<N> operations across <N> namespaces, developed against <N> automated tests'; update the pattern in this script to match its new wording: " + readme_path)
    readme = re.sub(facts_sentence, lambda _: '{:,} operations across {} namespaces, developed against {:,} automated tests'.format(operations, namespace_count, tests), readme)
    write_text(readme_path, readme)

    print('Registry: ' + registry_json)
    print('Version: %s   UE: %s (%s)' % (version, ue_range, ', '.join(ue_versions)))
    print('Operations: %d -> %s' % (operations, operations_rounded))
    print('Namespaces: %d -> %s' % (namespace_count, namespaces_rounded))
    print('Tests: %d -> %s' % (tests, tests_rounded))
    print('Facts: ' + facts_path)
    print('Fab listing: ' + listing_path)


if __name__ == '__main__':
    main()
