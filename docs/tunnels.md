# Tunnels (SSH port forwarding)

**Tunnels > Manage Tunnels...** keeps a list of saved local port forwards for your SFTP and SCP sites and starts and stops
them with one click. A tunnel listens on a port of this computer and forwards each connection through the site's
SSH connection to a host and port as seen from the server, like `ssh -L`. Typical use: reach a database, admin
panel or other service that only listens on the server (or on its private network).

Free; no NixSCP Pro account needed. WinSCP has no equivalent (its own "tunnel" setting is a jump host for the
connection itself, which NixSCP supports too, under Advanced > Connection > Tunnel).

## Adding a tunnel

1. Save the SFTP or SCP site first; tunnels go through its SSH connection.
2. Open **Tunnels > Manage Tunnels...** and click **Add...**.
3. Fill in:
   - **Site**: the saved site to tunnel through.
   - **Name**: a name for the tunnel, unique within the site (e.g. `postgres`).
   - **Local port (127.0.0.1)**: the port on this computer, 1024 to 65535.
   - **Forward to host** and **Forward to port**: the target as seen from the server. `localhost` is the server
     itself; a private address or name reaches another machine the server can reach.
4. Click **OK**.

Example: Local port `5433`, host `localhost`, port `5432` makes the server's PostgreSQL available as
`127.0.0.1:5433` on your computer (`psql -h 127.0.0.1 -p 5433`).

**Edit...** (or a double-click) changes a tunnel; **Remove** deletes it.

## Starting and stopping

Select a tunnel and click **Start**. NixSCP opens a separate SSH connection for it (with the site's stored
credentials and its usual host key check and prompts) and starts listening. The **Status** column shows
`Connecting...`, `Running`, `Stopped` or `Failed: <reason>`, and **Connections** shows how many connections are
open through it. **Stop** closes it.

- Tunnels listen on **127.0.0.1 only**: other computers on your network cannot use them.
- A started tunnel **keeps running when you close the Tunnels window**, until you stop it or NixSCP exits.
  Tunnels are not started automatically when NixSCP starts.
- The **Tunnels** menu of the main window lists every saved tunnel: click one to start it, click a running
  (checked) one to stop it. **Stop All Tunnels** stops them all.
- While tunnels run, the main window's status bar shows **⇄ 2 tunnels running**. Its tooltip lists them with their
  ports and open connections; a click opens Manage Tunnels.
- If the server refuses to forward a connection (for example `AllowTcpForwarding no` in its `sshd_config`, or nothing
  listens on the target), the tunnel stays up and the status shows the reason, e.g.
  `Running; The server refused to forward the connection to localhost:5432: ...`.
- If the SSH connection breaks, the tunnel stops and shows the error.
- A local port that is already in use fails with `Cannot listen on 127.0.0.1:<port>`.

## Where tunnels are stored

Tunnels are part of the site, in the `PortForwards` value of the site in `nixscp.ini` (one line per tunnel:
`name|local port|host|port`). So they are included in exports, [cloud sync](cloud-sync.md) and, for team sites,
shared with the [team](teams.md). WinSCP does not know this value. Tunnels cannot be set through a session URL or
`/rawconfig`.

## Tunnels for agents

With **Open tunnels** allowed in the site's [Agent Access](agent-access.md) settings, a program can open a tunnel
through NixSCP without getting the site's credentials:

```bash
nixscp --tunnel db1 5433:localhost:5432    # any forward
nixscp --tunnel db1 postgres               # a saved tunnel, by name (not case-sensitive)
```

The tunnel stays open until the command ends (Ctrl+C); run it in the background while the port is in use. Local
ports below 1024 are refused. Agent tunnels are separate from the ones started in the Tunnels window, follow the
site's approval setting, and are logged (and recorded, with the number of bytes forwarded). Tunnels are not
available through the [MCP server](mcp.md).
