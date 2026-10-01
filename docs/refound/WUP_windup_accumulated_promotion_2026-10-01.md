# WUP: accumulated phase wind-up for the field lineages, by the promotion path

**STATUS**

- **id:** WUP
- **topic:** network-vehicle
- **status:** closed
- **summary:** ARMED 2026-10-01 (sections 5 and 6): every gate passed. G0 byte-identical on both beds; the fixed response answers 27 of 28 in-horizon restarts on the field (52 arcs, none bridged; 3 were bridged before) and 14 of the sim golden's 15, which had none answered; the field's 100 staircase steps per 6 h are gone, IF-TD flags 111 to 70; WTZZ00DEU within 3 mm in every hour; the census with accumulating consumers 2.6 cm better at the median site in hours 2 to 6 (9 of 17 sites by more than 2 cm, none worse; hour 1 cross-site median +6.6 cm at ignition), the wire alone neutral: the gain is the consumers' own mode. Both knobs in the live bases and pinned on the goldens (34 pins, 65 armed), ledger A14, posture rows; baseline pair `runs/_rf0hv12_{field,sim}6`. PRE-REGISTERED 2026-10-01 (the outside review's decision after card FWS: accumulated wind-up on the field lineages, through the ledger, the goldens and the verify, gated on the producer's own instruments because the consumer does not move). The decision rested on card FWS's statement that the restart response answered every restart it owed; it did not (section 1, card FWS section 6): the drain walked the live ambiguity map, which a re-entering arc no longer has, and counted the miss as a response. The fix (the drain reaches a re-entering arc through its leveling carry, as the gap re-entry drain does) comes first; then the gates of section 3 on the program posture and on both goldens, then the promotion of section 4.
- **source:** [FWS](FWS_field_walk_share_sources_2026-10-01.md) sections 3.2, 4 to 6; CLAUDE.md, the wu93 amendment 3 (the open default card and its blocking restart response); `docs/archive/SPEC_phase_windup_sign.md` sections 10 and 11; card PBF section 5 (the promotion template); `docs/CONFIG_promotion_ledger.md`
- **knobs / posture:** `estimation.accumulate_phase_windup` (field; the sim golden already pins it), `network.cycle_slip.windup_restart_refloat` (both), `estimation.trace_windup_census` (read-only, on every arm)
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** exes `gnss_processor_wup1.exe` `576ec0e7007d29a0` (the fix, main `6ad24c7`) and `gnss_processor_wup2.exe` `ef766aaedd7b2d55` (plus the posture rows); arms `runs/_rap_wup_*`; census keys `wupc`, `wup`, `wupf`, `wupcs`, `wups`; baseline pair `runs/_rf0hv12_{field,sim}6`

## 1. The response that did not act

`networkSlipDrainWindupRestart` (`src/core/slip/NetworkSlipQc.cpp`) looked for the stepped arcs only
in `sm.getAmbiguities()`. A restart is a return after a gap longer than the wind-up horizon
(300 s), so the arc has left the model set: its column was pruned with the gap, and PASS B's
pre-pass re-creates it only after the slip drains. The gap re-entry drain sited beside it says so in
its own comment and takes the leveling carry for that reason. On the 6 h field arm
`runs/_rap_fws_wuacc6_f` the response drained 69 restarts: 41 beyond the carry horizon and 28 inside
it, all 28 with 0 arcs (`no_arc`), each counted as RESPONDED. Of the 28, the gap re-entry drain
refloated 20 for its own evidence, 5 had no carry, and 3 had the carry bridged across the restart
unanswered (card FWS section 6). Invariant #1's response to the restart was absent, and invariant
#4's counter could not show it.

**The fix** (built with this card; the knob stays default off, so every posture without it is
byte-identical): `networkWindupRestartTargets` resolves the arcs a restart stepped, the satellite's
live columns when it has any, otherwise its leveling carry entries inside the carry horizon, on the
synthesised key the PASS-B inject reads; the drain frees them as before (free-epoch latch, cause
`kCauseWindup`, the C3c funnel under `carry_invariant`, the bridge pends voided), the pending-reseed
mark only on a live column. Counters: `wu_restart_carry_arcs` (arcs reached through the carry) and
`wu_restart_no_arc` (a warm, in-horizon restart with nothing to free), now outside the responded
count; the closure gains the new bucket; the run-end line warns when in-horizon restarts refloat no
arc. Unit gate: `tests/test_windup_restart_targets.cpp` (suite `WindupRestartTargets`, 5 cases).

## 2. Why the promotion, and what it does not claim

The two beds have sat on different wind-up modes through every sim-against-field comparison of the
clock-level program (the sim accumulates, the field wraps), and card FWS showed the difference is
material: the field's fractional staircase is a one-cycle, MW-blind step at each half-cycle crossing
(107 mm in the ionosphere-free combination), half of them unflagged, and it carries about half of
the field's hour-scale walk-share excess. The consumer does not move under the phase-clock hold, so
the promotion is graded on the producer's own instruments and on a census that must not lose.

## 3. The gates, written before any arm ran

All 6 h, on `gnss_processor_wup1.exe`, every arm with `estimation.trace_windup_census`.

- **G0 (the fix's off path).** The two golden controls (`config/golden/golden_{field,sim}_refound.json`
  plus the census knob, which is read-only) are byte-identical in products, `closed_loop_trace.csv`
  and `network_epoch_status.csv` to `runs/_rf0hv11_{field,sim}6`.
- **G1 (the fix's on path, program posture).** `runs/_rap_fws_wuacc6_f` re-run on the new exe
  (`wup_prog_f`): the same 69 restart events, 41 beyond the carry; the 28 inside it split into at
  least 23 responded (the 20 the gap drain also frees and the 3 bridged arcs, reached through the
  carry) and at most 5 with no arc; no in-horizon restart whose same-epoch gap row reads
  `unmeas_bridged`; closure OK. The walk-share persistence (`wsf_persist.py`) within 0.05 of the
  arm's 1.54 / 1.53 at 60 / 120 epochs (three more refloats in 6 h).
- **G2 (the golden pair).** Field golden plus accumulation and the response (`wup_field6`), sim
  golden plus the response (`wup_sim6`), against the G0 controls: the census line's restarts and
  their buckets reported on both; the field arm's IF-TD flags at most the control's (the staircase
  is gone; the program posture's arm read 111 to 70); the in-process consumers (WTZZ00DEU, SZZ000SIM)
  no hour worse than the control by more than 0.01 m.
- **G3 (the census, the multi-site rule).** The field arm's wire replayed at the 16 census sites and
  WTZZ00DEU with accumulating consumers (`wup`), against the control wire with the current golden's
  fractional consumers (`wupc`), and the same arm wire with fractional consumers (`wupf`, which
  separates the wire from the consumer); the sim arm's wire on the sim census (`wups` against
  `wupcs`). Statistic, frozen here: per site, the change of the mean hourly p50 over hours 2 to 6.
  Bar: the median change at most +0.005 m and at most 3 sites worse by more than 0.02 m, on both
  beds; the wire's code-bias records (`B,` lines) per epoch within 1 % of the control's.

**What follows.** G0 to G3 pass: section 4. G0 fails: the fix touched a shared path, found before
anything is armed. G1 fails: the fix does not reach what it should, back to the build. G2 or G3
fails: recorded, the knobs stay off, and the field-only staircase stays a known difference between
the beds.

## 4. The promotion (the PBF template), if section 3 passes

Both live bases (`config/refound/{field,sim}_gps_dual_rf0hv.json`) gain the knobs with a dated
comment; the manifest pins them on the frozen 2026-09-22 bases; ledger rows (RECOMMEND_SCOPED,
regime: the float vehicle with phase leveling and accumulation) and loader posture rows; the goldens
regenerated and verified (`make_golden_config.py`, `config_posture_check.py --verify-loader`); the
pair renamed the new baseline `runs/_rf0hv12_{field,sim}6`; CLAUDE.md's golden paragraph and the
wu93 amendment annotated. Wind-up mode stops being a bed key (`scripts/make_refound_config.py`) and
becomes a posture key.

## 5. The gates (2026-10-01)

Exe `gnss_processor_wup1.exe` (sha256[:16] `576ec0e7007d29a0`, main `6ad24c7`); every arm 6 h with the
read-only wind-up census; reads `scripts/clock_level/wup_grade.py`.

**G0 passes.** `runs/_rap_wup_ctl_{field,sim}6` (the goldens plus the census) are byte-identical to
`runs/_rf0hv11_{field,sim}6` in products, `closed_loop_trace.csv` and `network_epoch_status.csv` on
both beds.

**G1 passes.** `runs/_rap_wup_prog_f` (card FWS's arm on the fixed exe): the same 69 restarts, 41
beyond the carry horizon; of the 28 inside it, 27 answered through the leveling carry (52 arcs) and 1
with nothing to free; no in-horizon restart's arc reads `*_bridged` (3 did before the fix); closure
OK; persistence at 5 / 20 / 60 / 120 epochs 1.45 / 1.52 / 1.54 / 1.53 (the arm's 1.45 / 1.51 / 1.54 /
1.53). Four of the 27 are arcs the inject classifies as fresh (`clean_fresh`, `pl=0`): the response
uses the gap re-entry drain's carry predicate (an entry inside the horizon), so it latches them too,
redundantly.

**G2 passes.**

| | field control | field arm | sim control | sim arm |
| --- | --- | --- | --- | --- |
| wind-up mode | fractional | accumulated | accumulated | accumulated |
| one-cycle steps inside the horizon (the staircase) | 100 | 0 | 0 | 0 |
| restarts: beyond the carry / inside it | (28 transitions would restart) | 41 / 28 | 70 / 15, no response | 70 / 15 |
| inside the carry horizon: answered (arcs) / nothing to free / bridged | | 27 (52) / 1 / 0 | 0 / 0 / 15 unanswered | 14 (28) / 1 / 0 |
| IF-TD flags (refloats) | 111 (108) | 70 (67) | 0 | 0 |
| code-bias records per epoch | 94.11 | 94.11 | 62.74 | 62.74 |
| in-process consumer p50 by hour (m) | 0.560 0.059 0.028 0.030 0.039 0.048 | 0.559 0.059 0.029 0.033 0.038 0.041 | 0.601 0.104 0.096 0.056 0.055 0.049 | 0.601 0.104 0.096 0.055 0.055 0.049 |

The sim golden accumulated with no response: 15 restarts in 6 h folded the leveling carry across an
unanswered integer step (the census counts restarts that stepped by at least 0.9 cycle: 2 on the field arm, 1 on the sim). WTZZ00DEU
moves by at most +3 mm (hour 4, bar 1 cm) and 611 epochs stay below a decimetre.

**G3 passes**, and the field census moves the other way from card FWS's.

| census (per site, change of the mean hourly p50 over hours 2 to 6) | median | worse by > 0.02 m | better by > 0.02 m |
| --- | --- | --- | --- |
| field, arm wire, accumulating consumers (`wup`) against the golden (`wupc`) | -0.026 m | 0 | 9 of 17 (POHN -0.124, BADG -0.104, UTQI -0.091, WUH2 -0.063, KOKB, ASCG, ANMG, SCRZ, HARB) |
| field, arm wire, fractional consumers (`wupf`) against the golden | +0.000 m | 2 (ANMG +0.038, BADG +0.024) | 1 (UTQI -0.021) |
| sim, arm wire (`wups`) against the golden (`wupcs`) | 0.000 m | 0 | 0 |

By hour, the field census median goes 0.666 0.169 0.104 0.077 0.092 0.070 to 0.732 0.149 0.073
0.058 0.049 0.048 m: hours 2 to 6 better by 2 to 4 cm; hour 1 (ignition, outside the frozen
statistic) 6.6 cm worse in the cross-site median, a reshuffle (per family, C1W -0.001 and C1C +0.011
m). The arm's wire alone is neutral, as card FWS's fractional-consumer census found; the gain is the
consumers' own mode. A float consumer that wraps its own wind-up meets the same staircase the
producer did, and a producer and its consumers belong on the same mode (the wu93 sign rule's
corollary). Card FWS's "consumer-neutral" held only for fractional consumers.

## 6. Armed (2026-10-01)

By the promotion path of section 4 (the outside review's decision; every gate passed):
`estimation.accumulate_phase_windup: true` in `config/refound/field_gps_dual_rf0hv.json`,
`network.cycle_slip.windup_restart_refloat: true` in both live bases, each with a dated comment; both
pinned in `config/golden/golden_manifest.json` on the frozen 2026-09-22 bases (34 pins, no second
freeze); ledger rows RECOMMEND_SCOPED (appendix A14), loader posture rows after the slip-tracker row
(exe `gnss_processor_wup2.exe`, sha256[:16] `ef766aaedd7b2d55`); `make_golden_config.py` PASS (65
recommended knobs armed in regime on both goldens), `config_posture_check.py --verify-loader` 0 gaps.
The regenerated goldens differ from the gate arms only by the read-only census knob and the paths.
`scripts/make_refound_config.py` still lists the wind-up mode among its bed keys: it is the record of
how the RF-0 configs were generated from the two retired goldens and is left as it is; the manifest
pins make the mode a posture key.

Baseline pair `runs/_rf0hv12_{field,sim}6` (the regenerated goldens, `gnss_processor_wup2.exe`): products,
`closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical to the gate arms `runs/_rap_wup_{field,sim}6`
on both beds (the census knob is read-only, the posture rows log-only); WTZZ00DEU 0.559 0.059 0.029 0.033 0.038
0.041 m by hour, 611 of 720 epochs below a decimetre; SZZ000SIM within 1 mm of `runs/_rf0hv11_sim6` in every hour.
Census key note: `scripts/clock_level/census6h.py` still builds fractional-mode consumers; a census of the golden
consumer passes `estimation.accumulate_phase_windup=true` (key `wup`).
