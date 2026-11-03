#!/usr/bin/env python3
"""Offline test of tools/winscp_upstream.py: classification of WinSCP commits,
the WinSCP -> NixSCP file mapping, the issue body and its dedupe marker."""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.environ["GITHUB_TOKEN"] = "offline"  # no gh lookup; no request is made
spec = importlib.util.spec_from_file_location("upstream", os.path.join(ROOT, "tools", "winscp_upstream.py"))
up = importlib.util.module_from_spec(spec)
spec.loader.exec_module(up)

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL", what)


def commit(title, files):
    return {"sha": "3b3f024794ff3386137ef0fa457de07901d4607c",
            "commit": {"message": title + "\n\nhttps://winscp.net/tracker/2477",
                       "committer": {"date": "2026-10-23T22:44:55Z"}},
            "files": [{"filename": f, "additions": 2, "deletions": 1, "patch": "@@ -1 +1 @@\n-a\n+b"} for f in files]}


mapping = up.port_map()
check(len(mapping) > 100, "mapping has the ported files")
check("src/core/ScpFileSystem.cpp" in up.counterparts(mapping, "source/core/ScpFileSystem.cpp"), "ScpFileSystem mapped")
check(up.counterparts(mapping, "source/dotnet/Session.cs") == [], ".NET has no counterpart")

state = up.load_state()
check(len(state["baseline"]) == 40, "baseline is a full sha")

sec = up.classify(commit("Issue 2477 – Recursive SCP directory download allows local path traversal", ["source/core/ScpFileSystem.cpp"]), mapping)
check(sec["kind"] == "security fix" and "upstream-security" in sec["labels"], "security fix labelled")
check(sec["tracker"] == "https://winscp.net/tracker/2477", "tracker link")
check("upstream-no-counterpart" not in sec["labels"], "mapped change has a counterpart")

bug = up.classify(commit("Issue 2476 – Memory leak in encrypted HTTP connections", ["libs/neon/src/ne_openssl.c"]), mapping)
check(bug["kind"].startswith("bug fix in a bundled library"), "bundled library fix")
check("upstream-no-counterpart" in bug["labels"], "library change has no counterpart")

tr = up.classify(commit("Translations", ["translations/German.ini", "translations/French.ini"]), mapping)
check(tr["kind"] == "translations" and "upstream-translations" in tr["labels"], "translations")

ui = up.classify(commit("Rearranging the About dialog", ["source/forms/About.cpp"]), mapping)
check(ui["kind"] == "change" and "upstream-security" not in ui["labels"], "plain change")

body = up.issue_body(state, commit("Issue 2477 – x", ["source/core/ScpFileSystem.cpp"]), sec, mapping)
check(up.MARKER_RE.search(body).group(1) == "3b3f024794ff3386137ef0fa457de07901d4607c", "dedupe marker")
check("src/core/ScpFileSystem.cpp" in body and "```diff" in body, "counterpart and diff in body")

print("FAILED (%d)" % failures if failures else "OK")
sys.exit(1 if failures else 0)
