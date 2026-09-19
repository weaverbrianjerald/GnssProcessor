#!/usr/bin/env python3
"""ALC-STAT: theory, production-test census and descriptive saved-trace read.

Requires numpy. Does not replay the engine's unrecorded arc/admission decisions,
estimate field false-alarm probabilities, or modify configurations/products.
Run from the repository root; see --help for optional input/output paths.
"""
import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path

import numpy as np

F1, F2 = 1575.42e6, 1227.60e6
LAM = 299792458.0 / (F1 - F2)
N, K = 20, 4.0


def t_tail(k, df, panels=10000):
    """Two-sided t tail via x=sqrt(df)*tan(theta), composite Simpson rule."""
    lo, hi = math.atan(k / math.sqrt(df)), math.pi / 2
    step = (hi - lo) / panels
    integral = sum((1 if j in (0, panels) else 4 if j % 2 else 2)
                   * math.cos(lo + j * step) ** (df - 1)
                   for j in range(panels + 1)) * step / 3
    return 2 * math.exp(math.lgamma((df + 1) / 2) - math.lgamma(df / 2)) / math.sqrt(math.pi) * integral


def theory():
    wl = np.array([F1 / (F1 - F2), -F2 / (F1 - F2),
                   -F1 / (F1 + F2), -F2 / (F1 + F2)]) / LAM
    a, b = F1**2 / (F1**2 - F2**2), F2**2 / (F1**2 - F2**2)
    ifd = np.array([a, -b, -a, b])
    variance = np.array([0.005**2, 0.005**2, 0.5**2, 0.5**2])
    covariance = (np.array([wl, ifd]) * variance) @ np.array([wl, ifd]).T
    p = t_tail(K, N - 1)
    assert abs(p - t_tail(K, N - 1, 20000)) < 1e-12
    return dict(n=N, k=K, t19_two_sided=p, normal_two_sided=math.erfc(K / math.sqrt(2)),
                channel_sigma_per_epoch=np.sqrt(np.diag(covariance)).tolist(),
                channel_sigma_mean=np.sqrt(np.diag(covariance) / N).tolist(),
                wl_if_correlation=float(covariance[0, 1] / math.sqrt(covariance[0, 0] * covariance[1, 1])),
                ar1_variance_inflation={str(phi): 1 + 2 * sum((1 - h / N) * phi**h for h in range(1, N))
                                        for phi in (0.0, 0.5, 0.9)})


def wilson(successes, total):
    z = 1.959963984540054
    p = successes / total
    denominator = 1 + z*z / total
    center = (p + z*z / (2*total)) / denominator
    radius = z * math.sqrt(p*(1-p)/total + z*z/(4*total*total)) / denominator
    return [center - radius, center + radius]


def test_census(path):
    rows = []
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if not line.startswith("ALC_STAT "):
            continue
        row = json.loads(line[len("ALC_STAT "):])
        if row["kind"] == "independent":
            row["either_probability"] = row["either"] / row["windows"]
            row["either_95pct_wilson"] = wilson(row["either"], row["windows"])
        else:
            # Operational descriptive rates: overlapping trials are dependent.
            # Do not attach an independent-binomial CI to these rates.
            trips = row["wl"] + row["if"]
            row["events_per_1000_samples"] = 1000 * trips / row["samples"]
            row["events_per_check"] = trips / row["checks"]
        rows.append(row)
    if len(rows) != 6:
        raise ValueError(f"Expected six production experiment rows, found {len(rows)}")
    return rows


def describe_segments(segments):
    kept = [np.asarray(s) for s in segments if len(s) >= N]
    result = dict(segments=len(kept), samples=sum(map(len, kept)))
    for col, name in enumerate(("wl", "if")):
        correlations, means = [], []
        for segment in kept:
            y = segment[:, col]
            means.append(abs(float(y.mean())))
            centered = y - y.mean()
            denominator = float(centered @ centered)
            if denominator > 0:
                correlations.append(float(centered[:-1] @ centered[1:] / denominator))
        result[name] = dict(lag1_quantiles=np.quantile(correlations, [0.1, 0.5, 0.9]).tolist() if correlations else [],
                            abs_segment_mean_median=float(np.median(means)) if means else None)
    return result


def read_trace(run):
    grouped = defaultdict(dict)
    path = run / "logs" / "closed_loop_sat_trace.csv"
    with path.open(newline="", encoding="utf-8-sig") as handle:
        for row in csv.DictReader(handle):
            if not row["sat"].startswith("G") or len(row["sig"]) < 2:
                continue
            sig = row["sig"]
            if sig[:2] not in ("L1", "L2") or "_" in sig or row["code_sig"] != "C" + sig[1:]:
                continue
            time = int(row["week"]) * 604800 + float(row["tow"])
            pair = grouped[(time, row["sat"])]
            if sig[1] not in pair or sig < pair[sig[1]]["sig"]:
                pair[sig[1]] = row
    if not grouped:
        raise ValueError(f"No GPS L1/L2 trace in {path}")
    times = sorted({key[0] for key in grouped})
    cadence = float(np.median(np.diff(times)))
    result = dict(run=str(run), cadence_s=cadence, warmup_excluded_s=600,
                  caveat="Descriptive stretches, not recovered ALC windows or verified nulls.")
    for screen in (False, True):
        current, prior, completed = {}, {}, []
        for (time, sat), pair in sorted(grouped.items()):
            if time < times[0] + 600:
                continue
            if len(pair) != 2:
                if sat in current:
                    completed.append(current.pop(sat))
                prior.pop(sat, None)
                continue
            rows = [pair["1"], pair["2"]]
            values = [float(r[key] or "nan") for r in rows for key in ("innov_phase_m", "innov_code_m", "pb_m")]
            if not all(math.isfinite(v) for v in values):
                if sat in current:
                    completed.append(current.pop(sat))
                prior.pop(sat, None)
                continue
            old = prior.get(sat)
            boundary = old is None or abs(time - old[0] - cadence) > 1e-3
            if old:
                boundary |= any(r["sig"] != p["sig"] for r, p in zip(rows, old[1]))
                if screen:
                    boundary |= any(abs(float(r["pb_m"]) - float(p["pb_m"])) > 0.005
                                    or any(r[key] != p[key] for key in ("pb_disc", "cb_disc", "clk_disc"))
                                    or any(r[key] != "none" for key in ("ev_pb", "ev_cb", "ev_clk"))
                                    for r, p in zip(rows, old[1]))
            if boundary and sat in current:
                completed.append(current.pop(sat))
            p1, c1, _, p2, c2, _ = values
            wl = ((F1*p1-F2*p2)/(F1-F2) - (F1*c1+F2*c2)/(F1+F2))/LAM
            ifd = (F1**2*(p1-c1) - F2**2*(p2-c2))/(F1**2-F2**2)
            current.setdefault(sat, []).append((wl, ifd))
            prior[sat] = time, rows
        completed.extend(current.values())
        result["product_screened" if screen else "contiguous_pair"] = describe_segments(completed)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-log", type=Path)
    parser.add_argument("--runs", nargs="*", type=Path, default=[Path("runs/_pbt_sim6"), Path("runs/_pbt_field6")])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = dict(theory=theory(), traces=[read_trace(run) for run in args.runs])
    if args.test_log:
        result["production_experiments"] = test_census(args.test_log)
    encoded = json.dumps(result, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
