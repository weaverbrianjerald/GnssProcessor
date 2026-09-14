# A3 — the wu93 night — the wind-up fix landed, the slip-response lane verdicts, and the golden capture

**STATUS**

- **id:** A3
- **topic:** config-ledger
- **status:** closed
- **summary:** the wu93 night — the wind-up fix landed, the slip-response lane verdicts, and the golden capture
- **dates in the text:** 2026-08-21 → 2026-09-09
- **knobs / posture:** `estimation.accumulate_phase_windup`
- **hashes named in the text** (commit / exe / byte-cert): `4f49fa2`
- **run dirs named in the text:** `runs/camp60wu_f1`, `runs/camp60wu_quiet12h_r2q3rfoff`
- **source:** `docs/archive/CONFIG_promotion_ledger.md` — A3

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: CONFIG_promotion_ledger.md -->
## A3 — the wu93 night (2026-08-21/22): wind-up fix landed; slip-response lane verdicts; golden capture

Session commits `4f49fa2..daadb73` (+ the A3 golden capture itself).  Scoped
promotions and pins — no loader default moved (standing rule: defaults are a
fingerprint-migration card).

| knob / posture | verdict | evidence |
|---|---|---|
| `cycle_slip.relevel_free_epoch` | **RECOMMEND_SCOPED: ARM on every sim posture, slipped AND quiet** (field arm still blocked on the withhold-era release policy). ⚠ Widened 2026-09-01 by the OFF-check: the win is NOT slip-confined — the latch guards EVERY arc re-latch against stale-carry injection (1608 marks vs only 159 MW detections on the ZERO-slip bed) | wu93 rerun `runs/camp60wu_f1`: walk dead (hourly ZWD 1.85–3.08 flat), 720/720, census 94.0 %; quiet twin ZWD rms 0.252 m / p50 2.5 cm. **OFF-check on the 12 h quiet flagship (`runs/camp60wu_quiet12h_r2q3rfoff`, single knob, 2026-09-01): OFF costs +29 % settled consumer (3D rms 0.204 → 0.263, p50 +30 %) and +17 % ZWD rms; detection layer identical both arms (159 MW / 16 refloats) — the delta is exactly the 1514 stale injections the ON arm's free-epoch latch SKIPPED (run-end: 1608 detector-marked latches, 1514 inject-site skips taken); clock channel + event census flat** |
| `cycle_slip.relevel_retap_statistical` | **KEEP_OFF — FALSIFIED for lanes** (ZWD-neutral, lane-harmful) | F3: settled GPS EWL at-own-integer 70.2 → **54.0 %** (SPEC_lane_slip_response §3) |
| `cycle_slip.relevel_retap_corroborate` | **KEEP_OFF — falsified AS ARMED** (raw MW/GF evidence lacks resolution; refuses 1.5 %) | F4C: 59.6 % (SPEC_lane_slip_response §5); knob stays built for a finer evidence source |
| `cycle_slip.relevel_delta_bridge` | KEEP_OFF (research) | 33 applies / 720 ep — negligible yield, unchanged verdict.  **Field number added 2026-09-09 — see §A5** (52 of 282 pends replaced over 12 h; consumer and ZWD sink flat-to-worse) |
| wind-up `nominal-wu93` frame | **CODE — landed**, product token + fingerprint + all-consumer interlocks | SPEC_phase_windup_sign §9.7–9.10; pre-fix fixtures/products/snapshots are expected-fail baselines |
| fixture discipline | **RULE: fixture and producer must sit on the same side of the wind-up fix** | §9.9: the mixed pairing is INVISIBLE to σ̂₀ (absorbed), enforce by provenance (`_wu93` fixtures) |
| `estimation.accumulate_phase_windup` | OPEN CARD (queued): loader now WARNS on fractional mode; do not flip the default without the migration card | SPEC §7 + §9.5; warn verified live on the golden63 chain |

Applied to the tree: `_simnet60_campaign_template.json` arms `relevel_free_epoch`
and PINS the three falsified/off knobs at their verdict values with the verdict
comment (the template's restate-the-default idiom).  Canonical champion arm =
`_camp60wu_f1.json` on the `_wu93` fixtures; graders unchanged.

