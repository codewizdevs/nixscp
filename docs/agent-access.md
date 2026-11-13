# Agent Access

Agent Access lets AI agents (Claude Code, Codex, Cursor, scripts, anything that can run a command) use your saved
SSH sites **without ever getting their passwords or private keys**. The running NixSCP application holds the
credentials and makes the SSH connection itself. The agent only talks to NixSCP through the `nixscp` command
(or the [MCP server](mcp.md)) and only gets the output.

Agent Access is **free**. It does not need a NixSCP Pro account, except for
[session recordings](#session-recordings), which need the NixSCP password.

- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Setting up a site](#setting-up-a-site)
- [Permissions](#permissions)
- [Approval: Ask me each time](#approval-ask-me-each-time)
- [Settings prompt (master password only)](#settings-prompt-master-password-only)
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
3. That command connects to the running NixSCP over a local socket that only your user account can open. It lives
   only in a private folder: `$XDG_RUNTIME_DIR/nixscp-agent.sock` on Linux and `nixscp-agent.sock` in `$TMPDIR` on
   macOS, when that folder is private to you, else `~/.nixscp-agent.sock`, never `/tmp`. On Windows it is a per-user
   named pipe, and a pipe of that name created by someone else is refused.
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
| Allow Agent Access (in the site's note) | Reading the site's note | `--note`, MCP `read_note` |
| Allow Agent Edit (in the site's note, needs Allow Agent Access) | Replacing the site's note | `--note-set`, MCP `write_note` |
| Record sessions | Not a permission: keeps a transcript of each request ([Session recordings](#session-recordings)) | |

The first four permissions and recording are on by default when you switch a site on. The note permissions are off
until you tick them above the note in the Login dialog ([Site notes](#site-notes)). With **Download files** on and
**Upload files** off, the site is download-only. Command rules apply to commands only; they do not restrict
downloads, uploads or tunnels.

`nixscp --list` shows each site's permissions as `commands`, `files` (downloads and uploads) or `downloads`
(download-only), `tunnels`, and `note` or `note-edit`, with `(restricted commands)` when the site has command rules.

## Site notes

Every site has a note: what you need to know about the server, such as where its logs are, what runs there or what
not to restart. It is shown in the Login dialog under the site's settings. Click **Edit** to change it and **Save** to
keep it; the site itself is not edited for this. When you edit the site (its own **Edit**, or **New Site**), the note
is edited with it and saved by the site's **Save**. A site with a note has a small yellow page on its icon, and its
tooltip shows the start of the note. A note holds up to 64 KB of text. It travels with the site: in cloud sync, in
exports, and in WinSCP's own **Note** field (**Advanced › Note**), so WinSCP shows it too.

On a team site, the note is private to you until an owner or admin shares it with the team; then everyone in the
team sees it, and owners and admins (or everyone, with **Allow Team Edits**) edit it. See [Teams](teams.md#notes).

A note does not need a server. **New Note**, right below **New Site** at the top of the site list, opens an empty note:
type it, then **Save** asks for its name (the first line is offered; use / for a folder). **New Note...** in the
**Manage** menu, or the context menu of the site list, asks for the name first. Either way you get a note item: a yellow page in the list, with only the note and no connection settings. Use it for what
belongs with your sites but is not one, such as office network details or who to call when a server is down. Note
items live in folders, sync, and can be shared with a team like sites; they cannot be opened, so **Login** stays off,
and scripts refuse them. Agents see them in `nixscp --list` as `(note)` and can only read or edit their note. WinSCP,
which knows nothing of them, shows a note item as a site without a host name.

The two checkboxes above the note decide what agents may do with it:

- **Allow Agent Access**: agents may read the note (`nixscp --note <site>`, the MCP tool `read_note`). Give agents the
  facts they need before they work on the server, so they do not have to guess.
- **Allow Agent Edit**: agents may replace the note (`nixscp --note-set <site>`, the MCP tool `write_note`), for
  example to record what they found or changed. On a team site only someone who may edit the note can tick it,
  because an agent cannot do more than its user.

They are Agent Access permissions of the site, kept on this computer like the others. When Agent Access is off for the
site, ticking one asks whether to turn it on, with **Ask me each time** and the note as the only permission: agents get
no commands, files or tunnels from it. Unticking the last permission turns Agent Access off again. Notes are for SFTP
and SCP sites, like the rest of Agent Access.

A note request needs no connection to the server: NixSCP answers it from the saved site. It is asked about and logged
like any other request. The audit log records that the note was read or replaced and the new note's size, not its
text.

## Approval: Ask me each time

With **Ask me each time**, each request opens a NixSCP window (on top of other windows, not tied to the main window)
that names the site and shows what is requested: the command, the file to download or upload, the tunnel, or an
interactive shell. The agent's command waits until you answer:

| Button | Effect |
|---|---|
| **Allow once** | Runs this request only. |
| **1 hour** | Runs it, and allows all requests for this site for one hour without asking. |
| **24 hours** | The same for 24 hours. |
| **Always** | Runs it and changes the site to **Allowed without asking** (saved; logged as a settings change). Change it back in Agent Access... |
| **Deny** (default) | Refuses the request: the agent gets exit code 255 and "The user denied the request in NixSCP." |

The 1 hour and 24 hours allowances are kept in memory only; they end when NixSCP exits. `nixscp --list` shows
such sites with `APPROVAL` = `asks user` (`"approval": "ask"` in JSON).

## Settings prompt (master password only)

With the NixSCP password the whole configuration is encrypted, and this section does not apply. With a **master
password alone**, only the passwords are encrypted: `nixscp.ini` is text that any program you run can edit, an agent
included, to raise its own permissions or to point a site with a stored password at another server.

So the settings a site runs with carry a seal that only the master password can make, in the file
`nixscp-agent-seals` next to the configuration. The seal covers every saved setting of the site, except the passwords
and what only the panels use (color, note, start folders).

A site without a matching seal (new settings, settings edited outside the Agent Access dialog, a new master password)
is not used as it is: the next agent request opens a window that shows the server, the permissions, the approval, the
rules and every other setting that is not at its default. **Accept these settings** trusts them until they change;
**Deny** refuses the request and keeps asking. `nixscp --list` shows such a site as `asks user`.

Settings you make in the Agent Access dialog are sealed when you click **OK**. The seal covers more than it used to, so
after this update each site asks once. The seal cannot detect an old settings file put back together with its old seal:
those are settings you accepted at some time.

## Name for agents

By default agents see and address a site by its full name, including folders (`Clients/Acme/web1`). Set
**Name for agents** to give it another name, e.g. `web1`. Agents then see only that name in `nixscp --list` and
must use it with `--connect`, `--get`, `--put` and `--tunnel`; the real site name and folder stay hidden from them.
Host name, user name and port are still listed.

Names must be unique among the sites with Agent Access on; the Agent Access window refuses a duplicate. The audit log
records both names (`Clients/Acme/web1 [agent name: web1]`), and the approval window shows both.

A copy of a site does not take its name for agents: **Save As** and sharing a copy into a team leave the copy without
one, so agents know it by its site name, and the original keeps its name for agents. The copy keeps the original's
Agent Access settings; with a master password, it shows the [settings prompt](#settings-prompt-master-password-only)
once, as any new site does.

Two sites can still end up with one name for agents, for example when two computers set it at the same time and sync.
NixSCP never guesses which one an agent means: requests to that name are refused, with a message that names the
other way in. Until you give them different names, each of those sites is listed and reached by its site name,
folders included (`Clients/Acme/web1`), so for these sites the folder names are not hidden.

## Command rules

Command rules are optional patterns that limit the commands agents may run on a site. Both lists empty (the
default) means every command is allowed, except one with a NUL character or text that is not valid Unicode. Rules are
[a safety net, not a security boundary](#rules-are-a-safety-net-not-a-security-boundary).

- **Allowed commands**: one pattern per line. When the list is not empty, every part of a command must match one of
  these patterns.
- **Denied commands**: one pattern per line. No part of a command may match any of these.
- Empty lines and lines starting with `#` are ignored; leading and trailing spaces are trimmed.
- A pattern with a `[` and no closing `]` is not valid, and the dialog does not accept it. If one gets into the
  settings another way, a denied pattern that is not valid blocks every command on the site.

### Pattern syntax

Patterns are wildcards that must match the **whole** part of the command (not a substring). Allowed patterns are
case-sensitive; denied patterns are not, as some servers have case-insensitive file systems.

| Wildcard | Matches |
|---|---|
| `*` | Any text, including spaces and `/`; also nothing |
| `?` | Any single character |
| `[abc]`, `[a-z]` | One character from the set or range |

So `uptime` allows only `uptime` exactly, `uptime*` also allows `uptime -p`, and `systemctl status *` allows
`systemctl status nginx` but not `systemctl restart nginx`. Quoting is removed before matching. Denied patterns see
spaces normalized (runs of whitespace count as one space). Allowed patterns match the words exactly, joined by one
space: `'a  b'` keeps its two spaces, and a no-break space is not a space.

### How a command is checked

A request must be valid Unicode text, on every site: no lone surrogates, and no byte order mark at the start. A check
that takes over 1 second, or a line too complex for the patterns to give an answer, is refused; for a denied pattern, a
match that fails with an error counts as denied.

1. The whole command (with whitespace normalized) is checked against the **denied** patterns. Any match refuses it.
2. An **allowed** pattern without wildcards that equals the whole command line lets it through, past the steps below:
   you have spelled out this very line.
3. The command line is read the way a POSIX shell reads it and split into **simple commands** at `;`, `&`, `&&`, `|`,
   `||` and new lines. Characters inside single or double quotes, and characters escaped with `\`, do not split. For
   example `cd /var/log && tail -n 50 syslog | grep error` is checked as three parts: `cd /var/log`,
   `tail -n 50 syslog` and `grep error`.
4. **What the checker cannot read with certainty is refused**, on every site with rules, also one with denied patterns
   only: control characters, comments, `$VAR`, `$(...)` and backticks, `$'...'`, process substitution, here-documents,
   line continuations, subshells and functions, unquoted braces, shell keywords (`if`, `for`...), `VAR=value` prefixes,
   unterminated quotes, and wildcards in a program name or an option. Text in single quotes is fine.
5. A program that runs shell text is refused unless an allowed pattern names it, which also means on a site with
   denied patterns only: `sh`, `bash` and other shells, `eval`, `source`, `.`, `alias`, `trap`, `su`, `runuser`, `sg`,
   `script`, `watch`, `compgen`, `complete`, `mapfile`, `readarray`, `bind`, `fc`, `enable`, and the options `sudo -s`,
   `sudo -i`, `doas -s`, `env -S`, `flock -c` and `strace -o`. A bash builtin (`let`, `printf`, `declare`, `read`...) with
   an array subscript the shell would evaluate is refused too, whatever the patterns say.
6. If an **allowed** list exists, redirection that writes to a file (`>`, `>>`, `<>`, `>&file`, `&>`, followed by
   anything other than `/dev/null`) is refused. Redirection to another descriptor (`2>&1`, `>&2`) and to `/dev/null`
   is fine.
7. Each part is checked against the **denied** patterns, also what follows programs that start other programs
   (`sudo`, `env`, `xargs`, `find -exec` and the like). Any match refuses the command.
8. If an **allowed** list exists, each part must match at least one allowed pattern. A program name with spaces in it
   needs a pattern equal to the whole command.

A refused command never reaches the server. The agent gets exit code 255 and a message such as
`"systemctl restart nginx" is not among the commands allowed on this site.` The refusal is logged.

When a site has any command rules, **interactive shells are refused** (`nixscp --connect <site>` without `--cmd`),
because typed input cannot be checked.

### Read-only preset

**Read-only preset** fills in inspection commands as allowed (`ls`, `cat *`, `tail *`, `grep *`, `find *`, `du`,
`df`, `ps`, `journalctl`, `docker ps`, `docker logs *`, `git status`, `git log`, `git diff`, `nginx -t` and others).
Programs with options that write or change something are narrowed to their harmless forms:

- `sort` and `uniq` only in forms that read standard input, without file names or `-o`;
- `date` and `hostname` only to print;
- `ip` only to show addresses, routes and links;
- `systemctl` only `status`, `is-active`, `is-enabled`, `list-units` and `--failed`.

It denies options of the allowed programs that write files or run others (`find` actions such as `-delete` and
`-exec`, `journalctl --rotate`, `git --output` and more), ticks **Download files** and unticks **Upload files** and
**Open tunnels**. Review the list and adjust it. **Clear rules** empties both lists. Read-only does not mean private:
`cat *` and **Download files** reach whatever the server account can read.

### Rules are a safety net, not a security boundary

Rules check text, and the server runs programs. They stop an agent from running something you did not intend by
mistake, and they refuse what they cannot parse. They cannot stop:

- **An allowed program doing more than its name suggests**: options nobody listed, programs that run others (`awk`,
  `vim`, `less`, `tar --to-command`), `git` running the hooks and filters a repository configures.
- **A denied program under another name**: a copy, a symbolic link, a script, `python3 -c ...`. Denied patterns are a
  block list of spellings.
- **A `*` that matches spaces and `/`**: `rm *.tmp` also allows `rm /etc/passwd x.tmp`, and `cat /var/log/*` also
  allows `cat /var/log/../x`.
- **What a wildcard in an argument expands to** on the server.
- **A login shell that is not POSIX-like** (`csh`, `fish`), which reads the same text differently.
- **A server in a legacy multibyte locale**: Shift JIS, GBK or Big5 can take the second byte of a UTF-8 character for a
  `\` or a `|`.

Input redirection (`<`) is not restricted. The command runs in your login shell on the server with your account's
rights.

A site that must not be changed needs a server account without those rights, not command rules: use a dedicated server
account with only the rights the agent needs (and, for example, a restricted `sudo` configuration), and use rules on
top of that.

## Session recordings

With **Record sessions** on (default), NixSCP keeps a transcript of every agent request on the site: time, site,
request type, the command or file, the output (standard output and standard error, up to 4 MB per request), the
exit code or error. Standard input sent to a command is not recorded, only its size.

Recordings hold whatever commands print, so they are **always encrypted** with the NixSCP key
([encryption.md](encryption.md)) and refused without it. With a master password only, nothing is recorded: the Agent
Access dialog warns about it, and the audit log marks such requests `allowed; not recorded`.

- View them with **Commands > Agent Sessions...**; **Delete all** removes them.
- They are stored in `agent-sessions/` next to the configuration file, readable by your user only.
- A recording made under another NixSCP password shows as "(encrypted with another NixSCP password)".
- Retention: the newest 1000 recordings from the last 90 days are kept; older ones are deleted automatically.

## Audit log

Every agent request is written to the audit log, allowed or not: listings, commands, transfers, tunnels, denials
(with the reason), failures, and changes made from the approval window (Always). Open it with
**Commands > Agent Access Log...**.

The log is the tab-separated text file `nixscp-agent.log` next to the configuration file (columns: time, site,
request, result, detail). It is readable by your user only and is **not** encrypted. It stores site names and the first
500 characters of each command as it was sent, so a secret typed inline into a command lands there. It holds no stored
passwords or keys and no output. If the log cannot be written, NixSCP refuses the request.

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
| `nixscp --note <site>` | Prints the site's note. Needs **Allow Agent Access** in the site's note. |
| `nixscp --note-set <site> [<file>]` | Replaces the site's note with the text of the file, or with standard input (at most 64 KB; an empty input clears the note). Needs **Allow Agent Edit**. |
| `nixscp --mcp` | Runs the [MCP server](mcp.md) on standard input/output. |
| `nixscp --help` | Shows the help text. (WinSCP-compatible switches: `nixscp /help`.) |

`--get` and `--put` transfer single files; for folders, use `tar` through `--connect` (see the examples). The file
name of an upload cannot hold a path: give the folder as the remote file. Sites
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

# what to know about the server, then a note of what was done
nixscp --note web1
printf 'Checked 2026-11-12: nginx 1.26, disk 41%% used.\n' | nixscp --note-set web1
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
- **Visibility.** Every request is in the audit log; with the NixSCP password, recordings keep the output.

What it does not protect against:

- **Programs running as your user.** The socket accepts any program of your user account, not a particular agent.
  Anything you run can use the sites you enabled, within their settings. Use **Ask me each time** for sites that
  matter.
- **What the agent does on the server.** An allowed command runs with the server account's full rights. Command
  rules are a safety net, not a security boundary (see above).
- **Secrets in commands.** A password typed into a command is sent to the server as part of it, and lands in the audit
  log.
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
| More than one site is available to agents as "x". | Use one of the site names the message lists. To keep the short name, give the sites different names for agents. |
| The user did not accept the Agent Access settings of the site in NixSCP. | With a master password only: see [Settings prompt](#settings-prompt-master-password-only). |
| The agent access log cannot be written, so NixSCP refuses agent requests. | Check `nixscp-agent.log` next to the configuration and the free disk space. |
| ... are not allowed on this site. | The rules refused what they cannot check: run plain commands, and quote text with such characters in single quotes. |
| ... runs commands NixSCP cannot check ... | Run the command itself, or add an allowed pattern that names it. |
| The command is too complex to be checked against the rules of this site. | Split it into shorter commands. |
| A request must be valid Unicode text ... | Send the text without lone surrogates or a leading byte order mark. |
| The denied pattern "x" of this site is not valid, so no command can be checked. | Correct the pattern in Agent Access... |
| Site "x" does not allow commands / file transfers for agents. | Tick the permission in Agent Access... |
| The host key of ... is not known yet. / has CHANGED | Connect to the site in NixSCP and verify and accept the key. |
| The site has no stored password, or the stored password was refused. | Save the password with the site. |
| The server asks for interactive authentication, which agent access does not support. | Use a stored password or a key. |
| ... restricts the commands agents may run, so an interactive shell is not available. | Use `--cmd`. |
| An interactive shell needs a terminal. | Use `--cmd "<command>"`. |

See also: [MCP server](mcp.md), [Tunnels](tunnels.md), [Local encryption](encryption.md).
