"""Side-by-side arm comparator for the PD-safe re-ask queue (docs/RUNQUEUE_pdfix_reask.md).

    python scripts/grade_pdfix_arms.py <run_dir> [<run_dir> ...]

Pulls the numbers the queue actually grades on, from artifacts that already exist, so an
arm is read the same way every time and two arms can never be compared on differently
sliced statistics:

  * PD-safe INC-0/1/2 run-end counters      (run.log)
  * D5 population + policy counters + sigma0 (run.log)
  * carry lifecycle: births / refloats / pop (run.log)
  * per-quarter flag and birth sums          (logs/network_epoch_status.csv)
  * chord informational lifetime             (pop / births-per-epoch)
  * wall cost                                (run.log)

Not computed here: the PSD gate, the spillway steps/structure function and total_IF.
Those have their own tools (`parse_carry_psd.py`, `grade_dd_spillway.py`,
`chain_stability.py`), and re-implementing them here would create a second, divergent
definition of the same quantity. Run those separately; this is the counter sheet.

`dd_sigma_cyc` is absent by design: it is carry-blind (SPEC_pd_safe_dd_fold.md §8).
"""
import csv
import io
import os
import re
import sys


def read_log(run):
    for name in ("run.log", "logs/main_log.log"):
        p = os.path.join(run, name)
        if os.path.exists(p):
            # PowerShell Out-File writes UTF-16; errors='replace' + NUL strip handles both.
            return io.open(p, encoding="utf-8", errors="replace").read().replace("\x00", "")
    return ""


def grab(txt, pattern, *groups, **kw):
    m = re.search(pattern, txt)
    if not m:
        return kw.get("default", None) if len(groups) < 2 else tuple(
            kw.get("default", None) for _ in groups)
    if len(groups) == 1:
        return m.group(groups[0])
    return tuple(m.group(g) for g in groups)


def num(x, cast=float):
    try:
        return cast(str(x).replace(",", "").replace(" ", ""))
    except (TypeError, ValueError):
        return None


def arm(run):
    t = read_log(run)
    d = {"run": os.path.basename(os.path.normpath(run))}

    # --- PD-safe INC-0 / INC-1 / INC-2 -------------------------------------------
    m = re.search(r"PD-safe fold, INC-0\): (\d+) epoch\(s\) folded (\d+) DD-prior block\(s\) / "
                  r"(\d+) carried row\(s\); (\d+) block-epoch\(s\) and (\d+) row-epoch\(s\) "
                  r"\(([\d.]+) %.*?over (\d+) epoch\(s\)\.\s*Worst relative pivot [^=]*= "
                  r"([-\d.e+]+)\.\s*Edges: (\d+) healthy->degenerate, (\d+) degenerate->healthy.*?"
                  r"not positive\): (\d+)\.", t, re.S)
    if m:
        (d["fold_eps"], d["fold_blocks"], d["fold_rows"], d["degen_blk"], d["degen_rows"],
         d["degen_pct"], d["degen_eps"], d["min_piv"], d["edge_down"], d["edge_up"],
         d["d5_untestable"]) = m.groups()
    m = re.search(r"INC-1/2\): ARMED . (\d+) eigen-clamp repair\(s\) over (\d+) row\(s\).*?"
                  r"(\d+) fold\(s\) above the 1e-6.*?\), (\d+) block rebirth\(s\)", t, re.S)
    if m:
        d["repairs"], d["repair_rows"], d["repair_loud"], d["rebirths"] = m.groups()

    # --- D5 population + the three policy counters --------------------------------
    d["d5_flags"], d["d5_retaps"] = grab(
        t, r"D5 fine-slip innovation gate\): (\d+) carried-DD innovation flag\(s\), "
           r"(\d+) fresh re-tap", 1, 2)
    d["v3_supp"], d["v3_trips"] = grab(
        t, r"D5 v3 corroboration gate\): (\d+) of (\d+) gate trips SUPPRESSED", 1, 2)
    d["v5_bornskip"] = grab(t, r"D5 v5 carried-only\): (\d+) chord-row\(s\) NOT gated", 1)
    d["v4_killsupp"] = grab(t, r"D5 v4 re-tap-only\): (\d+) carry-arc membership kill\(s\) SUPPRESSED", 1)
    d["sigma0"] = grab(t, r"final empirical inflation .*?= ([\d.]+)", 1)

    # --- carry lifecycle ----------------------------------------------------------
    m = re.search(r"invariant carry\): (\d+) epoch\(s\) driven, (\d+) carried DD\(s\) at run end; "
                  r"(\d+) chord birth\(s\), \d+ marginalization\(s\), (\d+) slip refloat", t)
    if m:
        d["eps"], d["pop"], d["births"], d["refloats"] = m.groups()
    d["ms_ep"] = grab(t, r"epoch-loop wall [\d.]+ ms over \d+ epochs = ([\d.]+) ms/epoch", 1)

    # --- per-quarter churn (the hump vs ramp discriminator) -----------------------
    es = os.path.join(run, "logs", "network_epoch_status.csv")
    if os.path.exists(es):
        rows = list(csv.DictReader(open(es)))
        for key, tag in (("n_d5_flags", "fq"), ("n_chord_births", "bq")):
            v = [int(r[key]) for r in rows if r.get(key)]
            s = len(v) // 4
            if s:
                d[tag] = [sum(v[i * s:(i + 1) * s]) for i in range(4)]
    return d


def main(argv):
    runs = [a for a in argv[1:] if os.path.isdir(a)]
    if not runs:
        print(__doc__.splitlines()[2].strip())
        return 2
    arms = [arm(r) for r in runs]

    def row(label, key, fmt="%s", scale=None):
        cells = []
        for a in arms:
            v = a.get(key)
            if v is None:
                cells.append("-")
            else:
                x = num(v)
                cells.append(fmt % (x / scale if (scale and x is not None) else
                                    (x if x is not None else v)))
        print("  %-26s %s" % (label, "".join("%14s" % c for c in cells)))

    print("=" * (28 + 14 * len(arms)))
    print("  %-26s %s" % ("", "".join("%14s" % a["run"][:13] for a in arms)))
    print("=" * (28 + 14 * len(arms)))
    print("-- PD screen (INC-0; always-on) --")
    row("fold epochs", "fold_eps", "%.0f")
    row("carried rows folded", "fold_rows", "%.0f")
    row("INERT row-epochs", "degen_rows", "%.0f")
    row("  ... as % of carried", "degen_pct", "%.2f%%")
    row("epochs w/ a degen block", "degen_eps", "%.0f")
    row("worst rel pivot", "min_piv", "%.3e")
    row("edges down / up", "edge_down", "%.0f")
    row("D5 rows untestable", "d5_untestable", "%.0f")
    print("-- INC-1c / INC-2a backstops --")
    row("eigen-clamp repairs", "repairs", "%.0f")
    row("  ... loud (>1e-6)", "repair_loud", "%.0f")
    row("block rebirths", "rebirths", "%.0f")
    print("-- D5 population + policy --")
    row("flags", "d5_flags", "%.0f")
    row("re-taps", "d5_retaps", "%.0f")
    row("v3 trips suppressed", "v3_supp", "%.0f")
    row("v5 born-skipped rows", "v5_bornskip", "%.0f")
    row("v4 kills suppressed", "v4_killsupp", "%.0f")
    row("sigma0-hat", "sigma0", "%.2f")
    print("-- carry lifecycle --")
    row("carried DD at end", "pop", "%.0f")
    row("chord births", "births", "%.0f")
    row("slip refloats", "refloats", "%.0f")
    for a in arms:
        pass
    print("  %-26s %s" % ("informational lifetime*", "".join(
        "%14s" % ("%.1f ep" % (num(a["pop"]) / (num(a["births"]) / num(a["eps"])))
                  if a.get("pop") and a.get("births") and num(a.get("births")) else "-")
        for a in arms)))
    print("-- churn shape (per-quarter sums; HUMP = the freeze, RAMP = physics) --")
    for tag, label in (("fq", "flags   Q1|Q2|Q3|Q4"), ("bq", "births  Q1|Q2|Q3|Q4")):
        print("  %-26s %s" % (label, "".join(
            "%22s" % ("|".join(str(x) for x in a[tag]) if a.get(tag) else "-") for a in arms)))
    print("-- cost --")
    row("ms / epoch", "ms_ep", "%.0f")
    print()
    print("  * informational lifetime = carried population / births-per-epoch: how many")
    print("    epochs the average chord survives before something re-borns it.  Higher is")
    print("    a carry that ACCUMULATES; the corrupt substrate sat at ~7.5 ep.")
    print("  Run parse_carry_psd.py / grade_dd_spillway.py / chain_stability.py separately —")
    print("  this sheet deliberately does not re-implement them.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
