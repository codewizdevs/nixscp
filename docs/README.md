# NixSCP documentation

NixSCP is a native port of WinSCP for Linux, macOS and Windows. Everything WinSCP does is documented by WinSCP itself
([winscp.net/eng/docs](https://winscp.net/eng/docs/)); the Help buttons and F1 in NixSCP open those pages.
The pages below cover what NixSCP adds on top of WinSCP.

| Page | What it covers | Availability |
|---|---|---|
| [Agent Access](agent-access.md) | Let AI agents and other programs use your saved SSH sites without giving them passwords or keys: permissions, approval, command rules, recordings, audit log, the `nixscp --list/--connect/--get/--put/--tunnel` commands | Free |
| [MCP server](mcp.md) | `nixscp --mcp`: the same access for MCP clients such as Claude Code | Free |
| [Tunnels](tunnels.md) | Saved SSH port forwards per site (Tunnels > Manage Tunnels...) | Free |
| [Cloud sync](cloud-sync.md) | End-to-end encrypted sync of sites, passwords, preferences, workspaces, bookmarks and open tabs | NixSCP Pro (free during the beta) |
| [Local encryption and the NixSCP password](encryption.md) | The whole local configuration encrypted with your NixSCP password, recovery key, encrypted and WinSCP-compatible exports, `nixscp-cli` unlock | NixSCP Pro (free during the beta) |
| [Teams](teams.md) | Shared, end-to-end encrypted site folders with owner/admin/member roles and one-time invite codes | NixSCP Pro (free during the beta) |
| [New-release check](updates.md) | The startup check for new versions and Preferences > Updates | Free |
| [Differences from WinSCP](differences-from-winscp.md) | What NixSCP adds or changes, and what is not ported | |

The website versions of these pages are at [nixscp.com/docs](https://nixscp.com/docs).

## Where NixSCP keeps its files

All files live next to the configuration file: `~/.config/nixscp/` on Linux, `~/Library/Application Support/NixSCP` on macOS, `%APPDATA%\NixSCP` on Windows
(or the folder of the file given with `/ini=`).

| File | Contents | Encrypted with the NixSCP password (Pro) |
|---|---|---|
| `nixscp.ini` | Sites, stored passwords, preferences, workspaces, bookmarks (WinSCP.ini format) | Yes |
| `nixscp-cloud-session.ini` | Cached open tabs of cloud sync | Yes |
| `nixscp-teams.ini` | Team names, team sync state, verified member list pins and older team keys | Yes |
| `backups/nixscp-*.ini` | Copies made before cloud settings replace the local ones (last 10) | Yes |
| `agent-sessions/*.log` | Agent session recordings | Yes |
| `nixscp-cloud.ini` | Cloud account, sign-in token, sync state, the wrapped (password-protected) data key | Yes, once a NixSCP password is set |
| `nixscp-cloud-keys.ini` | Key changes under way: a new data key, old keys, team keys on their way to the server, a recovery key not confirmed yet | Yes |
| `nixscp-cloud-bridge.ini` | The old data key, only while a new key reaches the local files | Yes |
| `nixscp-agent.log` | Agent access audit log | No (owner-only permissions) |
