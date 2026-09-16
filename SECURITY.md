# Security Policy

## Reporting a vulnerability

Report vulnerabilities through GitHub **private vulnerability reporting** on
[PinWright/pinwright-ue](https://github.com/PinWright/pinwright-ue/security/advisories/new).
Do not open a public issue for a vulnerability.

## Threat model

PinWright runs an MCP server inside the Unreal Editor process and exposes editor automation that
can create, edit, save, delete, and compile project assets. Editor-only does not make it
harmless: the plugin has the editor's access to the project. Treat it as a local developer tool
with asset-mutation privileges.

## Operating rules

- **Loopback only.** The listener binds to `127.0.0.1`. Do not expose the configured port to a
  LAN, VPN, container bridge, or the internet, and do not put a reverse proxy in front of it.
- **Bearer token.** Authentication is on by default; the token (64 hex characters) is generated
  on editor start and written to `<Project>/Saved/PinWright/gateway-token`. It blocks
  browser-based drive-by requests (CSRF, DNS rebinding) and other OS users. It is not a defense
  against code already running as your user - those processes are inside the trust boundary by
  design.
- **Never commit agent configs that embed the token.** Direct-HTTP client configs (Gemini
  `httpUrl`, VS Code, Cursor, Claude Code HTTP fallback) carry the literal secret in an
  `Authorization` header and must stay machine-local. The stdio-proxy config carries only the
  token file path and is safe to commit on that count.
- **Rotate** by deleting `gateway-token`, restarting the editor, and re-running **Install** in
  the setup screen.
- Use only trusted local MCP clients, do not run untrusted prompts against production project
  files, and keep the project under source control so generated changes are reviewable.

## Supported versions

Fixes land on `master` and ship in the next release. Only the latest release is supported.
