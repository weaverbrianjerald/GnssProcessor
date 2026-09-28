#!/usr/bin/env python3
"""
Generate docs/README.md, a navigable index of the docs/ corpus.

    python scripts/gen_docs_index.py            # write docs/README.md
    python scripts/gen_docs_index.py --check     # exit 1 if out of date (CI-able)
    python scripts/gen_docs_index.py --stdout    # print, don't write

The docs/ tree grows by a doc or two per session; a hand-maintained index goes
stale quickly, so this one is rebuilt from the files themselves, keeping the
cost of adding a doc at zero.

Each row carries the three facts needed to decide whether to open a doc:

  STATUS   read from explicit markers in the doc's own header region (these docs
           state their own status): CLOSED / OPEN / MIXED / unmarked.
  IN       inbound citations from other docs, the agent memory store,
           code and scripts, and CLAUDE.md/README. This is the load-bearing
           count: a doc cited by many source files (for example
           SPEC_day129_frame_guards) must not be renamed or merged. A finished
           doc with IN = 0 is an archive candidate (docs/attic/).
  COMMIT   last commit date touching the file, or "uncommitted".

Two counting rules:

  * `.claude/worktrees/` holds full repo copies. An unfiltered repo-wide grep
    would report those copies as citations and score a live doc as uncited, so
    only the canonical trees below are scanned.
  * A bare stem is only matched when it is distinctive. Counting the stem of
    `CONFIG.md` or `test_suite.md` also matches the word "CONFIG" and the
    `config/test_suite/` directory name, inflating those two rows.
"""
import argparse
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
ATTIC = DOCS / "attic"
MEM = Path.home() / ".claude" / "projects" / "C--GnssProcessor" / "memory"
OUT = DOCS / "README.md"

CLOSED_MARKS = ["CLOSED", "LANDED", "COMMITTED", "EXECUTED", "SUPERSEDED",
                "RETIRED", "COMPLETE", "DONE", "VALIDATED"]
OPEN_MARKS = ["OPEN", "NOT BUILT", "UNBUILT", "TODO", "OWED", "DRAFT",
              "IN PROGRESS", "STAGED", "BLOCKED", "PENDING", "NEXT"]

# Ordered; first match wins. Tested against the current corpus: a doc that lands
# in "Other" should get a keyword here rather than being renamed.
GROUPS = [
    ("Real-time / RTF",                 r"^rt_|^rtf|realtime|streaming_frontend"),
    ("Synthetic network / simulator",    r"synthetic|^sim_"),
    ("Ingest / RINEX",                   r"rinex|ingest|zero_sentinel"),
    ("Ionosphere / level",               r"iono|level|vtec|slant|triple"),
    ("Lanes / ambiguity / integer",      r"lane|uwl|ewl|_wl|wl_|eq34|ambiguity|integer|mw_wl"),
    ("Datum / S-system / clock",         r"datum|sbasis|ssystem|clock|day129|stransform|beidou|w6|pivot_visibility"),
    ("Carry / covariance / VCE",         r"carry|covarian|vce|pd_safe|factored_pz|warm_resume"),
    ("QC / RAIM / admission / slip",     r"slip|raim|admission|census|baarda|congruence|mdb|redundan|churn|flicker|guard|sigma0|deweight|reliab|turbo_edit|lli|t3_null"),
    ("Product / consumer / emit",        r"product|consumer|emit|cbpin|legend|contract"),
    ("Architecture / decomposition / perf", r"decomposition|seam|perf|inc8b|aiding|arch|election|rate_transferable|decimat"),
    ("Field-run reports",                r"nightrun|night305|investigation|^REPORT_"),
    ("Config / process / reference",     r"CONFIG|promotion|latex|test_suite|glonass|nav|transformation|session|queue|overnight"),
]

# Distinctive-stem test: these prefixes are unambiguous, or the stem is long and
# multi-underscore.  Everything else is matched by full filename only.
DISTINCT_PREFIX = ("SPEC_", "HANDOFF_", "REPORT_", "CONTRACT_", "SESSION_",
                   "RUNQUEUE_", "QUEUE_", "NIGHTRUN_", "CLOCK_", "CONFIG_")


def is_distinctive(stem: str) -> bool:
    return stem.startswith(DISTINCT_PREFIX) or (stem.count("_") >= 2 and len(stem) >= 16)


def git_last(path: Path) -> str:
    try:
        r = subprocess.run(["git", "log", "-1", "--format=%ad", "--date=short",
                            "--", str(path)], cwd=ROOT,
                           capture_output=True, text=True, timeout=60)
        return (r.stdout or "").strip() or "uncommitted"
    except Exception:
        return "?"


def status_of(head_upper: str) -> str:
    closed = any(m in head_upper for m in CLOSED_MARKS)
    openm = any(m in head_upper for m in OPEN_MARKS)
    if closed and openm:
        return "MIXED"
    if closed:
        return "CLOSED"
    if openm:
        return "OPEN"
    return "—"


def group_of(name: str, title: str) -> str:
    hay = f"{name} {title}"
    for label, pat in GROUPS:
        if re.search(pat, hay, re.IGNORECASE):
            return label
    return "Other"


def collect_referrers():
    """Canonical trees only: .claude/worktrees holds full repo copies and is excluded."""
    files = []
    files += sorted(DOCS.glob("*.md"))
    files += sorted(MEM.glob("*.md")) if MEM.is_dir() else []
    for d in ("src", "include", "scripts", "tests", "config", "pyviz"):
        for ext in ("*.cpp", "*.hpp", "*.py", "*.ps1", "*.json", "*.md"):
            files += (ROOT / d).rglob(ext) if (ROOT / d).is_dir() else []
    for f in ("CLAUDE.md", "README.md"):
        if (ROOT / f).is_file():
            files.append(ROOT / f)
    return files


def build():
    docs = sorted(p for p in DOCS.glob("*.md") if p.name != "README.md")
    inbound = defaultdict(int)
    stems = {p.name: p.stem for p in docs}

    for f in collect_referrers():
        try:
            txt = f.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for name, stem in stems.items():
            if f.name == name:          # never count a doc citing itself
                continue
            hit = name in txt
            if not hit and is_distinctive(stem):
                hit = re.search(rf"\b{re.escape(stem)}\b", txt) is not None
            if hit:
                inbound[name] += 1

    rows = []
    for p in docs:
        lines = p.read_text(encoding="utf-8", errors="ignore").splitlines()
        title = next((l.lstrip("#").strip() for l in lines if l.startswith("#")), "")
        rows.append({
            "name": p.name, "title": title, "lines": len(lines),
            "status": status_of("\n".join(lines[:45]).upper()),
            "git": git_last(p), "in": inbound[p.name],
            "group": group_of(p.name, title),
        })

    by_group = defaultdict(list)
    for r in rows:
        by_group[r["group"]].append(r)

    order = [g for g, _ in GROUPS] + ["Other"]
    L = []
    L.append("# docs/ index")
    L.append("")
    L.append("**Generated — do not edit by hand.** Regenerate after adding or "
             "renaming a doc:")
    L.append("")
    L.append("```")
    L.append("python scripts/gen_docs_index.py")
    L.append("```")
    L.append("")
    L.append(f"{len(rows)} documents, {sum(r['lines'] for r in rows):,} lines.")
    L.append("")
    L.append("**Columns.** `status` is read from each doc's own header markers "
             "(these docs declare their own state). `in` counts inbound "
             "citations from other docs, the agent memory store, code/scripts, "
             "and CLAUDE.md — it is the *load-bearing* number: a doc cited by "
             "many source files must not be renamed, merged or moved without "
             "updating its referrers, because code cites these by name and "
             "section (e.g. `// gotcha in SPEC_day129_frame_guards §6.`). "
             "A finished doc with `in` = 0 is an archive candidate for "
             "`docs/attic/`.")
    L.append("")

    # summary
    L.append("| Group | Docs | Lines |")
    L.append("|---|---:|---:|")
    for g in order:
        if by_group.get(g):
            rs = by_group[g]
            L.append(f"| [{g}](#{g.lower().replace(' / ', '--').replace(' ', '-')}) "
                     f"| {len(rs)} | {sum(r['lines'] for r in rs):,} |")
    L.append("")

    for g in order:
        rs = by_group.get(g)
        if not rs:
            continue
        L.append(f"## {g}")
        L.append("")
        L.append("| doc | status | in | lines | last commit |")
        L.append("|---|---|---:|---:|---|")
        for r in sorted(rs, key=lambda r: (-r["in"], r["name"])):
            L.append(f"| [{r['name'][:-3]}]({r['name']}) | {r['status']} "
                     f"| {r['in']} | {r['lines']} | {r['git']} |")
        L.append("")

    if ATTIC.is_dir():
        att = sorted(ATTIC.iterdir())
        if att:
            L.append("## attic/")
            L.append("")
            L.append("Retired: work finished **and** zero inbound citations when "
                     "archived. Moved, not deleted — full git history is intact, "
                     "and `git log --follow` works across the move.")
            L.append("")
            for p in att:
                why = ""
                if p.suffix == ".json":
                    why = " — companion data for the audit above"
                L.append(f"- [{p.name}](attic/{p.name}){why}")
            L.append("")

    return "\n".join(L) + "\n"


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if docs/README.md is stale")
    ap.add_argument("--stdout", action="store_true")
    a = ap.parse_args()
    text = build()
    if a.stdout:
        print(text)
    elif a.check:
        cur = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if cur != text:
            print("docs/README.md is STALE — run: python scripts/gen_docs_index.py")
            sys.exit(1)
        print("docs/README.md is up to date")
    else:
        OUT.write_text(text, encoding="utf-8")
        print(f"wrote {OUT}")
