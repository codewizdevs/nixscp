# MCP server

`nixscp --mcp` runs NixSCP as a [Model Context Protocol](https://modelcontextprotocol.io/) server, so AI tools
that speak MCP (Claude Code, Claude Desktop, Cursor, and others) can use your saved SSH sites as tools. It is the
same [Agent Access](agent-access.md) as the `nixscp --connect/--get/--put` commands, with the same settings,
approvals, rules, recordings and audit log: the agent never gets passwords or private keys.

Free; no NixSCP Pro account needed.

## Requirements

Everything from [Agent Access: Requirements](agent-access.md#requirements): NixSCP running and unlocked, saved
passwords encrypted (master password or NixSCP password), and Agent Access switched on for each site the agent
may use. The MCP server itself is started by the AI tool; it relays each tool call to the running NixSCP.

## Setup

Claude Code:

```bash
claude mcp add nixscp -- nixscp --mcp
```

Other MCP clients (a stdio server entry):

```json
{
  "mcpServers": {
    "nixscp": { "command": "nixscp", "args": ["--mcp"] }
  }
}
```

If `nixscp` is not on the `PATH` of the AI tool, give the full path (for example `/usr/bin/nixscp`, the AppImage
file, or `C:\Program Files\NixSCP\nixscp.exe`).

## Protocol

- Transport: JSON-RPC 2.0 over standard input/output, one message per line. Batches are accepted: the replies of a
  batch go out together, as one array.
- Protocol versions: `2025-06-18`, `2025-03-26` and `2024-11-05` (the client's version when it is one of these,
  otherwise the newest).
- Capabilities: `tools` (the list does not change). Methods: `initialize`, `ping`, `tools/list`, `tools/call`.
- Tool calls run one at a time, in the order they came. `ping` is answered during a call, and
  `notifications/cancelled` stops a call while it runs: it gets no reply, and NixSCP stops the work on the server.
- Text in a request must be valid Unicode: no lone surrogates, and no byte order mark at the start. Anything else comes
  back as a tool error.
- Server name `nixscp`, version the NixSCP version. The server's instructions tell the model to call
  `list_sites` first and to use site names exactly as listed.

## Tools

### list_sites

No arguments. Lists the sites the agent may use: `name`, `host`, `user`, `port`, `permissions` and `approval`
(`ask` or `none`), as text and as `structuredContent` `{"sites": [...]}`.

### run_command

Runs a shell command on a site (the user's login shell on the server, without a terminal).

| Argument | Required | Description |
|---|---|---|
| `site` | yes | Site name exactly as returned by `list_sites` |
| `command` | yes | The command line, e.g. `systemctl status nginx --no-pager` |
| `stdin` | no | Text passed to the command's standard input; it can be larger than 1 MiB |
| `max_output_bytes` | no | Output kept per stream; default 102400 (100 KiB), between 1024 and 2097152 (2 MiB) |

Result: text with the exit code, standard output and standard error, and `structuredContent`
`{"exit_code": n, "stdout": "...", "stderr": "...", "truncated": false}`. A non-zero exit code of the remote command
is a normal result; NixSCP errors (refused by the rules, approval denied, connection failed...) come back as a tool
error (`isError: true`) with the message.

### download_file

Downloads one file from a site to this computer.

| Argument | Required | Description |
|---|---|---|
| `site` | yes | Site name |
| `remote_path` | yes | File on the server (relative paths are in the home folder) |
| `local_path` | yes | Where to save it: a file name, or an existing folder |

For a folder, create an archive first with `run_command` (for example `tar czf /tmp/site.tar.gz /var/www/site`)
and download that.

### upload_file

Uploads one file from this computer to a site.

| Argument | Required | Description |
|---|---|---|
| `site` | yes | Site name |
| `local_path` | yes | File on this computer |
| `remote_path` | no | File or folder on the server; default: the home folder |

The file name of an upload cannot hold a path: give the folder as `remote_path`.

### read_note

Reads the site's note: what the user wrote down about the server. Needs **Allow Agent Access** in the site's note
([Site notes](agent-access.md#site-notes)).

| Argument | Required | Description |
|---|---|---|
| `site` | yes | Site name |

The result's `structuredContent` has `note` (empty when the site has no note).

### write_note

Replaces the whole note of a site (at most 64 KB). Needs **Allow Agent Edit** in the site's note. Agents should read
the note first and keep what is still true.

| Argument | Required | Description |
|---|---|---|
| `site` | yes | Site name |
| `text` | yes | The complete new note |

## What MCP does not offer

Interactive shells and tunnels are only available through the command line (`nixscp --connect <site>` and
`nixscp --tunnel`), not as MCP tools.

## Approval and long calls

On sites set to **Ask me each time**, a tool call waits until you answer in NixSCP. If you deny it, the tool
returns an error: "The user denied the request in NixSCP."

See also: [Agent Access](agent-access.md) for permissions, command rules, recordings and the security model.
