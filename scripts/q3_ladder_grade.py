#!/usr/bin/env python3
"""Card Q3 ladder leg — one table per arm, scraped from the run console (SPEC §8).

Everything here is an ON-PATH COUNTER or a run-end statistic the producer itself
emitted; nothing is recomputed from the product (that is
``scripts/product_step_census.py``'s job, and duplicating it would give two
numbers that can silently disagree).  Standing invariant #4: the counters, not
the absence of crashes, are the evidence — so an arm whose ``folded_anchored``
is 0 prints as INERT no matter how clean the rest reads.

Columns, and why each is here:
  q, floors        the armed posture — a table without it cannot be re-derived
  fold / stat      §2.1's two independently-selectable halves (the bisection axes)
  anch / loose     invariant #4's denominator
  floor%           the deviation-#1 consistency cap ACTUALLY binding (§7.4: at the
                   bottom rungs it is supposed to)
  trips            split by reason — they are not interchangeable evidence
  res / marks      the adapt-and-EMIT half, and the wire tie-out's numerator
  mean sqrtS       the guard's denominator SCALE ⇒ what k costs in METRES here.
                   This is the number that makes a fixed k mean different things
                   at different rungs (§3's k re-derivation).
  k_m              k * mean sqrtS — the bar in metres
  guard s0         sqrt(sum w^2 / n): the guard's own sigma0 on its own statistic
  s0_clk           the census's per-family sigma0 — the independent instrument.
                   Only comparable ACROSS arms now that the census projects the
                   same way the guard does.
  s0_code          post-fit code sigma0 — the pre-registered "flat" gate
  Sum|mu|          CLK-R's drained common mode: the GAUGE channel (never a quality
                   number; it RISES when the fold stops doing the datum's work)

Usage: python scripts/q3_ladder_grade.py <run_dir> [run_dir ...] [--ladders]
"""
import os
import re
import sys

# The console is UTF-8 and so is this table (sigma-hat, sqrt, mu); a Windows
# console defaulting to cp1252 would otherwise kill the run on the header line.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def txt(run):
    p = run if os.path.isfile(run) else os.path.join(run, "console.txt")
    with open(p, "rb") as f:
        return f.read().decode("utf-8", "replace")


def find(s, pat, *groups, cast=float):
    m = None
    for m in re.finditer(pat, s):
        pass
    if not m:
        return [None] * len(groups) if len(groups) > 1 else None
    out = []
    for g in groups:
        try:
            out.append(cast(m.group(g)))
        except (TypeError, ValueError):
            out.append(m.group(g))
    return out if len(groups) > 1 else out[0]


def grade(run):
    s = txt(run)
    d = {"arm": os.path.basename(os.path.normpath(run))}
    m = re.search(r"guarded temporal clock anchor\): (\d+) epoch\(s\); (\d+) clock "
                  r"carry fold\(s\) — (\d+) ANCHORED at q = ([\d.e+-]+) m²/s[^,]*, (\d+) LOOSE, "
                  r"(\d+) at the variance floor\.\s+Trips (\d+) = (\d+) nis \+ (\d+) abs \+ (\d+) gap "
                  r"\+ (\d+) datum \+ (\d+) ext; (\d+) in-epoch re-solve\(s\) \((\d+) failed\), "
                  r"(\d+) re-coupling edge\(s\), (\d+) wire mark\(s\)", s)
    if m:
        (d["epochs"], d["folds"], d["anch"], d["q"], d["loose"], d["floorhit"],
         d["trips"], d["nis"], d["abs"], d["gap"], d["datum"], d["ext"],
         d["res"], d["resfail"], d["recpl"], d["marks"]) = m.groups()
    d["maxw"] = find(s, r"Largest \|w\| ([\d.]+) \(k = ([\d.]+)\)", 1)
    d["k"] = find(s, r"Largest \|w\| ([\d.]+) \(k = ([\d.]+)\)", 2)
    d["varfloor"] = find(s, r"variance floor ([\d.e+-]+) m²\. Guard", 1)
    d["sfloor"] = find(s, r"√S floored at ([\d.]+) m", 1)
    d["fold"] = "gf" if "clock prior folds GAUGE-FREE" in s else "raw"
    st = find(s, r"guard/census DENOMINATOR = '([a-z>-]+)'", 1, cast=str)
    d["stat"] = st or ("post" if d["fold"] == "gf" else "raw")
    g = re.search(r"guard ladder over (\d+) ANCHORED row\(s\)\): mean √S = ([\d.]+) m ⇒ the armed "
                  r"k = ([\d.]+) is a ([\d.]+) m bar at this rung; mean \|ê\| = ([\d.]+) m; "
                  # NB: the console is CRLF, so anchor on a lookahead for the line
                  # break rather than on `$` — `$` sits AFTER the \r and never matches.
                  r"guard σ̂₀ = √\(Σw²/n\) = ([\d.]+)\.  \|w\| ladder: ([^\r\n]*?)\.  "
                  r"\|ê\| ladder: ([^\r\n]*?)\.(?=[\r\n])",
                  s)
    if g:
        (d["wrows"], d["msqrtS"], _, d["k_m"], d["mabse"], d["gs0"],
         d["wlad"], d["elad"]) = g.groups()
    d["floorpct"] = find(s, r"floor BOUND on \d+ of \d+ anchored fold\(s\) \(([\d.]+) %\)", 1)
    d["s0_clk"] = find(s, r"family 'clk': \d+ row\(s\) over \d+ epoch\(s\); σ̂₀ = ([\d.]+)", 1)
    d["clk_ladder"] = find(s, r"'clk'/CLEAN: n = [^\r\n]*?LADDER  ([^\r\n]*)", 1, cast=str)
    d["s0_code"] = find(s, r"σ̂0 [\d.]+ all / ([\d.]+) code", 1)
    d["summu"] = find(s, r"Σ\|μ\| drained ([\d.]+) m", 1)
    d["worst_t"] = find(s, r"worst \|t\| of the run = ([\d.e+-]+) at (\S+)", 1)
    return d


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want_lad = "--ladders" in sys.argv
    rows = [grade(r) for r in args]
    hdr = ("arm", "q", "varfl", "sfl", "fold", "stat", "anch", "loose", "flr%",
           "trips", "nis/abs/gap", "res", "marks", "recpl", "m√S", "k_m",
           "g_s0", "s0_clk", "s0_code", "Σ|μ|", "max|w|")
    w = [22, 9, 8, 7, 4, 9, 6, 6, 6, 6, 11, 5, 6, 6, 8, 7, 6, 7, 7, 7, 7]

    def cell(v, i):
        return ("" if v is None else str(v))[:w[i]].ljust(w[i])
    print("  ".join(cell(h, i) for i, h in enumerate(hdr)))
    print("  ".join("-" * w[i] for i in range(len(hdr))))
    for d in rows:
        vals = (d.get("arm"), d.get("q"), d.get("varfloor"), d.get("sfloor"),
                d.get("fold"), d.get("stat"), d.get("anch"), d.get("loose"),
                d.get("floorpct") if d.get("floorpct") is not None else "0",
                d.get("trips"),
                "/".join(str(d.get(k, "?")) for k in ("nis", "abs", "gap")),
                d.get("res"), d.get("marks"), d.get("recpl"), d.get("msqrtS"),
                d.get("k_m"), d.get("gs0"), d.get("s0_clk"), d.get("s0_code"),
                d.get("summu"), d.get("maxw"))
        print("  ".join(cell(v, i) for i, v in enumerate(vals)))
        # Only meaningful for an ARMED arm: a control has no anchor to be inert.
        if d.get("q") is not None and d.get("anch") in (None, "0"):
            print(f"    ** {d['arm']}: anchor folded ZERO anchored rows — INERT (invariant #4)")
    if want_lad:
        for d in rows:
            print(f"\n--- {d['arm']}")
            print(f"  guard |w| : {d.get('wlad')}")
            print(f"  guard |ê| : {d.get('elad')}")
            print(f"  census |t|: {d.get('clk_ladder')}")


if __name__ == "__main__":
    sys.exit(main())
