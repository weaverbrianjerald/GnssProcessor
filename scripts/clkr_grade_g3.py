"""Card CLK-R G3 grader (docs/SPEC_clock_emit_rebase.md §4).

Grades the 12 h A/B (runs/clkr_12h_ctl vs runs/clkr_12h_on) against the stored
denominator runs/golden63_12h_internal:

  G0  ctl products.txt record stream byte-identical to the stored run
      (rebuild determinism; the card's OFF path never runs).
  P2  ON vs ctl: non-C/R record stream byte-identical (outputs-only).
  P1  census clock channel per arm (common epochs / outlier epochs / R marks)
      + per-sat dev distribution unchanged between arms.
  P4  consumer hourly medians from closed_loop_trace.csv per arm; AR fix rate.
  Marks->consumer: R-record epochs vs the ON consumer's reset behaviour is
      graded via the census tie-out (R count) + run-end lines (read manually).
"""
import os, sys, collections

STORED = r"C:\GnssProcessor\runs\golden63_12h_internal"
CTL    = r"C:\GnssProcessor\runs\clkr_12h_ctl"
ON     = r"C:\GnssProcessor\runs\clkr_12h_on"


def lines(path):
    with open(path) as f:
        return f.readlines()


def split_cr(ls):
    c, other = [], []
    for ln in ls:
        (c if ln.startswith(("C,", "R,")) else other).append(ln)
    return c, other


def first_diff(a, b, tag):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print(f"  first diff [{tag}] at line {i}:\n    A: {x.strip()}\n    B: {y.strip()}")
            return
    print(f"  length diff [{tag}]: {len(a)} vs {len(b)}")


def census(path, clk_common=0.10, clk_dev=0.50):
    clk = {}; tows = []; tow = None; r_marks = 0; datum_marks = 0
    for ln in open(path):
        if ln.startswith("E,"):
            p = ln.split(",")
            tow = round(float(p[2])); tows.append(tow); clk[tow] = {}
            if len(p) > 6 and p[6].strip() not in ("0", ""):
                datum_marks += 1
        elif ln.startswith("R,"):
            r_marks += 1
        elif ln.startswith("C,") and tow is not None:
            p = ln.split(","); clk[tow][p[1]] = float(p[2])
    tows.sort()
    common_ep = 0; outl_ep = 0; devs = []
    for i in range(1, len(tows)):
        t0, t1 = tows[i - 1], tows[i]
        if t1 - t0 != 30: continue
        ds = sorted(clk[t1][s] - clk[t0][s] for s in clk[t1] if s in clk[t0])
        if not ds: continue
        med = ds[len(ds) // 2]
        if abs(med) > clk_common: common_ep += 1
        outl = [s for s in clk[t1] if s in clk[t0]
                and abs(clk[t1][s] - clk[t0][s] - med) > clk_dev]
        if outl: outl_ep += 1
        for s in clk[t1]:
            if s in clk[t0]:
                devs.append(abs(clk[t1][s] - clk[t0][s] - med))
    devs.sort()
    n = max(len(devs), 1)
    return {
        "epochs": len(tows), "common_ep": common_ep, "outl_ep": outl_ep,
        "r_marks": r_marks, "datum_marks": datum_marks,
        "dev_med": devs[n // 2] if devs else 0.0,
        "dev_p90": devs[int(n * 0.9)] if devs else 0.0,
        "dev_p99": devs[int(n * 0.99)] if devs else 0.0,
    }


def consumer_hourly(run):
    path = os.path.join(run, "logs", "closed_loop_trace.csv")
    rows = lines(path)[1:]
    t0 = None
    byh = collections.defaultdict(list)
    ar_ran = ar_fix = 0
    for ln in rows:
        p = ln.split(",")
        tow = float(p[1]); err = float(p[8])
        ar_ran += int(p[3]); ar_fix += int(p[4])
        if t0 is None: t0 = tow
        byh[int((tow - t0) // 3600)].append(err)
    med = {}
    for h, v in sorted(byh.items()):
        v.sort(); med[h] = v[len(v) // 2]
    return med, ar_ran, ar_fix


print("=" * 100)
print("G0 — ctl record stream vs the STORED denominator:")
sa = lines(os.path.join(STORED, "products.txt"))
sc = lines(os.path.join(CTL, "products.txt"))
if sa == sc:
    print("  BYTE-IDENTICAL, whole file.")
else:
    ident = sum(1 for a, b in zip(sa, sc) if a == b)
    print(f"  NOT identical: {ident}/{max(len(sa), len(sc))} lines match.")
    first_diff(sa, sc, "stored-vs-ctl")

print("P2 — ON vs ctl, non-C/R stream (outputs-only proof at 12 h):")
c_ctl, o_ctl = split_cr(sc)
c_on,  o_on  = split_cr(lines(os.path.join(ON, "products.txt")))
if o_ctl == o_on:
    print(f"  BYTE-IDENTICAL ({len(o_ctl)} lines).")
else:
    ident = sum(1 for a, b in zip(o_ctl, o_on) if a == b)
    print(f"  NOT identical: {ident}/{max(len(o_ctl), len(o_on))}")
    first_diff(o_ctl, o_on, "ctl-vs-on")

print("P1 — census clock channel:")
for tag, run in (("stored", STORED), ("ctl", CTL), ("ON", ON)):
    c = census(os.path.join(run, "products.txt"))
    print(f"  {tag:6s}: {c['epochs']} ep | common>0.1m {c['common_ep']:4d} | "
          f"outlier ep {c['outl_ep']:3d} | R marks {c['r_marks']:3d} | datum marks {c['datum_marks']} | "
          f"dev med/p90/p99 {c['dev_med']*100:.1f}/{c['dev_p90']*100:.1f}/{c['dev_p99']*100:.1f} cm")

print("P4 — consumer (live closed loop) hourly 3D medians [m] + AR:")
for tag, run in (("stored", STORED), ("ctl", CTL), ("ON", ON)):
    med, ar_ran, ar_fix = consumer_hourly(run)
    hs = " ".join(f"{med[h]:.1f}" for h in sorted(med))
    print(f"  {tag:6s}: AR {ar_fix}/{ar_ran}  hourly: {hs}")
