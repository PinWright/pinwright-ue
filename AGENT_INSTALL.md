# Installing PinWright (for AI assistants)

Steps for an AI coding assistant installing PinWright into an Unreal project and connecting itself to it. `<project>` is the folder holding the `.uproject`. `<engine>` is the engine root, the folder that contains `Engine/`.

## 1. Install

The plugin goes in `<project>/Plugins/PinWright` and the folder must be named exactly that. Replace an existing copy only while the editor is closed.

Read `EngineAssociation` in the `.uproject`. A value like `"5.8"` means a launcher engine of that version. A GUID or a path means a source-built engine.

| Project | Method |
|---|---|
| C++ project (has `Source/` and a `"Modules"` array in the `.uproject`), **or** Linux, **or** source-built engine | Clone the latest release tag, not `master` |
| Blueprint-only project on Windows with a launcher engine | Download the prebuilt zip |

Find the latest release tag:

```sh
gh release view -R PinWright/pinwright-ue --json tagName -q .tagName
# or: curl -s https://api.github.com/repos/PinWright/pinwright-ue/releases/latest | grep '"tag_name"'
# or: git ls-remote --tags --sort=-v:refname https://github.com/PinWright/pinwright-ue.git
```

Clone that tag:

```sh
git clone --depth 1 --branch <tag> https://github.com/PinWright/pinwright-ue.git <project>/Plugins/PinWright
```

The zip is `PinWright-<version>-UE<major.minor>-Win64.zip` from the latest release, for example `PinWright-1.0.0-UE5.8-Win64.zip`. Extract its `PinWright/` folder into `<project>/Plugins/`.

## 2. Connect

Write the entry that the **Install** button in the editor's Setup window writes. The entry starts the bundled stdio proxy, which reads the port and token files before every call. That is why the config works before the editor has ever been launched. Always pass `--token-file` and `--port-file` explicitly. Use absolute paths with forward slashes.

- **Server name:** `pinwright`
- **Command:** the engine's bundled Python. On Windows that is `<engine>/Engine/Binaries/ThirdParty/Python3/Win64/python.exe`, on Linux `<engine>/Engine/Binaries/ThirdParty/Python3/Linux/bin/python3`. Any Python 3 also works, because the proxy uses only the standard library.
- **Args:** `<project>/Plugins/PinWright/Content/Python/mcp_proxy.py --token-file <project>/Saved/PinWright/gateway-token --port-file <project>/Saved/PinWright/gateway-port`

Claude Code uses `<project>/.mcp.json`. If the file exists, merge this entry into it:

```json
{
  "mcpServers": {
    "pinwright": {
      "type": "stdio",
      "command": "<engine>/Engine/Binaries/ThirdParty/Python3/Linux/bin/python3",
      "args": [
        "<project>/Plugins/PinWright/Content/Python/mcp_proxy.py",
        "--token-file", "<project>/Saved/PinWright/gateway-token",
        "--port-file", "<project>/Saved/PinWright/gateway-port"
      ]
    }
  }
}
```

Codex CLI uses `<project>/.codex/config.toml`:

```toml
[mcp_servers.pinwright]
command = "<engine>/Engine/Binaries/ThirdParty/Python3/Win64/python.exe"
args = ["<project>/Plugins/PinWright/Content/Python/mcp_proxy.py", "--token-file", "<project>/Saved/PinWright/gateway-token", "--port-file", "<project>/Saved/PinWright/gateway-port"]
startup_timeout_sec = 5.0
tool_timeout_sec = 300.0
```

| Client | File | Format |
|---|---|---|
| Claude Code | `<project>/.mcp.json` | JSON above |
| Codex CLI | `<project>/.codex/config.toml` | TOML above. It loads only after the user trusts the project. |
| Cursor | `<project>/.cursor/mcp.json` | Same JSON as Claude Code. Cursor may add it disabled, so the user enables it in Cursor's MCP settings. |
| Gemini CLI | `<project>/.gemini/settings.json` | Same JSON. It loads only in trusted folders. |
| VS Code (GitHub Copilot) | `<project>/.vscode/mcp.json` | Same entry under the top-level key `servers` instead of `mcpServers` |
| Windsurf | `~/.codeium/windsurf/mcp_config.json` (user-level) | `mcpServers.pinwright` with the same `command` and `args` |
| Cline | `cline_mcp_settings.json` (in VS Code globalStorage) | `mcpServers.pinwright` with the same `command` and `args` |
| Anything else | Its own MCP config | The same command and args in that client's format |

## 3. Finish

Tell the user to restart you, the assistant, so it loads the new MCP server, and then to open the project in the Unreal Editor. On a C++ project or a source install, the editor compiles the plugin the first time it opens and asks to rebuild modules; the user answers yes. Once the editor is running, the `call` tool works. Call `call()` to get the namespace index.

## 4. Update

Repeat step 1 with the latest release tag or zip, replacing the old `Plugins/PinWright` copy while the editor is closed. If the paths have not changed, the step 2 config stays valid.
