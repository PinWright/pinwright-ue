#!/usr/bin/env python3
# Copyright (c) 2026 Alexander Penkin. MIT License.

"""
pinwright_cli.py - command-line client for PinWright servers, no MCP client needed.

Drives one or several PinWright processes (editors and -game processes) of the SAME project at
once over the loopback JSON-RPC endpoint every server exposes (POST http://127.0.0.1:<port>/mcp).
The transport logic is mcp_proxy.Proxy's: the same liveness/readiness probe, the same error codes,
the same editor launch through pinwright_supervisor (memory cap, priority, detach). This file adds
instance discovery, instance names and argv handling.

    pinwright_cli.py list
    pinwright_cli.py call [--pid P | --name N | --port N] <method> [<args json> | @file | -]
    pinwright_cli.py start --name N --mode visible|offscreen|headless --reason "<why>"
                           [--game] [--port N] [--map M] [--timeout S] [--extra-arg=<switch> ...]
    pinwright_cli.py stop (--pid P | --name N | --port N) [--save | --discard] [--force]

No selector on `call` means the primary editor: the one Saved/PinWright/gateway-port names, which
is the editor the MCP proxy talks to. Exit codes: 0 success, 1 the server or launch refused (an
RPC error, a tool result with isError, a refused start/stop), 2 bad usage or no such instance.

Instance discovery follows docs/runtime-module.md: every serving process writes
<Project>/Saved/PinWright/instances/<pid>.json ({pid, port, token, kind, project, startTime,
reason, map, engine}) after it binds and deletes it on a clean shutdown. A file whose pid is gone,
or whose pid belongs to a process started after the file was written (pid reuse), is stale and
skipped. The primary editor is also found from gateway-port when it has no instance file, and is
always authorized with gateway-token (the MCP proxy's credential); every other instance with the
token from its own file.

Names are this client's: `start --name` records <Project>/Saved/PinWright/cli-names/<name>.json
({name, pid, mode, kind, reason, logPath, createdAt}) as soon as the launched pid is known, and
--name resolves through that pid. A server-written `name` field, if one ever appears, matches too.

Security: the host is always 127.0.0.1 and the path /mcp; an instance file can only choose a
loopback port and a token, and the token is only ever sent to that port.

Client id: requests carry X-PinWright-Client = $PINWRIGHT_CLIENT_ID, else "pinwright-cli". One id
across invocations is what lets `stop` quit an editor this agent drove a minute ago; agents
sharing one editor should each set their own id so editor.quit's EDITOR_IN_USE guard can tell
them apart.

Runs on Unreal's bundled Python, standard library only, like mcp_proxy.py.
"""

import argparse
import glob
import json
import os
import re
import sys
import time

import mcp_proxy
import pinwright_supervisor

CLIENT_ID_ENV = "PINWRIGHT_CLIENT_ID"
DEFAULT_CLIENT_ID = "pinwright-cli"
LAUNCHED_BY = "pinwright_cli"
PORT_SWITCH = "-PinWrightPort="
_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")
# A process that started more than this after a file naming its pid was written is not its author.
_PID_REUSE_SLACK_MS = 5000


class UsageError(Exception):
    """Bad arguments or no matching instance: exit code 2."""


def _url(port):
    return "http://127.0.0.1:%d/mcp" % port


def _is_port(value):
    return isinstance(value, int) and not isinstance(value, bool) and 1 <= value <= 65535


def _is_pid(value):
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def resolve_project(explicit):
    """Absolute .uproject: --project (a .uproject or the folder holding one), else the project
    this plugin is installed in."""
    if explicit:
        path = os.path.abspath(explicit)
        found = path if path.lower().endswith(".uproject") else \
            mcp_proxy._find_uproject_in_root(path, os.path.exists)
        if not found or not os.path.isfile(found):
            raise UsageError("no .uproject at %s" % explicit)
        return os.path.normpath(found)
    found = mcp_proxy.resolve_uproject(None, None, __file__, os.path.exists)
    if not found:
        raise UsageError("no .uproject found near %s; pass --project" % os.path.abspath(__file__))
    return found


def _pinwright_dir(project):
    return os.path.join(os.path.dirname(project), "Saved", "PinWright")


def _names_dir(project):
    return os.path.join(_pinwright_dir(project), "cli-names")


def _read_live_record(path, start_ms_fn):
    """The JSON object at path when the pid it names is alive and started before the file was
    written, else None."""
    try:
        written_ms = os.path.getmtime(path) * 1000
        with open(path, encoding="utf-8-sig") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None  # being written or removed right now; the next look sees it settled
    if not isinstance(data, dict) or not _is_pid(data.get("pid")):
        return None
    start_ms = start_ms_fn(data["pid"])
    if start_ms is None or start_ms > written_ms + _PID_REUSE_SLACK_MS:
        return None
    return data


def load_names(project, start_ms_fn=pinwright_supervisor.process_start_ms):
    """{pid: name} from this client's live name records."""
    names = {}
    for path in glob.glob(os.path.join(_names_dir(project), "*.json")):
        record = _read_live_record(path, start_ms_fn)
        if record and isinstance(record.get("name"), str):
            names[record["pid"]] = record["name"]
    return names


def load_instances(project, start_ms_fn=pinwright_supervisor.process_start_ms):
    """Live instances of project, oldest file first: one dict per live instance file, plus the
    primary editor from gateway-port when no instance file serves that port. Each entry gains
    instanceFile (None for the gateway-port entry), primary, name (None when unnamed) and
    tokenFile (gateway-token, for the primary only)."""
    pw_dir = _pinwright_dir(project)
    gateway_port = mcp_proxy._read_gateway_port(os.path.dirname(project))
    names = load_names(project, start_ms_fn)
    out = []
    for path in sorted(glob.glob(os.path.join(pw_dir, "instances", "*.json")),
                       key=lambda p: (os.path.getmtime(p) if os.path.exists(p) else 0, p)):
        data = _read_live_record(path, start_ms_fn)
        if data is None or not _is_port(data.get("port")):
            continue
        data["instanceFile"] = path
        data["primary"] = data["port"] == gateway_port
        data["name"] = data.get("name") if isinstance(data.get("name"), str) \
            else names.get(data["pid"])
        out.append(data)
    if _is_port(gateway_port) and not any(entry["port"] == gateway_port for entry in out):
        out.append({"pid": None, "port": gateway_port, "kind": "editor", "project": project,
                    "primary": True, "name": None, "instanceFile": None,
                    "source": "gateway-port"})
    for entry in out:
        if entry["primary"]:
            # The primary editor's credential is the MCP proxy's: gateway-token.
            entry["tokenFile"] = os.path.join(pw_dir, "gateway-token")
    return out


def select_instance(instances, pid=None, name=None, port=None):
    """The one instance a selector names; no selector = the primary editor."""
    if pid is not None:
        matches = [entry for entry in instances if entry.get("pid") == pid]
        what = "pid %d" % pid
    elif name is not None:
        matches = [entry for entry in instances if entry.get("name") == name]
        what = "name %r" % name
    elif port is not None:
        matches = [entry for entry in instances if entry["port"] == port]
        what = "port %d" % port
    else:
        matches = [entry for entry in instances if entry.get("primary")]
        what = "primary editor (no gateway-port published)"
    if not matches:
        raise UsageError("no live PinWright instance with %s; run `list`" % what)
    if len(matches) > 1:
        raise UsageError("%d live instances match %s (pids %s); select by --pid"
                         % (len(matches), what, ", ".join(str(m.get("pid")) for m in matches)))
    return matches[0]


class Endpoint(mcp_proxy.Proxy):
    """mcp_proxy.Proxy aimed at one instance instead of the gateway-port file. With find, the
    instance is re-resolved before every request (a launch whose port is not known yet), and
    on_spawn(pid) runs as soon as a launch has its editor pid."""

    def __init__(self, project, instance=None, find=None, on_spawn=None, call_timeout=150.0,
                 url=None):
        super().__init__(url or (_url(instance["port"]) if instance else None), 3.0, call_timeout,
                         5.0, (instance or {}).get("tokenFile"), None, None, 180.0, project)
        self.instance = instance
        self.find = find
        self.on_spawn = on_spawn
        self.launched_pid = None

    def _current(self):
        return self.find() if self.find else self.instance

    def _resolve_url(self):
        current = self._current() if self.find else None
        return _url(current["port"]) if current else self.url

    def _read_token(self):
        current = self._current()
        if current and current.get("tokenFile"):
            self.token_file = current["tokenFile"]
            return super()._read_token()
        token = current.get("token") if current else None
        return token.strip() if isinstance(token, str) and token.strip() else None

    def _wait_for_ready(self, proc, *args, **kwargs):
        self.launched_pid = proc.pid
        if self.on_spawn:
            self.on_spawn(proc.pid)
        return super()._wait_for_ready(proc, *args, **kwargs)

    def rpc(self, method, args):
        """Forward one `call`; args None fetches the wiki page reference for method."""
        arguments = {"method": method}
        if args is not None:
            arguments["args"] = args
        return self.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                            "params": {"name": "call", "arguments": arguments}})


def _emit(obj):
    sys.stdout.write(json.dumps(obj, indent=2, ensure_ascii=False) + "\n")


def _emit_tool_result(response, raw=False):
    """Print a tools/call response; return the exit code."""
    if not isinstance(response, dict):
        _emit({"error": "NO_RESPONSE"})
        return 1
    if "error" in response:
        _emit(response["error"])
        return 1
    result = response.get("result") or {}
    if raw:
        _emit(result)
    else:
        structured = result.get("structuredContent")
        text = "\n".join(block.get("text", "") for block in result.get("content") or []
                         if isinstance(block, dict))
        if structured is not None:
            _emit(structured)
            if result.get("isError") and text:
                sys.stderr.write(text + "\n")
        else:
            sys.stdout.write(text + "\n")
    return 1 if result.get("isError") else 0


def _parse_args_value(value):
    if value is None:
        return None
    if value == "-":
        value = sys.stdin.read()
    elif value.startswith("@"):
        with open(value[1:], encoding="utf-8-sig") as fh:
            value = fh.read()
    try:
        parsed = json.loads(value)
    except ValueError as exc:
        raise UsageError("args must be a JSON object: %s" % exc)
    if not isinstance(parsed, dict):
        raise UsageError("args must be a JSON object, got %s" % type(parsed).__name__)
    return parsed


def _public(entry):
    """An instance as printed: never the literal token."""
    return {key: value for key, value in entry.items() if key != "token"}


def cmd_list(opts, project):
    entries = []
    for entry in load_instances(project):
        endpoint = Endpoint(project, entry)
        state, detail = endpoint._probe_state(_url(entry["port"]))
        entry = _public(entry)
        entry["state"] = state
        if detail:
            entry["stateDetail"] = str(detail)
        entries.append(entry)
    _emit({"project": project, "instances": entries, "count": len(entries)})
    return 0


def cmd_call(opts, project):
    entry = select_instance(load_instances(project), opts.pid, opts.name, opts.port)
    endpoint = Endpoint(project, entry, call_timeout=opts.timeout)
    return _emit_tool_result(endpoint.rpc(opts.method, _parse_args_value(opts.args)), opts.raw)


def _write_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = "%s.%d.tmp" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
    os.replace(tmp, path)


def cmd_start(opts, project):
    if not _NAME_RE.match(opts.name):
        raise UsageError("--name must match %s" % _NAME_RE.pattern)
    port = opts.port or 0
    if port and not _is_port(port):
        raise UsageError("--port must be 0 (ephemeral) or 1-65535")
    record_path = os.path.join(_names_dir(project), opts.name + ".json")
    held = _read_live_record(record_path, pinwright_supervisor.process_start_ms)
    taken = [e for e in load_instances(project) if port and e["port"] == port]
    if held or taken:
        _emit({"error": "INSTANCE_IN_USE",
               "detail": "name %r is held by live pid %s; stop it or pick another name"
                         % (opts.name, held["pid"]) if held else
                         "port %d is served by a live instance; pick another port" % port,
               "instance": _public(taken[0]) if taken else held})
        return 1
    extra = [PORT_SWITCH + str(port)] + (["-game"] if opts.game else []) + list(opts.extra_arg or [])
    if mcp_proxy._abslog_path(extra) is None:
        extra.append("-Abslog=%s" % os.path.join(
            os.path.dirname(project), "Saved", "Logs", "%s-%s.log" % (
                os.path.splitext(os.path.basename(project))[0], opts.name)))
    kind = "game" if opts.game else "editor"

    def find():
        pid = endpoint.launched_pid
        return next((e for e in load_instances(project) if e.get("pid") == pid), None) \
            if pid else None

    def on_spawn(pid):
        _write_json(record_path, {
            "name": opts.name, "pid": pid, "mode": opts.mode, "kind": kind,
            "reason": opts.reason, "logPath": mcp_proxy._abslog_path(extra),
            "createdAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())})

    # An explicit port is probed by the start guard before anything launches; an ephemeral one is
    # unknown until the new process writes its instance file, which find() polls for.
    endpoint = Endpoint(project, find=find, on_spawn=on_spawn, url=_url(port) if port else None)
    start_args = {"mode": opts.mode, "reason": opts.reason, "extra_args": extra}
    if opts.map is not None:
        start_args["map"] = opts.map
    if opts.timeout is not None:
        start_args["timeout"] = opts.timeout
    result = endpoint._editor_start(start_args, launched_by=LAUNCHED_BY)
    structured = result.get("structuredContent")
    if isinstance(structured, dict):
        current = find()
        structured.update({"name": opts.name, "kind": kind,
                           "instanceFile": current.get("instanceFile") if current else None})
        if current:
            structured["port"] = current["port"]
        if result.get("isError") and structured.get("error") == "EDITOR_START_TIMEOUT" \
                and current is None:
            structured["hint"] = (
                "no instance file for pid %s appeared under %s; a server without instance-file "
                "support (docs/runtime-module.md) cannot be found on an ephemeral port"
                % (endpoint.launched_pid, os.path.join(_pinwright_dir(project), "instances")))
    return _emit_tool_result({"result": result})


def cmd_stop(opts, project):
    if opts.pid is None and opts.name is None and opts.port is None:
        raise UsageError("stop needs --pid, --name or --port")
    if opts.save and opts.discard:
        raise UsageError("--save and --discard are mutually exclusive")
    entry = select_instance(load_instances(project), opts.pid, opts.name, opts.port)
    endpoint = Endpoint(project, entry)
    quit_args = {"reason": opts.reason}
    for flag in ("save", "discard", "force"):
        if getattr(opts, flag):
            quit_args[flag] = True
    response = endpoint.rpc("editor.quit", quit_args)
    result = (response or {}).get("result") if isinstance(response, dict) else None
    if not isinstance(result, dict) or result.get("isError"):
        return _emit_tool_result(response)
    down = endpoint._wait_for_endpoint_down(_url(entry["port"]))
    _emit({"stopped": down, "pid": entry.get("pid"), "port": entry["port"],
           "name": entry.get("name"),
           "detail": None if down else "acknowledged editor.quit but still answering after "
           "%.0fs; nothing was killed" % mcp_proxy._EDITOR_STOP_TIMEOUT})
    return 0 if down else 1


def _selector(parser):
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--pid", type=int, help="instance by process id")
    group.add_argument("--name", help="instance by the name `start --name` gave it")
    group.add_argument("--port", type=int, help="instance by loopback port")


def build_parser():
    parser = argparse.ArgumentParser(
        prog="pinwright_cli.py",
        description="Drive PinWright editors and games of one project over loopback JSON-RPC.")
    parser.add_argument("--project", help=".uproject (or its folder); default: the host project")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="live instances of the project, probed")

    call = sub.add_parser("call", help="run one RPC; no args = its wiki page reference")
    _selector(call)
    call.add_argument("method", help="dotted RPC name, e.g. system.identity")
    call.add_argument("args", nargs="?", help="JSON object, @file, or - for stdin; {} for none")
    call.add_argument("--timeout", type=float, default=150.0, help="seconds (default 150)")
    call.add_argument("--raw", action="store_true", help="print the whole tool result")

    start = sub.add_parser("start", help="launch an editor (or -game) on its own port, wait ready")
    start.add_argument("--name", required=True, help="instance name, [A-Za-z0-9_.-], unique")
    start.add_argument("--mode", required=True, choices=mcp_proxy.LAUNCH_MODES)
    start.add_argument("--reason", required=True, help="why it is launched (one line)")
    start.add_argument("--game", action="store_true", help="launch with -game")
    start.add_argument("--port", type=int, help="loopback port; default 0 = ephemeral")
    start.add_argument("--map", help="startup map")
    start.add_argument("--timeout", type=float, help="readiness ceiling in seconds")
    start.add_argument("--extra-arg", action="append",
                       help="extra switch; repeat; write --extra-arg=-Switch")

    stop = sub.add_parser("stop", help="editor.quit, then wait for the endpoint to go down")
    _selector(stop)
    stop.add_argument("--reason", default="pinwright_cli stop", help="recorded in the exit log")
    stop.add_argument("--save", action="store_true")
    stop.add_argument("--discard", action="store_true")
    stop.add_argument("--force", action="store_true", help="past EDITOR_IN_USE only")
    return parser


_COMMANDS = {"list": cmd_list, "call": cmd_call, "start": cmd_start, "stop": cmd_stop}


def main(argv=None):
    opts = build_parser().parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    mcp_proxy._CLIENT_ID = os.environ.get(CLIENT_ID_ENV) or DEFAULT_CLIENT_ID
    try:
        return _COMMANDS[opts.command](opts, resolve_project(opts.project))
    except UsageError as exc:
        sys.stderr.write("pinwright_cli: %s\n" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main())
