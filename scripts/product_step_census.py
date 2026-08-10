"""Wire-side step census for an emitted network product (products.txt).

The disc record the product may not carry, computed after the fact: scans P (phase
bias) and C (clock) records for value steps between consecutive emitted epochs and
splits them into

  MID-ARC steps  - key present at the immediately previous epoch (tow-30): a true
                   unmarked discontinuity the consumer experiences as physics;
  GAP RE-INITS   - key returning after absence: a legitimate re-datum the consumer
                   re-initializes through.

Clock channel: per-epoch common-mode step (median per-sat delta) plus per-sat
outliers about it.  Also greps the wire for the marks that SHOULD carry these events
(phase_disc/clock_disc fields, R records, E.datum_changed) and reports the producer
tie-out hint (compare the mid-arc count against the run console's "PB emission-
continuity filter ... N reset (M ref-path slip, ...)" line - on the 12 h EXT-CB pair
the census recovered 387/385 of the producer's 394 counted ref-path-slip steps,
SPEC_level_information_budget SS25.2).

Usage: python scripts/product_step_census.py <run_dir_or_products.txt> [...]
       [--pb-step 0.10] [--clk-common 0.10] [--clk-dev 0.50]
"""
import argparse, collections, os, sys


def census(path, pb_step, clk_common, clk_dev):
    prev_val, prev_tow = {}, {}
    clk_prev = {}
    midarc = collections.defaultdict(list)
    reinit = collections.Counter()
    clk_delta = collections.defaultdict(list)
    marks = collections.Counter()
    epochs = []
    tow = None
    with open(path) as f:
        for line in f:
            if line.startswith("E,"):
                p = line.rstrip("\n").split(",")
                tow = round(float(p[2]))
                epochs.append(tow)
                if len(p) > 6 and p[6] not in ("0", ""):
                    marks["E.datum_changed"] += 1
            elif line.startswith("R,"):
                marks["R records"] += 1
            elif line.startswith("P,") and tow is not None:
                p = line.rstrip("\n").split(",")
                if "disc" in line:
                    marks["P disc fields"] += 1
                key = (p[1], p[2])
                v = float(p[3])
                if key in prev_val:
                    d = v - prev_val[key]
                    if abs(d) > pb_step:
                        if prev_tow[key] == tow - 30:
                            midarc[tow].append((key, d))
                        else:
                            reinit[tow] += 1
                prev_val[key] = v
                prev_tow[key] = tow
            elif line.startswith("C,") and tow is not None:
                p = line.rstrip("\n").split(",")
                if "disc" in line:
                    marks["C disc fields"] += 1
                sat = p[1]
                v = float(p[2])
                if sat in clk_prev:
                    clk_delta[tow].append((sat, v - clk_prev[sat]))
                clk_prev[sat] = v

    clk_common_ep, clk_outl_ep = {}, {}
    # Card Q3 (docs/SPEC_clock_temporal_anchor.md) grades the DEV CHANNEL — the
    # per-satellite common-removed step, which is the datum-invariant estimable
    # content the anchor is supposed to quieten.  Keep the whole population (and
    # its per-satellite split) rather than only the outlier epochs: the card's
    # target is a distribution shift (median 6.23 cm -> toward 2), not a count,
    # and the PER-SATELLITE spread is the evidence that the walk is estimation
    # noise rather than clock physics (measured 7.93..10.87 cm across 32
    # satellites of three clock technologies -- 1.37x, which no physical fleet is).
    dev_all = []
    dev_by_sat = collections.defaultdict(list)
    for t, devs in clk_delta.items():
        ds = sorted(d for _, d in devs)
        med = ds[len(ds) // 2] if ds else 0.0
        if abs(med) > clk_common:
            clk_common_ep[t] = med
        for s, d in devs:
            dev_all.append(d - med)
            dev_by_sat[s].append(d - med)
        outl = [(s, d - med) for s, d in devs if abs(d - med) > clk_dev]
        if outl:
            clk_outl_ep[t] = outl
    return (epochs, midarc, reinit, clk_common_ep, clk_outl_ep, marks,
            dev_all, dev_by_sat)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("products", nargs="+")
    ap.add_argument("--pb-step", type=float, default=0.10)
    ap.add_argument("--clk-common", type=float, default=0.10)
    ap.add_argument("--clk-dev", type=float, default=0.50)
    a = ap.parse_args()
    for arg in a.products:
        path = os.path.join(arg, "products.txt") if os.path.isdir(arg) else arg
        (epochs, midarc, reinit, ckc, cko, marks,
         dev_all, dev_by_sat) = census(path, a.pb_step, a.clk_common, a.clk_dev)
        n_ep = max(len(epochs), 1)
        steps = [x for v in midarc.values() for x in v]
        mags = sorted((abs(d) for _, d in steps), reverse=True)
        print("=" * 90)
        print(f"{path}: {n_ep} emitted epochs")
        print(f"  PB MID-ARC steps > {a.pb_step} m: {len(steps)} key-steps on "
              f"{len(midarc)} epochs ({100 * len(midarc) / n_ep:.1f}% of epochs); "
              f"gap re-inits: {sum(reinit.values())}")
        if mags:
            print(f"    |step| median {mags[len(mags) // 2]:.3f}  "
                  f"p90 {mags[len(mags) // 10]:.2f}  max {mags[0]:.2f} m")
            top = sorted(midarc.items(),
                         key=lambda kv: -max(abs(d) for _, d in kv[1]))[:5]
            for t, lst in top:
                worst = sorted(lst, key=lambda x: -abs(x[1]))[:3]
                print(f"    tow {t}: " + ", ".join(
                    f"{s}/{sg} {d:+.2f}" for (s, sg), d in worst))
        print(f"  clock common-mode steps > {a.clk_common} m: {len(ckc)} epochs; "
              f"per-sat outliers > {a.clk_dev} m: {len(cko)} epochs")
        # DEV CHANNEL (Card Q3's graded quantity).  Reported in cm because the
        # target scale is centimetres: 6.23 cm median / robust sigma 9.23 cm on the
        # reference 12 h internal lineage vs ~2 cm of physical change per 30 s.
        if dev_all:
            a_ = sorted(abs(x) for x in dev_all)
            q = lambda f: a_[min(len(a_) - 1, int(f * (len(a_) - 1)))]
            sig = 1.4826 * q(0.5)          # robust sigma of a zero-median population
            per = sorted(1.4826 * sorted(abs(v) for v in vs)[len(vs) // 2]
                         for vs in dev_by_sat.values() if vs)
            print(f"  clock DEV channel ({len(a_)} rows, {len(dev_by_sat)} sats): "
                  f"|step| med {100 * q(0.5):.2f} cm  p90 {100 * q(0.9):.2f}  "
                  f"p99 {100 * q(0.99):.2f}  max {100 * a_[-1]:.1f}")
            print(f"    robust sigma_w {100 * sig:.2f} cm/step; per-sat sigma_w "
                  f"min {100 * per[0]:.2f} med {100 * per[len(per) // 2]:.2f} "
                  f"max {100 * per[-1]:.2f} cm "
                  f"(spread {per[-1] / per[0]:.2f}x -- a physical fleet spans orders)")
        print(f"  wire marks: " + (", ".join(f"{k}={v}" for k, v in marks.items())
                                   or "NONE (mark-free wire)"))


if __name__ == "__main__":
    main()
