#!/usr/bin/env python3
"""Ledger pass for the 2026-09-20 RF-0hv session: the gap re-entry response and its rider.

    python scripts/_gapreentry_float_ledger_oneoff.py

Does, in order, the steps the generator turns into a gate:
  1. docs/CONFIG_promotion_ledger.json
       a. network.cycle_slip.gap_reentry_relevel  KEEP_OFF -> RECOMMEND_SCOPED, regime
          SCOPED to the FLOAT vehicle, with the 2026-09-20 amendment APPENDED to the scope
          and the evidence (the 2026-09-09 text is the scientific record and is untouched);
       b. NEW row network.cycle_slip.gap_reentry_per_signal (UNVALIDATED, the rider).
  2. docs/CONFIG_promotion_ledger.md   the Tier 2 section for the promoted knob and the A6
     session appendix (the A5 pattern: one table row per knob that moved or landed).
  3. src/core/config/GnssConfigNetworkPosture.cpp   the Tier::SCOPED posture row, without
     which config_posture_check.py --verify-loader reports a coverage gap.  The UNVALIDATED
     rider gets NO posture row (a Tier row with no RECOMMEND_* entry is an orphan and fails
     the reverse direction of the same check).

The regime needs no DSL extension: val(path, default) == "string" is already inside the
checker's AST allowlist and is how network.apriori_source is read.  The code default of
network.ambiguity_vehicle is "level" (GnssConfigLoadNetwork.cpp:4042), so the two goldens,
which do not set the key, evaluate FALSE and the RF-0hv bases evaluate TRUE.

Then, by hand:
    python scripts/make_golden_config.py --verify
    python scripts/config_posture_check.py --verify-loader
    python scripts/config_posture_check.py config/refound/field_gps_dual_rf0hv.json --tier all
    python scripts/docs_index_check.py

Modelled on scripts/_scopedpromo_ledger_oneoff.py (verdict change + posture row) and
scripts/_rs2_refarc_ledger_oneoff.py (row insertion).  ROOT is this file's own repository,
so it works from a worktree.  Idempotent.
"""
import io
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEDGER_JSON = os.path.join(ROOT, "docs", "CONFIG_promotion_ledger.json")
LEDGER_MD = os.path.join(ROOT, "docs", "CONFIG_promotion_ledger.md")
POSTURE = os.path.join(ROOT, "src", "core", "config", "GnssConfigNetworkPosture.cpp")

RELEVEL = "network.cycle_slip.gap_reentry_relevel"
RIDER = "network.cycle_slip.gap_reentry_per_signal"

REGIME = 'val("network.ambiguity_vehicle", "level") == "float_kf"'

REGIME_PROSE = (
    "the RF-0hv FLOAT vehicle only (network.ambiguity_vehicle = \"float_kf\", which the loader "
    "admits only with phase_leveling and without carry_invariant).  On the LEVELING vehicle and "
    "under carry_invariant the 2026-09-09 KEEP_OFF stands: it is the same mechanism, graded on a "
    "posture that pays a different price for a re-level.")

REGIME_WHY = (
    "The response frees ambiguities so a re-entering arc re-levels from this epoch's data, and the "
    "2026-09-09 cost was exactly that re-level tax on the converging LEVELING vehicle (float "
    "consumer +56 % in hours 3 to 9).  The float vehicle carries the ambiguity as a float state "
    "with a floored birth prior instead of a zero-q level, so the same freeing is priced "
    "differently: on the same field bed at 12 h the armed arm is never worse than 0.03 m of "
    "consumer p50 in any hour from 3 to 12.  The code default of network.ambiguity_vehicle is "
    "\"level\" (src/core/config/GnssConfigLoadNetwork.cpp:4042), so val(..., \"level\") is the "
    "honest reading of an absent key and both goldens, which are on the leveling vehicle, are out "
    "of regime.  A value test, not on(...): the knob is a string enum.")

SCOPE_APPEND = (
    "  AMENDED 2026-09-20 (docs/refound/AB2_l5_third_frequency_2026-09-20.md sections 10 to 15).  "
    "The KEEP_OFF above is the verdict of the CONVERGING LEVELING posture and it stands there, and "
    "under carry_invariant, where the response has never been graded.  The verdict is now SCOPED "
    "to the RF-0hv FLOAT VEHICLE (network.ambiguity_vehicle = \"float_kf\" with "
    "phase_datum_held_basis, phase_leveling, relevel_free_epoch, GPS-only dual, free slant "
    "ionosphere and the phase-bias hold), on which the response was re-graded and ARMED in the "
    "working bases config/refound/{field,sim}_gps_dual_rf0hv.json at the author's instruction "
    "(main 6369cda).  KNOWN LIMIT of the tier, and not a reason to unarm it: it keys on the "
    "SATELLITE, so a loss of lock on the primary pair while band 5 or a fallback signal tracks "
    "through is never seen.  The rider network.cycle_slip.gap_reentry_per_signal (card GRS, its "
    "own row) is the fix and is armed nowhere.")

EVIDENCE_APPEND = (
    "  AMENDED 2026-09-20, RE-GRADED ON THE FLOAT VEHICLE AND ARMED (AB2 sections 12 to 15, exe "
    "gnss_processor_pbh1.exe, one knob differing in every pair).  FIELD 6 h (DOY 129 of 2024, 59 "
    "stations, unarmed runs/_rf0hv3_field6 against armed runs/_rf0hv4_field6): shifted-and-bridged "
    "arcs 41 -> 0, T4_attrib 457 -> 228, CLK_ANCHOR decouples 598 -> 424, clock wire against COD "
    "(within-arc RMS, difference-median, hours 2 to 6) 0.0274 -> 0.0107 m on the consumer's "
    "satellites and 0.0281 -> 0.0173 m on all, consumer 3D p50 by hour 0.57 0.17 0.11 0.10 0.13 "
    "0.11 -> 0.53 0.11 0.09 0.10 0.14 0.12 m, epochs below a decimetre 167 -> 203.  FIELD 12 h, "
    "the pre-registered reading of the old cost window (runs/_rap_d12_off against "
    "runs/_rap_d12_on): the armed arm is never worse by more than 0.03 m of consumer p50 in hours "
    "3 to 12 (largest loss 0.03 m in hour 7) and is better by 0.04 to 0.05 m in hours 9, 10 and "
    "12; epochs below a decimetre 223 -> 321; T4_attrib 1069 -> 702; 12 h clock wire 0.0406 -> "
    "0.0203 m on the consumer's satellites.  THE HOURS 3 TO 9 COST OF THE LEVELING POSTURE DOES "
    "NOT RETURN.  SIM (60 stations, truth): the undisturbed pair is unchanged to the printed digit "
    "(runs/_rf0hv3_sim6 against runs/_rf0hv4_sim6), and the sim reproduction this row records as "
    "owed now exists.  With 81 injected losses of lock (RINEX op dropslip of "
    "scripts/rapid_arm.py: every phase of the satellite blanked for three epochs, integer steps at "
    "the return) the unarmed runs/_rap_sO_dsfull_dual_off ends at 0.45 m with T4_attrib 2416 and "
    "141 shifted-and-bridged arcs, while the armed runs/_rap_sM_dsfull_dual_on is at the "
    "undisturbed level (0.05 m, T4_attrib 64, 161 re-entry refloats).  That closes the \"NO SIM "
    "REPRODUCTION IS POSSIBLE YET\" clause above.")

POSTURE_WHY = (
    "free a re-entering arc's ambiguities on the FLOAT vehicle (KEEP_OFF still stands on the "
    "leveling one): 12 h field post-fit refloats 1069 -> 702, epochs below a decimetre 223 -> 321, "
    "clock wire on the consumer's satellites 0.0406 -> 0.0203 m, and never worse than 0.03 m of "
    "consumer p50 in hours 3 to 12")

RIDER_ROW = {
    "knob": RIDER + "  (+ _unmeasurable, default TRUE)",
    "default_at": (
        "false at src/core/config/GnssConfigLoadNetwork.cpp:5607-5609 (jval "
        "\"network.cycle_slip.gap_reentry_per_signal\"); the sub-gate "
        "\"network.cycle_slip.gap_reentry_per_signal_unmeasurable\" at :5610-5612 defaults TRUE, "
        "deliberately unlike the row-level gap_reentry_relevel_unmeasurable, because a partial "
        "dropout with no witness is the measured poison case; struct "
        "include/config/GnssConfigTypes.hpp SlipQcConfig::gap_reentry_per_signal / "
        "gap_reentry_per_signal_unmeasurable (mirrored in include/slip/NetworkRawObservables.hpp "
        "SlipQcParams); per-signal history and qualify pass src/core/slip/NetworkRawObservables.cpp "
        ":349, :392, :471; response src/core/slip/NetworkSlipQc.cpp:2333; rider interlock warning "
        "GnssConfigLoadNetwork.cpp:382-406; built 2026-09-20 at main 9edc0ea."),
    "verdict": "UNVALIDATED",
    "regime": "on(\"" + RELEVEL + "\")",
    "scope": (
        "Card GRS (docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md).  SOLVE-AFFECTING, "
        "default OFF, byte-identical OFF on three controls.  RIDER of the row-level gap re-entry "
        "response, which keys on the SATELLITE: networkComputeRawObservables builds one raw row per "
        "(station, satellite) from the lowest two distinct bands present, so a receiver that loses "
        "lock on the semi-codeless L1/L2 phases while the civil signals keep tracking never writes "
        "a resumption, no detector covers the return epoch (the MW tier needs an adjacent "
        "predecessor of the same pair, the IF-TD tier three consecutive epochs), and the carried "
        "ambiguities of the returning phases are folded across the break.  With a third band this "
        "is the rule rather than the exception.  Armed, the tier keeps the last-seen epoch per "
        "(satellite, signal), writes a per-signal resumption for a signal absent two or more "
        "processed epochs inside the leveling maximum-gap window on a satellite whose row-level "
        "resumption did not fire, judges it on a geometry-free difference against a partner present "
        "on both sides (first choice a partner that tracked through the gap, second choice the "
        "pre-gap partner of the same pair) through the station's existing empirical GF gate, and "
        "applies the row-level verdicts to the ABSENT signals only, through the same free-epoch "
        "latch and pending-reseed mark.  Both edges are events (standing invariant 7): T0_gap "
        "sig_resume with the verdict and T0_gap reentry_relevel with scope=signal, plus run-end "
        "counters in the card-G line.  NOT ARMED ANYWHERE: the first validation is mixed and the "
        "field triple consumer bar B1 fails."),
    "evidence": (
        "FIRST VALIDATION, card GRS section 4 (2026-09-20, main 9edc0ea, exe "
        "gnss_processor_grs.exe).  G0 PASS on three controls, rider OFF byte-identical to the "
        "pre-build exe: the dual field base (_rap_g0_field against _rf0hv3_field6), the dual sim "
        "base (_rap_g0_sim against _rf0hv3_sim6) and the armed row-level arm (_rap_g0_dc against "
        "_rap_dC_gapre), the last of which certifies the relocation of the responder body.  New "
        "suite GapReentryPerSignalQualify 9 of 9; gnss_tests 2305 passed with the one known skip.  "
        "G1 PASS: the field triple arm writes 149 per-signal re-entries, 117 shifted, 81 freed with "
        "the closure exact, and the BSHM00ISR G14 return of TOW 268860 carries sig_resume and "
        "reentry_relevel with scope=signal for L1C (GF step -57.86 m against L1L, which tracked "
        "through) and for L2W.  SIM FAULT BED, REPAIRED COMPLETELY ON BOTH POSTURES (the 81 "
        "injected losses of lock of AB2 section 13): clock wire against truth 0.0670 -> 0.0030 m on "
        "dual and 0.0939 -> 0.0029 m on triple, T4_attrib 1940 -> 61 and 2944 -> 72; the "
        "undisturbed sim bases are unchanged to the printed digit, no per-signal re-entry occurring "
        "there.  FIELD DUAL (runs/_rap_gD_dual, the working base plus the rider): T4_attrib 228 -> "
        "92, clock wire on the consumer's satellites 0.0107 -> 0.0090 m, consumer p50 equal within "
        "0.02 m, epochs below a decimetre 203 -> 162.  FIELD TRIPLE (runs/_rap_gF_tri): T4_attrib "
        "716 -> 219, clock wire 0.0269 -> 0.0095 m, consumer hours 4 to 6 from 0.30 / 0.35 / 0.52 "
        "to 0.14 / 0.12 / 0.17 m, and BAR B1 FAILS ON THE CONSUMER: 6 epochs below a decimetre "
        "against a bar of 250, the first two hours still those of the unrepaired triple.  The rider "
        "is not the cause (its control has none): the RINEX emulation of AB2 section 12 is better "
        "exactly there and differs in one respect, deleting the epochs in which a satellite is "
        "present WITHOUT its primary phases, where the rider only answers the return.  OWED before "
        "any verdict: a 12 h field arm (none exists) and the field triple consumer bar.  GRS-2 "
        "(card section 5, the RINEX emulation on the rider posture) has since shown that the "
        "residual triple deficit is not the rider's to close: with the partial-signal epochs at "
        "the ARC EDGES removed the same arm gives 0.08 0.05 0.09 0.11 0.07 m in hours 2 to 6 with "
        "393 epochs below a decimetre, better than the dual working base in every one of those "
        "hours, so the class that still costs the triple network is an ADMISSION question (GRS-3 "
        "pre-registered), not a further slip response."),
    "interlock": (
        "RIDER of network.cycle_slip.gap_reentry_relevel: armed without a row-level card the loader "
        "WARNS and the tier counts itself inert (standing invariant 4), because the per-signal "
        "history is filled only when gap_instrument or gap_reentry_relevel is armed.  Needs the "
        "same free-epoch latch as the row-level tier (network.cycle_slip.relevel_free_epoch on the "
        "leveling and float vehicles).  Its own sub-gate "
        "network.cycle_slip.gap_reentry_per_signal_unmeasurable defaults TRUE, the opposite of the "
        "row-level sub-gate, so read it with val(..., True) and never with on(...), which returns "
        "False for an absent key.  NO loader posture row while the verdict is UNVALIDATED: a "
        "Tier:: row with no RECOMMEND_* ledger entry is an orphan and fails the reverse direction "
        "of config_posture_check.py --verify-loader."),
    "fingerprinted": False,
    "family": "QC, RAIM and SLIP knobs",
}

# --------------------------------------------------------------------------- 1. ledger JSON
L = json.load(io.open(LEDGER_JSON, encoding="utf-8"))
rows = L["knobs"]
row = next(r for r in rows if r["knob"].split(" ")[0] == RELEVEL)
if row["verdict"] != "RECOMMEND_SCOPED":
    row["verdict"] = "RECOMMEND_SCOPED"
    row["regime"] = REGIME
    row["regime_prose"] = REGIME_PROSE
    row["regime_why"] = REGIME_WHY
    row["scope"] = row["scope"] + SCOPE_APPEND
    row["evidence"] = row["evidence"] + EVIDENCE_APPEND
    row["posture_why"] = POSTURE_WHY
    print("JSON: %s -> %s" % (RELEVEL, row["verdict"]))
else:
    print("JSON: %s already RECOMMEND_SCOPED (no-op)" % RELEVEL)

if not any(r["knob"].split(" ")[0] == RIDER for r in rows):
    idx = next(i for i, r in enumerate(rows) if r["knob"].split(" ")[0] == RELEVEL)
    rows.insert(idx + 1, RIDER_ROW)
    print("JSON: %s row inserted after its master" % RIDER)
else:
    print("JSON: %s row present (no-op)" % RIDER)
json.dump(L, io.open(LEDGER_JSON, "w", encoding="utf-8"), indent=1, ensure_ascii=False)

# --------------------------------------------------------------------------- 2. ledger md
MD_SECTION = (
    "### `" + RELEVEL + "`  (+ `_lli`, default TRUE; + `_unmeasurable`, default FALSE)\n"
    "- **default:** `src/core/config/GnssConfigLoadNetwork.cpp:5596` (false), struct "
    "`SlipQcConfig::gap_reentry`.  **Regime:** `" + REGIME + "`, i.e. the RF-0hv FLOAT vehicle "
    "only.  **RECOMMEND_SCOPED, amended 2026-09-20.**  The 2026-09-09 **KEEP_OFF stands on the "
    "leveling vehicle and under `carry_invariant`** (appendix A5, unchanged): the mechanism worked "
    "there and the float consumer settled RMS went 3.178 to 4.946 m, concentrated in hours 3 to 9, "
    "because every freed arc re-levels from one epoch of code.  The float vehicle carries the "
    "ambiguity as a float state with a floored birth prior rather than as a zero-q level, and it "
    "pays a different price for the same freeing.\n"
    "- **evidence (2026-09-20, the float vehicle; `docs/refound/AB2_l5_third_frequency_2026-09-20.md`"
    " sections 12 to 15, exe `gnss_processor_pbh1.exe`):** field 6 h, 59 stations, DOY 129 of 2024, "
    "`runs/_rf0hv3_field6` against `runs/_rf0hv4_field6`: shifted-and-bridged arcs 41 to 0, "
    "`T4_attrib` 457 to 228, `CLK_ANCHOR` decouples 598 to 424, clock wire against COD 0.0274 to "
    "0.0107 m on the consumer's satellites and 0.0281 to 0.0173 m on all, consumer 3D p50 by hour "
    "0.57 0.17 0.11 0.10 0.13 0.11 to 0.53 0.11 0.09 0.10 0.14 0.12 m, epochs below a decimetre 167 "
    "to 203.  Field 12 h, the pre-registered reading of the old cost window (`runs/_rap_d12_off` "
    "against `runs/_rap_d12_on`): never worse by more than 0.03 m of consumer p50 in hours 3 to 12 "
    "(largest loss 0.03 m in hour 7), better by 0.04 to 0.05 m in hours 9, 10 and 12, epochs below "
    "a decimetre 223 to 321, `T4_attrib` 1069 to 702, clock wire 0.0406 to 0.0203 m.  Sim, 60 "
    "stations with truth: the undisturbed pair is unchanged to the printed digit, and with 81 "
    "injected losses of lock the unarmed `runs/_rap_sO_dsfull_dual_off` ends at 0.45 m with "
    "`T4_attrib` 2416 and 141 shifted-and-bridged arcs while the armed "
    "`runs/_rap_sM_dsfull_dual_on` is at the undisturbed level (0.05 m, `T4_attrib` 64, 161 "
    "re-entry refloats).  That closes the row's own \"no sim reproduction is possible yet\".\n"
    "- **armed:** `config/refound/{field,sim}_gps_dual_rf0hv.json` (main `6369cda`), at the "
    "author's instruction.  The goldens are on the leveling vehicle and are out of regime.\n"
    "- **interlock:** needs `network.cycle_slip.relevel_free_epoch` to act, warn-only otherwise.  "
    "Known limit, not a reason to unarm it: the tier keys on the SATELLITE, so a loss of lock on "
    "the primary pair while band 5 or a fallback signal tracks through is never seen.  The rider "
    "`network.cycle_slip.gap_reentry_per_signal` (card GRS, Tier 3) is the fix and is armed "
    "nowhere.\n")

MD_APPENDIX = (
    "\n## A6 (2026-09-20): the RF-0hv float-vehicle session, the gap re-entry response and its "
    "per-signal rider\n"
    "\nOne row moved and one landed.  Full records: "
    "`docs/refound/AB2_l5_third_frequency_2026-09-20.md` sections 10 to 15 (the defect, the field "
    "and sim grades, the arming and its 12 h confirmation) and "
    "`docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md` (the per-signal defect, the build and "
    "its first validation).\n"
    "\n| knob | verdict | evidence |\n|---|---|---|\n"
    "| `" + RELEVEL + "` (+ `_lli`, `_unmeasurable`) | **KEEP_OFF to RECOMMEND_SCOPED**, regime "
    "`" + REGIME + "` (the float vehicle only; the A5 KEEP_OFF stands on the leveling vehicle and "
    "under `carry_invariant`) | Field 6 h, 59 stations: shifted-and-bridged arcs 41 to 0, "
    "`T4_attrib` 457 to 228, clock wire 0.0274 to 0.0107 m on the consumer's satellites, epochs "
    "below a decimetre 167 to 203.  Field 12 h (`runs/_rap_d12_off` against `runs/_rap_d12_on`): "
    "never worse by more than 0.03 m of consumer p50 in hours 3 to 12, `T4_attrib` 1069 to 702, "
    "epochs below a decimetre 223 to 321, clock wire 0.0406 to 0.0203 m, so the hours 3 to 9 cost "
    "of the leveling posture does not return.  Sim with truth: undisturbed pair unchanged, and "
    "under 81 injected losses of lock 0.45 m with 2416 refloats becomes the undisturbed 0.05 m "
    "with 64, which closes the row's owed sim reproduction.  Armed in "
    "`config/refound/{field,sim}_gps_dual_rf0hv.json` (`6369cda`). |\n"
    "| `" + RIDER + "` (+ `_unmeasurable`, default TRUE) | **UNVALIDATED** (new row; built "
    "`9edc0ea`, default OFF, armed nowhere) | Rider of the row-level response, which keys on the "
    "SATELLITE and therefore never sees a loss of lock on the primary pair while band 5 tracks "
    "through.  G0 byte-identical OFF on three controls; G1 drives (149 per-signal re-entries, 81 "
    "freed, the BSHM00ISR G14 return of TOW 268860 named with `scope=signal`).  Sim fault bed "
    "repaired completely on dual and triple (clock wire against truth 0.0670 to 0.0030 m and "
    "0.0939 to 0.0029 m, `T4_attrib` 1940 to 61 and 2944 to 72).  Field dual `runs/_rap_gD_dual`: "
    "`T4_attrib` 228 to 92, clock wire 0.0107 to 0.0090 m, consumer p50 equal within 0.02 m, "
    "epochs below a decimetre 203 to 162.  Field triple `runs/_rap_gF_tri`: `T4_attrib` 716 to "
    "219, clock wire 0.0269 to 0.0095 m, hours 4 to 6 from 0.30 / 0.35 / 0.52 to 0.14 / 0.12 / "
    "0.17 m, and **bar B1 fails on the consumer** (6 epochs below a decimetre against a bar of "
    "250).  Owed: a 12 h field arm and the field triple consumer bar.  GRS-2 has since shown the "
    "residual triple deficit is an admission question, the partial-signal epochs at the arc "
    "edges, and not the rider's to close. |\n"
    "\n**Regime note.** The float vehicle is a string enum, so the regime is a value test, "
    "`val(\"network.ambiguity_vehicle\", \"level\") == \"float_kf\"`, with the code default named "
    "as the DSL requires (`GnssConfigLoadNetwork.cpp:4042`).  No predicate was added to the "
    "checker or to `networkPostureRegime_`: the posture row tests "
    "`nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf` inline, as the other value rows "
    "test `nc.cycle_slip_if_phase_td_pairs` and `nc.slip_qc.ref_gate`.  The UNVALIDATED rider gets "
    "no posture row, which is the A5 convention and is also what the reverse direction of "
    "`--verify-loader` requires.\n")

s = io.open(LEDGER_MD, encoding="utf-8").read()
if "### `" + RELEVEL + "`" not in s:
    anchor = "### `network.cycle_slip.if_phase_td_ref_gate`"
    i = s.find(anchor)
    assert i > 0, "Tier 2 anchor section not found"
    s = s[:i] + MD_SECTION + s[i:]
    print("md: Tier 2 section inserted for %s" % RELEVEL)
else:
    print("md: Tier 2 section present (no-op)")
if "## A6 (2026-09-20)" not in s:
    s = s.rstrip("\n") + "\n" + MD_APPENDIX
    print("md: appendix A6 appended")
else:
    print("md: appendix A6 present (no-op)")
io.open(LEDGER_MD, "w", encoding="utf-8").write(s)

# --------------------------------------------------------------------------- 3. posture row
POSTURE_ROW = (
    "            // --- Card G RESPONSE on the FLOAT vehicle (AB2 sections 12 to 15, ledger\n"
    "            //     amended 2026-09-20).  Freeing a re-entering arc makes it re-level from ONE\n"
    "            //     epoch of code, which bought a KEEP_OFF on the converging LEVELING vehicle\n"
    "            //     (float consumer +56 % in hours 3 to 9) and is priced differently by a float\n"
    "            //     ambiguity state with a floored birth prior.  Regime = the vehicle; on the\n"
    "            //     leveling one the KEEP_OFF still stands, so this row must NOT report there.\n"
    "            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf,\n"
    "              nc.slip_qc.gap_reentry,\n"
    "              Tier::SCOPED, false, \"network.cycle_slip.gap_reentry_relevel\",\n"
    "              \"free a re-entering arc's ambiguities on the float vehicle: 12 h field post-fit \"\n"
    "              \"refloats 1069 -> 702, epochs below a decimetre 223 -> 321, clock wire on the \"\n"
    "              \"consumer's satellites 0.0406 -> 0.0203 m, never worse than 0.03 m of consumer \"\n"
    "              \"p50 in hours 3 to 12; needs cycle_slip.relevel_free_epoch to act\" },\n")

src = io.open(POSTURE, encoding="utf-8", newline="").read()
if '"' + RELEVEL + '"' not in src:
    # The working tree's line endings depend on how git last touched this file (core.autocrlf
    # is true in this repo), so detect them instead of assuming LF.
    nl = "\r\n" if "\r\n" in src else "\n"
    key = "              Tier::SCOPED, false, \"network.cycle_slip.if_phase_td_ref_arm_all\","
    i = src.find(key)
    assert i > 0, "posture anchor row not found"
    tail = " }," + nl
    j = src.find(tail, i)
    assert j > 0, "posture anchor row has no terminator"
    j += len(tail)
    src = src[:j] + POSTURE_ROW.replace("\n", nl) + src[j:]
    io.open(POSTURE, "w", encoding="utf-8", newline="").write(src)
    print("posture: Tier::SCOPED row inserted for %s" % RELEVEL)
else:
    print("posture: row present (no-op)")
