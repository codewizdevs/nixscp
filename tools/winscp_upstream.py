#!/usr/bin/env python3
"""Tracks the official WinSCP repository (github.com/winscp/winscp) for NixSCP.

NixSCP is a port of WinSCP. Every change WinSCP makes after the reviewed
baseline (upstream/winscp.json) gets a GitHub issue in the NixSCP repository
for manual review: what changed, which WinSCP files, their NixSCP
counterparts (from the "Port of WinSCP source/..." headers), the diff, and a
review checklist. Security fixes are labelled. New WinSCP releases get a
summary issue. Nothing is changed in the code automatically.

Commands:
  report [--since SHA] [--limit N]   print the review report (no issues)
  sync [--dry-run]                   create the missing issues (GITHUB_TOKEN, GITHUB_REPOSITORY)
  advance [--dry-run]                move the baseline past the commits whose issues are closed
  map                                show the WinSCP -> NixSCP file mapping

Needs only the Python standard library. Uses GITHUB_TOKEN when set (else
`gh auth token`, else unauthenticated with a low rate limit).
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE_FILE = os.path.join(ROOT, "upstream", "winscp.json")
API = "https://api.github.com"

LABEL = "winscp-upstream"
LABELS = {
    LABEL: ("0e8a16", "A change in the official WinSCP repository to review for NixSCP"),
    "upstream-security": ("b60205", "WinSCP security fix: review with priority"),
    "upstream-release": ("1d76db", "A new WinSCP release"),
    "upstream-translations": ("c5def5", "WinSCP translation update (tools/build_translations.py)"),
    "upstream-no-counterpart": ("d4c5f9", "Changes WinSCP files that NixSCP has no port of"),
}
SECURITY_RE = re.compile(
    r"secur|vulnerab|travers|overflow|inject|cve-|exploit|arbitrary|remote code|bypass|privilege|spoof|"
    r"certificate|host key|tls|ssl|unauthori|denial of service|crash when .*server|malicious|sanitiz",
    re.I)
BUGFIX_RE = re.compile(r"fail|crash|error|leak|bug|broken|wrong|incorrect|not work|hang|freeze|regression", re.I)
MARKER_RE = re.compile(r"\[winscp ([0-9a-f]{7,40})\]")
TAG_MARKER_RE = re.compile(r"\[winscp-release ([^\]]+)\]")
MAX_BODY = 60000
MAX_PATCH_LINES = 120


def token():
    value = os.environ.get("GITHUB_TOKEN")
    if value:
        return value
    try:
        return subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


TOKEN = token()


def request(method, url, data=None):
    if not url.startswith("http"):
        url = API + url
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "nixscp-winscp-upstream",
               "X-GitHub-Api-Version": "2022-11-28"}
    if TOKEN:
        headers["Authorization"] = "Bearer " + TOKEN
    body = json.dumps(data).encode() if data is not None else None
    if body is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            text = response.read().decode()
            return json.loads(text) if text else None
    except urllib.error.HTTPError as e:
        raise SystemExit(f"GitHub API {method} {url}: {e.code} {e.read().decode()[:300]}")


def paged(url):
    page = 1
    while True:
        sep = "&" if "?" in url else "?"
        items = request("GET", f"{url}{sep}per_page=100&page={page}")
        if not items:
            return
        yield from items
        if len(items) < 100:
            return
        page += 1


def load_state():
    with open(STATE_FILE, encoding="utf-8") as f:
        return json.load(f)


def save_state(state):
    with open(STATE_FILE, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)
        f.write("\n")


# ---------------------------------------------------------------- mapping

PATH_RE = re.compile(r"(?:source/)?((?:core|windows|forms|resource|putty|filezilla|packages|console|dotnet|components)/"
                     r"[A-Za-z0-9_./]+?)(?:\.\{([a-z,]+)\}|\.([a-zA-Z]+))\b")


def port_map():
    """WinSCP source path -> NixSCP files that say they port it."""
    mapping = {}
    for folder in ("src", "tests"):
        for directory, _, names in os.walk(os.path.join(ROOT, folder)):
            for name in names:
                if not name.endswith((".cpp", ".h")):
                    continue
                path = os.path.join(directory, name)
                with open(path, encoding="utf-8", errors="replace") as f:
                    head = "".join(f.readlines()[:40])
                if "WinSCP" not in head:
                    continue
                relative = os.path.relpath(path, ROOT)
                for match in PATH_RE.finditer(head):
                    base, braces, ext = match.group(1), match.group(2), match.group(3)
                    for extension in (braces.split(",") if braces else [ext]):
                        mapping.setdefault(f"source/{base}.{extension}", set()).add(relative)
    return mapping


def counterparts(mapping, upstream_file):
    hits = set(mapping.get(upstream_file, ()))
    if not hits:
        # a header and its .cpp go together (Port of X.cpp covers X.h edits too)
        stem = upstream_file.rpartition(".")[0]
        for other in ("h", "cpp"):
            hits |= mapping.get(f"{stem}.{other}", set())
    return sorted(hits)


# ---------------------------------------------------------------- commits

def compare(state, since=None):
    base = since or state["baseline"]
    data = request("GET", f"/repos/{state['repository']}/compare/{base}...{state['branch']}")
    return data.get("commits", [])


def commit_details(state, sha):
    return request("GET", f"/repos/{state['repository']}/commits/{sha}")


def classify(details, mapping):
    message = details["commit"]["message"]
    first = message.splitlines()[0]
    files = details.get("files", [])
    names = [f["filename"] for f in files]
    labels = [LABEL]
    if names and all(n.startswith("translations/") for n in names):
        kind = "translations"
        labels.append("upstream-translations")
    elif SECURITY_RE.search(first):
        kind = "security fix"
    elif BUGFIX_RE.search(first):
        kind = "bug fix"
    else:
        kind = "change"
    if names and kind != "translations":
        if all(n.startswith("libs/") for n in names):
            kind += " in a bundled library (NixSCP links system libraries: check the version it needs)"
        elif all(not n.startswith(("source/", "libs/")) for n in names):
            kind += " (build/docs only)"
    if SECURITY_RE.search(first):
        labels.append("upstream-security")
    if kind != "translations" and names and not any(counterparts(mapping, n) for n in names):
        labels.append("upstream-no-counterpart")
    issue = re.match(r"Issue (\d+)", first)
    return {"title": first, "kind": kind, "labels": labels, "files": files,
            "tracker": f"https://winscp.net/tracker/{issue.group(1)}" if issue else None}


def issue_body(state, details, info, mapping):
    sha = details["sha"]
    url = f"https://github.com/{state['repository']}/commit/{sha}"
    date = details["commit"]["committer"]["date"]
    lines = [
        f"WinSCP changed `{state['branch']}` in [{sha[:12]}]({url}) on {date[:10]}.",
        "",
        f"**{info['title']}**",
        "",
        f"- Kind: **{info['kind']}**" + (" (review with priority)" if "upstream-security" in info["labels"] else ""),
    ]
    if info["tracker"]:
        lines.append(f"- WinSCP tracker: {info['tracker']}")
    message = details["commit"]["message"].strip().splitlines()[1:]
    if any(l.strip() for l in message):
        lines += ["", "Commit message:", "", "```", *[l for l in message if l.strip()][:20], "```"]
    lines += ["", "| WinSCP file | +/- | NixSCP counterpart |", "|---|---|---|"]
    for f in info["files"][:80]:
        hits = counterparts(mapping, f["filename"])
        lines.append(f"| `{f['filename']}` | +{f['additions']} -{f['deletions']} | "
                     + (", ".join(f"`{h}`" for h in hits) if hits else "none") + " |")
    if len(info["files"]) > 80:
        lines.append(f"| ... {len(info['files']) - 80} more files | | |")
    patches = []
    for f in info["files"]:
        if f.get("patch") and not f["filename"].startswith("translations/"):
            patch = f["patch"].splitlines()
            shown = "\n".join(patch[:MAX_PATCH_LINES])
            more = f"\n... {len(patch) - MAX_PATCH_LINES} more lines" if len(patch) > MAX_PATCH_LINES else ""
            patches.append(f"<details><summary><code>{f['filename']}</code></summary>\n\n```diff\n{shown}{more}\n```\n</details>")
    if patches:
        lines += ["", "### Diff", "", *patches]
    lines += [
        "", "### Review (manual)",
        "- [ ] Does it apply to NixSCP? (the NixSCP counterparts above; Windows-only code may not apply, "
        "but the Windows build of NixSCP shares the core)",
        "- [ ] Ported in commit: ...",
        "- [ ] Test added or updated",
        "- [ ] Not applicable, because: ...",
        "",
        "Close this issue when it is decided. `tools/winscp_upstream.py advance` then moves the reviewed baseline.",
        "",
        f"[winscp {sha}]",
    ]
    body = "\n".join(lines)
    return body if len(body) <= MAX_BODY else body[:MAX_BODY - 200] + f"\n\n... (shortened; see {url})\n\n[winscp {sha}]"


# ---------------------------------------------------------------- issues

def ensure_labels(repo):
    existing = {l["name"] for l in paged(f"/repos/{repo}/labels")}
    for name, (color, description) in LABELS.items():
        if name not in existing:
            request("POST", f"/repos/{repo}/labels", {"name": name, "color": color, "description": description})


def reported(repo):
    """Upstream commits and releases that already have an issue: sha/tag -> issue."""
    commits, tags = {}, {}
    for issue in paged(f"/repos/{repo}/issues?state=all&labels={LABEL}"):
        for match in MARKER_RE.finditer(issue.get("body") or ""):
            commits[match.group(1)] = issue
        for match in TAG_MARKER_RE.finditer(issue.get("body") or ""):
            tags[match.group(1)] = issue
    return commits, tags


def release_issues(state, repo, tags_reported, commit_issues, dry_run):
    """A summary issue for each WinSCP release after the baseline release."""
    tags = [t["name"] for t in request("GET", f"/repos/{state['repository']}/tags?per_page=30")]
    known = state.get("baseline_release")
    created = []
    if known not in tags:
        print(f"baseline_release {known!r} is not among the newest WinSCP tags; no release issues")
        return created
    for tag in tags:
        if tag == known:
            break
        if tag in tags_reported:
            continue
        data = request("GET", f"/repos/{state['repository']}/compare/{known}...{tag}") if known else {"commits": []}
        lines = [f"WinSCP **{tag}** was released: https://github.com/{state['repository']}/releases/tag/{tag} "
                 f"(history: https://winscp.net/eng/docs/history).", "",
                 f"Changes since {known} (each has its own review issue):", ""]
        for c in data.get("commits", []):
            issue = commit_issues.get(c["sha"])
            ref = f" #{issue['number']}" if issue else ""
            lines.append(f"- [{c['sha'][:12]}](https://github.com/{state['repository']}/commit/{c['sha']}) "
                         f"{c['commit']['message'].splitlines()[0]}{ref}")
        lines += ["", "Close when the changes listed above are reviewed.", "", f"[winscp-release {tag}]"]
        title = f"WinSCP {tag} released: review the changes"
        if dry_run:
            print(f"would create: {title}")
        else:
            request("POST", f"/repos/{repo}/issues", {"title": title, "body": "\n".join(lines),
                                                       "labels": [LABEL, "upstream-release"]})
        created.append(tag)
    return created


def cmd_sync(args):
    state = load_state()
    repo = os.environ.get("GITHUB_REPOSITORY") or state["nixscp_repository"]
    if not TOKEN and not args.dry_run:
        raise SystemExit("GITHUB_TOKEN is needed to create issues")
    mapping = port_map()
    commit_issues, tag_issues = reported(repo) if TOKEN else ({}, {})
    if not args.dry_run:
        ensure_labels(repo)
    new = 0
    for c in compare(state):
        sha = c["sha"]
        if any(known.startswith(sha) or sha.startswith(known) for known in commit_issues):
            continue
        details = commit_details(state, sha)
        info = classify(details, mapping)
        title = f"WinSCP: {info['title']}"[:240]
        body = issue_body(state, details, info, mapping)
        if args.dry_run:
            print(f"would create: {title}  {info['labels']}")
        else:
            commit_issues[sha] = request("POST", f"/repos/{repo}/issues", {"title": title, "body": body, "labels": info["labels"]})
        new += 1
    releases = release_issues(state, repo, tag_issues, commit_issues, args.dry_run)
    print(f"{new} new upstream change(s), {len(releases)} new release(s)")


def cmd_report(args):
    state = load_state()
    mapping = port_map()
    commits = compare(state, args.since)
    if args.limit:
        commits = commits[-args.limit:]
    print(f"# WinSCP changes since {(args.since or state['baseline'])[:12]} ({len(commits)})\n")
    for c in commits:
        details = commit_details(state, c["sha"])
        info = classify(details, mapping)
        print(f"## {info['title']}\n")
        print(issue_body(state, details, info, mapping))
        print()


def cmd_advance(args):
    """Moves the baseline to the newest commit before the first still-open review issue."""
    state = load_state()
    repo = os.environ.get("GITHUB_REPOSITORY") or state["nixscp_repository"]
    commit_issues, _ = reported(repo)
    newest = None
    for c in compare(state):
        issue = next((i for known, i in commit_issues.items() if c["sha"].startswith(known) or known.startswith(c["sha"])), None)
        if issue is None or issue["state"] != "closed":
            break
        newest = c
    if newest is None:
        print("Nothing to advance: the next change is not reviewed (its issue is open or missing).")
        return
    print(f"baseline {state['baseline'][:12]} -> {newest['sha'][:12]} ({newest['commit']['message'].splitlines()[0]})")
    if not args.dry_run:
        state["baseline"] = newest["sha"]
        state["baseline_date"] = newest["commit"]["committer"]["date"]
        save_state(state)


def cmd_map(args):
    mapping = port_map()
    for upstream in sorted(mapping):
        print(f"{upstream}: {', '.join(sorted(mapping[upstream]))}")
    print(f"\n{len(mapping)} WinSCP files have a NixSCP counterpart")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    report = sub.add_parser("report")
    report.add_argument("--since")
    report.add_argument("--limit", type=int)
    sync = sub.add_parser("sync")
    sync.add_argument("--dry-run", action="store_true")
    advance = sub.add_parser("advance")
    advance.add_argument("--dry-run", action="store_true")
    sub.add_parser("map")
    args = parser.parse_args()
    {"report": cmd_report, "sync": cmd_sync, "advance": cmd_advance, "map": cmd_map}[args.command](args)


if __name__ == "__main__":
    main()
