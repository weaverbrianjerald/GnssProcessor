"""SLIPPED-BED STORM-ARM GRADER — the pre-registered gate table of SPEC_event_harness §Z.6 / §Z.7 in one command.

Usage (from C:/GnssProcessor, Release run finished):
    python scripts/simnet_storm_grade.py runs/<arm> <out.json>

Fixture: runs/simnet60_campaign_wu93 (60 sta, 6 h, 2,486 truth slips).  Reports, settled = tow >= 313200 (h3-h6):
  * ZWD settled truth rms + hourly (simnet_plateau_decompose D0) and the iono<->ZWD interaction rms
  * consumer settled 3D p50 / rms, UP mean, hourly rms (logs/closed_loop_trace.csv)
  * per-station settled ZWD span p50 and the count > 1 m (logs/network_zwd.csv)
  * T4 events, ambient chord-median |e| of a 3000-arc quiet control, erased-arc ratio (simnet_t4_census)
  * ref-free WL / EWL / L1C at their own true integer, last 25 % (simnet_amb_integer --ref-free)
  * run-end clauses: band-scope census, T4/T3 flag-of-opportunity, CLK-R drained / max |A_c|, exit code + seconds
Reference values (2026-09-10): in-block control runs/golden_sim_campaign_zwdblk ZWD 1.188 m, consumer 1.084 / 2.389 m,
ambient 1.66 cyc, T4 9,871, WL 86.2 %.  The ambient control is a random 3000-arc sample: +/- ~0.1 cyc between runs.
"""
import csv, json, math, re, statistics as st, subprocess, sys
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
run, out = sys.argv[1], sys.argv[2]
fx = "runs/simnet60_campaign_wu93"
R = {}

def sh(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return p.stdout + p.stderr

# 1. ZWD settled truth rms (plateau D0)
t = sh(["python", "scripts/simnet_plateau_decompose.py", run, fx])
m = re.search(r"D0 ZWD settled rms ([0-9.]+)\s+hourly \[([^\]]*)\]", t)
R["t4_zwd"] = float(m.group(1)); R["zwd_hourly"] = m.group(2)
m2 = re.search(r"interaction rms \(settled\) ([0-9.]+)", t)
R["interaction_rms"] = float(m2.group(1)) if m2 else None

# 2. consumer settled h3-h6 (tow >= 313200) from closed_loop_trace
rows = list(csv.DictReader(open(run + "/logs/closed_loop_trace.csv")))
e = [float(r["pos_err_3d_m"]) for r in rows if float(r["tow"]) >= 313200 and r["pos_err_3d_m"] not in ("", "nan")]
up = [float(r["up_m"]) for r in rows if float(r["tow"]) >= 313200 and r["up_m"] not in ("", "nan")]
R["t4_cons_p50"] = st.median(e); R["t4_cons_rms"] = math.sqrt(sum(x*x for x in e)/len(e))
R["cons_up_mean"] = st.mean(up); R["cons_n"] = len(e)
# hourly consumer rms
hr = {}
for r in rows:
    if r["pos_err_3d_m"] in ("", "nan"): continue
    h = int((float(r["tow"]) - 302400) // 3600); hr.setdefault(h, []).append(float(r["pos_err_3d_m"]) ** 2)
R["cons_hourly_rms"] = [round(math.sqrt(sum(v)/len(v)), 3) for h, v in sorted(hr.items())]

# 3. per-station settled ZWD span p50 (tow >= 313200)
z = {}
for r in csv.DictReader(open(run + "/logs/network_zwd.csv")):
    if float(r["tow"]) >= 313200 and r["zwd_m"] not in ("", "nan"):
        z.setdefault(r["station"], []).append(float(r["zwd_m"]))
spans = sorted(max(v) - min(v) for v in z.values())
R["zwd_span_p50"] = st.median(spans); R["zwd_span_gt1m"] = sum(1 for x in spans if x > 1.0)

# 4. T4 census: events + ambient control median
t = sh(["python", "scripts/simnet_t4_census.py", run, fx, "--control-n", "3000"])
m = re.search(r"T4 events found\s*:\s*([0-9]+)", t); R["t4_events"] = int(m.group(1))
m = re.search(r"control population\s*:\s*n=([0-9]+)\s+median \|e\| =\s*([0-9.]+)", t); R["t4_ambient"] = float(m.group(2))
m = re.search(r"detector refloat events\s*:\s*([0-9]+)", t); R["det_events"] = int(m.group(1)) if m else None
m = re.search(r"T4 erased arcs\s+([0-9]+)\s+([0-9.]+)\s+([0-9.]+)\s+\|\s+([0-9.]+)", t)
R["t4_erased_ratio"] = float(m.group(4)) if m else None

# 5. ref-free integers, settled (last 25 %)
t = sh(["python", "scripts/simnet_amb_integer.py", fx, "--run", run, "--ref-free"])
sec = t[t.index("-- ref-free lanes --"):]
last = sec[sec.index("-- same, LAST 25% of epochs only --"):]
def pick(block, band):
    m = re.search(band.replace("(", r"\(").replace(")", r"\)") + r"\s+n=\d+.*?round\(e\)=0\s+([0-9.]+)%", block)
    return float(m.group(1))
R["t4_wl"] = pick(last, "WL(1,2)"); R["t4_ewl"] = pick(last, "EWL(2,5)")
l1 = t[t.index("-- REF-FREE"):]; l1last = l1[l1.index("-- same, LAST 25% of epochs only --"):]
R["l1c_reffree_settled"] = pick(l1last, "L1C")

# 6. run-end clauses
import os
so_path = run + "/stdout.txt" if os.path.exists(run + "/stdout.txt") else run + "/run.log"
so = open(so_path, encoding="utf-8", errors="replace").read()
m = re.search(r"band-scoped responses, §Z\): (.*)", so); R["bscope_clause"] = m.group(1)[:400] if m else None
m = re.search(r"attribution ARC \(T4\)\s*:\s*([0-9]+) opp,\s*([0-9]+) flag", so); R["t4_flag_of_opp"] = (int(m.group(2)), int(m.group(1))) if m else None
m = re.search(r"RAIM consensus \(T3\)\s*:\s*([0-9]+) opp,\s*([0-9]+) flag", so); R["t3_flag_of_opp"] = (int(m.group(2)), int(m.group(1))) if m else None
m = re.search(r"Card CLK-R.*?Σ\|μ\| drained ([0-9.]+) m, max \|A_c\| ([0-9.]+) m", so); R["clk_drained_maxAc"] = (float(m.group(1)), float(m.group(2))) if m else None
if os.path.exists(run + "/exitcode.txt"):
    ec = open(run + "/exitcode.txt").read().split(); R["t4_secs"] = float(ec[1]); R["exit"] = int(ec[0])
else:
    R["t4_secs"] = None; R["exit"] = None
R["t4_run"] = run
json.dump(R, open(out, "w"), indent=1)
for k, v in R.items():
    if k not in ("bscope_clause",): print(f"{k:22s} {v}")
print("bscope_clause:", R["bscope_clause"])
