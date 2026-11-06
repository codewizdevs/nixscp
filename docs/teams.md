# Teams (NixSCP Pro)

Teams share site folders between NixSCP accounts, **end-to-end encrypted**: colleagues get the same sites, with
their stored passwords, without e-mailing configuration files around. Owners and admins edit the sites; members use
them. The NixSCP cloud stores the sites, their passwords and the team's name only in encrypted form. It does see who
is in which team: see [What the server can and cannot do](#what-the-server-can-and-cannot-do) and
[What you trust your team with](#what-you-trust-your-team-with).

Teams are part of **NixSCP Pro**, **free during the public beta**. Every person in a team needs a NixSCP account with
a verified e-mail address (creating or joining a team needs it), signed in with [cloud sync](cloud-sync.md) unlocked
on their computer.

Teams made with an earlier version of NixSCP are not carried over: create the team again and invite everyone again.

## Quick start

1. **Teams > New Team...** (on Android: menu > Teams), and give it a name. You are its owner.
2. Share sites with it, in any of these ways:
   - In the Login dialog, select a site and click **Share with Team...** (next to Agent Access), or right-click a site
     or a folder > **Share with Team...**.
   - In the main window, **Teams > Share Current Site with Team...** shares the site of the current tab.
   - In **Teams > Manage Teams...**, select the team and use **Share Sites...** (tick any of your sites) or
     **Share Folder...**.

   Shared sites appear in the site tree under **Team: &lt;team name&gt;**. When you share single sites you can keep
   your own copy as well; otherwise the site moves into the team's folder.
3. On the **Members** tab, click **Invite...**, choose the role and how long the code is valid, and click
   **Create Code**. Send the code privately (chat, in person): whoever has it can join.
4. The colleague chooses **Teams > Join Team with Code...** and enters the code. The team's sites appear in their site
   tree right away.

## Roles

| | Owner | Admin | Member |
|---|---|---|---|
| Use the team's sites | Yes | Yes | Yes |
| Edit the team's sites (changes are shared) | Yes | Yes | No: local changes are reverted |
| Share sites and folders with the team, stop sharing them | Yes | Yes | No |
| Restore an earlier version of the team's sites | Yes | Yes | No |
| Invite members | Yes | Yes | No |
| Invite admins | Yes | No | No |
| See and revoke pending invites | Yes | Yes | No |
| Make a member admin, or an admin member | Yes | No | No |
| Remove members | Yes | Yes | No |
| Remove admins | Yes | No | No |
| Rename the team | Yes | Yes | No |
| Leave the team | No (delete it instead) | Yes | Yes |
| Delete the team | Yes | No | No |

Each team has exactly one owner, who cannot leave or be removed. A team of yours that other people are in blocks deleting
your account until you delete the team in the app; a team with nobody else goes with the account. The **Members** tab
shows everyone's e-mail address, role, join date and key fingerprint; owners and admins also see the
pending invites on the **Invites** tab.

## The Teams menu and window

The main window's **Teams** menu has:

| Item | What it does |
|---|---|
| Manage Teams... | Opens the Teams window. |
| Share Current Site with Team... / Stop Sharing Current Site... | For the stored site of the current tab. |
| Your teams | Each team with your role and how many sites it shares; opens it in the Teams window. |
| New Team..., Join Team with Code... | Creates a team, or joins one with an invite code. |
| Sync Teams Now | Syncs all teams now (otherwise 20 seconds after NixSCP starts, every minute, and when the Login dialog opens). |

The **Teams window** lists your teams on the left. For the selected team:

| Part | What it does |
|---|---|
| Shared Sites | Everything the team shares, as a tree. **Share Sites...** (tick any of your sites), **Share Folder...**, **Stop Sharing...** for the selected sites or folders. |
| Members | Everyone in the team with role, join date and key fingerprint, and your own fingerprint; **Invite...**, **Make Admin**, **Make Member**, **Remove...**. |
| Invites | Pending invite codes (owners and admins); **New Invite...**, **Revoke**. |
| Team menu | **Rename...**; **Restore from History...**; **Delete Team...** (owner) or **Leave Team...** (everyone else). |

## Stop sharing

Owners and admins can stop sharing any site or folder of a team: in the Teams window (**Stop Sharing...**), in the
Login dialog (select a team site > **Stop Sharing...**, or right-click > **Stop Sharing with Team...**), or from the
**Teams** menu for the current site. Choose:

- **Stop Sharing, Keep My Copy**: the site moves back to your own sites (with its password).
- **Stop Sharing and Delete**: the site is deleted.

Either way the members lose it at their next sync (within about a minute while NixSCP runs). Stopping a share does
not undo what they saw: they could have copied the stored passwords while it was shared, so change those passwords on
the server.

## Team sites

- A team's sites live in the site folder **`Team: <team name>`** (with subfolders as you added them). New, changed and
  removed team sites show up in the open Login dialog right away, with a note in the status bar. They work
  like any other site. A team whose name looks like one you already have (a lookalike, or the same name) gets a folder
  of its own with `(2)`.
- **Owners and admins** edit them in the Login dialog as usual; NixSCP uploads the changes to the team. If the team's
  sites changed on the server in the meantime, your edit stays on your computer and is not uploaded, and NixSCP tells
  you. You decide in the Teams window: **Keep My Version for Everyone** (the team's version stays in its history) or
  **Use the Team's Version** (your changes are lost). The same choice comes up when all of a team's sites are gone
  from your computer: nothing is uploaded until you decide.
- **Members'** local changes to team sites are reverted at the next sync, with a notice. To keep a changed copy,
  members can save a copy of a site outside the team folder.
- Team sites are synced only with the team, not with your personal [cloud sync](cloud-sync.md).
- [Agent Access](agent-access.md) settings stay personal: they are never shared through a team. Each person decides
  for themselves whether agents may use a team site, and those settings survive team updates.

## What a team shares

A team shares only a fixed list of plain site settings: host name, port, user name, stored password, protocol and
FTPS mode, start folders, note and color, an exact host key fingerprint, and plain connection and transfer options
(timeouts, keepalive, directory caching, time zone handling, FTP and S3 modes and the like). A setting that is not on
the list is not shared.

Anything that runs commands or weakens checks is not shared, and neither is anything that depends on your computer.
These stay on each computer, and each person's own values survive team updates:

- proxies, saved [tunnels](tunnels.md) and the connection through a jump host;
- private key and certificate files;
- shell, SFTP server and post-login commands, and the recycle bin;
- Agent Access settings and agent forwarding;
- algorithm lists, SSH bug switches and the logical host name.

So a site that uses a key file or a proxy reaches the others without it, and each person sets that up for themselves.
Only host key fingerprints can travel with a site, never `*` or `acceptnew`. Host and user names that would expand a
variable or start with `-` are refused.

## Invites

- An invite code looks like `<id>-<secret>`: a 16-character id, a dash and a 32-character secret.
- It works **once**, for the role chosen (member, or admin when the owner invites), and expires after the chosen
  number of days (1 to 30, default 7). Pending invites can be revoked on the **Invites** tab.
- **NixSCP does not keep the code**: copy it when it is shown.
- The **secret never reaches the server**. NixSCP encrypts the team key with a key derived from the secret, and the
  server stores only that encrypted copy and a hash of a separate proof (verifier) derived from the secret. To join,
  the app presents the proof and signs with a key only the whole code gives; only the whole code can decrypt the team
  key. A wrong code does not use up the invite.
- The encrypted copy also names the entry of the signed member list that created the invite, so the joining app can
  tell that it joined the real team and not a copy the server made up.
- When typing a code, lower case is accepted, `O` is read as `0` and `I`/`L` as `1`.

## Encryption overview

- Every account has an **X25519 key pair**, which receives team keys, and an **Ed25519 identity key pair**, which
  signs. The private keys are stored on the server encrypted with your personal sync data key, which is why Teams need
  cloud sync unlocked. NixSCP remembers the identity key of each account and never makes a new one while it knows one.
- Who is in a team, with which role and which keys, is recorded in a **signed member list**: a hash-chained log in
  which every change (join, removal, role, new team key) is signed by the person who made it. NixSCP replays the whole
  list itself and ignores what the server says about members, roles and keys.
- Every team has a random 256-bit **team key**. Each member receives it wrapped for their public key (an X25519
  sealed box, bound to the team and the key version). The hash of every wrap is in the signed member list, so a key
  the server hands out on its own is refused.
- The team's sites and its name are uploaded as one snapshot encrypted with **AES-256-GCM** under the team key and
  **signed** by the owner or admin who made it. NixSCP accepts it only from someone the member list shows as owner or
  admin at that point.

## What the server can and cannot do

The server still enforces membership and roles, but your NixSCP does not depend on that. The server:

- **cannot read** the team's sites, their passwords or the team's name;
- **cannot add members, change roles or forge changes**: those need signatures it does not have. Clients detect
  tampering and refuse it.

The server **still sees** who is in which team, roles, e-mail addresses, sizes, times and device names.

A stolen account password, mailbox or refresh token does not give team data without the NixSCP password. With the
account password someone can still delete the account, and with it the teams it owns that nobody else is in. A team
with other members blocks that: deleting it takes the owner's signature, in the app.

Two things cannot be detected:

- **A rollback on a new computer.** A computer that is new to the team has nothing to compare with, so it cannot tell
  when the server shows it an older member list or older sites, as long as they contain your own signed join. A
  computer that has already synced refuses both.
- **A complete freeze.** A server that simply stops delivering updates looks the same as a team where nothing changes.

When a check fails, NixSCP stops syncing that team and says why, in a notice and in the Teams window (on Android, in
the Teams list):

```
The team "Prod" failed verification and is not synced: <reason>. Your sites were not touched.
```

Nothing is uploaded or changed until it is resolved. Tell the team's owner, and write to the address on
[nixscp.com/security](https://nixscp.com/security).

If it stays that way, you can take the team off this computer: **Remove from This Computer...** in the Teams window
offers **Remove, Keep the Sites as My Own** (they become a plain folder, `<team name> (former team)`) and **Remove with
the Sites**. On Android these are **Remove, keep the sites** and **Remove with the sites**. Nothing changes for the
other members, and the team stays away until someone invites you again. The owner can still delete a team that fails
verification.

## What you trust your team with

The encryption protects a team from the server, not from its own members:

- **Every member can read the shared passwords.** NixSCP needs them to connect, so a member's computer has them.
- **Stopping a share or removing a member does not undo what they saw.** Those passwords should be changed on the
  server, and keys they had access to replaced.
- **An admin controls the site list.** An admin can still point a shared site at another host, and your NixSCP then
  connects there with the site's stored password. Every member is told when the host, port, user, protocol, encryption
  or host key of a shared site changes, agent forwarding switches off when the host changes, and the usual host key
  check applies, but only make people admins if you trust them.

## Member fingerprints and notices

The **Members** tab shows each member's key fingerprint, with your own below the list. Compare fingerprints with your
colleagues over another channel (in person, by phone): a match proves the key is really theirs. The e-mail address next
to it does not prove that: it is what the server says about the account, and accounts created before e-mail
verification existed count as verified.

NixSCP tells you when the list changes:

- when someone joins, with their e-mail address and fingerprint;
- when someone comes back with a different key: compare the new fingerprint with them;
- when your own role changes.

A member whose key cannot be used is labelled in the list: nothing can be wrapped for that key, so they get no new team
key.

## Restoring an earlier version

Owners and admins can go back with **Team > Restore from History...** in the Teams window (on Android: in the team's
menu). The chosen version becomes the team's content for everyone, as a new version, and later versions stay in the
history. The server keeps the last 30 versions. A version under an older team key can be restored only on a computer
that has that key. A restore also works while a key renewal is pending: the restored sites go up under the new key.

## Removing members and key rotation

When an owner or admin removes someone (or someone leaves):

1. The server immediately stops giving them access to the team, including old snapshots.
2. On the next sync, an admin's or the owner's NixSCP creates a **new team key**, wraps it for every remaining member,
   and uploads the sites again under the new key, in one step. Until then the server refuses new content and new
   invites under the key that person has. NixSCP renews the key only after a signed removal or leave in the member
   list, never because the server asks.
3. The removed person's NixSCP removes the team's sites at its next sync and shows a notice.

**Limitation:** a removed member could have copied the server passwords (or the whole sites) while they were a
member, and a computer that stays offline keeps its local copy. NixSCP reminds you of this when you remove someone:
**change the server passwords that matter**, and replace keys they had access to.

## If you lose your NixSCP password and recovery key

Your team keys are encrypted with your personal data key. Without the NixSCP password and the recovery key they cannot
be recovered, and there is no reset. The way back is a new account and a new invite to each team. If you own a team
that other people are in, you cannot delete it without your keys, and that blocks deleting the old account as well:
write to the address on [nixscp.com/security](https://nixscp.com/security).

## Limits

- At most 20 teams owned per account, 50 members per team and 50 pending invites per team.
- Teams need an active NixSCP Pro license for everyone, also for reading (during the beta every account has one).
- Teams share sites (with their stored passwords and the settings listed [above](#what-a-team-shares)), not
  preferences, workspaces or bookmarks.
- The team key and invites are handled by NixSCP; there is no admin console on the website beyond a read-only list
  of your teams on the account page.

See also: [Cloud sync](cloud-sync.md), [Local encryption](encryption.md), [Agent Access](agent-access.md).
