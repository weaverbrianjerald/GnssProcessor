#!/usr/bin/env python3
"""PROMOTION PASS for network.scoped_handover_response (SPEC_event_harness §19.5).

    python scripts/_scopedpromo_ledger_oneoff.py

Does, in order, the four steps the generator turns into a gate:
  1. docs/CONFIG_promotion_ledger.json — the row's verdict UNVALIDATED -> RECOMMEND_ON,
     with the executable regime, the scope sentence, the kept interlock notes and the
     §19.1/§19.4/§19.5 evidence;
  2. docs/CONFIG_promotion_ledger.md   — the matching prose row;
  3. src/core/config/GnssConfigNetworkPosture.cpp — the Tier::ON posture row;
  4. config/golden/golden_manifest.json — the sim-golden pin and the field exception.
Then, by hand:
     python scripts/make_golden_config.py            # regenerate
     python scripts/make_golden_config.py --verify   # PASS required
     python scripts/config_posture_check.py --verify-loader   # 0 gaps

Promoted (not left in a scratchpad) for the same reason `_study7_ledger_oneoff.py` and
`_coldfb_ledger_oneoff.py` were: the pass must be re-runnable from any session.  ROOT is
this file's own repository, so it works from a worktree.  Idempotent.
"""
import io, json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KNOB = "network.scoped_handover_response"

REGIME = ('n_sys >= 2 and not on("network.clock_datum_sum_to_zero") '
          'and not on("network.clock_datum_broadcast_anchored")')

SCOPE = (
 "SCOPE the pivot-handover response to the constellation whose clock datum actually changed.  "
 "On a pivot handover the producer skips the D2 clock/code-bias carry, clears the Q3 clock "
 "oscillator state and suppresses the code-bias jump test; OFF, it does all three "
 "NETWORK-WIDE, so a Galileo pivot change re-realizes every GPS satellite's carry and vice "
 "versa.  ON, the skip/clear/suppress applies only to the changed constellation(s) — the "
 "SH-VTEC carry skip deliberately stays global (NetworkRunContext.cpp:2153).  Every producer "
 "whose products a consumer consumes (closed-loop, service_mode, RT) wants this: the "
 "collateral constellation's re-realization is what the user sees as a metre-class shock, and "
 "the producer's own per-constellation contrasts barely move — the damage is entirely in what "
 "the WIRE tells the consumer to throw away.")

INTERLOCK = (
 "service_mode force-arms it UNCONDITIONALLY (GnssConfigLoadNetwork.cpp:3093), including "
 "under the anchor-free clock datums where pivot handovers cannot occur — so under "
 "service_mode it is armed-but-inert.  Requires a PIVOT datum to do anything: provably inert "
 "under network.clock_datum_sum_to_zero and network.clock_datum_broadcast_anchored, and inert "
 "on a single-constellation lineage (there is no other constellation to spare).  "
 "⚠ FINGERPRINTED — solve-affecting, so it is fresh-convergence only: it cannot be adopted "
 "mid-chain and NO warm-resume A/B is possible (the §19.4 pair had to be a fresh-convergence "
 "pair for exactly this reason).  SIDE EFFECT, and it is a fix: the READMIT run-end clause's "
 "'dropped at a datum change' count goes 7 -> 0 — parked re-admission entries belonging to "
 "the constellation whose datum did NOT change were being discarded, silently degrading the "
 "already-promoted network.readmit_seed_horizon_s card.")

EVIDENCE = (
 "MECHANISM (SPEC_event_harness §19.1, census on cbscope5's own products.txt across the "
 "319230 -> 319260 pivot edge): the 1.30 m consumer shock at a pivot departure is NOT a datum "
 "move the consumer could transform away — it is producer-internal carry free-fall applied to "
 "the WRONG constellation.  "
 "FRESH PAIR (§19.4, runs/evseed_pivfresh_off / _scoped, 643 epochs each, one exe F4FF8EF0…, "
 "single knob; a fresh pair because the knob is fingerprinted): at the Galileo edge the GPS "
 "PB L1C step rms falls 0.704 -> 0.0033 m (to the 0.0019 control), consumer 3D at the edge "
 "1.143 -> 0.117 m, whole-run consumer rms 0.562 -> 0.185 and max 1.99 -> 0.36; Galileo's own "
 "share is untouched (PB E L1C 0.5576 -> 0.5573) and the producer contrasts are identical.  "
 "STORM ARM (§19.5, runs/golden_sim_campaign_zwdblk_scoped = cbscope5 + this knob, 720/720, "
 "1242 s): consumer settled p50/rms 0.1204/0.2215 -> 0.0825/0.0899, window-excluded "
 "0.1028/0.1848 -> 0.0804/0.0853, hourly p50 h0-h3 IDENTICAL and h5 0.236 -> 0.047, the "
 "pivot-recovery window 4.68-5.40 h 0.2211 -> 0.0938, last 36 min 0.2671 -> 0.0553, settled "
 "max |3D| 1.3377 -> 0.2009, Up rms/max 0.1557/1.2164 -> 0.0752/0.1968; ZWD 0.0069 -> 0.0066, "
 "WL/EWL/L1C 99.2/100/89.9 -> 99.2/100/90.2, T4 11 = 11, W2 absent, split 0.0/0, PHASE_ANCHOR "
 "315/78 and PB-GAUGE 462/362/100 IDENTICAL, sat-clock contrast +0.10 %, code-bias rms "
 "0.0912 -> 0.0902, rx-DCB +0.01 %, rx-clock +0.41 %.  "
 "THE LEDGER'S OWN PRE-REGISTERED SETTLING RUN IS SATISFIED — the blast radius shrinks to the "
 "changed constellation, measured at BOTH edges.  GPS edge (319260): consumer 3D 1.338 -> "
 "0.126 m (Up -1.216 -> -0.118), Galileo CB C1C/C7Q step rms 0.0038/0.159 -> 0.00001/0.00027 "
 "and PB E L1C/L5Q 0.447/0.649 -> 0.0012/0.0017 (all to control), the GPS clock step unchanged "
 "at +1.837 (the datum DID move and still does), cta_decouple 52 (21 E + 31 G) -> 31 (G only), "
 "consumer resets PB/CB/CLK 57/57/19 -> 30/30/10 = exactly the GPS share.  Galileo edge "
 "(321900) is symmetric: consumer 0.777 -> 0.031 m, GPS CB 0.0063/0.179 -> 0.00007/0.0020, GPS "
 "PB L1C 0.542 -> 0.045 (12x; the residual is concurrent anchor handovers / slip responses, "
 "not the datum), decouples 50 -> 21 (E only), resets 54/51/17 -> 27/24/8.  "
 "ISOLATION: the ONLY event counters that moved are cta_decouple 103 -> 52, cta_disc_landed "
 "103 -> 52, cta_recouple 154 -> 103 and seed_held 24 -> 31; common_band / if_slip / mw / lli / "
 "anchor_handover / handover_continuous / anchor_slip_handover / handover_cold / col_removed / "
 "col_added are all identical, the CLK-R clause is identical except Sum|mu| 3.59 -> 3.57 m, and "
 "the PB-GAUGE clause is byte-identical.  SECOND DEFECT FIXED: the READMIT clause's '7 dropped "
 "at a datum change' -> 0, and Q3 trips within 300 s of a clock seed 1 -> 0.  "
 "ONLY CHANNEL WORSE: run time 1050 -> 1242 s, CONFOUNDED by a different exe — a same-exe "
 "timing pair is owed, and the knob removes work rather than adding it.  "
 "RESIDUALS: the SH-VTEC carry skip stays GLOBAL by design (NetworkRunContext.cpp:2153, the "
 "RTA-2 line says so); there is still NO run-end clock-pivot handover counter, so the handover "
 "population has to be recovered from the R records; and the FIELD arm is owed — "
 "golden_field_gps_dual is GPS-only, so the knob is structurally inert there (a documented "
 "manifest exception, NOT a pin) and it becomes armable when that lineage goes "
 "multi-constellation.")

POSTURE_WHY = (
 "scope the pivot-handover carry skip / oscillator clear / jump-test suppression to the "
 "constellation whose datum changed: the collateral constellation's re-realization is the "
 "user's metre-class shock (storm arm settled p50 0.1204 -> 0.0825, max |3D| 1.34 -> 0.20 m, "
 "hour 5 0.236 -> 0.047; fresh pair edge 1.143 -> 0.117 m) and the producer's own contrasts "
 "move < 0.5 %")

# ---------------- 1. ledger JSON ----------------
p = os.path.join(ROOT, "docs", "CONFIG_promotion_ledger.json")
L = json.load(io.open(p, encoding="utf-8"))
row = next(r for r in L["knobs"] if r["knob"] == KNOB)
row["verdict"] = "RECOMMEND_ON"
row["regime"] = REGIME
row["regime_why"] = (
 "A PIVOT clock datum is what makes a handover exist at all: under "
 "network.clock_datum_sum_to_zero / clock_datum_broadcast_anchored there is no pivot to hand "
 "over and the response never fires (the loader warns).  And the whole benefit is sparing the "
 "OTHER constellation, so a single-constellation lineage has nothing to spare — hence n_sys "
 ">= 2 rather than a frequency-mode or station-count test.")
row["scope"] = SCOPE
row["interlock"] = INTERLOCK
row["evidence"] = EVIDENCE
row["posture_why"] = POSTURE_WHY
row["fingerprinted"] = True
json.dump(L, io.open(p, "w", encoding="utf-8", newline="\n"), indent=1, ensure_ascii=False)
print("JSON: verdict ->", row["verdict"])

# ---------------- 2. ledger md ----------------
p = os.path.join(ROOT, "docs", "CONFIG_promotion_ledger.md")
s = io.open(p, encoding="utf-8", newline="").read().replace("\r\n", "\n")
head = "### `" + KNOB + "`"
a = s.find(head)
assert a > 0, "md row not found"
b = s.find("\n### ", a + 10)
sec = ("### `" + KNOB + "`  (SCOPE the pivot-handover response to the constellation whose datum "
       "changed; PROMOTED 2026-09-11, SPEC_event_harness §19)\n"
       "- **default:** src/core/config/GnssConfigLoadNetwork.cpp:2015 (false)  — **regime:** a "
       "PIVOT clock datum (inert under the anchor-free `clock_datum_sum_to_zero` / "
       "`clock_datum_broadcast_anchored`, where handovers cannot occur) AND >= 2 constellations "
       "AND a consumer of the products (closed-loop / service / RT)  — **RECOMMEND_ON.**  "
       + POSTURE_WHY + "\n"
       "- **mechanism:** " + SCOPE + "\n"
       "- **interlock:** " + INTERLOCK + "\n"
       "- **evidence:** " + EVIDENCE + "\n"
       "- **see:** `docs/SPEC_event_harness.md` §19 (§19.1 census, §19.4 fresh pair, §19.5 storm "
       "arm + this promotion).\n")
s = s[:a] + sec + s[b + 1:]
io.open(p, "w", encoding="utf-8", newline="\n").write(s)
print("md: row rewritten")

# ---------------- 3. posture row ----------------
p = os.path.join(ROOT, "src", "core", "config", "GnssConfigNetworkPosture.cpp")
s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
if KNOB not in s:
    anchor = ("            { r.tri && !network_clock_datum_sum_to_zero_ && "
              "!network_clock_datum_broadcast_anchored_,").replace("\n", nl)
    assert anchor in s, "posture anchor row not found"
    row_src = (
      "            // --- PIVOT DEPARTURE (SPEC_event_harness §19, promoted 2026-09-11).  A pivot"
      + nl +
      "            //     handover's carry skip / oscillator clear / jump-test suppression is"
      + nl +
      "            //     applied NETWORK-WIDE by default, so one constellation's datum change"
      + nl +
      "            //     re-realizes the other's carry — producer-internal free-fall, not a datum"
      + nl +
      "            //     move the consumer could transform away.  Needs a pivot datum (inert"
      + nl +
      "            //     under the anchor-free ones) and a second constellation to spare."
      + nl +
      "            //     Fingerprinted: fresh-convergence only, no warm-resume A/B exists."
      + nl +
      "            { r.n_sys >= 2 && !network_clock_datum_sum_to_zero_" + nl +
      "                  && !network_clock_datum_broadcast_anchored_," + nl +
      "              network_scoped_handover_response_," + nl +
      "              Tier::ON, true , \"" + KNOB + "\"," + nl +
      "              \"scope the pivot-handover carry skip to the constellation whose datum "
      "changed: \"" + nl +
      "              \"collateral re-realization is the user's metre-class shock (storm arm "
      "settled \"" + nl +
      "              \"p50 0.1204 -> 0.0825 m, max |3D| 1.34 -> 0.20, hour 5 0.236 -> 0.047; "
      "fresh \"" + nl +
      "              \"pair edge 1.143 -> 0.117 m) while the producer's own contrasts move "
      "< 0.5 % \"" + nl +
      "              \"(runs/golden_sim_campaign_zwdblk_scoped, exe F4FF8EF0)\" }," + nl)
    s = s.replace(anchor, row_src + anchor, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    print("posture: row inserted")
else:
    print("posture: row already present")

# ---------------- 4. manifest ----------------
p = os.path.join(ROOT, "config", "golden", "golden_manifest.json")
M = json.load(io.open(p, encoding="utf-8"))
for g in M["goldens"]:
    if g["name"] == "golden_sim_tri_w6":
        g["pins"][KNOB] = True
        print("manifest: pinned on", g["name"])
    elif g["name"] == "golden_field_gps_dual":
        g.setdefault("exceptions", {})[KNOB] = (
            "GPS-ONLY lineage: the knob's entire benefit is sparing the OTHER constellation's "
            "carry at a pivot handover, so with one constellation there is nothing to spare and "
            "it is structurally inert (the regime predicate already excludes it; this entry is "
            "the record, not a suppression).  Arm it when the field lineage becomes "
            "multi-constellation — a field arm is owed then.")
        print("manifest: field exception recorded")
json.dump(M, io.open(p, "w", encoding="utf-8", newline="\n"), indent=1, ensure_ascii=False)
print("\nNow run:  python scripts/make_golden_config.py && python scripts/make_golden_config.py "
      "--verify && python scripts/config_posture_check.py --verify-loader")
