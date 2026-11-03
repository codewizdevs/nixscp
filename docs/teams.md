# Teams (NixSCP Pro)

Teams share site folders between NixSCP accounts, **end-to-end encrypted**: colleagues get the same sites, with
their stored passwords and settings, without e-mailing configuration files around. Owners and admins edit the sites;
members use them. The NixSCP cloud stores only encrypted data and never sees the sites, the passwords or the team
key.

Teams are part of **NixSCP Pro**, **free during the public beta**. Every person in a team needs a NixSCP account,
signed in with [cloud sync](cloud-sync.md) unlocked on their computer.

## Quick start

1. **Pro > Teams...** > **New Team...**, and give it a name. You are its owner.
2. Put the sites you want to share into a site folder (Login dialog: **New Folder**), then select the team and click
   **Add Folder...**. The folder's sites move into the team and appear in the site tree under
   **Team: &lt;team name&gt;**.
3. Click **Invite...**, choose the role and how long the code is valid, and click **Create Code**. Send the code
   privately (chat, in person).
4. The colleague opens **Pro > Teams... > Join with Code...** and enters the code. The team's sites appear in their
   site tree.

## Roles

| | Owner | Admin | Member |
|---|---|---|---|
| Use the team's sites | Yes | Yes | Yes |
| Edit the team's sites (changes are shared) | Yes | Yes | No: local changes are reverted |
| Add folders to the team | Yes | Yes | No |
| Invite members | Yes | Yes | No |
| Invite admins | Yes | No | No |
| See and revoke pending invites | Yes | Yes | No |
| Make a member admin, or an admin member | Yes | No | No |
| Remove members | Yes | Yes | No |
| Remove admins | Yes | No | No |
| Rename the team | Yes | Yes | No |
| Leave the team | No (delete it instead) | Yes | Yes |
| Delete the team | Yes | No | No |

Each team has exactly one owner, who cannot leave or be removed. **Members...** shows everyone's e-mail address,
role and join date; owners and admins also see the pending invites there.

## The Teams window

| Button | What it does |
|---|---|
| New Team... | Creates a team with you as owner. |
| Join with Code... | Joins a team with an invite code. |
| Sync Now | Syncs all teams now (otherwise 20 seconds after NixSCP starts and every 3 minutes). |
| Add Folder... | Moves the sites of one of your site folders into the selected team. |
| Invite... | Creates an invite code. |
| Members... | Members and roles; Make Admin, Make Member, Remove; pending invites and Revoke Invite. |
| Rename... | Renames the team (the `Team: <name>` folder follows on every computer). |
| Leave | Leaves the team; its sites are removed from this computer. |
| Delete | Deletes the team for everyone: all members lose its sites and its synced data is deleted. Cannot be undone. |

The **Status** column shows `Synced`, `Renewing the team key...`, or `Waiting for the team key from an admin`.

## Team sites

- A team's sites live in the site folder **`Team: <team name>`** (with subfolders as you added them). They work
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
