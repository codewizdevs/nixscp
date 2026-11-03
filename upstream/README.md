# Tracking WinSCP

NixSCP is a port of [WinSCP](https://github.com/winscp/winscp). Bug fixes and
security fixes WinSCP makes later usually apply to NixSCP too, so every
upstream change is reviewed by hand. Detection is automatic, porting is not.

## Baseline

[`winscp.json`](winscp.json) records the last WinSCP commit that is reviewed:

- `baseline`: NixSCP was ported from WinSCP `master` at
  `246bbd42342c` (2026-10-24), which is WinSCP 6.7 plus 7 commits.
- `baseline_release`: the newest WinSCP release included (6.7).

## What happens automatically

The [WinSCP upstream](../.github/workflows/winscp-upstream.yml) workflow runs
daily (and on demand from the Actions tab). It runs
`tools/winscp_upstream.py sync`, which opens one GitHub issue for each WinSCP
commit after the baseline that has no issue yet. Each issue has:

- the commit title, the WinSCP tracker link (`Issue NNNN`) and the message;
- the changed WinSCP files with their NixSCP counterparts, found from the
  `Port of WinSCP source/...` comments at the top of NixSCP sources;
- the diff (shortened for large commits);
- a review checklist.

Labels: `winscp-upstream` on all, `upstream-security` when the title looks like
a security fix (review these first), `upstream-translations` for translation
only changes, `upstream-no-counterpart` when NixSCP has no port of the changed
files (Windows-only UI, .NET assembly...), `upstream-release` for the summary
issue opened when WinSCP tags a release.

Issues are found again by the `[winscp <sha>]` marker in their body, so a
change never gets two issues, also after the issue is closed.

## Reviewing a change

1. Read the WinSCP diff and the tracker entry.
2. Decide whether it applies. Most of `source/core` (protocols, transfers,
   scripting) applies; `source/forms` and `source/windows` are VCL UI that
   NixSCP rewrote in Qt, so check whether the behaviour exists in the Qt
   dialog; Windows-only behaviour can still matter for the Windows build.
3. Port it in the NixSCP counterpart, with a test where possible. Mention the
   upstream commit or tracker issue in the commit message.
4. Tick the checklist and close the issue. If it does not apply, write why and
   close it as "not planned".

## Moving the baseline

When the oldest issues are closed:

```sh
python3 tools/winscp_upstream.py advance --dry-run
python3 tools/winscp_upstream.py advance
```

`advance` moves `baseline` up to the last commit whose issue (and every
issue before it) is closed. Commit `winscp.json`. When a reviewed release is
fully included, update `baseline_release` too.

## Local use

```sh
python3 tools/winscp_upstream.py report              # changes since the baseline
python3 tools/winscp_upstream.py report --since 6.7  # e.g. to look at older ones
python3 tools/winscp_upstream.py sync --dry-run      # what the workflow would open
python3 tools/winscp_upstream.py map                 # WinSCP -> NixSCP file map
```

Uses `GITHUB_TOKEN` or `gh auth token` when available.

## Translations

WinSCP translation updates are imported with `tools/build_translations.py`
rather than reviewed line by line.

## Reviewed before the tracker existed

- WinSCP issue 2477 (SCP download could write outside the target directory
  with `.`/`..` names): NixSCP already refused `.`, `..` and `/`; on Windows
  it now also refuses `\` in names the server sends
  (`src/core/ScpFileSystem.cpp`).
