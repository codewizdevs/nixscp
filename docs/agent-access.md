# Agent Access

Agent Access lets AI agents (Claude Code, Codex, Cursor, scripts, anything that can run a command) use your saved
SSH sites **without ever getting their passwords or private keys**. The running NixSCP application holds the
credentials and makes the SSH connection itself. The agent only talks to NixSCP through the `nixscp` command
(or the [MCP server](mcp.md)) and only gets the output.

Agent Access is **free**. It does not need a NixSCP Pro account.

- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Setting up a site](#setting-up-a-site)
- [Permissions](#permissions)
- [Approval: Ask me each time](#approval-ask-me-each-time)
- [Name for agents](#name-for-agents)
- [Command rules](#command-rules)
- [Session recordings](#session-recordings)
- [Audit log](#audit-log)
- [Command line reference](#command-line-reference)
- [Exit codes](#exit-codes)
- [Examples](#examples)
- [Security model and its limits](#security-model-and-its-limits)
- [Troubleshooting](#troubleshooting)

## How it works

1. You enable Agent Access for a site in NixSCP and choose what agents may do there.
2. An agent runs, for example, `nixscp --connect web1 --cmd "df -h"`.
3. That command connects to the running NixSCP over a local socket that only your user account can open
   (`$XDG_RUNTIME_DIR/nixscp-agent.sock` on Linux, a per-user named pipe on Windows).
4. NixSCP checks the site's settings, asks you if the site is set to ask, logs the request, opens the SSH
   connection with the stored credentials, runs the command and streams the output and exit code back.

Passwords and private keys are never printed, written to files or passed to the agent or any other program.

## Requirements

- **NixSCP must be running** (and, with NixSCP Pro, unlocked with your NixSCP password). The `nixscp --...`
  commands only relay requests to it.
- **Your saved passwords must be encrypted**, either with a **master password** (Preferences > Security) or with
  your **NixSCP password** (NixSCP Pro, see [encryption.md](encryption.md)), which encrypts the whole configuration.
  Without either, saved passwords are only obfuscated in the configuration file, which any program you run could
  decode; NixSCP then refuses every agent request and the Agent Access setting cannot be switched on.
- **SFTP or SCP sites only** (the site must use SSH). Workspaces are not offered.
- **The host key must already be known.** Connect to the site once in NixSCP and accept its key. Agent requests
  refuse unknown and changed host keys; they never ask you to accept one.
- **Authentication without prompts.** The site needs a stored password or a private key (or ssh-agent).
  Keyboard-interactive prompts are not supported. If the private key has a passphrase, NixSCP asks **you** for it
  in its own window when an agent connects; the passphrase stays in NixSCP.

A private key file **without** a passphrase is readable by any program you run, including an agent. The Agent
Access dialog warns about this. Protect the key with a passphrase (Tools > Run PuTTYgen, or `ssh-keygen -p`).

## Setting up a site

1. Open the Login dialog (Site Manager), select a saved SFTP or SCP site and click **Edit**.
2. Click **Agent Access...** (to the left of **Advanced...**).
3. Under **Access for agents**, choose:
   - **Off** (default): agents cannot see or use the site.
   - **Allowed without asking**: requests run right away (and are logged).
   - **Ask me each time**: NixSCP shows every request and waits for your answer.
4. Tick what agents may do (see [Permissions](#permissions)).
5. Optionally set a **Name for agents** and **Command rules**.
6. Click **OK**, then **Save** the site.

The Help button in the dialog opens the Agent Access FAQ. **Commands > Agent Access Help** shows the full
`nixscp --help` text.

## Permissions

| Checkbox | Allows | Agent commands |
|---|---|---|
| Run commands and interactive shells | Running commands and opening an interactive shell | `--connect`, MCP `run_command` |
| Download files | Downloading single files | `--get`, MCP `download_file` |
| Upload files (needs Download files) | Uploading single files | `--put`, MCP `upload_file` |
| Open tunnels | Local port forwarding through the site | `--tunnel` |
| Record sessions | Not a permission: keeps a transcript of each request ([Session recordings](#session-recordings)) | |

All four permissions and recording are on by default when you switch a site on. With **Download files** on and
**Upload files** off, the site is download-only. Command rules apply to commands only; they do not restrict
downloads, uploads or tunnels.

`nixscp --list` shows each site's permissions as `commands`, `files` (downloads and uploads) or `downloads`
(download-only) and `tunnels`, with `(restricted commands)` when the site has command rules.

## Approval: Ask me each time

With **Ask me each time**, each request opens a NixSCP window (on top of other windows, not tied to the main window)
that names the site and shows what is requested: the command, the file to download or upload, the tunnel, or an
interactive shell. The agent's command waits until you answer:

| Button | Effect |
|---|---|
| **Allow once** (default) | Runs this request only. |
| **1 hour** | Runs it, and allows all requests for this site for one hour without asking. |
| **24 hours** | The same for 24 hours. |
| **Always** | Runs it and changes the site to **Allowed without asking** (saved; logged as a settings change). Change it back in Agent Access... |
| **Deny** | Refuses the request: the agent gets exit code 255 and "The user denied the request in NixSCP." |

The 1 hour and 24 hours allowances are kept in memory only; they end when NixSCP exits. `nixscp --list` shows
such sites with `APPROVAL` = `asks user` (`"approval": "ask"` in JSON).

## Name for agents

By default agents see and address a site by its full name, including folders (`Clients/Acme/web1`). Set
**Name for agents** to give it another name, e.g. `web1`. Agents then see only that name in `nixscp --list` and
must use it with `--connect`, `--get`, `--put` and `--tunnel`; the real site name and folder stay hidden from them.
Host name, user name and port are still listed.

Names must be unique among the sites with Agent Access on; NixSCP refuses a duplicate. The audit log records both
names (`Clients/Acme/web1 [agent name: web1]`), and the approval window shows both.

## Command rules

Command rules are optional patterns that limit the commands agents may run on a site. Both lists empty (the
default) means every command is allowed.

- **Allowed commands**: one pattern per line. When the list is not empty, every part of a command must match one of
  these patterns.
- **Denied commands**: one pattern per line. No part of a command may match any of these.
- Empty lines and lines starting with `#` are ignored; leading and trailing spaces are trimmed.

### Pattern syntax

Patterns are wildcards that must match the **whole** part of the command (not a substring), case-sensitively:

| Wildcard | Matches |
|---|---|
| `*` | Any text, including spaces and `/`; also nothing |
| `?` | Any single character |
| `[abc]`, `[a-z]` | One character from the set or range |

So `uptime` allows only `uptime` exactly, `uptime*` also allows `uptime -p`, and `systemctl status *` allows
`systemctl status nginx` but not `systemctl restart nginx`. Spaces in the command are normalized (runs of
whitespace count as one space) before matching.

### How a command is checked

1. The command line is split into **simple commands** at `;`, `&`, `&&`, `|`, `||` and new lines. Characters inside
   single or double quotes, and characters escaped with `\`, do not split. For example
   `cd /var/log && tail -n 50 syslog | grep error` is checked as three parts: `cd /var/log`,
   `tail -n 50 syslog` and `grep error`.
2. If an **allowed** list exists:
   - command substitution (`$(...)` or backticks, outside single quotes) is refused;
   - output redirection to a file (`>` or `>>` followed by anything other than `/dev/null`) is refused.
     Redirection to another descriptor (`2>&1`, `>&2`) and to `/dev/null` is fine.
3. Each part is checked against the **denied** patterns; the whole command (with whitespace normalized) is also
   checked against them. Any match refuses the command.
4. If an **allowed** list exists, each part must match at least one allowed pattern.

A refused command never reaches the server. The agent gets exit code 255 and a message such as
`"systemctl restart nginx" is not among the commands allowed on this site.` The refusal is logged.

When a site has any command rules, **interactive shells are refused** (`nixscp --connect <site>` without `--cmd`),
because typed input cannot be checked.

### Read-only preset

**Read-only preset** fills in inspection commands (`ls*`, `cat *`, `tail *`, `grep *`, `find *`, `du *`, `df*`,
`ps *`, `systemctl status *`, `journalctl *`, `docker ps*`, `docker logs *`, `git status*`, `git log*`,
`git diff*`, `nginx -t` and others) as allowed, denies `find` actions that change things (`* -delete*`,
`* -exec *`, `* -execdir *`, `* -ok *`, `* -fprint*`), ticks **Download files** and unticks **Upload files**.
Review the list and adjust it. **Clear rules** empties both lists.

### Rules are a safety net, not a sandbox

Rules stop an agent from running something you did not intend by mistake. They cannot contain a determined
program:

- An allowed program can do more than its name suggests (`journalctl *` allows any `journalctl` options,
  `git diff*` can run configured external tools, editors and pagers can start shells, and so on).
- Without an allowed list, only the denied patterns are checked, and substitution and redirection are not
  refused, so a denied command can be reached in other ways (`sh -c '...'`, variables, scripts on the server).
- Only `$(...)` and backticks count as substitution; other shell expansions (variables, `<(...)`) are not
  inspected. Input redirection (`<`) is not restricted.
- The command runs in your login shell on the server with your account's rights.

For real limits, use a dedicated server account with only the rights the agent needs (and, for example, a
restricted `sudo` configuration), and use rules on top of that.

## Session recordings

With **Record sessions** on (default), NixSCP keeps a transcript of every agent request on the site: time, site,
request type, the command or file, the output (standard output and standard error, up to 4 MB per request), the
exit code or error. Standard input sent to a command is not recorded, only its size.

- View them with **Commands > Agent Sessions...**; **Delete all** removes them.
- They are stored in `agent-sessions/` next to the configuration file, readable by your user only.
- When the configuration is encrypted with the NixSCP password ([encryption.md](encryption.md)), the recordings are
  encrypted too. A recording made under another NixSCP password shows as "(encrypted with another NixSCP password)".
- Retention: the newest 1000 recordings from the last 90 days are kept; older ones are deleted automatically.

## Audit log

Every agent request is written to the audit log, allowed or not: listings, commands, transfers, tunnels, denials
(with the reason), failures, and changes made from the approval window (Always). Open it with
**Commands > Agent Access Log...**.

The log is the tab-separated text file `nixscp-agent.log` next to the configuration file (columns: time, site,
request, result, detail; details are cut at 500 characters). It is readable by your user only and is **not**
encrypted, so it shows site names and commands but never credentials or output.

## Command line reference

The agent commands are options of the `nixscp` program (not `nixscp-cli`). `nixscp --help` prints instructions
written for both people and AI agents. On Windows the program is `nixscp.exe` in the NixSCP folder.

| Command | What it does |
|---|---|
| `nixscp --list [--json]` | Lists the sites agents may use: name, `user@host[:port]`, what is permitted and whether you approve each request. `--json` prints an array of objects with `name`, `host`, `user`, `port`, `permissions` and `approval` (`ask` or `none`). |
| `nixscp --connect <site> --cmd "<command>"` | Runs the command on the server through the remote login shell, without a terminal. Standard input is passed to the command; standard output and standard error come back separately; the exit code is the command's. Everything after `--cmd` is the command, so `--cmd uname -a` works too. `--cmd="<command>"` is the same (then nothing may follow it). |
| `nixscp --connect <site>` | Opens an interactive shell. Needs a terminal (standard input must be a terminal). Refused on sites with command rules. |
| `nixscp --get <site> <remote-file> [<local-file-or-folder>]` | Downloads one file. Without a local name, saves into the current folder. Relative remote paths and `~/...` are in the home folder. |
| `nixscp --put <site> <local-file> [<remote-file-or-folder>]` | Uploads one file. Without a remote name, uploads into the home folder. |
| `nixscp --tunnel <site> <local-port>:<host>:<port>` | Forwards `127.0.0.1:<local-port>` to `<host>:<port>` as seen from the server (like `ssh -L`) until the command ends (Ctrl+C). The local port must be 1024 or higher. Instead of the specification you can give the name of one of the site's saved [tunnels](tunnels.md). |
| `nixscp --mcp` | Runs the [MCP server](mcp.md) on standard input/output. |
| `nixscp --help` | Shows the help text. (WinSCP-compatible switches: `nixscp /help`.) |

`--get` and `--put` transfer single files; for folders, use `tar` through `--connect` (see the examples). Sites
are addressed by the name shown in `nixscp --list`, exactly. At most 8 agent requests run at the same time; more
are refused with "Too many agent requests are running".

Notes for agents (also in `nixscp --help`): commands run without a TTY, so avoid interactive programs (`top`,
`less`, `vim`, password prompts) and use non-interactive flags (`--no-pager`, `-y`, `top -bn1`); quote the whole
remote command; do not try to read NixSCP's configuration files.

## Exit codes

| Code | Meaning |
|---|---|
| 0-254 | `--connect`: the exit code of the remote command. `--get`, `--put`, `--list`: 0 means success. |
| 255 | A NixSCP error, with a message on standard error: NixSCP not running or locked, passwords not encrypted, site not available to agents, permission missing, command refused by the rules, approval denied, unknown or changed host key, connection or authentication failure, wrong usage. |

A remote command that itself exits with 255 cannot be told apart from a NixSCP error by the code alone; the
NixSCP messages start with `nixscp:`.

## Examples

```bash
nixscp --list
nixscp --list --json

nixscp --connect "My Server" --cmd "uname -a && df -h"
nixscp --connect web1 --cmd "sudo systemctl status nginx --no-pager"
nixscp --connect web1 --cmd "tail -n 200 /var/log/nginx/error.log"

# standard input goes to the remote command
echo "hello" | nixscp --connect web1 --cmd "cat > /tmp/hello.txt"

# single files
nixscp --get web1 /etc/nginx/nginx.conf ./nginx.conf
nixscp --put web1 ./deploy.tar.gz /tmp/deploy.tar.gz

# folders through tar
nixscp --connect web1 --cmd "tar czf - /var/www/site" > site.tar.gz
nixscp --connect web1 --cmd "tar xzf - -C /var/www/site" < site.tar.gz

# a tunnel to the server's PostgreSQL, then use it
nixscp --tunnel db1 5433:localhost:5432 &
psql -h 127.0.0.1 -p 5433

# a saved tunnel by name
nixscp --tunnel db1 postgres
```

A typical instruction for an AI agent: *"Use `nixscp` to work on my servers. Run `nixscp --help` first, then
`nixscp --list`."*

Example of a read-only monitoring site: Access **Allowed without asking**, Run commands on, Download files on,
Upload files off, Open tunnels off, **Read-only preset** applied, Name for agents `prod-web`.

## Security model and its limits

What Agent Access protects:

- **Credentials.** The agent never gets the stored password, the private key or its passphrase. With the
  configuration encrypted (master password or NixSCP password), the agent also cannot decode them from NixSCP's
  files.
- **Scope.** Only sites you switched on are visible. Each site has its own permissions, rules and approval mode.
- **Server identity.** Unknown or changed host keys are refused, so an agent request never trusts a new server
  key on its own.
- **Visibility.** Every request is in the audit log; recordings keep the output.

What it does not protect against:

- **Programs running as your user.** The socket accepts any program of your user account, not a particular agent.
  Anything you run can use the sites you enabled, within their settings. Use **Ask me each time** for sites that
  matter.
- **What the agent does on the server.** An allowed command runs with the server account's full rights. Command
  rules are a safety net, not a sandbox (see above).
- **Unprotected key files.** A private key file without a passphrase can be read directly by any program you run.
- **A compromised computer.** Malware running as you could, for example, read NixSCP's memory or watch what you
  type. Agent Access keeps credentials away from well-behaved programs; it is not a defense against malware.
- **Copies the agent makes.** Output and downloaded files belong to the agent once they are returned.

Agent Access settings are personal: they are not shared through [Teams](teams.md) and cannot be set by a session
URL or `/rawconfig`.

## Troubleshooting

| Message | Fix |
|---|---|
| NixSCP is not running. | Start NixSCP (and unlock it with your NixSCP password). |
| Agent access needs the saved passwords encrypted. | Set a master password (Preferences > Security), or use NixSCP Pro's NixSCP password. |
| Site "x" is not available to agents. | Use a name exactly as listed by `nixscp --list`; switch the site on in Agent Access... |
| Site "x" does not allow commands / file transfers for agents. | Tick the permission in Agent Access... |
| The host key of ... is not known yet. / has CHANGED | Connect to the site in NixSCP and verify and accept the key. |
| The site has no stored password, or the stored password was refused. | Save the password with the site. |
| The server asks for interactive authentication, which agent access does not support. | Use a stored password or a key. |
| ... restricts the commands agents may run, so an interactive shell is not available. | Use `--cmd`. |
| An interactive shell needs a terminal. | Use `--cmd "<command>"`. |

See also: [MCP server](mcp.md), [Tunnels](tunnels.md), [Local encryption](encryption.md).
