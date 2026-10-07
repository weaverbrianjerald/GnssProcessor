# GRB: golden re-base onto the RF-0hv float-vehicle posture (2026-09-22)

**STATUS**

- **id:** GRB
- **topic:** config-ledger
- **status:** closed
- **summary:** RE-BASED 2026-09-22: both goldens moved from the RF-0g leveling posture onto the RF-0hv float-vehicle posture; `make_golden_config.py --verify` PASS, 57 recommended knobs armed in regime (from 51).
- **source:** [RF-3-prep](RF-3-prep_golden_rebase_2026-09-15.md) section 4 (the previous re-base and its recipe); the author's plan review of 2026-09-22 (the goldens two orders of magnitude behind the working bases); the author's instruction of 2026-09-22 ("go ahead with the golden re-base"; a config worker, every number re-graded by the manager)
- **knobs / posture:** the goldens `config/golden/golden_{field,sim}_refound.json`, generated from `config/golden/golden_manifest.json`; the retired goldens untouched; bases frozen as `config/refound/{field,sim}_gps_dual_rf0hv_golden_2026-09-22.json`; the replaced posture RF-0g of 2026-09-15 (field consumer 3.77 to 1.08 m by hour, sim 6.23 to 0.46 m)
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** frozen bases sha256 `35aa24b3...` (field), `893be0d2...` (sim); confirmation pair `runs/_rf0hv9_{field,sim}6`; certification `runs/_rap_grb_field2`, `runs/_rap_grb_sim2`; exe `gnss_processor_grk4.exe` (`CEB14EAB...`); the owed ledger rows landed at `4dc8687`

**Outcome.** The goldens' validated consumer trailed the working bases by two orders of magnitude at hours 3 to 6, and the generator could not see any float-vehicle ledger row. The re-base pinned 27 knobs (16 carried, 11 added) with the same 7 exceptions, passed `--verify` on the first pass, and 2 h arms of each generated golden are byte-identical to the confirmation pair over the shared window. The ledger rows owed for four pinned knobs (the vehicle pair, `tracker_modeled_first`, `emit_pb_hold`) landed as appendix A10 of the [promotion ledger](../CONFIG_promotion_ledger.md). The goldens have moved on since (latest: card [EXL](EXL_exact_update_lah_promotion_2026-10-01.md), 2026-10-01).

## 1. Why

The Sept-14 decision froze the goldens on the leveling vehicle and RF-3-prep re-based them on
RF-0g on 2026-09-15, when the float vehicle failed on that posture. Since 2026-09-16 the working
bases `config/refound/{field,sim}_gps_dual_rf0hv.json` have carried the float vehicle with the
held basis and every knob the author armed since (cards GRS, CSL, EV7, UPO, SCC, GRK), and every
arm of the last week started from them, not from a golden. The goldens' validated consumer
(field 3.77 1.08 1.28 1.25 1.78 2.68 m) is two orders of magnitude behind the working bases at
hours 3 to 6, they carry the monument roster and 59 stations, and the generator's promotion
check (every RECOMMEND_ON or RECOMMEND_SCOPED ledger row in regime must be armed) could not see
any float-vehicle row because all of them carry the predicate
`val("network.ambiguity_vehicle", "level") == "float_kf"`.

## 2. What was done

1. The bases frozen: `config/refound/field_gps_dual_rf0hv_golden_2026-09-22.json` and
   `config/refound/sim_gps_dual_rf0hv_golden_2026-09-22.json`, byte-for-byte copies of the
   working bases at commit `97b5c78`; each equals the confirmation arm of the day
   (`runs/_rf0hv9_{field,sim}6/arm.json`) on all 628 (field) and 638 (sim) leaf keys except
   `network.product_output_path`. The live bases keep moving; the frozen copies are what the
   manifest points at (the `*_rf0g.json` precedent).
2. The manifest re-pointed: `base` for both goldens; `description` rewritten for the posture
   and the validated numbers; `pins` re-derived: the 16 pins of RF-0g all still hold and stay,
   and 11 are added from the base's own values (`network.ambiguity_vehicle: float_kf`,
   `network.phase_datum_held_basis`, `network.cycle_slip.gap_reentry_relevel`,
   `network.cycle_slip.gap_reentry_per_signal`, `network.cycle_slip.tracker_modeled_first`,
   `processing.canonical_signal_rule: priority`, `network.phase_datum_gate_drop_erase`,
   `network.station_marker_to_arp`, `network.second_code_channel`,
   `network.second_code_sigma_scale: 10`, `network.emit_pb_hold`); `exceptions` unchanged in
   membership (the seven of RF-3-prep), one text adjusted where it named the leveling vehicle
   (`network.carry_invariant`); a dated paragraph appended to the manifest's `_comment`; the
   `retired` block untouched.
3. Generated and verified: `python scripts/make_golden_config.py` (regenerate), `--verify`
   PASS on both goldens, `--selftest` PASS. Each generated golden equals its confirmation arm
   on every leaf key except the output path.

## 3. The promotion step, measured

| | RF-0g goldens (2026-09-15) | RF-0hv goldens (2026-09-22) |
| --- | --- | --- |
| recommended knobs armed in regime | 51 | **57** |
| unarmed in regime, documented exceptions | 7 | 7 (the same seven) |
| failures | 0 | 0 |
| loader posture (`config_posture_check.py --tier all`, and the run's own `Network POSTURE` line) | | ON tier 20 of 22, SCOPED tier 37 of 42, 7 in-regime gaps (the exceptions), 42 out of regime |

The brief expected the first verify to fail with float-vehicle rows the base does not arm. It did
not: the in-regime-and-unarmed set on the new base is exactly the seven carried exceptions, and
the six rows that entered the regime with the vehicle (`gap_reentry_relevel`,
`gap_reentry_per_signal`, `canonical_signal_rule`, `phase_datum_gate_drop_erase`,
`station_marker_to_arp`, `second_code_channel`) are all armed in the base. The generator now
enforces them on the goldens, which is what the re-base was for.

## 4. Certification (rapid mode: 2 h arms of the generated goldens, exe `gnss_processor_grk4.exe`)

| arm | exit / wall | 3D p50 by hour (m) | against the confirmation pair |
| --- | --- | --- | --- |
| `runs/_rap_grb_field2` (from `golden_field_refound.json`) | 0 / 275 s | 0.560 0.059 | the 241 lines of `closed_loop_trace.csv` identical to `runs/_rf0hv9_field6`; `products.txt` identical over the 240 shared epoch blocks (the reference's 241st `H` heartbeat excluded) |
| `runs/_rap_grb_sim2` (from `golden_sim_refound.json`) | 0 / 249 s | 0.598 0.103 | identical in the same way to `runs/_rf0hv9_sim6` |

The 6 h record of the posture is the confirmation pair itself: field 0.560 0.059 0.028 0.030
0.036 0.045 m (611 epochs below a decimetre of 720, all 720 emitted; clock wire 0.0045 /
0.0028 m), sim 0.598 0.103 0.096 0.056 0.055 0.049 m (448 of 720). The goldens ARE that
posture; only the output paths differ.

## 5. Owed

- Ledger rows for four pinned knobs that have no ledger entry: `network.ambiguity_vehicle`
  (with `network.phase_datum_held_basis`, one value row on the precedent of
  `processing.canonical_signal_rule`), `network.cycle_slip.tracker_modeled_first` (card GRK,
  armed 2026-09-22), `network.emit_pb_hold` (card PBH). RF-3-prep section 5 promised KEEP_OFF
  rows for the vehicle pair on the refound posture; they never landed and would now be wrong,
  since the posture arms both. Each row needs its loader posture row (`--verify-loader`
  parity), so they land together in the next build window.
- The generator stamps the box clock (`2026-09-21`) while the cards and file names carry
  2026-09-22: the project's card dates run a day ahead of the machine; not changed.
- The consumer-side settings of card PBH (`closed_loop.pb_step_translate: false`,
  `amb_level_check: false`) are in the base and not pinned: consumer settings, not posture.
