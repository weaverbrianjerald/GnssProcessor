#!/usr/bin/env python3
"""Silent/fast net305 25deg producer plus a closed-loop, held-out consumer
(ZIM200CHE) for a quick end-to-end validation: whether a user PPP-RTK station
kept out of the network gets a stable kinematic position using only the
network's products.

Producer = net305 25deg with all estimation/QC/datum features (the validated
set), with every network.trace_* / diagnostic turned off (silent) for speed.
Consumer runs as a derived RealTimeEngine reading the in-memory SSR store
(full clock and bias), undifferenced, kinematic (inherits position_q=100).
Writes logs/closed_loop_trace.csv.
"""
import json, os

REPO = "C:/GnssProcessor"
BASE = os.path.join(REPO, "runs/net_matrix_2024-129/configs/net305_allfeat_10ep_em25.json")
OUT  = os.path.join(REPO, "runs/net_matrix_2024-129/configs")

# Held-out consumer (not in the 305 roster). Truth ECEF from the RINEX APPROX POSITION XYZ header field.
CONSUMER = {
    "name": "ZIM200CHE",
    "rinex_path": "C:/GnssProcessor/data/2024-129/obs/zim200che/ZIM200CHE_R_20241290000_01D_30S_MO.rnx",
    "position_ecef": [4331300.1600, 567537.0810, 4633133.5100],
}

# Every network.trace_* / diagnostic flag armed for the viz matrix, turned off (silent/fast).
SILENCE = [
    "trace_stations", "trace_ionosphere", "trace_estimability", "trace_slip_events",
    "trace_slip_raw", "trace_clock_full", "trace_anchor_info", "trace_dd_fractional",
    "trace_flag_rates", "trace_epoch_status", "trace_events", "trace_zwd",
    "trace_station_residuals", "write_run_manifest",
]

CLOSED_LOOP = {
    "enable": True,
    "station": CONSUMER,
    "consume_network_products": True,
    "consume_network_clock": True,      # full: use the network satellite clocks
    "consumer_undifferenced": True,     # undifferenced PPP-RTK user (preferred path)
    "ar_mode": 1,
    "ar_wl_only": False,                # full NL AR, not WL-only
}

DURATIONS = {"val10ep": 285.0, "val30min": 1800.0}

def build(tag, dur):
    c = json.load(open(BASE))
    c["processing"]["max_duration_s"] = dur
    nw = c["network"]
    for k in SILENCE:
        nw[k] = False
    nw["health_interval_s"] = 0.0
    nw["product_output_path"] = ""      # closed loop uses the in-memory sink
    nw["closed_loop"] = CLOSED_LOOP
    out = os.path.join(OUT, f"net305_{tag}_em25.json")
    json.dump(c, open(out, "w"), indent=2)
    print(f"wrote {os.path.basename(out)}  dur={dur:.0f}s  mask={c['processing']['elevation_mask_deg']} "
          f"consumer={CONSUMER['name']} FULLclock  traces=OFF")
    return out

for tag, dur in DURATIONS.items():
    build(tag, dur)
