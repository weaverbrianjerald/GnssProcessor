#!/usr/bin/env python3
"""Ledger pass for the second 2026-09-20 RF-0hv session: the rider, the latch, the admission rule.

    python scripts/_grs_csl_cpa_ledger_oneoff.py

Does, in order, the steps the generator turns into a gate:
  1. docs/CONFIG_promotion_ledger.json
       a. network.cycle_slip.gap_reentry_per_signal  UNVALIDATED -> RECOMMEND_SCOPED, regime
          SCOPED to the FLOAT vehicle AND the row-level knob (it is a rider), with the
          2026-09-20 amendments APPENDED to the scope and the evidence (this morning's
          UNVALIDATED text is the scientific record and is untouched);
       b. NEW row processing.canonical_signal_rule (RECOMMEND_SCOPED, a VALUE knob);
       c. NEW row network.canonical_pair_admission (KEEP_OFF, falsified as the cause);
       d. a dated amendment APPENDED to the master row's scope, whose "armed nowhere"
          sentence about the rider is now out of date (appended to, never rewritten).
  2. docs/CONFIG_promotion_ledger.md   the Tier 2 sections for the two promoted knobs, the
     Tier 4 section for the falsified one, and the A7 session appendix (the A5 / A6 pattern:
     one table row per knob that moved or landed).
  3. src/core/config/GnssConfigNetworkPosture.cpp   the two Tier::SCOPED posture rows, without
     which config_posture_check.py --verify-loader reports a coverage gap.  The KEEP_OFF row
     gets NO posture row (a Tier row with no RECOMMEND_* entry is an orphan and fails the
     reverse direction of the same check).

THE VALUE KNOB.  processing.canonical_signal_rule is a string enum whose DEFAULT ("count") is
truthy, so the checker's default armed test (found and not is_off(v)) would read the default
as ARMED.  It therefore carries an explicit armed_when, exactly as network.apriori_source does:
val("processing.canonical_signal_rule", "count") == "priority".  No DSL extension is needed --
val(path, default) == "string" is already inside the checker's AST allowlist, and make_env's
val() resolves any dotted path in the flattened config.

THE processing.* PATH.  The loader posture table reaches it without any change either:
config_posture_check.loader_knobs() accepts the prefixes network|estimation|processing|filter,
and the table builders are const member functions of GnssConfig, so the private member
canonical_signal_rule_ (include/config/GnssConfig.hpp:7764) is readable inline, the way the
other value rows read network_apriori_source_ and nc.cycle_slip_if_phase_td_pairs.  Only the
regime aliases live on the PostureRegime struct; a row's own armed test does not.

Then, by hand:
    python scripts/make_golden_config.py --verify
    python scripts/config_posture_check.py --verify-loader
    python scripts/config_posture_check.py config/refound/field_gps_dual_rf0hv.json --tier all
    python scripts/config_posture_check.py config/golden/golden_field_refound.json --tier all
    python scripts/docs_index_check.py

Modelled on scripts/_gapreentry_float_ledger_oneoff.py (the same morning's pass: verdict change
+ row insertion + posture row).  ROOT is this file's own repository, so it works from a
worktree.  Idempotent.
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
LATCH = "processing.canonical_signal_rule"
ADMIT = "network.canonical_pair_admission"

VEHICLE = 'val("network.ambiguity_vehicle", "level") == "float_kf"'
RIDER_REGIME = VEHICLE + ' and on("' + RELEVEL + '")'
LATCH_ARMED = 'val("' + LATCH + '", "count") == "priority"'

FAMILY = ("the canonical-signal latch and the canonical-pair admission rule "
          "(the per-band tracking attribute on the network path)")

# --------------------------------------------------------------------------- texts, rider
RIDER_SCOPE_APPEND = (
    "  AMENDED 2026-09-20 (card GRS section 7, the 12 h dual field arm, and the CSL / CPA "
    "session of the same day).  The verdict is now RECOMMEND_SCOPED and the rider is ARMED in "
    "the working bases config/refound/{field,sim}_gps_dual_rf0hv.json at the author's "
    "instruction.  The regime is the RF-0hv FLOAT vehicle AND the row-level knob, "
    + RIDER_REGIME + ", because this is a RIDER: without the row-level response the per-signal "
    "history is never filled, the loader WARNS and the tier counts itself inert (standing "
    "invariant 4).  The two arms this row records as owed are settled.  The 12 h field arm "
    "exists and passes (evidence below).  The field triple consumer bar B1 still FAILS and it "
    "is NOT the rider's to close: GRS-3 (card section 6) showed the residual triple deficit is "
    "an ADMISSION question about the partial-signal epochs at the arc edges, and the admission "
    "rule built for it is falsified as the cause (network.canonical_pair_admission, card CPA, "
    "KEEP_OFF, its own row).  What those emulation arms actually moved was the producer's "
    "first-epoch canonical-signal latch (processing.canonical_signal_rule, its own row).")

RIDER_EVIDENCE_APPEND = (
    "  AMENDED 2026-09-20: THE 12 H FIELD ARM IS DELIVERED AND THE RIDER IS ARMED (card GRS "
    "section 7).  runs/_rap_d12_grs against runs/_rap_d12_on, dual field posture, 12 h, 1440 "
    "consumer epochs, one knob differing.  Read by the rule frozen for the row-level response, "
    "the rider PASSES: never worse by more than 0.02 m of consumer p50 in hours 3 to 12, hour 12 "
    "at 0.12 m against 0.35 m, T4_attrib 702 to 370, clock wire on the consumer's satellites "
    "0.0203 to 0.0148 m.  Its epochs below a decimetre are fewer (321 to 275), as on the 6 h "
    "arm (203 to 162); the hourly medians are equal within 0.02 m, so the count moves because "
    "the consumer sits close to the 0.10 m line, and that is reported, not explained.  The 6 h "
    "and sim numbers of the first validation above stand unchanged.")

RIDER_POSTURE_WHY = (
    "see a loss of lock per SIGNAL, not per satellite: the sim fault bed is repaired completely "
    "(clock wire against truth 0.0670 -> 0.0030 m dual, 0.0939 -> 0.0029 m triple; T4_attrib "
    "1940 -> 61 and 2944 -> 72), the field dual T4_attrib 228 -> 92, and the 12 h field arm is "
    "never worse than 0.02 m of consumer p50 in hours 3 to 12 with T4_attrib 702 -> 370; inert "
    "and warn-only without cycle_slip.gap_reentry_relevel")

RELEVEL_SCOPE_APPEND = (
    "  AMENDED LATER ON 2026-09-20: the sentence above that the rider is armed nowhere is "
    "superseded.  network.cycle_slip.gap_reentry_per_signal is RECOMMEND_SCOPED and armed in "
    "both working bases from the same date (its own row); this row's KNOWN LIMIT is therefore "
    "answered on the RF-0hv float vehicle, and nowhere else.")

# --------------------------------------------------------------------------- texts, latch
LATCH_ROW = {
    "knob": LATCH + " = \"priority\"  (+ processing.canonical_signal_min_coverage, default 0.5)",
    "family": FAMILY,
    "verdict": "RECOMMEND_SCOPED",
    "regime": VEHICLE,
    "armed_when": LATCH_ARMED,
    "regime_prose": (
        "the RF-0hv FLOAT vehicle only (network.ambiguity_vehicle = \"float_kf\"), which is the "
        "ONLY posture the rule has been graded on.  The two goldens are on the leveling vehicle "
        "and are deliberately out of regime: the latch feeds every downstream channel decision, "
        "and nothing says a leveling lineage prices it the same way."),
    "regime_why": (
        "A VALUE knob, not a toggle, and the reason it needs an explicit armed_when: \"count\" "
        "is the code default (src/core/config/GnssConfigLoad.cpp:351-358) and a non-empty "
        "string, so the checker's default truthiness test would read an unarmed config as "
        "armed.  It is read with val(\"" + LATCH + "\", \"count\") == \"priority\" and never "
        "with on(...).  The regime is the vehicle for the same reason the row-level gap "
        "re-entry response is scoped to it: every arm of this knob was run on the RF-0hv float "
        "posture with the re-entry response and its per-signal rider, and the leveling vehicle "
        "was never measured.  The code default of network.ambiguity_vehicle is \"level\" "
        "(src/core/config/GnssConfigLoadNetwork.cpp:4042), so val(..., \"level\") is the honest "
        "reading of an absent key."),
    "default_at": (
        "\"count\" at src/core/config/GnssConfigLoad.cpp:351-358 (jval<std::string> "
        "\"processing.canonical_signal_rule\", alias \"canonical_signal_rule\"; an unknown "
        "value warns and falls back to count); the coverage floor "
        "\"processing.canonical_signal_min_coverage\" at :360-367 defaults 0.5 and is clamped "
        "to [0, 1]; enum CanonicalSignalRule at include/config/GnssConfigTypes.hpp:3394; members "
        "include/config/GnssConfig.hpp:7764 and :7767 with getters canonicalSignalRule() / "
        "canonicalSignalMinCoverage() at :243 and :257; the decision is the pure function "
        "PppModel::decideCanonicalPicks (src/core/model/PppModel.cpp:415) called from "
        "decideConstellationCanonicals (:491); the closed-loop consumer is PINNED to Count at "
        "src/core/config/GnssConfigLoadNetwork.cpp:11809 (deriveClosedLoopConsumerConfig); suite "
        "tests/test_canonical_signal_latch.cpp; built 2026-09-20 at main df691cd."),
    "scope": (
        "Card CSL (docs/refound/CSL_canonical_signal_latch_2026-09-20.md).  SOLVE-AFFECTING on "
        "the NETWORK stations, byte-identical at the default.  What it decides: under "
        "ChannelMode::Single the producer latches ONE tracking attribute per band and "
        "observation type for each station, at that station's FIRST processed epoch.  Under "
        "\"count\" the signal seen on the most satellites of that epoch wins, ties to the "
        "earlier entry of the priority list, so ONE satellite without the semi-codeless pair at "
        "that epoch latches the civil signal for the whole run: on the field bed C1C at 31 "
        "stations and C1W at 28, L1C at 47 and L1W at 12, although 18 stations carry L1W.  "
        "Under \"priority\" the first priority-list signal of the band whose satellite count "
        "reaches canonical_signal_min_coverage times the band's largest count wins, and the "
        "code then follows the phase's tracking attribute where that code exists: C1C 41 / "
        "C1W 18 and L1C 41 / L1W 18, the emulated latch.  MECHANISM HALF READ, stated here "
        "because it is part of the recommendation (card CSL section 4).  Slip and refloat "
        "traffic is REFUTED as the cause (at the six stations that move: MW detections 3 to 1, "
        "IF-TD 9 to 5, band scope 5 to 5, gaps 36 to 32, post-fit refloats 16 to 26; and the "
        "coherence claim inverts, cells with band 2 only are 0.16 % on L1C with L2W and 7.6 % "
        "on L1W with L2W).  Post-fit residuals are REFUTED, they get LOUDER (band 1 at the 41 "
        "unchanged stations 4.0 to 5.6 mm, band 2 at the six 4.2 to 8.4 mm).  The support and "
        "anchoring of the L1W phase-bias columns is SUPPORTED, not established (stations per "
        "(satellite, L1W) column 4.58 to 5.94, cold re-realizations of L1W columns 2 to 1, "
        "anchor-slip handovers 5 to 3, and the clock wire improves only on the consumer's "
        "satellites).  The two coverage-floor arms then show the consumer's gain is COMPLETE "
        "with four of the six stations that move and does not grow with the fifth and sixth, "
        "while the clock wire on its satellites keeps improving with them (0.0096, 0.0080, "
        "0.0069 m at floors 0.85, 0.78, 0.5): the consumer effect is a STATION SUBSET, not a "
        "support count, and the two instruments part company there.  One consumer at one "
        "site."),
    "evidence": (
        "FIRST VALIDATION, card CSL section 4 (2026-09-20, main df691cd, exe "
        "gnss_processor_csl.exe; every number re-read from the run directories by the manager; "
        "no arm of this card edits an observation file).  Posture: RF-0hv float vehicle, "
        "GPS-only, DOY 129 of 2024, 59 field stations and 60 sim stations, with the gap "
        "re-entry response and its per-signal rider.  G0 PASS, byte-identical at the default on "
        "three controls.  G1 PASS: under \"priority\" the field latch reads C1C 41 / C1W 18 and "
        "L1C 41 / L1W 18 from network_pf_rows.csv, the run's own log agrees, and band 2 is "
        "C2W / L2W everywhere under both rules.  SIM: both sim arms are BYTE-IDENTICAL under "
        "\"priority\" (one signal per band there), so the knob cannot move that bed.  FIELD "
        "DUAL 6 h (runs/_rap_lD_dual against runs/_rap_gD_dual): consumer 3D p50 by hour 0.53 "
        "0.11 0.10 0.11 0.12 0.11 to 0.43 0.07 0.06 0.08 0.08 0.07 m, epochs below a decimetre "
        "162 to 603 of 720, clock wire on the consumer's satellites 0.0090 to 0.0069 m (0.0128 "
        "m on all, unchanged).  FIELD DUAL 12 h (runs/_rap_lD_dual12 against "
        "runs/_rap_d12_grs): 275 to 994 of 1440 epochs below a decimetre, clock wire 0.0148 to "
        "0.0128 m; hour 12 at 0.17 m against 0.12 m is the ONE hour worse by more than 0.03 m, "
        "and the emulation arm the bar was set from does the same, so that clause was "
        "unattainable as written.  FIELD TRIPLE (runs/_rap_lF_tri): 6 to 314 epochs below a "
        "decimetre, with a slow first hour (1.47 m).  The build reproduces the RINEX emulation "
        "of card CPA section 5 to the digit with nothing else moved.  STANDING OF THE RULE, "
        "independent of the unread half: \"count\" is an artefact, one satellite that lacks the "
        "semi-codeless pair in a station's first processed epoch decides that station's band-1 "
        "signal for the run, and \"priority\" is what a priority list is for."),
    "interlock": (
        "INERT where a band offers one signal per station (the sim bed: both arms "
        "byte-identical), so a sim A/B cannot certify it.  The CLOSED-LOOP CONSUMER is pinned "
        "to \"count\" by deriveClosedLoopConsumerConfig "
        "(src/core/config/GnssConfigLoadNetwork.cpp:11809), so an arm of this knob moves the "
        "network stations ONLY; the consumer's own latch is a separate, already-graded variable "
        "(moving the consumer to the civil family costs it 162 to 80 epochs below a decimetre, "
        "card CSL section 1).  processing.canonical_signal_min_coverage is inert under "
        "\"count\" and selects station subsets under \"priority\" (floor 0.85 gives 16 L1W "
        "stations, 0.78 gives 17, the shipped 0.5 gives 18).  WARM RESUME: the canonical-signal "
        "map is carried state (NetworkStateSnapshot::canonical_signals) but its CONTENTS are "
        "deliberately NOT in the config fingerprint (NetworkRunContext.cpp:1142-1152), so this "
        "knob is not fingerprinted and a flip on a resumed leg does not abort: with "
        "network.resume_signal_canon_restore armed the restored map wins and the flip is INERT "
        "(the restore audit reports the disagreement), and without it the resume re-decides "
        "every station's channels from the resume epoch under the NEW rule (the RS-8 seam "
        "defect).  Flip it on a fresh lineage."),
    "posture_why": (
        "latch each station's band signal by PRIORITY instead of by the satellite count of its "
        "first processed epoch (one satellite without the semi-codeless pair latches the civil "
        "signal for the whole run): field dual 6 h epochs below a decimetre 162 -> 603 of 720 "
        "and clock wire on the consumer's satellites 0.0090 -> 0.0069 m, 12 h 275 -> 994 of "
        "1440, field triple 6 -> 314; byte-identical on the sim; mechanism half read (a station "
        "subset, one consumer at one site)"),
    "fingerprinted": False,
}

# --------------------------------------------------------------------------- texts, admission
ADMIT_ROW = {
    "knob": ADMIT + "  (+ network.canonical_pair_keep_epochs, default 3)",
    "family": FAMILY,
    "verdict": "KEEP_OFF",
    "regime": VEHICLE,
    "armed_when": 'on("' + ADMIT + '")',
    "default_at": (
        "false at src/core/config/GnssConfigLoadNetwork.cpp:4616-4618 (jval "
        "\"network.canonical_pair_admission\"); the tolerance "
        "\"network.canonical_pair_keep_epochs\" at :4619-4626 defaults 3 and is CLAMPED at 0 "
        "rather than refused (0 is a meaningful arm: admit only the epochs that hold the pair); "
        "members include/config/GnssConfig.hpp:7966-7967; seam "
        "include/filter/NetworkCanonicalPairAdmission.hpp + "
        "src/core/filter/NetworkCanonicalPairAdmission.cpp (pure state machine "
        "canonicalPairStep), state NetworkRunContext.hpp:2374-2383 "
        "(NetworkCanonicalPairState cpa, not snapshotted and not in the warm-resume "
        "fingerprint); suite tests/test_canonical_pair_admission.cpp (12 cases); built "
        "2026-09-20 at main 3b155f7."),
    "scope": (
        "Card CPA (docs/refound/CPA_canonical_pair_admission_2026-09-20.md, CLOSED).  The rule: "
        "a (station, satellite) record enters the network path (raw observables, slip tiers, "
        "assembly) only while the station holds its canonical phase AND code on band digits 1 "
        "and 2, or the last good processed epoch is at most canonical_pair_keep_epochs back; a "
        "withheld record is absent for every downstream consumer exactly as if the RINEX record "
        "were missing, so the row-level gap re-entry tier sees the return as a re-entry.  Both "
        "edges are events and counters (invariants 4 and 7): ADMISSION canonical_pair with "
        "withhold and readmit, the reason (before_first, beyond_keep), and run-end counters per "
        "reason plus the distinct pairs affected.  KEEP_OFF, and the build stays in the tree "
        "default OFF as the INSTRUMENT that separated two variables: it is what proved the gain "
        "of the RINEX emulation belongs to the canonical-signal latch and not to admission.  "
        "ARMED NOWHERE.  Graded only on the RF-0hv float vehicle with the re-entry response and "
        "the per-signal rider; the regime records that posture, not a recommendation."),
    "evidence": (
        "FIRST AND ONLY VALIDATION, card CPA section 5 (2026-09-20, main 3b155f7, exe "
        "gnss_processor_cpa.exe; every number re-read from the run directories by the manager).  "
        "G0 PASS on three controls.  G1 PASS, and it is the finding: the built rule withholds "
        "the emulated records EXACTLY.  Field dual 6 h, 471 076 records screened, 16 244 "
        "withheld (4873 before the first good epoch, 11 371 beyond the tolerance) over 851 "
        "pairs, against the emulation's 16 014 in the same window, 15 986 of them shared.  The "
        "built rule IS the emulated rule.  It does NOT reproduce the emulation's gain, and on "
        "the dual field bed it is WORSE THAN NO RULE: runs/_rap_cD_dual gives 16 epochs below a "
        "decimetre of 720 against 162 without the rule and 627 for the emulation; tolerance 0 "
        "gives 106.  Field triple 6 to 60 epochs.  Both sim arms, undisturbed and fault bed, at "
        "tolerance 3 and 0, equal their bases to the printed digit.  Bars B1 and B2 FAIL, B3 "
        "PASSES.  WHY: the RINEX emulation that motivated the card changed a SECOND variable.  "
        "Deleting records changes the tally of the first processed epoch, on which the producer "
        "decides each station's canonical signal per band, and the emulation moved that latch "
        "at 13 of 59 stations (six in the band-1 PHASE: BOGT00COL, KOKV00USA, STHL00GBR, "
        "ULAB00MNG, URUM00CHN, WIND00NAM, L1C in the base and L1W in the emulation; seven in "
        "the code only).  The built rule leaves the latch where the base has it.  Forcing every "
        "station onto its priority-first signal with NO rule gives 601 epochs below a decimetre, "
        "and the rule on top of it COSTS epochs (601 to 489).  The gain belongs to the latch "
        "(processing.canonical_signal_rule, its own row).  Lesson recorded by the card for the "
        "emulation tool: an edit of the observation files can move the producer's first-epoch "
        "decisions, so an emulation arm is checked for an unchanged latch "
        "(network_pf_rows.csv, signals by station) before its result is attributed."),
    "interlock": (
        "network.canonical_pair_keep_epochs is INERT without this knob.  OFF is byte-identical "
        "(no copy of the epoch is taken).  A constellation with no canonical pair decided on "
        "band digits 1 and 2 is left untouched.  NOT fingerprinted and NOT snapshotted: a "
        "resumed leg re-earns each pair's first good epoch, which is the same forward-only "
        "posture as the other observer states on that path.  NO loader posture row: a "
        "KEEP_OFF row is not a RECOMMEND_* entry, and a Tier:: row without one is an orphan "
        "that fails the reverse direction of config_posture_check.py --verify-loader."),
    "fingerprinted": False,
}

# --------------------------------------------------------------------------- 1. ledger JSON
L = json.load(io.open(LEDGER_JSON, encoding="utf-8"))
rows = L["knobs"]


def find(path):
    return next((i for i, r in enumerate(rows) if r["knob"].split(" ")[0] == path), None)


i_rider = find(RIDER)
assert i_rider is not None, "the rider row must already exist (commit c79a085)"
row = rows[i_rider]
if row["verdict"] != "RECOMMEND_SCOPED":
    row["verdict"] = "RECOMMEND_SCOPED"
    row["regime"] = RIDER_REGIME
    row["armed_when"] = 'on("' + RIDER + '")'
    row["scope"] = row["scope"] + RIDER_SCOPE_APPEND
    row["evidence"] = row["evidence"] + RIDER_EVIDENCE_APPEND
    row["posture_why"] = RIDER_POSTURE_WHY
    print("JSON: %s UNVALIDATED -> RECOMMEND_SCOPED" % RIDER)
else:
    print("JSON: %s already RECOMMEND_SCOPED (no-op)" % RIDER)

i_rel = find(RELEVEL)
assert i_rel is not None, "the master row must exist"
if "AMENDED LATER ON 2026-09-20" not in rows[i_rel]["scope"]:
    rows[i_rel]["scope"] = rows[i_rel]["scope"] + RELEVEL_SCOPE_APPEND
    print("JSON: %s scope amended (the rider is no longer armed nowhere)" % RELEVEL)
else:
    print("JSON: %s scope amendment present (no-op)" % RELEVEL)

# Both new rows sit immediately after the rider: they are one session's cluster and they cite
# each other (the latch is where the admission rule's measured gain went).  Array order is not
# semantically load-bearing -- the checker keys on the knob path.
for offset, (path, new_row) in enumerate(((LATCH, LATCH_ROW), (ADMIT, ADMIT_ROW))):
    if find(path) is None:
        rows.insert(find(RIDER) + 1 + offset, new_row)
        print("JSON: %s row inserted (%s)" % (path, new_row["verdict"]))
    else:
        print("JSON: %s row present (no-op)" % path)
json.dump(L, io.open(LEDGER_JSON, "w", encoding="utf-8"), indent=1, ensure_ascii=False)

# --------------------------------------------------------------------------- 2. ledger md
MD_RELEVEL_AMEND = (
    "- **amended later on 2026-09-20:** the interlock line above says the rider is armed "
    "nowhere.  That is superseded on the same date: `" + RIDER + "` is **RECOMMEND_SCOPED** and "
    "armed in both working bases (its own section below).  This row's known limit is therefore "
    "answered on the RF-0hv float vehicle, and nowhere else.\n")

MD_RIDER = (
    "### `" + RIDER + "`  (+ `_unmeasurable`, default TRUE)\n"
    "- **default:** `src/core/config/GnssConfigLoadNetwork.cpp:5607-5609` (false), struct "
    "`SlipQcConfig::gap_reentry_per_signal` (`include/config/GnssConfigTypes.hpp:597`); the "
    "sub-gate `_unmeasurable` at `:5610-5612` defaults TRUE, deliberately unlike the row-level "
    "sub-gate, because a partial dropout with no witness is the measured poison case.  "
    "**Regime:** `" + RIDER_REGIME + "`, i.e. the RF-0hv FLOAT vehicle with the row-level "
    "response armed.  **UNVALIDATED to RECOMMEND_SCOPED, 2026-09-20.**  A RIDER: the row-level "
    "gap re-entry tier keys on the SATELLITE, so a loss of lock on the primary pair while band "
    "5 or a fallback signal tracks through is never seen; armed without the row-level card the "
    "loader WARNS and the tier counts itself inert.\n"
    "- **evidence (2026-09-20, `docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md` sections "
    "4 and 7, main `9edc0ea`):** G0 byte-identical OFF on three controls; G1 drives (149 "
    "per-signal re-entries, 117 shifted, 81 freed with the closure exact, the BSHM00ISR G14 "
    "return of TOW 268860 named with `scope=signal`).  Sim fault bed (81 injected losses of "
    "lock while band 5 tracks through) REPAIRED COMPLETELY on both postures: clock wire against "
    "truth 0.0670 to 0.0030 m on dual and 0.0939 to 0.0029 m on triple, `T4_attrib` 1940 to 61 "
    "and 2944 to 72, the undisturbed sim arms identical.  Field dual `runs/_rap_gD_dual` "
    "against the working base: `T4_attrib` 228 to 92, `CLK_ANCHOR` 424 to 232, clock wire "
    "against COD 0.0107 to 0.0090 m on the consumer's satellites and 0.0173 to 0.0128 m on all, "
    "consumer p50 equal within 0.02 m, epochs below a decimetre 203 to 162 (reported, not "
    "explained).  Field triple `runs/_rap_gF_tri`: `T4_attrib` 716 to 219, clock wire 0.0269 to "
    "0.0095 m.  **12 h dual arm** `runs/_rap_d12_grs` against `runs/_rap_d12_on`: never worse "
    "by more than 0.02 m of consumer p50 in hours 3 to 12, hour 12 at 0.12 m against 0.35 m, "
    "`T4_attrib` 702 to 370, clock wire 0.0203 to 0.0148 m.\n"
    "- **armed:** `config/refound/{field,sim}_gps_dual_rf0hv.json` (2026-09-20), at the "
    "author's instruction.  The goldens are on the leveling vehicle and are out of regime.\n"
    "- **interlock:** inert and warn-only without `" + RELEVEL + "`; needs the same free-epoch "
    "latch (`network.cycle_slip.relevel_free_epoch`).  Read `_unmeasurable` with "
    "`val(..., True)` and never with `on(...)`, which is False for an absent key.  The field "
    "triple consumer bar B1 still fails and is **not the rider's to close**: GRS-3 (card "
    "section 6) put the residual triple deficit on the partial-signal epochs at the arc edges, "
    "and the admission rule built for them is falsified as the cause (`" + ADMIT + "`, Tier 4).  "
    "The lever those arms moved was the canonical-signal latch (`" + LATCH + "`).\n")

MD_LATCH = (
    "### `" + LATCH + " = \"priority\"`  (+ `processing.canonical_signal_min_coverage`, default "
    "0.5)\n"
    "- **default:** `\"count\"` at `src/core/config/GnssConfigLoad.cpp:351-358`; the coverage "
    "floor at `:360-367` (default 0.5, clamped to [0, 1]); enum `CanonicalSignalRule` at "
    "`include/config/GnssConfigTypes.hpp:3394`; decision `PppModel::decideCanonicalPicks` "
    "(`src/core/model/PppModel.cpp:415`).  **Regime:** `" + VEHICLE + "`, i.e. the RF-0hv FLOAT "
    "vehicle only, the one posture it has been graded on.  **RECOMMEND_SCOPED, new row "
    "2026-09-20 (card CSL).**  A **VALUE knob**: the default string is truthy, so it is read "
    "with `armed_when` = `" + LATCH_ARMED + "` and never with `on(...)`.\n"
    "- **what it decides:** under `ChannelMode::Single` the producer latches ONE tracking "
    "attribute per band and observation type for each station, at that station's FIRST "
    "processed epoch.  Under `count` the signal seen on the most satellites of that epoch wins, "
    "so one satellite without the semi-codeless pair at that epoch latches the civil signal for "
    "the whole run: field bed C1C 31 / C1W 28 and L1C 47 / L1W 12, although 18 stations carry "
    "L1W.  Under `priority` the first priority-list signal of the band that reaches the "
    "coverage floor wins and the code follows the phase's attribute: C1C 41 / C1W 18 and L1C 41 "
    "/ L1W 18.\n"
    "- **evidence (2026-09-20, `docs/refound/CSL_canonical_signal_latch_2026-09-20.md` section "
    "4, main `df691cd`, exe `gnss_processor_csl.exe`; no arm edits an observation file):** G0 "
    "byte-identical at the default on three controls; G1 the field latch reads as above from "
    "`network_pf_rows.csv` and the run's own log agrees.  Sim: both arms **byte-identical** "
    "under `priority` (one signal per band there).  Field dual 6 h, 59 stations, DOY 129 of "
    "2024 (`runs/_rap_lD_dual` against `runs/_rap_gD_dual`): consumer 3D p50 by hour 0.53 0.11 "
    "0.10 0.11 0.12 0.11 to 0.43 0.07 0.06 0.08 0.08 0.07 m, epochs below a decimetre 162 to "
    "**603** of 720, clock wire on the consumer's satellites 0.0090 to 0.0069 m (0.0128 m on "
    "all, unchanged).  Field dual 12 h (`runs/_rap_lD_dual12` against `runs/_rap_d12_grs`): 275 "
    "to **994** of 1440, clock wire 0.0148 to 0.0128 m, hour 12 at 0.17 m against 0.12 m the "
    "one hour worse by more than 0.03 m.  Field triple (`runs/_rap_lF_tri`): 6 to **314** "
    "epochs, first hour 1.47 m.\n"
    "- **mechanism half read, and that is part of the recommendation:** slip and refloat "
    "traffic REFUTED as the cause (at the six stations that move: MW detections 3 to 1, IF-TD 9 "
    "to 5, gaps 36 to 32; the coherence claim inverts, band-2-only cells 0.16 % on L1C with L2W "
    "against 7.6 % on L1W with L2W); post-fit residuals REFUTED, they get louder (band 1 at the "
    "41 unchanged stations 4.0 to 5.6 mm, band 2 at the six 4.2 to 8.4 mm); the support and "
    "anchoring of the L1W phase-bias columns SUPPORTED but not established (stations per "
    "(satellite, L1W) column 4.58 to 5.94, cold re-realizations 2 to 1, anchor-slip handovers 5 "
    "to 3).  The coverage-floor arms make it a **station subset, not a support count**: the "
    "consumer's gain is complete with four of the six stations that move, while the clock wire "
    "on its satellites keeps improving with the fifth and sixth (0.0096, 0.0080, 0.0069 m at "
    "floors 0.85, 0.78, 0.5).  One consumer at one site.\n"
    "- **armed:** `config/refound/{field,sim}_gps_dual_rf0hv.json` (2026-09-20), at the "
    "author's instruction, with the caveat above stated to the author.  The goldens are on the "
    "leveling vehicle and are out of regime.\n"
    "- **interlock:** inert where a band offers one signal per station (the sim bed), so a sim "
    "A/B cannot certify it.  The **closed-loop consumer is pinned to `count`** by "
    "`deriveClosedLoopConsumerConfig` (`GnssConfigLoadNetwork.cpp:11809`), so an arm moves the "
    "network stations only; the consumer's own latch is a separate, already-graded variable "
    "(moving it to the civil family costs 162 to 80 epochs below a decimetre).  "
    "`canonical_signal_min_coverage` is inert under `count` and selects station subsets under "
    "`priority` (0.85 gives 16 L1W stations, 0.78 gives 17, the shipped 0.5 gives 18).  Warm "
    "resume: the canonical map is carried state but its CONTENTS are deliberately not in the "
    "config fingerprint (`NetworkRunContext.cpp:1142-1152`), so this is **not fingerprinted** "
    "and a flip does not abort; with `network.resume_signal_canon_restore` armed the restored "
    "map wins and the flip is INERT, without it the resume re-decides every station's channels "
    "under the new rule.  Flip it on a fresh lineage.\n")

MD_ADMIT = (
    "### `" + ADMIT + "`  (+ `network.canonical_pair_keep_epochs`, default 3)\n"
    "- **default:** false at `src/core/config/GnssConfigLoadNetwork.cpp:4616-4618`; the "
    "tolerance at `:4619-4626` (default 3, clamped at 0 rather than refused); seam "
    "`filter/NetworkCanonicalPairAdmission` (pure state machine `canonicalPairStep`), state "
    "`NetworkRunContext.hpp:2374-2383`.  **Regime:** graded only on the RF-0hv float vehicle "
    "with the gap re-entry response and its per-signal rider, dual and triple, DOY 129 of 2024.  "
    "**KEEP_OFF, new row 2026-09-20 (card CPA, CLOSED).  Armed nowhere.**\n"
    "- **evidence (`docs/refound/CPA_canonical_pair_admission_2026-09-20.md` section 5, main "
    "`3b155f7`, exe `gnss_processor_cpa.exe`):** the rule is built EXACTLY and that is the "
    "finding.  G0 PASS on three controls.  G1 PASS: field dual 6 h, 471 076 records screened, "
    "**16 244 withheld** (4873 before the first good epoch, 11 371 beyond the tolerance) over "
    "851 pairs, against the emulation's **16 014** in the same window, **15 986 shared**, and "
    "the built rule IS the emulated rule.  It does not reproduce the emulation's gain and on the "
    "dual field bed it is **worse than no rule**: `runs/_rap_cD_dual` gives **16** epochs below "
    "a decimetre of 720 against 162 without the rule and 627 for the emulation; tolerance 0 "
    "gives 106.  Field triple 6 to 60.  Both sim arms equal their bases.  **Why:** the RINEX "
    "emulation moved a second variable.  Deleting records changes the tally of the first "
    "processed epoch, on which the producer decides each station's canonical signal per band, "
    "and it moved that latch at 13 of 59 stations (six in the band-1 phase).  The built rule "
    "leaves the latch where the base has it.  Forcing every station onto its priority-first "
    "signal with no rule gives 601 epochs below a decimetre, and the rule on top of it costs "
    "epochs (601 to 489).  The gain belongs to the latch (`" + LATCH + "`, Tier 2).\n"
    "- **interlock:** the tolerance is inert without this knob; OFF is byte-identical (no copy "
    "of the epoch is taken); a constellation with no canonical pair decided on band digits 1 "
    "and 2 is left untouched; not snapshotted and not in the warm-resume fingerprint (a resumed "
    "leg re-earns each pair's first good epoch).  The build stays in the tree default OFF as "
    "the INSTRUMENT that separated the two variables.  Lesson recorded by the card for the "
    "emulation tool: an edit of the observation files can move the producer's first-epoch "
    "decisions, so an emulation arm is checked for an unchanged latch (`network_pf_rows.csv`, "
    "signals by station) before its result is attributed.\n")

MD_APPENDIX = (
    "\n## A7 (2026-09-20): the RF-0hv float-vehicle session, part two: the per-signal rider, "
    "the canonical-signal latch, the canonical-pair admission rule\n"
    "\nOne row moved and two landed, all on the RF-0hv float-vehicle posture "
    "(`network.ambiguity_vehicle: float_kf`, GPS-only, DOY 129 of 2024, 59 field stations and "
    "60 sim stations, 6 h unless stated).  Full records: "
    "`docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md` (the rider and its 12 h arm), "
    "`docs/refound/CSL_canonical_signal_latch_2026-09-20.md` (the latch) and "
    "`docs/refound/CPA_canonical_pair_admission_2026-09-20.md` (the admission rule, closed).  "
    "Two knobs were armed in `config/refound/{field,sim}_gps_dual_rf0hv.json` at the author's "
    "instruction on the same date.\n"
    "\n| knob | verdict | evidence |\n|---|---|---|\n"
    "| `" + RIDER + "` (+ `_unmeasurable`) | **UNVALIDATED to RECOMMEND_SCOPED**, regime "
    "`" + RIDER_REGIME + "` (the float vehicle AND the row-level knob: it is a rider, inert and "
    "warned without it) | Byte-identical OFF on three controls.  Sim fault bed (81 injected "
    "losses of lock while band 5 tracks through) repaired completely on dual and triple: clock "
    "wire against truth 0.0670 to 0.0030 m and 0.0939 to 0.0029 m, `T4_attrib` 1940 to 61 and "
    "2944 to 72, the undisturbed sim identical.  Field dual `runs/_rap_gD_dual`: `T4_attrib` "
    "228 to 92, `CLK_ANCHOR` 424 to 232, clock wire against COD 0.0107 to 0.0090 m on the "
    "consumer's satellites and 0.0173 to 0.0128 m on all, consumer p50 equal within 0.02 m, "
    "epochs below a decimetre 203 to 162 (reported, not explained).  Field triple: `T4_attrib` "
    "716 to 219, clock wire 0.0269 to 0.0095 m.  **The owed 12 h dual arm** "
    "(`runs/_rap_d12_grs` against `runs/_rap_d12_on`): never worse by more than 0.02 m of "
    "consumer p50 in hours 3 to 12, hour 12 at 0.12 m against 0.35 m, `T4_attrib` 702 to 370, "
    "clock wire 0.0203 to 0.0148 m.  Armed. |\n"
    "| `" + LATCH + "` (`count` default, `priority`; + `processing.canonical_signal_min_coverage"
    "`, default 0.5) | **RECOMMEND_SCOPED** (new row), regime `" + VEHICLE + "` (the float "
    "vehicle: the only posture it was graded on, so the goldens stay out of regime) | "
    "Byte-identical at the default on three controls; both sim arms byte-identical under "
    "`priority` (one signal per band there).  Field latch under `count` C1C 31 / C1W 28 and L1C "
    "47 / L1W 12 although 18 stations carry L1W; under `priority` C1C 41 / C1W 18 and L1C 41 / "
    "L1W 18.  Field dual `runs/_rap_lD_dual` against `runs/_rap_gD_dual`: consumer 3D p50 by "
    "hour 0.53 0.11 0.10 0.11 0.12 0.11 to 0.43 0.07 0.06 0.08 0.08 0.07 m, epochs below a "
    "decimetre 162 to 603 of 720, clock wire 0.0090 to 0.0069 m on the consumer's satellites "
    "(0.0128 m on all, unchanged).  12 h `runs/_rap_lD_dual12` against `runs/_rap_d12_grs`: 275 "
    "to 994 of 1440, clock wire 0.0148 to 0.0128 m, hour 12 at 0.17 m against 0.12 m the one "
    "hour worse by more than 0.03 m.  Field triple `runs/_rap_lF_tri`: 6 to 314 epochs, first "
    "hour 1.47 m.  **Mechanism half read**, stated with the recommendation: slip traffic and "
    "post-fit residuals refuted as the cause, the support and anchoring of the L1W phase-bias "
    "columns supported and not established, and the consumer's gain complete with four of the "
    "six stations that move (a station subset, not a support count); one consumer at one site.  "
    "Armed. |\n"
    "| `" + ADMIT + "` (+ `network.canonical_pair_keep_epochs`, default 3) | **KEEP_OFF** (new "
    "row; card CPA CLOSED, armed nowhere) | The built rule withholds the emulated records "
    "exactly (16 244 against the emulation's 16 014 in 6 h, 15 986 shared) and does NOT "
    "reproduce the emulation's gain: field dual `runs/_rap_cD_dual` 16 epochs below a decimetre "
    "against 162 without the rule (worse than no rule), tolerance 0 gives 106; field triple 6 "
    "to 60; both sim arms equal their bases.  The RINEX emulation that motivated it had also "
    "moved the first-epoch canonical-signal latch at 13 of 59 stations, which is where its gain "
    "came from (card CPA section 5).  The build stays in the tree default OFF as the instrument "
    "that separated the two variables. |\n"
    "\n**The VALUE knob.** `" + LATCH + "` is a string enum whose default, `\"count\"`, is "
    "truthy, so the checker's default armed test would read an unarmed config as armed.  It "
    "carries an explicit `armed_when` (`" + LATCH_ARMED + "`), the same shape "
    "`network.apriori_source` uses, and the regime is a value test on the vehicle.  No DSL "
    "extension was needed: `val(path, default) == \"string\"` is already inside the checker's "
    "AST allowlist.\n"
    "\n**The `processing.*` path.** The loader posture table reaches it unchanged.  "
    "`config_posture_check.loader_knobs()` already accepts the prefixes "
    "`network|estimation|processing|filter`, and the table builders are const member functions "
    "of `GnssConfig`, so the private member `canonical_signal_rule_` "
    "(`include/config/GnssConfig.hpp:7764`) is read inline in the row's armed test, the way the "
    "other value rows read `network_apriori_source_` and `nc.cycle_slip_if_phase_td_pairs`.  "
    "Only the regime aliases live on the `PostureRegime` struct; a row's own armed test does "
    "not, so no predicate was added there or to the checker.  The KEEP_OFF row gets no posture "
    "row, which is the A5 / A6 convention and is what the reverse direction of "
    "`--verify-loader` requires.\n")

s = io.open(LEDGER_MD, encoding="utf-8").read()

if "- **amended later on 2026-09-20:**" not in s:
    anchor = "### `" + RIDER + "`"
    assert anchor not in s, "rider section already present but the master amendment is not"
    i = s.find("### `network.cycle_slip.if_phase_td_ref_gate`")
    assert i > 0, "Tier 2 anchor section not found"
    s = s[:i] + MD_RELEVEL_AMEND + s[i:]
    print("md: dated amendment appended to the %s section" % RELEVEL)
else:
    print("md: master amendment present (no-op)")

if "### `" + RIDER + "`" not in s:
    i = s.find("### `network.cycle_slip.if_phase_td_ref_gate`")
    assert i > 0, "Tier 2 anchor section not found"
    s = s[:i] + MD_RIDER + s[i:]
    print("md: Tier 2 section inserted for %s" % RIDER)
else:
    print("md: Tier 2 section present for %s (no-op)" % RIDER)

if "### `" + LATCH + " = " not in s:
    i = s.find("## Tier 3 ")
    assert i > 0, "Tier 3 heading not found"
    s = s[:i] + MD_LATCH + s[i:]
    print("md: Tier 2 section inserted for %s (end of tier: a processing.* path)" % LATCH)
else:
    print("md: Tier 2 section present for %s (no-op)" % LATCH)

if "### `" + ADMIT + "`" not in s:
    i = s.find("### `network.carry_coupling_joint_whiten`")
    assert i > 0, "Tier 4 anchor section not found"
    s = s[:i] + MD_ADMIT + s[i:]
    print("md: Tier 4 section inserted for %s" % ADMIT)
else:
    print("md: Tier 4 section present for %s (no-op)" % ADMIT)

if "## A7 (2026-09-20)" not in s:
    s = s.rstrip("\n") + "\n" + MD_APPENDIX
    print("md: appendix A7 appended")
else:
    print("md: appendix A7 present (no-op)")
io.open(LEDGER_MD, "w", encoding="utf-8").write(s)

# --------------------------------------------------------------------------- 3. posture rows
POSTURE_ROWS = (
    "            // --- Card GRS, the PER-SIGNAL rider of the row above (ledger row promoted\n"
    "            //     2026-09-20).  The row-level tier keys on the SATELLITE, so a loss of\n"
    "            //     lock on the primary pair while band 5 or a fallback signal tracks\n"
    "            //     through is never seen.  Regime = the float vehicle AND the row-level\n"
    "            //     knob: without it the per-signal history is never filled, the loader\n"
    "            //     WARNS and the tier counts itself inert.\n"
    "            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf\n"
    "                  && nc.slip_qc.gap_reentry,\n"
    "              nc.slip_qc.gap_reentry_per_signal,\n"
    "              Tier::SCOPED, false, \"network.cycle_slip.gap_reentry_per_signal\",\n"
    "              \"see the re-entry per SIGNAL, not per satellite: sim fault bed repaired \"\n"
    "              \"completely (clock wire against truth 0.0670 -> 0.0030 m dual, 0.0939 -> \"\n"
    "              \"0.0029 m triple), field dual T4_attrib 228 -> 92, and the 12 h field arm \"\n"
    "              \"never worse than 0.02 m of consumer p50 in hours 3 to 12 (T4_attrib 702 \"\n"
    "              \"-> 370)\" },\n"
    "            // --- Card CSL, the canonical-signal latch (ledger row landed 2026-09-20).\n"
    "            //     A VALUE knob, and the ONLY processing.* row in this table: the default\n"
    "            //     string \"count\" is truthy, so the armed test is an explicit comparison\n"
    "            //     on canonical_signal_rule_ (a direct GnssConfig member, not part of\n"
    "            //     network_config_ and not a PostureRegime alias) exactly as the\n"
    "            //     apriori_source row compares network_apriori_source_.  Regime = the\n"
    "            //     float vehicle: it is the only posture the rule was graded on.\n"
    "            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf,\n"
    "              canonical_signal_rule_ == CanonicalSignalRule::Priority,\n"
    "              Tier::SCOPED, false, \"processing.canonical_signal_rule\",\n"
    "              \"latch each station's band signal by PRIORITY, not by the satellite count \"\n"
    "              \"of its first processed epoch (one satellite without the semi-codeless \"\n"
    "              \"pair latches the civil signal for the whole run): field dual epochs below \"\n"
    "              \"a decimetre 162 -> 603 of 720, 12 h 275 -> 994 of 1440, triple 6 -> 314; \"\n"
    "              \"byte-identical on the sim; mechanism half read (a station subset)\" },\n"
)

src = io.open(POSTURE, encoding="utf-8", newline="").read()
if '"' + RIDER + '"' not in src:
    # The working tree's line endings depend on how git last touched this file (core.autocrlf
    # is true in this repo), so detect them instead of assuming LF.
    nl = "\r\n" if "\r\n" in src else "\n"
    key = "              Tier::SCOPED, false, \"network.cycle_slip.gap_reentry_relevel\","
    i = src.find(key)
    assert i > 0, "posture anchor row not found"
    tail = " }," + nl
    j = src.find(tail, i)
    assert j > 0, "posture anchor row has no terminator"
    j += len(tail)
    src = src[:j] + POSTURE_ROWS.replace("\n", nl) + src[j:]
    io.open(POSTURE, "w", encoding="utf-8", newline="").write(src)
    print("posture: two Tier::SCOPED rows inserted (%s, %s)" % (RIDER, LATCH))
else:
    print("posture: rows present (no-op)")
