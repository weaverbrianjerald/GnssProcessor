#!/usr/bin/env python3
"""Run the PPP-RTK network+user constellation test suite and report.

For every combo config in --config-dir this:
  1. runs gnss_processor.exe <config> with cwd set to a per-combo output dir
     (each combo's logs/ are isolated this way, since the engine's trace CSV
     names are fixed and would otherwise clobber each other);
  2. renders a per-combo HTML report via pyviz/run_report.py (network viewers,
     the held-out consumer's per-station viewers, and network_closed_loop);
  3. parses the converged final-hour window of network_trace.csv and
     closed_loop_trace.csv into a cross-combo metrics summary;
  4. writes a suite index.html linking every per-combo report and the summary.

This runs and reports; it does not grade against external truth. The
consumer's own RINEX-survey truth (pos_err_3d_m in closed_loop_trace.csv) is the
only accuracy signal, and the network side is reported through its emission and
clock diagnostics (rms_clock_m, solved/gate rates).
"""
import argparse
import csv
import datetime as dt
import glob
import html
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_python():
    venv = os.path.join(REPO, "venv", "Scripts", "python.exe")
    return venv if os.path.exists(venv) else sys.executable


def read_csv(path):
    if not os.path.exists(path):
        return [], []
    with open(path, newline="", encoding="utf-8") as f:
        r = csv.reader(f)
        rows = list(r)
    if not rows:
        return [], []
    return rows[0], rows[1:]


def to_f(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def window_rows(header, rows, window_s):
    """Keep rows whose tow is within window_s of the max tow (the converged tail)."""
    if not rows:
        return []
    try:
        ti = header.index("tow")
    except ValueError:
        return rows
    tows = [to_f(r[ti]) for r in rows if to_f(r[ti]) is not None]
    if not tows:
        return rows
    tmax = max(tows)
    keep = []
    for r in rows:
        t = to_f(r[ti])
        if t is not None and t >= tmax - window_s:
            keep.append(r)
    return keep or rows


def col(header, rows, name):
    if name not in header:
        return []
    i = header.index(name)
    return [to_f(r[i]) for r in rows if i < len(r) and to_f(r[i]) is not None]


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def rms(xs):
    xs = [x for x in xs if x is not None]
    return (sum(x * x for x in xs) / len(xs)) ** 0.5 if xs else None


def fmt(x, nd=3):
    return "-" if x is None else f"{x:.{nd}f}"


def network_metrics(log_dir, window_s):
    h, rows = read_csv(os.path.join(log_dir, "network_trace.csv"))
    if not rows:
        return None
    n_epochs = len(rows)
    w = window_rows(h, rows, window_s)
    solved = col(h, w, "solved")
    solved_mask = [r for r, s in zip(w, solved) if s and s > 0.5] if solved else w
    return {
        "n_epochs": n_epochs,
        "solved_rate": mean([1.0 if (s and s > 0.5) else 0.0 for s in solved]) if solved else None,
        "gate_ok_rate": mean([1.0 if (g and g > 0.5) else 0.0 for g in col(h, w, "gate_ok")]) or None,
        "rms_clock_m": mean(col(h, solved_mask, "rms_clock_m")),
        "max_clock_m": (max(col(h, solved_mask, "max_clock_m")) if col(h, solved_mask, "max_clock_m") else None),
        "n_clock_sats": mean(col(h, w, "n_clock_sats")),
        "n_phase_bias": mean(col(h, w, "n_phase_bias")),
    }


def consumer_metrics(log_dir, window_s):
    h, rows = read_csv(os.path.join(log_dir, "closed_loop_trace.csv"))
    if not rows:
        return None
    n_epochs = len(rows)
    w = window_rows(h, rows, window_s)
    pe = col(h, w, "pos_err_3d_m")
    ar_ran = col(h, w, "ar_ran")
    ar_suc = col(h, w, "ar_success")
    fix_rate = None
    if ar_ran:
        ran_idx = [i for i, x in enumerate(ar_ran) if x and x > 0.5]
        if ran_idx and len(ar_suc) == len(ar_ran):
            fix_rate = mean([1.0 if (ar_suc[i] and ar_suc[i] > 0.5) else 0.0 for i in ran_idx])
    pe_sorted = sorted([x for x in pe if x is not None])
    median = pe_sorted[len(pe_sorted) // 2] if pe_sorted else None
    return {
        "n_epochs": n_epochs,
        "pos3d_median_m": median,
        "pos3d_last_m": (pe[-1] if pe else None),
        "rms_e_m": rms(col(h, w, "east_m")),
        "rms_n_m": rms(col(h, w, "north_m")),
        "rms_u_m": rms(col(h, w, "up_m")),
        "ar_fix_rate": fix_rate,
        "num_fixed": mean(col(h, w, "num_fixed")),
    }


def run_combo(exe, config_path, out_dir, timeout, force):
    os.makedirs(out_dir, exist_ok=True)
    sentinel = os.path.join(out_dir, ".success")
    if os.path.exists(sentinel) and not force:
        return "skipped", 0.0
    for stale in (sentinel, os.path.join(out_dir, ".failed")):
        if os.path.exists(stale):
            os.remove(stale)
    t0 = dt.datetime.now()
    with open(os.path.join(out_dir, "stdout.log"), "w", encoding="utf-8", errors="replace") as so, \
         open(os.path.join(out_dir, "stderr.log"), "w", encoding="utf-8", errors="replace") as se:
        try:
            rc = subprocess.run([exe, config_path], cwd=out_dir, stdout=so, stderr=se,
                                timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            open(os.path.join(out_dir, ".failed"), "w").close()
            return "timeout", (dt.datetime.now() - t0).total_seconds()
    dur = (dt.datetime.now() - t0).total_seconds()
    ok = rc == 0 and os.path.exists(os.path.join(out_dir, "logs", "network_trace.csv"))
    open(sentinel if ok else os.path.join(out_dir, ".failed"), "w").close()
    return ("ok" if ok else f"fail(rc={rc})"), dur


def render_report(python, config_path, out_dir, data_root):
    log_dir = os.path.join(out_dir, "logs")
    if not os.path.isdir(log_dir):
        return None
    rep = os.path.join(log_dir, "gnss_report.html")
    cmd = [python, os.path.join(REPO, "pyviz", "run_report.py"), log_dir,
           "-o", rep, "--network-config", config_path, "--data-root", data_root]
    try:
        subprocess.run(cmd, cwd=os.path.join(REPO, "pyviz"),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    except subprocess.TimeoutExpired:
        return None
    return rep if os.path.exists(rep) else None


SUMMARY_COLS = [
    ("combo", "Combo"),
    ("status", "Status"),
    ("runtime_s", "Run (s)"),
    ("net_epochs", "Net ep"),
    ("net_solved", "Solved %"),
    ("net_rms_clock", "Clk RMS (m)"),
    ("net_max_clock", "Clk max (m)"),
    ("net_clk_sats", "Clk sats"),
    ("net_pb", "Phase-bias"),
    ("usr_epochs", "Usr ep"),
    ("usr_pos3d_med", "User 3D med (m)"),
    ("usr_rms_u", "User Up RMS (m)"),
    ("usr_fix", "AR fix %"),
    ("usr_nfixed", "Fixed/ep"),
]


def write_index(out_root, summary, window_s):
    rows_html = []
    for s in summary:
        rep_rel = os.path.relpath(s["report"], out_root) if s.get("report") else None
        combo_cell = (f'<a href="{html.escape(rep_rel)}">{html.escape(s["combo"])}</a>'
                      if rep_rel else html.escape(s["combo"]))
        cells = [combo_cell, html.escape(s["status"]), fmt(s.get("runtime_s"), 0),
                 fmt(s.get("net_epochs"), 0),
                 fmt(s.get("net_solved"), 0) if s.get("net_solved") is None else f'{s["net_solved"]*100:.0f}',
                 fmt(s.get("net_rms_clock")), fmt(s.get("net_max_clock")),
                 fmt(s.get("net_clk_sats"), 1), fmt(s.get("net_pb"), 1),
                 fmt(s.get("usr_epochs"), 0), fmt(s.get("usr_pos3d_med")),
                 fmt(s.get("usr_rms_u")),
                 (f'{s["usr_fix"]*100:.0f}' if s.get("usr_fix") is not None else "-"),
                 fmt(s.get("usr_nfixed"), 1)]
        td = "".join(f"<td>{c}</td>" for c in cells)
        rows_html.append(f"<tr>{td}</tr>")
    th = "".join(f"<th>{html.escape(lbl)}</th>" for _, lbl in SUMMARY_COLS)
    ts = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    doc = f"""<!doctype html><html><head><meta charset="utf-8">
<title>PPP-RTK network+user constellation suite</title>
<style>
 body{{font-family:Segoe UI,Arial,sans-serif;margin:24px;color:#1a1a1a}}
 h1{{font-size:20px}} .sub{{color:#555;font-size:13px;margin-bottom:16px}}
 table{{border-collapse:collapse;font-size:13px}}
 th,td{{border:1px solid #ccc;padding:5px 9px;text-align:right}}
 th{{background:#f0f3f7}} td:first-child,th:first-child{{text-align:left}}
 tr:nth-child(even) td{{background:#fafbfc}}
 .note{{color:#666;font-size:12px;margin-top:14px;max-width:880px}}
</style></head><body>
<h1>PPP-RTK network + user &mdash; constellation test suite</h1>
<div class="sub">Generated {ts}. 50-station global network producer (float) with held-out
PPP-RTK user consumer CIT100USA applying the network's own products
(consume_network_clock=true). Metrics over the converged final {int(window_s)} s window.</div>
<table><thead><tr>{th}</tr></thead><tbody>
{os.linesep.join(rows_html)}
</tbody></table>
<div class="note">Network: <b>Solved %</b>/<b>Clk RMS</b> are the per-epoch solve health and
estimable satellite-clock spread vs the datum. User: <b>3D med</b> is the median 3D position
error vs the CIT1 survey; <b>AR fix %</b> is the GPS integer-fix rate. Combos containing BeiDou
may exhibit the known network sat-clock divergence (open development). GLONASS combos run with
estimate_glonass (no satellite code OSB; mutually exclusive with sat-code-bias estimation).</div>
</body></html>"""
    idx = os.path.join(out_root, "index.html")
    with open(idx, "w", encoding="utf-8") as f:
        f.write(doc)
    return idx


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config-dir", required=True)
    ap.add_argument("--output-root", required=True)
    ap.add_argument("--exe", default=os.path.join(REPO, "build", "x64-Release", "gnss_processor.exe"))
    ap.add_argument("--data-root", default="C:/GnssProcessor/data")
    ap.add_argument("--timeout", type=float, default=7200.0, help="per-combo timeout seconds")
    ap.add_argument("--window", type=float, default=3600.0,
                    help="converged-tail window seconds for metrics (default 3600 = last hour)")
    ap.add_argument("--glob", default="_ppprtk50_*.json")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--no-report", action="store_true")
    args = ap.parse_args()

    python = find_python()
    data_root = args.data_root.replace("\\", "/").rstrip("/")
    configs = sorted(glob.glob(os.path.join(args.config_dir, args.glob)))
    if not configs:
        print(f"No configs matching {args.glob} in {args.config_dir}", file=sys.stderr)
        return 2
    os.makedirs(args.output_root, exist_ok=True)
    print(f"Suite: {len(configs)} combos | exe={args.exe}")
    print(f"  output_root={args.output_root}\n")

    summary = []
    for cfg in configs:
        combo = os.path.splitext(os.path.basename(cfg))[0]
        out_dir = os.path.join(args.output_root, combo)
        print(f"==> {combo}")
        status, runtime = run_combo(args.exe, os.path.abspath(cfg), out_dir, args.timeout, args.force)
        print(f"    run: {status} ({runtime:.0f}s)")
        log_dir = os.path.join(out_dir, "logs")
        nm = network_metrics(log_dir, args.window)
        cm = consumer_metrics(log_dir, args.window)
        report = None
        if not args.no_report:
            report = render_report(python, os.path.abspath(cfg), out_dir, data_root)
            print(f"    report: {'ok' if report else 'missing'}")
        summary.append({
            "combo": combo, "status": status, "runtime_s": runtime, "report": report,
            "net_epochs": nm["n_epochs"] if nm else None,
            "net_solved": nm["solved_rate"] if nm else None,
            "net_rms_clock": nm["rms_clock_m"] if nm else None,
            "net_max_clock": nm["max_clock_m"] if nm else None,
            "net_clk_sats": nm["n_clock_sats"] if nm else None,
            "net_pb": nm["n_phase_bias"] if nm else None,
            "usr_epochs": cm["n_epochs"] if cm else None,
            "usr_pos3d_med": cm["pos3d_median_m"] if cm else None,
            "usr_rms_u": cm["rms_u_m"] if cm else None,
            "usr_fix": cm["ar_fix_rate"] if cm else None,
            "usr_nfixed": cm["num_fixed"] if cm else None,
        })
        if nm:
            print(f"    net : solved={fmt(nm['solved_rate'],2)} rms_clock={fmt(nm['rms_clock_m'])}m "
                  f"clk_sats={fmt(nm['n_clock_sats'],1)}")
        if cm:
            print(f"    user: pos3d_med={fmt(cm['pos3d_median_m'])}m fix_rate={fmt(cm['ar_fix_rate'],2)}")

    idx = write_index(args.output_root, summary, args.window)
    print(f"\nSuite index: {idx}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
