# Teams (NixSCP Pro)

Teams share site folders between NixSCP accounts, **end-to-end encrypted**: colleagues get the same sites, with
their stored passwords and settings, without e-mailing configuration files around. Owners and admins edit the sites;
members use them. The NixSCP cloud stores only encrypted data and never sees the sites, the passwords or the team
key.

Teams are part of **NixSCP Pro**, **free during the public beta**. Every person in a team needs a NixSCP account,
signed in with [cloud sync](cloud-sync.md) unlocked on their computer.

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
   **Create Code**. Send the code privately (chat, in person).
4. The colleague chooses **Teams > Join Team with Code...** and enters the code. The team's sites appear in their site
   tree right away.

## Roles

| | Owner | Admin | Member |
|---|---|---|---|
| Use the team's sites | Yes | Yes | Yes |
| Edit the team's sites (changes are shared) | Yes | Yes | No: local changes are reverted |
| Share sites and folders with the team, stop sharing them | Yes | Yes | No |
| Invite members | Yes | Yes | No |
| Invite admins | Yes | No | No |
| See and revoke pending invites | Yes | Yes | No |
| Make a member admin, or an admin member | Yes | No | No |
| Remove members | Yes | Yes | No |
| Remove admins | Yes | No | No |
| Rename the team | Yes | Yes | No |
| Leave the team | No (delete it instead) | Yes | Yes |
| Delete the team | Yes | No | No |

Each team has exactly one owner, who cannot leave or be removed. The **Members** tab shows everyone's e-mail
address, role and join date; owners and admins also see the pending invites on the **Invites** tab.

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
| Members | Everyone in the team with role and join date; **Invite...**, **Make Admin**, **Make Member**, **Remove...**. |
| Invites | Pending invite codes (owners and admins); **New Invite...**, **Revoke**. |
| Team menu | **Rename...**; **Delete Team...** (owner) or **Leave Team...** (everyone else). |

## Stop sharing

Owners and admins can stop sharing any site or folder of a team: in the Teams window (**Stop Sharing...**), in the
Login dialog (select a team site > **Stop Sharing...**, or right-click > **Stop Sharing with Team...**), or from the
**Teams** menu for the current site. Choose:

- **Stop Sharing, Keep My Copy**: the site moves back to your own sites (with its password).
- **Stop Sharing and Delete**: the site is deleted.

Either way the members lose it at their next sync (within about a minute while NixSCP runs). They could have copied
the stored passwords while it was shared, so change the passwords that matter.

## Team sites

- A team's sites live in the site folder **`Team: <team name>`** (with subfolders as you added them). New, changed and
  removed team sites show up in the open Login dialog right away, with a note in the status bar. They work
  like any other site.
- **Owners and admins** edit them in the Login dialog as usual; NixSCP uploads the changes to the team. If two admins
  change the team's sites at the same time, the version that reached the cloud first wins, and the other admin is
  told that their change was not kept.
- **Members'** local changes to team sites are reverted at the next sync, with a notice. To keep a changed copy,
  members can save a copy of a site outside the team folder.
- Team sites are synced only with the team, not with your personal [cloud sync](cloud-sync.md).
- [Agent Access](agent-access.md) settings stay personal: they are never shared through a team. Each person decides
  for themselves whether agents may use a team site, and those settings survive team updates.
- Saved [tunnels](tunnels.md) of a team site are part of the site and are shared.

## Invites

- An invite code looks like `<id>-<secret>`: a 16-character id, a dash and a 32-character secret.
- It works **once**, for the role chosen (member, or admin when the owner invites), and expires after the chosen
  number of days (1 to 30, default 7). Pending invites can be revoked under **Members...**.
- **NixSCP does not keep the code**: copy it when it is shown.
- The **secret never reaches the server**. NixSCP encrypts the team key with a key derived from the secret, and the
  server stores only that encrypted copy and a hash of a separate proof (verifier) derived from the secret. To join,
  the app presents the proof; only the whole code can decrypt the team key. A wrong code does not use up the invite.
- When typing a code, lower case is accepted, `O` is read as `0` and `I`/`L` as `1`.

## Encryption overview

- Every account has an **X25519 key pair**. The private key is stored on the server encrypted with your personal
  sync data key, which is why Teams need cloud sync unlocked.
- Every team has a random 256-bit **team key**. Each member receives it wrapped for their public key (an X25519
  sealed box, bound to the team and the key version, so the server cannot move a wrap to another team or version).
- The team's sites are uploaded as one snapshot encrypted with **AES-256-GCM** under the team key, in the same
  authenticated format as cloud sync. Its team, version and key version are authenticated.
- The server enforces membership, roles and versions; it never sees the team key, the sites or their passwords.

## Removing members and key rotation

When an owner or admin removes someone (or someone leaves):

1. The server immediately stops giving them access to the team, including old snapshots.
2. On the next sync, an admin's or the owner's NixSCP creates a **new team key**, wraps it for every remaining member,
   and uploads the sites again under the new key. The server then refuses uploads under the old key.
3. The removed person's NixSCP removes the team's sites at its next sync and shows a notice.

**Limitation:** a removed member could have copied the server passwords (or the whole sites) while they were a
member, and a computer that stays offline keeps its local copy. NixSCP reminds you of this when you remove someone:
**change the server passwords that matter**, and replace keys they had access to.

## Limits

- At most 20 teams owned per account, 50 members per team and 50 pending invites per team.
- Teams need an active NixSCP Pro license for everyone, also for reading (during the beta every account has one).
- Teams share sites (with their stored passwords and settings), not preferences, workspaces or bookmarks.
- The team key and invites are handled by NixSCP; there is no admin console on the website beyond a read-only list
  of your teams on the account page.

See also: [Cloud sync](cloud-sync.md), [Local encryption](encryption.md), [Agent Access](agent-access.md).
