#!/usr/bin/env python3
"""Dangling links in the repo's Markdown: every relative link or image whose target file is missing.
Usage: python3 tools/md-dangling.py [root]   (default: the repo). Exit 1 when any link dangles.
Skips URLs, mailto:, in-page anchors (#...) and anything under .git/, dist/, build dirs. A link to a
folder passes when the folder exists. Used before a push to the public repo (docs/contributing.md)."""
import os, re, sys
root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), ".."))
skip_dirs = {".git", "dist", "build", ".godot", "node_modules", "__pycache__"}
link = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
bad = 0
for d, dirs, files in os.walk(root):
    dirs[:] = [x for x in dirs if x not in skip_dirs]
    for f in files:
        if not f.endswith(".md"):
            continue
        p = os.path.join(d, f)
        for n, line in enumerate(open(p, encoding="utf-8", errors="replace"), 1):
            for t in link.findall(line):
                if re.match(r"^[a-z][a-z0-9+.-]*:", t) or t.startswith("#"):
                    continue
                t = t.split("#", 1)[0]
                if not t:
                    continue
                if not os.path.exists(os.path.join(d, t)):
                    print(f"{os.path.relpath(p, root)}:{n}: {t}")
                    bad += 1
print(f"{'no dangling links' if not bad else str(bad) + ' dangling link(s)'} under {root}")
sys.exit(1 if bad else 0)
