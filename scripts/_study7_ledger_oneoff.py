#!/usr/bin/env python3
"""Study 7 ledger pass: (1) new row network.cycle_slip.if_phase_td_pairs, (2) update the
network.cycle_slip.common_band_scope row (verdict, F2/F3, storm evidence), (3) if RECOMMEND_ON:
posture rows for both, sim-golden pins for both, field exceptions for both.
Usage: ledger_study7.py <VERDICT> "<storm sentence>"   (VERDICT = RECOMMEND_ON | KEEP_OFF)"""
import io, json, sys
ROOT = 'C:/GnssProcessor/'
verdict, storm = sys.argv[1], sys.argv[2]
HARNESS_PAIRS = ("HARNESS (SPEC_event_harness s14.10-s14.12, s16; worker build ace5d3e + 1ef8ea8 on worktree "
    "agent-a0887be88117c9ad4, merged bbc8273; manager re-grade): G1 first + scope OFF byte-identical to main 420634f on "
    "the quiet 240-epoch null (products 4c96e34a...) and the L2 control (2c58b97f...), every artifact incl. events, "
    "rx-states and the consumer trace; S2 (L1C -29 and L5Q -29 in one epoch at S21000SIM/G08, quiet seed ep0665): scope "
    "alone -> keep implicated=L2W (the wrong narrowing, iono step 3.8 m); with all pairs -> all, basis iftd(L1C+L5Q)=flagged, "
    "products digit-for-digit = OFF; L2 control (L2W -33): all == first byte-for-byte, still narrowed to L2W (level residual "
    "0.0003 vs 0.33 cyc OFF); S1 (LLI-flagged L5Q -31 then L1C -29): closed by rule F2, not by the pairs (the (1,5) IF-TD had "
    "fired and reset on the LLI epoch) -> all / lli_shadow(n=10), residuals = OFF; S3 (LLI-flagged L5Q alone): under all "
    "WITHOUT rule F3 the pairs containing L5Q fired unscoped at the LLI epoch (level residual 0.005 -> 0.335 cyc at +45); "
    "with F3 all == first byte-for-byte on S1, S3, L2 and the 70-epoch null -- S2 is the only leg where the extra pair "
    "changes the solve.  Shared reference arm (s14.10.4): per-chain reference-slip tests took the quiet null's IF-TD ref "
    "gate from 25 candidates / 1 fire to 5,351 / 64 because (L2W,L5Q) amplifies noise 16.6x vs 2.98x on (L1C,L2W); "
    "additional chains adopt the first chain's reference and 25 / 1 is restored.  Tests 1897 / 259, 0 failures "
    "(+14 tests/test_if_td_pair_width.cpp, +9 common-band).  MW detector decision lines (MWDEC/MWSKIP, station-labelled) "
    "now exist behind the detector-log level: the detector WITHHOLDS the LLI epoch's sample for the whole satellite and "
    "keeps the arc, so the next sample reads the combined jump -- the mechanism of every wrong narrowing on the storm bed.")
# ---------------- ledger JSON ----------------
p = ROOT + 'docs/CONFIG_promotion_ledger.json'
L = json.load(io.open(p, encoding='utf-8')); rows = L['knobs']
cb = next(r for r in rows if r.get('knob') == 'network.cycle_slip.common_band_scope')
cb['verdict'] = verdict
if 'RULE F2' not in cb['scope']:
    cb['scope'] += ("  RULE F2 (the LLI shadow, s14.11): a pair whose band carried a receiver flag at the previous epoch is IMMATURE for "
        "the verdict (the MW detector withholds the flagged epoch's sample and keeps the arc, so the lane's next silence is judged "
        "against statistics that swallowed the slip); basis token lli_shadow(n=), counter cb_lli_shadow.  RULE F3 (the LLI-epoch "
        "verdict, s14.12): at a receiver-flagged epoch where no MW pair flagged, the verdict is formed from the receiver's bits "
        "(implicated = the flagged bands, no exoneration step; basis lli=;mw=withheld; counters cb_lli_verdicts / cb_lli_narrowed) "
        "so every drain at that epoch -- the LLI drain and every IF-TD pair drain -- frees only the flagged band; T1_if_slip rows "
        "carry cb_kept=<bands> when a verdict refused a band.  Storm-bed census that motivated both (s16.5-s16.6): 6 wrong keep "
        "rows in 2,994, every one 30 s after a receiver-flagged slip, five spurious-implicate and one wrong-exonerate "
        "(S37/G20 at 4.99 h: L5Q -31 flagged then L1C -29; the (L1C+L5Q) lane read +2 WL and stayed silent).")
if 'STUDY 7 STORM' not in cb['evidence']:
    cb['evidence'] = "STUDY 7 STORM ARM: " + storm + "  " + cb['evidence']
cb['interlock'] = cb['interlock'].replace(
    "Arm together with network.cycle_slip.mw_evidence_refloat and network.cycle_slip.anchor_slip_handover (the sim golden carries all three)",
    "Arm together with network.cycle_slip.mw_evidence_refloat, network.cycle_slip.anchor_slip_handover AND network.cycle_slip.if_phase_td_pairs = all "
    "(the IF-TD veto must cover every pair the rule can exonerate from -- with one IF pair per satellite the same-epoch equal-count trap is open on the other pairs)")
if not any(r.get('knob') == 'network.cycle_slip.if_phase_td_pairs' for r in rows):
    idx = rows.index(cb)
    new = {
     "knob": "network.cycle_slip.if_phase_td_pairs",
     "family": "QC, RAIM and SLIP knobs",
     "verdict": verdict,
     "scope": "first (default) = the IF-TD phase time-difference detector forms ONE pair per satellite, the first two distinct phase bands (L1C+L2W on GPS, L1C+L5Q on Galileo) -- today's byte-identical behaviour.  all = every distinct pair of the satellite's tracked phase bands (3 on a triple-band arc), each with its own pool (centre / scale / shrinkage), gradient chain, reset keys and per-pair-kind counters; additional chains adopt the first chain's reference satellite (s14.10.4) so the ref gate is not flooded by the 12-19x noise amplification of the close-spaced pairs; drains keep today's per-pair semantics (with the common-band verdict ON, pair bands intersected with implicated); T1_if_slip rows carry pair= under all only.  MECHANISM (SPEC_event_harness s14.10, s16): the common-band verdict vetoes a silent MW pair only when the IF-TD on THAT pair flagged; with one IF pair per satellite the same-epoch equal-count trap (two bands slipping by near-equal counts leave their MW lane silent) is structurally open on the other pairs -- the storm bed carried 9 near-equal adjacent slip pairs in 6 h, 6 on pairs the IF-TD never formed.",
     "regime": "on(\"network.phase_leveling\") and on(\"network.cycle_slip.mw_detect\") and on(\"network.cycle_slip.if_phase_td\")",
     "posture_why": "The IF-TD pair behind every MW lane is what lets the common-band verdict trust a silence: the pure equal-count trap on the (L1C,L5Q) pair (S2) is vetoed exactly (products = OFF digit for digit) and the fix is inert on a genuine single-band slip (L2 control byte-identical to first).  " + storm,
     "default_at": "first = byte-identical shipped behaviour (main bbc8273, merged 2026-09-11; worker commits ace5d3e + 1ef8ea8 on worktree agent-a0887be88117c9ad4).  Loader src/core/config/GnssConfigLoadNetwork.cpp (cycle_slip block; enum NetworkIfPhaseTdPairs First|All in include/config/GnssConfigTypes.hpp); detector include/slip/IfPhaseSlipDetector.hpp + src/core/slip/IfPhaseSlipDetector.cpp (per-pair chains <sys>|<sig1>_<sig2>, the first pair keeps the bare-system chain; per-pair resetSatelliteInState and reset keys); driver networkSlipIfTdDriver in src/core/slip/NetworkSlipQc.cpp; per-pair-kind tally and veto breakdown in NetworkTeardown.cpp ('Network (IF-TD pairs [all]): N pair kind(s), eval / flag / refloat'); the verdict (NetworkCommonBandScope) reads every evaluated pair's flag; 14 tests tests/test_if_td_pair_width.cpp.",
     "fingerprinted": False,
     "interlock": "Detection-side knob: it changes which IF pairs are evaluated, never a carried value's meaning, so a snapshot written first resumes all and vice versa.  Inert without the IF-TD detector.  With the common-band verdict OFF, all changes a response only when every MW pair is silent and only a non-first IF pair flags (the all-bands-equal case), absent from the sim bed; with the verdict ON it is the veto's coverage and must be armed with network.cycle_slip.common_band_scope (whose rule F3 keeps the new pairs' drains scoped at receiver-flagged epochs -- without F3, all turned every LLI epoch into an all-band refloat, S3 level residual 0.005 -> 0.335 cyc).  The close-spaced pairs amplify phase noise 12x (L2W,L5Q) and 19x (L5Q,L7Q): their pools are their own, and a one-cycle equal slip on those two bands stays marginal (~1.5 sigma) -- a 25 cm misfit the outlier tier catches in one epoch; the realized trap (large slips, small difference) is metres in every pair.",
     "evidence": HARNESS_PAIRS + "  STORM ARM: " + storm,
     "armed_when": "on(\"network.cycle_slip.if_phase_td_pairs\")",
     "armed_when_why": "phase_leveling lineages running the MW and IF-TD detectors together with network.cycle_slip.common_band_scope (the sim golden); the field golden is out of validation for the same reason as the scope (its L5 lanes are BIA-corrected under real OSB products and run the between-satellite MW path) -- a field harness leg is owed before a field pin."
    }
    rows.insert(idx + 1, new); print('JSON: pairs row inserted after common_band_scope')
json.dump(L, io.open(p, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
print('JSON: common_band_scope verdict ->', verdict)
# ---------------- ledger md ----------------
p = ROOT + 'docs/CONFIG_promotion_ledger.md'
s = io.open(p, encoding='utf-8').read()
a = s.find('### `network.cycle_slip.common_band_scope`'); b = s.find('\n### ', a + 10)
sec = s[a:b]
sec = sec.replace('**KEEP_OFF.**', '**' + verdict + '.**', 1).replace('**RECOMMEND_ON.**', '**' + verdict + '.**', 1)
if 'RULE F2' not in sec:
    sec = sec.replace('\n- **evidence:** ', '  RULE F2 / RULE F3 (s14.11 / s14.12) added 2026-09-11 pm -- see the JSON row scope.\n- **evidence:** STUDY 7 STORM ARM: ' + storm + '  ', 1)
sec = sec.replace('(the sim golden carries all three)', 'AND network.cycle_slip.if_phase_td_pairs = all (the veto must cover every pair the rule can exonerate from)', 1)
s = s[:a] + sec + s[b:]
if 'network.cycle_slip.if_phase_td_pairs`' not in s:
    r = next(x for x in L['knobs'] if x['knob'] == 'network.cycle_slip.if_phase_td_pairs')
    anchor = '### `network.cycle_slip.refloat_band_scope`'; i = s.find(anchor); assert i > 0
    msec = ("### `network.cycle_slip.if_phase_td_pairs`\n"
            "- **default:** " + r['default_at'] + "  — **regime:** `" + r['regime'] + "`.  **" + verdict + ".**  " + r['scope'] + "\n"
            "- **evidence:** " + r['evidence'] + "\n"
            "- **interlock:** " + r['interlock'] + "\n"
            "- **armed when:** `" + r['armed_when'] + "`  " + r['armed_when_why'] + "\n\n")
    s = s[:i] + msec + s[i:]; print('md: pairs section inserted')
io.open(p, 'w', encoding='utf-8').write(s); print('md: common_band_scope section updated')
if verdict != 'RECOMMEND_ON':
    print('not RECOMMEND_ON: no posture rows, no pins'); sys.exit(0)
# ---------------- posture rows ----------------
p = ROOT + 'src/core/config/GnssConfigNetworkPosture.cpp'
s = io.open(p, encoding='utf-8').read()
anchor = '"0.007 m, consumer p50 0.28 -> 0.17 m, T4 306 -> 78, WL 98.9 %" },\n'
i = s.find(anchor); assert i > 0, 'posture anchor not found'
short = storm.replace('"', "'")[:150]
rows_cpp = ''
if 'network.cycle_slip.common_band_scope' not in s:
    rows_cpp += ('            { network_phase_leveling_ && network_cycle_slip_mw_detect_,\n'
                 '              nc.cycle_slip_common_band_scope,\n'
                 '              Tier::ON, false, "network.cycle_slip.common_band_scope",\n'
                 '              "free only the band the MW evidence names: a silent, mature pair exonerates its bands unless "\n'
                 '              "IF-TD flagged it, LLI is set, or the pair is in the LLI shadow (rule F2); the anchor clause keeps "\n'
                 '              "a satellite\'s references co-located; the LLI-epoch verdict (rule F3) scopes every drain to the "\n'
                 '              "receiver\'s bits (SPEC_event_harness s14, s16): single-band legs 24-300x lower level residual, "\n'
                 '              "nine cold handovers made continuous on the storm bed; ' + short + '" },\n')
if 'network.cycle_slip.if_phase_td_pairs' not in s:
    rows_cpp += ('            { network_phase_leveling_ && network_cycle_slip_mw_detect_,\n'
                 '              nc.cycle_slip_if_phase_td_pairs == NetworkIfPhaseTdPairs::All,\n'
                 '              Tier::ON, false, "network.cycle_slip.if_phase_td_pairs",\n'
                 '              "an IF-TD pair behind EVERY MW lane (per-pair pools, shared reference arm) so the common-band "\n'
                 '              "verdict can veto an equal-count silence on any pair, not only the first (SPEC_event_harness "\n'
                 '              "s14.10, s16): the same-epoch trap S2 vetoed exactly, inert on a single-band slip; ' + short + '" },\n')
s = s[:i + len(anchor)] + rows_cpp + s[i + len(anchor):]
io.open(p, 'w', encoding='utf-8').write(s); print('posture rows inserted:', rows_cpp.count('Tier::ON'))
# ---------------- manifest: sim pins + field exceptions ----------------
p = ROOT + 'config/golden/golden_manifest.json'
m = json.load(io.open(p, encoding='utf-8'))
for e in m['goldens']:
    if e['name'] == 'golden_sim_tri_w6':
        e['pins']['network.cycle_slip.common_band_scope'] = True
        e['pins']['network.cycle_slip.if_phase_td_pairs'] = 'all'
    if e['name'] == 'golden_field_gps_dual':
        ex = e.setdefault('exceptions', {})
        ex['network.cycle_slip.common_band_scope'] = ("NOT field-validated (SPEC_event_harness s14.8, s16): the field's L5 lanes are BIA-corrected under real OSB "
            "products and run the between-satellite MW path, whose silence semantics this rule has not been priced on; a field harness leg must price it before a field pin; the sim golden carries it")
        ex['network.cycle_slip.if_phase_td_pairs'] = ("NOT field-validated (SPEC_event_harness s14.10, s16): armed with common_band_scope on the sim golden only; the field's "
            "close-spaced pairs (L2W,L5Q) amplify phase noise 12x and its pools have not been priced on real data; a field harness leg is owed before a field pin")
json.dump(m, io.open(p, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
print('manifest: sim pins + field exceptions written')
