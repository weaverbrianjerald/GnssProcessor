#!/usr/bin/env python3
"""COLD ANCHOR-FALLBACK ledger pass (SPEC_event_harness §15), parameterised by verdict.

    python scripts/_coldfb_ledger_oneoff.py <RECOMMEND_ON|KEEP_OFF> "<storm sentence>"

Does, in order:
  1. docs/CONFIG_promotion_ledger.json  — insert (or update) the
     `network.phase_datum_candidacy_usable` row with the verdict, regime, scope,
     interlock and the harness + storm evidence;
  2. docs/CONFIG_promotion_ledger.md    — the matching prose section;
  3. on RECOMMEND_ON only: the posture row in
     src/core/config/GnssConfigNetworkPosture.cpp and the sim-golden pin in
     config/golden/golden_manifest.json.
Then run, by hand:
     python scripts/make_golden_config.py          # regenerate
     python scripts/make_golden_config.py --verify
     python scripts/config_posture_check.py --verify-loader

Promoted (rather than left in a scratchpad) for the same reason
`_study7_ledger_oneoff.py` was: the card must be closable from any session.
ROOT is this file's own repository, so it works from a worktree.
"""
import io, json, os, sys

if len(sys.argv) < 3:
    raise SystemExit(__doc__)
verdict, storm = sys.argv[1], sys.argv[2]
if verdict not in ("RECOMMEND_ON", "KEEP_OFF"):
    raise SystemExit("verdict must be RECOMMEND_ON or KEEP_OFF")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KNOB = "network.phase_datum_candidacy_usable"

HARNESS = (
 "HARNESS (SPEC_event_harness §15; worker build cb1b427 on worktree coldfb off main cc689d5; "
 "quiet seed evseed_quiet6h ep0665, 60-epoch legs, event at +10, 16 legs).  "
 "G1 OFF byte-identity PASS twice: the unmodified-worktree exe 90566ebf... and the card exe "
 "c763c168... produce an IDENTICAL OFF null (products f3f37bc3..., network_dd_fractional "
 "852c4dc3..., events 690927f6..., solution, zwd, and the run-end counter block); with the knob "
 "ARMED the solve is still byte-identical and only the two new trace fields appear on each "
 "handover row.  "
 "THE POPULATION IS NATURAL, NOT FORCED: all five injected anchor_handover families (E09, E10, "
 "G25, G10 = the §12.7 family, G30) hand over CONTINUOUSLY with the knob OFF — arc age and "
 "elevation at the snapshot epoch do NOT predict carry usability, so the §15 census's proxy is "
 "the wrong screen and the branch the election takes is the only oracle.  The cold population on "
 "this bed is the UNFORCED handover every leg carries at +22 (tow 280140): G30's sticky incumbent "
 "S25000SIM loses the key, the OFF election takes the top-ranked candidate S31000SIM at 12.59 deg "
 "whose ledger value the 0.01 m^2 ceiling refuses, and all three bands pin at 0 (COLD).  With the "
 "pass armed the same election skips exactly that candidate (cand_skipped=1, "
 "cand_first_unusable=S31000SIM in the trace) and elects S12000SIM at 39.81 deg, continuous at "
 "-13.65 / -11.86 / +3.97 m.  Six independent OFF/ON pairs (null, E09, E10, G25, G10, and the "
 "§12.7 40-cycle L1C slip at S29000SIM/G10 with anchor_slip_handover armed on both sides): "
 "handover_cold 3 -> 0 in EVERY pair; ZWD pooled truth rms 0.0721-0.0726 -> 0.0718 and max|e| "
 "0.363 -> 0.335 m in every pair; consumer 3D better in five of six (-10 to -125 mm at the leg "
 "end) and WORSE by 110-128 mm on the slip leg, which is also the only leg where the pass moves "
 "more than the three G30 elections (9 rank changes) and the only one where the FREED clause "
 "fires (7 candidates refused because a slip response had freed their own arc that epoch).  T4 "
 "rows 0 in every leg both arms (quiet bed), so the T4 half of the gate is vacuous there.  "
 "⚠ HARNESS LIMIT, measured: LocalCarry::birth_ep is not snapshotted AND the harvest re-stamps an "
 "unstamped entry at the current n_loop_epochs, so the first post-resume harvest re-brands every "
 "restored arc as born at the resume — on a warm-resume leg shorter than min_age_ep NO candidate "
 "can clear the bar and the pass is structurally inert (coldfb_null_on at the default 60: 24/24 "
 "elections fell through, 390 of 414 skips 'younger than 60 epochs', solve byte-identical to OFF). "
 "Every ON leg therefore runs min_age_ep 0, which exercises the LEDGER half of the predicate; the "
 "storm arm is a COLD 6 h run where the bar is live from epoch 60 on.  Tests: 17 cases in two new "
 "suites (tests/test_candidacy_usable.cpp); full Debug suite 1917 / 261 suites, 1900 passed, "
 "17 skipped, 0 failures.")

ROW = {
 "knob": KNOB,
 "family": "COLD-START, DATUM and CLOCK knobs (network.cold_clock_seed, clock_datum_*, "
           "code_datum_time_linked, phase_datum_*, s2z/brdc-anchored clock realization, "
           "apriori_source / apriori_clock)",
 "verdict": verdict,
 "scope":
  "COLD ANCHOR-FALLBACK (SPEC_event_harness §15 + §16.8).  The anchor election sweeps the "
  "coverage-ranked candidates and takes the FIRST that tracks the key; only AFTERWARDS does the "
  "continuous branch look that successor up in the one-epoch handover ledger and apply its "
  "usability predicate.  A family whose top-ranked tracker has no usable carried value therefore "
  "goes COLD — re-realizing the family's gauge, marking it for re-reference and erasing its "
  "leveling carries at every station — while a lower-ranked tracker holding a mature converged arc "
  "sits unused (cbscope5 storm arm: 43 of 47 non-cold-start cold handovers had >= 3 mature "
  "alternatives, and in NONE was the successor the best-placed candidate).  ON: where a handover "
  "is about to happen (an incumbent exists for the key and the sticky branch did not keep it) the "
  "sweep gains a FIRST pass over the SAME coverage-ranked list accepting only candidates whose "
  "carry is USABLE — the continuous branch's OWN predicate (one shared lambda, two readers), plus "
  "'not itself freed this epoch' (both freed vehicles) and an arc-age bar "
  "(network.phase_datum_candidacy_min_age_ep, default 60 loop epochs; 0 = no test, unknown age = "
  "mature).  With none usable the election falls through to today's two sweeps unchanged, counted. "
  "PROPERTY: handover_cold then counts exactly the UNAVOIDABLE re-realizations.  Two deliberate "
  "non-entries: a kept sticky incumbent, and an incumbent-LESS key (a first sighting has no "
  "outgoing frame, the continuous branch cannot run for it, and every pin there is a 0-pin "
  "whichever station takes it).",
 "regime": "on(\"network.phase_datum_prioritized\") and on(\"network.phase_datum_handover_continuous\") "
           "and not on(\"network.phase_datum_eq34\") and not on(\"network.phase_datum_ensemble\")",
 "posture_why":
  "a handover's successor is chosen from the candidates whose carry is USABLE by the continuous "
  "branch's own predicate, so a cold gauge re-realization happens only when no tracking candidate "
  "has one.  " + storm,
 "default_at":
  "false = byte-identical shipped behaviour (include/config/GnssConfig.hpp "
  "network_phase_datum_candidacy_usable_{false} / network_phase_datum_candidacy_min_age_ep_{60}; "
  "parsed src/core/config/GnssConfigLoadNetwork.cpp beside phase_datum_handover_max_var_ratio, "
  "negative bar refused at load; resolved ctx.knobs.pd_candidacy_usable against "
  "pd_handover_continuous in NetworkRunContext.cpp; the pass itself in "
  "src/core/filter/NetworkPhaseDatum.cpp networkElectPhaseAnchors behind the "
  "AnchorCandidacyUsable view; counters on NetworkAnchorTallies; run-end clause in "
  "NetworkTeardown.cpp; trace fields cand_skipped / cand_first_unusable on the PBGAUGE and "
  "PHASE_ANCHOR handover rows; 17 tests tests/test_candidacy_usable.cpp).",
 "fingerprinted": False,
 "interlock":
  "INERT without network.phase_datum_handover_continuous — with no ledger there is nothing to "
  "screen against, and the loader says so.  Election-side only: it changes WHICH station reaches "
  "the continuous branch, never a carried value's meaning, so a snapshot written OFF resumes ON "
  "and vice versa.  The hygiene eligibility test rides inside the pass, so the elected station is "
  "always one today's FIRST sweep would also have accepted — the pass can only move the choice "
  "LATER down the same coverage order.  A forced-cold injector directive is part of the shared "
  "predicate, so a harness cold leg cannot be silently re-pointed.  "
  "⚠ network.phase_datum_candidacy_min_age_ep reads LocalCarry::birth_ep, which is NOT snapshotted "
  "and is re-stamped by the first post-resume harvest: on a WARM-RESUME lineage the bar refuses "
  "every candidate for its first min_age_ep epochs (the pass then falls through to today's "
  "election — the safe direction, but inert).  Set 0 to run on the ledger half alone, or take the "
  "owed card: snapshot birth_ep / birth_s, or give the bar a resume-aware clock.",
 "evidence": HARNESS + "  STORM ARM: " + storm,
 "armed_when": "on(\"" + KNOB + "\")",
 "armed_when_why":
  "prioritized-anchor lineages running the two-edge continuous handover (the sim golden); the "
  "field golden runs the same election and the same ledger, but the card has no field arm yet — "
  "a field leg is owed before a field pin.",
}

# ---------------- 1. ledger JSON ----------------
p = os.path.join(ROOT, "docs", "CONFIG_promotion_ledger.json")
L = json.load(io.open(p, encoding="utf-8"))
rows = L["knobs"]
old = next((r for r in rows if r.get("knob") == KNOB), None)
if old is None:
    anchor = next((r for r in rows
                   if r.get("knob") == "network.phase_datum_handover_max_var_ratio"), None)
    rows.insert(rows.index(anchor) + 1 if anchor else len(rows), ROW)
    print("JSON: row inserted")
else:
    old.update(ROW)
    print("JSON: row updated")
json.dump(L, io.open(p, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
print("JSON: verdict ->", verdict)

# ---------------- 2. ledger md ----------------
p = os.path.join(ROOT, "docs", "CONFIG_promotion_ledger.md")
s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
head = "### `" + KNOB + "`"
sec = (head + "  (COLD ANCHOR-FALLBACK: a usable carried value is a CANDIDACY condition, "
       "BUILT 2026-09-11)" + nl
       + "- **default:** " + ROW["default_at"] + "  — **regime:** prioritized-anchor lineages "
         "under the two-edge continuous handover — **" + verdict + ".**  " + storm + nl
       + "- **mechanism:** " + ROW["scope"] + nl
       + "- **interlock:** " + ROW["interlock"] + nl
       + "- **evidence:** " + HARNESS + nl
       + "- **tests:** `CandidacyUsableConfig.*` (4), `CandidacyUsableElection.*` (13) in "
         "`tests/test_candidacy_usable.cpp`." + nl + nl)
if head in s:
    a = s.find(head)
    b = s.find(nl + "### ", a + 10)
    s = s[:a] + sec + s[b + len(nl):]
    print("md: section replaced")
else:
    anchor = "### `network.phase_leveling_local_block_min_age_ep`"
    i = s.find(anchor)
    if i < 0:
        i = len(s)
    s = s[:i] + sec + s[i:]
    print("md: section inserted")
io.open(p, "w", encoding="utf-8", newline="").write(s)

if verdict != "RECOMMEND_ON":
    print("KEEP_OFF: no posture row, no manifest pin, no golden change.")
    raise SystemExit(0)

# ---------------- 3a. posture row ----------------
p = os.path.join(ROOT, "src", "core", "config", "GnssConfigNetworkPosture.cpp")
s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
if KNOB not in s:
    anchor = ("            { network_phase_datum_prioritized_ && !network_phase_datum_eq34_,"
              + nl + "              network_phase_anchor_carry_reref_,")
    assert anchor in s, "posture anchor row not found"
    row = ("            { network_phase_datum_prioritized_ && !network_phase_datum_eq34_"
           + nl + "                  && network_phase_datum_handover_continuous_,"
           + nl + "              network_phase_datum_candidacy_usable_,"
           + nl + "              Tier::SCOPED, false, \"" + KNOB + "\","
           + nl + "              \"a handover's successor is chosen from the candidates whose CARRY IS "
                  "USABLE by \""
           + nl + "              \"the continuous branch's own predicate, so a COLD gauge "
                  "re-realization happens \""
           + nl + "              \"only where no tracking candidate has one. \"" + nl
           + "              \"" + storm.replace('"', "'")[:220] + "\" },"
           + nl)
    s = s.replace(anchor, row + anchor, 1)
    io.open(p, "w", encoding="utf-8", newline="").write(s)
    print("posture: row inserted")
else:
    print("posture: row already present")

# ---------------- 3b. manifest pin ----------------
p = os.path.join(ROOT, "config", "golden", "golden_manifest.json")
M = json.load(io.open(p, encoding="utf-8"))
for g in M["goldens"]:
    if g["name"] == "golden_sim_tri_w6":
        g["pins"][KNOB] = True
        print("manifest: pinned on", g["name"])
    elif g["name"] == "golden_field_gps_dual":
        g.setdefault("exceptions", {})[KNOB] = (
            "validated on the sim bed only (SPEC_event_harness §15): the cold-handover population "
            "the card removes was measured on the 60-station synthetic truth bed and on the "
            "cbscope5 storm arm; a field harness leg is owed before a field pin.")
        print("manifest: field exception recorded")
json.dump(M, io.open(p, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
print("\nNow run:  python scripts/make_golden_config.py && python scripts/make_golden_config.py "
      "--verify && python scripts/config_posture_check.py --verify-loader")
