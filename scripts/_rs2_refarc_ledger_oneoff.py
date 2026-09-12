#!/usr/bin/env python3
"""Ledger pass for the two 2026-09-12 cards that share a session (SPEC_event_harness s21, s22).

  python scripts/_rs2_refarc_ledger_oneoff.py refarc
      -> NEW row network.cycle_slip.common_band_ref_arc (RECOMMEND_ON, regime = the scope armed),
         md section, posture row (Tier::ON beside the if_phase_td_pairs row), sim-golden pin.
         The field golden needs no exception: the scope itself is a field exception there, so the
         clause is out of regime rather than suppressed.

  python scripts/_rs2_refarc_ledger_oneoff.py rs2 PIN|EXCEPTION "<field sentence>"
      -> NEW rows network.state_snapshot_carry_lin (write side; regime = state_snapshot_carry armed)
         and network.resume_carry_rebase (read side; regime = warm resume with a carry blob), md
         sections, posture rows (Tier::SCOPED beside their siblings), and on the field golden a PIN
         of the write knob (products byte-identical, only the snapshot grows) or a documented
         EXCEPTION with the sentence.  The read knob is armed by the harness on the resume LEG
         (scripts/event_leg.py), never by a golden, so it is pinned nowhere.

Modelled on scripts/_study7_ledger_oneoff.py (row insertion) and _fieldharness_ledger_oneoff.py
(field pin/exception).  Idempotent.  Then, in this order:
  python scripts/make_golden_config.py
  python scripts/make_golden_config.py --verify
  python scripts/config_posture_check.py --verify-loader
"""
import io
import json
import sys

ROOT = 'C:/GnssProcessor/'
LEDGER_JSON = ROOT + 'docs/CONFIG_promotion_ledger.json'
LEDGER_MD = ROOT + 'docs/CONFIG_promotion_ledger.md'
POSTURE = ROOT + 'src/core/config/GnssConfigNetworkPosture.cpp'
MANIFEST = ROOT + 'config/golden/golden_manifest.json'


def load_json():
    return json.load(io.open(LEDGER_JSON, encoding='utf-8'))


def save_json(L):
    json.dump(L, io.open(LEDGER_JSON, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)


def md_section(r, verdict):
    return ("### `" + r['knob'] + "`\n"
            "- **default:** " + r['default_at'] + "  — **regime:** `" + r['regime'] + "`.  **" + verdict + ".**  "
            + r['scope'] + "\n"
            "- **evidence:** " + r['evidence'] + "\n"
            "- **interlock:** " + r['interlock'] + "\n"
            "- **armed when:** `" + r['armed_when'] + "`  " + r['armed_when_why'] + "\n\n")


def insert_md_after_section(s, after_knob, text):
    a = s.find('### `' + after_knob + '`')
    assert a > 0, after_knob
    b = s.find('\n### ', a + 10)
    assert b > 0
    return s[:b + 1] + text + s[b + 1:]


def insert_posture_after_knob(src, knob, row_cpp):
    i = src.find('"' + knob + '",')
    assert i > 0, knob
    j = src.find(' },\n', i)
    assert j > 0
    return src[:j + 4] + row_cpp + src[j + 4:]


# ----------------------------------------------------------------------------- refarc
REFARC = 'network.cycle_slip.common_band_ref_arc'
REFARC_ROW = {
    "knob": REFARC,
    "family": "QC, RAIM and SLIP knobs",
    "verdict": "RECOMMEND_ON",
    "scope": ("The common-band scope's IF-TD veto can only fire where the pooled IF-TD test can flag, and on a chain's "
              "REFERENCE satellite it cannot: that sample's double difference is identically 0 by construction "
              "(IfJumpInfo::is_reference), so `|dd - med| > thr` is structurally false however large the slip and "
              "the pair lands in iftd_silent.  Armed, such a pair is UNTESTED for the verdict (CommonBandEvidence::"
              "iftd_untested) and is refused as an exonerator exactly as immature / lli_shadow are (token "
              "iftd(<pair>)=ref_arc, counter iftd_ref_arc with a per-pair breakdown, run-end 'ref-arc refusals'); "
              "the verdict falls back to today's all-band response when nothing else exonerates.  Applies to every "
              "chain's reference, first chain included (its per-station reference gate is a separate instrument "
              "whose decision the preview never sees).  No detector change; network_if_slip.csv gains an is_ref "
              "column (trace-only).  NAMED COST: a genuine single-band L1/L2 slip on an L5-chain reference arc loses "
              "its narrowing because its only silent pair is the untested one (all-band = the pre-card response).  "
              "SPEC_event_harness s22."),
    "regime": "on(\"network.cycle_slip.common_band_scope\")",
    "posture_why": ("A clause OF the scope's exoneration rule: with the scope off no verdict is formed and the knob is a "
                    "structural zero (the loader disarms it).  Field identity legs close the s20.1 trap exactly; the sim "
                    "storm bed is neutral on every bar."),
    "default_at": ("false at src/core/config/GnssConfigLoadNetwork.cpp (jval \"network.cycle_slip.common_band_ref_arc\", "
                   "beside common_band_min_arc_epochs; warn+disarm without common_band_scope); NetworkConfig "
                   "cycle_slip_common_band_ref_arc; rule branch src/core/slip/NetworkCommonBandScope.cpp (the preview "
                   "carries IfJumpInfo::is_reference, IfTdPreview); merged main 4b88ca2 (worker 6d3c304 + c8096ee)."),
    "fingerprinted": False,
    "interlock": ("REQUIRES network.cycle_slip.common_band_scope (loader warns + disarms otherwise).  Arm WITH "
                  "if_phase_td_pairs = all (the veto's coverage) exactly as the scope's own interlock says.  Not "
                  "fingerprinted: it changes which silences may exonerate, never a carried value's meaning."),
    "evidence": ("FIELD IDENTITY (SPEC_event_harness s22.4, 2026-09-12, runs/evleg_cbref_*, seed runs/evseed_field6h "
                 "ep0604 on the s21.5 interim posture state_snapshot_carry:false, exe 5BD7D2DC): B1 OFF byte-identical "
                 "(null on/off products 1FF0BA20; 12 of 1992 event rows differ, 12/12 carry ref_arc); B2 the equal-count "
                 "L1C+29 & L5X+29 slip on GOPE00CZE/G24 (the reference of both L5 chains) goes from keep implicated=L2W "
                 "(the s20.1 trap: the innocent band freed, 29 cycles left on both guilty bands) to all-band with "
                 "iftd(L1C+L5X)=ref_arc;iftd(L2W+L5X)=ref_arc named on the row and the IF-TD refloat no longer holding "
                 "L1C back (cb_kept gone); E1 L5X -99 on G24 {L5X} on both arms, products DEF230D7 identical; E2 L1C +37 "
                 "on G24 all-band (the named cost; OFF keep L1C); E3 on G25 (not a reference) byte-identical to OFF "
                 "(44D461B0).  Run-end ref-arc refusals 6 on the null, 8 on E3/E2 at G24, attributing exactly by pair.  "
                 "B3 4 forfeited of 106 narrowings across the leg pairs (3.8 %), all four injected, 0 natural; the "
                 "clause also resolves 6 inconsistent fallbacks per leg into narrowings with products unchanged.  "
                 "Reference exposure under pairs=all: 18.3 % of IF-TD samples, 107/507 arcs.  B4 SIM STORM ARM "
                 "(runs/golden_sim_campaign_zwdblk_refarc = the increment-1 sts arm + the knob, exe cbref, 720/720, "
                 "1507 s) vs runs/golden_sim_campaign_zwdblk_sts: products byte-identical for the first 283 epochs "
                 "(first actuation at tow 310890), then within noise on every graded channel - ZWD settled 0.0059 / "
                 "0.0059 m, consumer settled p50 0.0807 / 0.0807 (rms 0.09006 -> 0.09004), T4 attribution 11 / 11, "
                 "split-referenced 0.0 / 0.0 per epoch, WL 99.2, EWL 100.0, L1C ref-free 90.4 / 90.4, IF-TD vetoes "
                 "116 / 116, handovers 462 (362 continuous / 100 cold) both; ref-arc refusals 29 [L1C_L5Q 12, L1C_L7Q "
                 "5, L2W_L5Q 12], inconsistent 202 -> 186 (13 -> no-exoneration, 3 -> narrowed).  Every s22.3 B4 bar "
                 "passes; the sim bed never carries the trap, so the clause is neutral there and fixing on the field."),
    "armed_when": "on(\"network.cycle_slip.common_band_ref_arc\")",
    "armed_when_why": ("with network.cycle_slip.common_band_scope wherever it is armed (the sim golden).  On the field "
                       "golden the scope itself carries a documented exception (s20.1: the equal-count trap this clause "
                       "closes, and no warm-resuming seed to price the wrong narrowing), so the clause is out of regime "
                       "there; the field pin of scope + pairs + clause rides on the RS-2 seed (s21.5) once the s20 "
                       "wire/consumer bars can be read."),
}

REFARC_POSTURE = '''            // --- s22 THE REFERENCE-ARC CLAUSE (promoted 2026-09-12): a silent MW pair whose
            //     IF-TD sample is the chain's REFERENCE satellite is UNTESTED, not clean (dd == 0
            //     by construction, so the pooled preview can never flag it and the veto is
            //     structurally unavailable).  A clause of the scope's rule: regime = the scope.
            { nc.cycle_slip_common_band_scope,
              nc.cycle_slip_common_band_ref_arc,
              Tier::ON, false, "network.cycle_slip.common_band_ref_arc",
              "refuse a reference-arc pair as an exonerator (its IF-TD test cannot run): the field "
              "equal-count L1C+L5X slip on GOPE/G24 goes from the wrong narrowing (keep L2W) to "
              "all-band with both reference arcs named; single-band slips elsewhere unchanged, G25 "
              "byte-identical; sim storm arm neutral on every bar (consumer p50 0.0807 both, T4 11 "
              "both, ZWD 0.0059 both; runs/golden_sim_campaign_zwdblk_refarc vs _sts)" },
'''


def do_refarc():
    L = load_json()
    rows = L['knobs']
    if not any(r['knob'] == REFARC for r in rows):
        idx = next(i for i, r in enumerate(rows) if r['knob'] == 'network.cycle_slip.if_phase_td_pairs')
        rows.insert(idx + 1, REFARC_ROW)
        print('JSON: ref_arc row inserted after if_phase_td_pairs')
    else:
        print('JSON: ref_arc row present (no-op)')
    save_json(L)
    s = io.open(LEDGER_MD, encoding='utf-8').read()
    if '### `' + REFARC + '`' not in s:
        s = insert_md_after_section(s, 'network.cycle_slip.if_phase_td_pairs', md_section(REFARC_ROW, 'RECOMMEND_ON'))
        io.open(LEDGER_MD, 'w', encoding='utf-8').write(s)
        print('md: ref_arc section inserted')
    src = io.open(POSTURE, encoding='utf-8').read()
    if '"' + REFARC + '"' not in src:
        src = insert_posture_after_knob(src, 'network.cycle_slip.if_phase_td_pairs', REFARC_POSTURE)
        io.open(POSTURE, 'w', encoding='utf-8', newline='').write(src)
        print('posture: ref_arc row inserted')
    m = json.load(io.open(MANIFEST, encoding='utf-8'))
    g = next(e for e in m['goldens'] if e['name'] == 'golden_sim_tri_w6')
    if REFARC not in g['pins']:
        g['pins'][REFARC] = True
        print('manifest: sim golden pins', REFARC)
    json.dump(m, io.open(MANIFEST, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)


# ----------------------------------------------------------------------------- rs2
LIN = 'network.state_snapshot_carry_lin'
REBASE = 'network.resume_carry_rebase'


def rs2_rows(verdict_word, sentence):
    lin = {
        "knob": LIN,
        "family": "Card-4 invariant DD carry, its carry couplings (C1/Card-1x), the LS-VCE monitor, and the RTB-7 warm-resume snapshot knobs",
        "verdict": "RECOMMEND_SCOPED",
        "scope": ("WRITE half of the RS-2 linearization-frame carry (SPEC_event_harness s21).  The RTB-7c carry blob's Z "
                  "rows are the CORRECTION-frame double differences (lambda*N - sum c*x_lin) and a resumed run re-seeds every "
                  "station ambiguity x_lin from code, so without a record of the writer's frame every restored chord folds "
                  "wrong by sum c*(x_lin,writer - x_lin,now) at full stiffness: the field golden withheld 61/61 epochs at "
                  "every warm resume (dd_fold 7.8e7 of RHS norm vs 616 for all code obs, a00 -11109 TECU, 676 D5 flags at "
                  "+1).  Armed, the driver captures per chord zl = sum c*x_lin over the same fundamental cycle that forms "
                  "z_free and serializeCarry writes one ZL row per block beside Z.  Write side only: products and every "
                  "trace byte-identical, only the snapshot grows; format-forward (a reader without the row behaves as "
                  "before); NOT fingerprinted (field seed v1 ep0604 and v2 ep0061 both 545b8bdd2b638231)."),
        "regime": "on(\"network.state_snapshot_carry\")",
        "posture_why": ("Every snapshot-writing carry_invariant lineage should carry its linearization frame or its "
                        "branch cannot be restored honestly.  " + sentence),
        "default_at": ("false at src/core/config/GnssConfigLoadNetwork.cpp (jval \"network.state_snapshot_carry_lin\", "
                       "beside state_snapshot_carry_diagonal; disarmed without state_snapshot_carry); NetworkConfig "
                       "state_snapshot_carry_lin; driver Config::carry_lin, CarriedBlock::zl, serializeCarry ZL row "
                       "(src/core/filter/NetworkInvariantCarryDriver.cpp); arc x_lin from the station state vector in "
                       "src/core/filter/NetworkDdPriorBuild.cpp; merged main 2c62724 (worker 62303d2)."),
        "fingerprinted": False,
        "interlock": ("REQUIRES network.state_snapshot_carry (warn + disarm otherwise).  Read half: "
                      "network.resume_carry_rebase.  scripts/event_leg.py write-seed arms it on every harness seed."),
        "evidence": ("RS-2 (SPEC_event_harness s21.5, 2026-09-12): P1 byte-identity on the worktree build AND on main's "
                     "own rebuild - 20-epoch field-golden smoke products b9da8cc1 in the OFF and write-ON arms, the "
                     "snapshot differing only by the ZL rows; Debug suite 1968 / 0 failed; ResumeCarryRebaseTest (4).  "
                     "Field seed runs/evseed_field6h_v2 (6 h, 720/720, 0 withheld, 12 snapshots, 4 ZL rows per block).  "
                     + sentence),
        "armed_when": "on(\"network.state_snapshot_carry_lin\")",
        "armed_when_why": ("every snapshot-writing run of a carry_invariant lineage; the field golden " +
                           ("PINS it since 2026-09-12 (write side, byte-identical products): " if verdict_word == 'PIN'
                            else "carries a documented EXCEPTION (2026-09-12): ") + sentence),
    }
    rebase = {
        "knob": REBASE,
        "family": lin["family"],
        "verdict": "RECOMMEND_SCOPED",
        "scope": ("READ half of the RS-2 linearization-frame carry (SPEC_event_harness s21.4): at the FIRST DD prior "
                  "build after a warm resume that restored ZL-bearing blocks, every carried chord the build projects is "
                  "re-based onto THIS run's linearization, z_hat += W*zl_writer - zl_now, with W the cycle-basis "
                  "projection the build already forms (frame-blind, so this is the general form of the per-chord rule "
                  "and reduces to it when the tree is unchanged); Pz untouched; applied to the emitted block so a RAIM "
                  "rebuild is idempotent.  A restored contributor with no ZL row cannot be re-based and its chords take "
                  "the ordinary birth path (counted n_resume_carry_dropped_unbased).  Armed against a blob with no ZL "
                  "rows: one loud warning and today's behaviour.  Forward-only resume response policy, NOT in the "
                  "warm-resume fingerprint (same exemption class as resume_frame_restore)."),
        "regime": "warm and on(\"network.state_snapshot_carry\")",
        "posture_why": ("Without it a restored invariant DD branch is folded in the WRITER's linearization frame (the "
                        "field golden's 61/61-withheld warm resume).  " + sentence),
        "default_at": ("false at src/core/config/GnssConfigLoadNetwork.cpp (jval \"network.resume_carry_rebase\", beside "
                       "resume_frame_restore / resume_coupling_restore); driver Config::carry_rebase, CarriedBlock::unbased, "
                       "the re-base in buildDdPriors (src/core/filter/NetworkInvariantCarryDriver.cpp); run-end line "
                       "'Network (RS-2 linearization carry)' in NetworkTeardown.cpp; the 'Linearization frame' token on "
                       "the RTB-7c restore line; merged main 2c62724."),
        "fingerprinted": False,
        "interlock": ("Needs a blob written under network.state_snapshot_carry_lin (else warns, no-op) and "
                      "network.state_snapshot_carry on the resume leg.  scripts/event_leg.py arms it on every harness leg "
                      "beside resume_frame_restore.  Note resume_coupling_restore is refused by the loader on the "
                      "carry_invariant field lineage (it hard-requires Card 1f, which is out of regime there) - set it "
                      "false on such legs; this knob does not depend on it."),
        "evidence": ("RS-2 (SPEC_event_harness s21.5, 2026-09-12): Phase 1 on the existing seed established the seam "
                     "(control 60/60 withheld with dd_fold 7.8e7 and 676 D5 flags at +1; the no-blob leg 60/60 emitted "
                     "with a00 within 2.25 TECU but a cold branch that re-realizes the phase wire - one declared R record "
                     "naming 30 satellites, per-satellite-median-removed |dPB| 9.4 m, two no-blob legs disagreeing by "
                     "8.4 m, clock contrast growing 0.89 -> 3.41 m over 60 epochs; blob + SH gate off is garbage to +60, "
                     "so the withhold era is the carried memory's).  " + sentence),
        "armed_when": "on(\"network.resume_carry_rebase\")",
        "armed_when_why": ("every warm-resume LEG of a carry_invariant lineage whose seed was written with "
                           "state_snapshot_carry_lin (the harness arms it; no golden is a resume leg, so it is pinned "
                           "nowhere).  " + sentence),
    }
    return lin, rebase


LIN_POSTURE = '''            // --- RS-2 (SPEC_event_harness s21, 2026-09-12): the WRITE half of the
            //     linearization-frame carry.  The blob's Z rows are correction-frame DDs;
            //     without the writer's sum c*x_lin per chord a resumed run folds every
            //     restored chord in the wrong frame.  Write side only (products byte-identical,
            //     only the snapshot grows); regime = a carry blob is being written at all.
            { nc.state_snapshot_carry,
              nc.state_snapshot_carry_lin,
              Tier::SCOPED, false, "network.state_snapshot_carry_lin",
              "write one ZL row (sum c*x_lin per chord) per carried block so a warm resume can "
              "re-base the restored branch onto its own linearization (network.resume_carry_rebase); "
              "the field golden's warm resume went from 61/61 withheld to emitting (SPEC_event_harness "
              "s21.5)" },
'''
REBASE_POSTURE = '''            // --- RS-2 (SPEC_event_harness s21.4): the READ half.  At the first post-resume DD
            //     prior build z_hat += W*zl_writer - zl_now (W = the cycle-basis projection the
            //     build already forms; Pz untouched).  Forward-only resume policy, not
            //     fingerprinted; a blob without ZL rows warns and keeps today's behaviour.
            { warm && nc.state_snapshot_carry,
              network_resume_carry_rebase_,
              Tier::SCOPED, false, "network.resume_carry_rebase",
              "re-base the restored invariant DD branch onto this run's linearization at the first "
              "post-resume build instead of folding it in the writer's frame (the field golden's "
              "61/61-withheld resume: dd_fold 7.8e7 of RHS norm, 676 D5 flags at +1; SPEC_event_harness "
              "s21.5)" },
'''


def do_rs2(verdict_word, sentence):
    assert verdict_word in ('PIN', 'EXCEPTION')
    lin, rebase = rs2_rows(verdict_word, sentence)
    L = load_json()
    rows = L['knobs']
    if not any(r['knob'] == LIN for r in rows):
        idx = next(i for i, r in enumerate(rows) if r['knob'] == 'network.state_snapshot_carry_diagonal')
        rows.insert(idx + 1, lin)
        print('JSON: carry_lin row inserted after state_snapshot_carry_diagonal')
    if not any(r['knob'] == REBASE for r in rows):
        idx = next(i for i, r in enumerate(rows) if r['knob'] == 'network.resume_coupling_restore')
        rows.insert(idx + 1, rebase)
        print('JSON: resume_carry_rebase row inserted after resume_coupling_restore')
    save_json(L)
    s = io.open(LEDGER_MD, encoding='utf-8').read()
    if '### `' + LIN + '`' not in s:
        s = insert_md_after_section(s, 'network.state_snapshot_carry_diagonal', md_section(lin, 'RECOMMEND_SCOPED'))
        print('md: carry_lin section inserted')
    if '### `' + REBASE + '`' not in s:
        s = insert_md_after_section(s, 'network.resume_coupling_restore', md_section(rebase, 'RECOMMEND_SCOPED'))
        print('md: resume_carry_rebase section inserted')
    io.open(LEDGER_MD, 'w', encoding='utf-8').write(s)
    src = io.open(POSTURE, encoding='utf-8').read()
    if '"' + LIN + '"' not in src:
        src = insert_posture_after_knob(src, 'network.state_snapshot_carry_diagonal', LIN_POSTURE)
        print('posture: carry_lin row inserted')
    if '"' + REBASE + '"' not in src:
        src = insert_posture_after_knob(src, 'network.resume_coupling_restore', REBASE_POSTURE)
        print('posture: resume_carry_rebase row inserted')
    io.open(POSTURE, 'w', encoding='utf-8', newline='').write(src)
    m = json.load(io.open(MANIFEST, encoding='utf-8'))
    g = next(e for e in m['goldens'] if e['name'] == 'golden_field_gps_dual')
    ex = g.setdefault('exceptions', {})
    if verdict_word == 'PIN':
        g['pins'][LIN] = True
        ex.pop(LIN, None)
        print('manifest: field golden pins', LIN)
    else:
        g['pins'].pop(LIN, None)
        ex[LIN] = 'RS-2 FIELD EXCEPTION (SPEC_event_harness s21.5, 2026-09-12): ' + sentence
        print('manifest: field golden exception for', LIN)
    json.dump(m, io.open(MANIFEST, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)


if __name__ == '__main__':
    if len(sys.argv) >= 2 and sys.argv[1] == 'refarc':
        do_refarc()
    elif len(sys.argv) >= 4 and sys.argv[1] == 'rs2':
        do_rs2(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)
    print('done -- now: make_golden_config.py ; make_golden_config.py --verify ; config_posture_check.py --verify-loader')
