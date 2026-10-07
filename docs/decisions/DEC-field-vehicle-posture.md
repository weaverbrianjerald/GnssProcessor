# DEC-field-vehicle: the field vehicle posture decision and its closing legs

**STATUS**

- **id:** DEC-field-vehicle
- **topic:** harness
- **status:** superseded
- **summary:** c8b (leveling vehicle plus Q3 anchor at the floor) stays the candidate; the 6 h twin DRIFTS, G4 INCONCLUSIVE; frozen by the author 2026-09-14, since superseded by the float vehicle.
- **dates in the text:** 2026-09-14
- **knobs / posture:** `network.carry_zwd` with `zwd_carry_q: 1e-7` (c9), `network.carry_rx_code_bias: false` (c9b); the c8b posture = `golden_field_gps_dual` + the 16 vehicle keys of §24.11 + the 12 Q3 keys with `clock_anchor_q: 2.9e-8`
- **hashes named in the text** (commit / exe / byte-cert): `b3c2bc2`, `b023b06` (worker commits), `869a2d6` (merge), exe `4CD7D664`
- **run dirs named in the text:** none as `runs/` paths (arms c9, c9b, c10, pvb6, g4a, g4b; control `_cm_pvb`)
- **source:** `docs/archive/SPEC_event_harness.md` §24.17

**Outcome.** The question: which field posture to name, and whether it holds over 6 h and through the 20:47 event (G4). The ZWD-carry and rx-CB-off stack arms are neutral. Both 6 h arms DRIFT: station code rms rises about 0.25 m/h from hour 2 as gross per-LOS errors are admitted and grow (the worst c10 station reaches 91 m), so the wire is not yet usable. G4's mechanics pass but the event does not express on this bed, so the card's rule keeps the anchor OFF. The author's [re-founding decision](../refound/NETWORK_REFOUNDING_2026-09-14.md) froze vehicle and anchor together; [GRB](../refound/GRB_golden_rebase_2026-09-22.md) later moved the goldens to the float vehicle.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.17 The posture decision, and the closing legs (pre-registered before any is read)

**The field posture this campaign names** — every step measured against its control on the same 2 h cold bed: the field golden's
`carry_invariant` vehicle → the leveling vehicle with the sim's gauge projection and phase-datum family (PV-B: consumer 7.80 → 1.19 m, the
station block full-rank, ZWD physical at 48 of 61 stations) → plus the guarded temporal clock anchor at the physical floor (c8b: code rms
1.79 → 1.09 m, unphysical ZWDs 13 → 8, consumer 0.95 m).  The exact delta from `golden_field_gps_dual` is the 16 vehicle keys of §24.11 +
the 12 Q3 keys with `clock_anchor_q: 2.9e-8`.  **It is a posture change of the field vehicle** — goldens, harness seeds (v5–v9 are all
carry_invariant), the RS-8/9/10 seam mechanisms, the RAIM re-host, the lanes and every level-channel number re-base with it — **and
its arming is the author's call, prepared here, not taken**: the manifest / ledger rows are written on the author's go, through the
generator with `--verify`.  Before that go, three legs close what the standing rules require of any stiff time-link (invariant #1) and
of any posture claim (the 2 h level budget):
- **The stack (2 h, base c8b's config):** **c9** = + `network.carry_zwd: true`, `zwd_carry_q: 1e-7` (§24.12's b2b — physical, no longer
  diverging, the best consumer of that card); **c9b** = c9 + `network.carry_rx_code_bias: false` (b1 — D_phase 0.95 → 0.83 there).  Bars:
  §24.8's, vs c8b as the control (1.09 / 8 / 0.87 / consumer 0.954); a knob that moves the consumer's settled p50 by more than 0.15 m or
  any bar by 20 % in either direction is reported as such; the stack winner is the posture candidate.
- **The 6 h twin:** the stack winner (or c8b) and PV-B, 6 h cold from the same start, graded hour by hour on (i), (ii), (iii) and the
  consumer's per-hour p50 — the level channel's ~2 h budget means the 2 h numbers above are ignition; the twin says whether they hold,
  improve, or drift (SPEC_level_information_budget §14's trap).  Frozen: hours 3–6 not worse than hour 2 on any bar ⇒ the posture holds;
  a drift ⇒ its rate and the channel it lives in are the finding.
- **G4 — the DOY-129 20:47 event-window replay** (SPEC_clock_temporal_anchor §4, invariant #1's actual bar for the anchor): the stack
  winner's posture run SEAMLESSLY from 15:00 (the half-epoch back-off on `start_offset_hours`) through 21:30, and PV-B over the same window
  as the no-anchor control; PASS = the guard FIRES at the event with marks at the decoupling edge, a `cta_recouple` at the re-coupling edge,
  and the epochs EMITTING (adapt-and-emit, not withhold), with the wire and the consumer through the event not worse than the control's;
  "quiet-day cleanliness alone does not pass".  FAIL ⇒ the anchor stays OFF on the field and the posture card ships the vehicle alone.

**§24.17 RESULTS (2026-09-14 ~07:00 local; worker commits `b3c2bc2` / `b023b06`, merged 869a2d6; exe `4CD7D664…`; manager grade).**
- **The stack (2 h):** c9 (+ ZWD carry q 1e-7) (i) 1.06 / (ii) 8; c9b (+ rx-CB carry off) 1.08 / 9 — against c8b's 1.09 / 8: neutral on every
  bar and within 0.15 m on the consumer; **c8b stays the posture candidate** (the ZWD carry is physical and free of cost on this vehicle,
  but buys nothing on the bars — a posture choice, not a lever).
- **The 6 h twin (c10 = c8b's posture, pvb6 = PV-B; hour by hour, k windows of 120):**

| hour (UT) | c10 (i) p50 / p90 / max m | c10 (ii) | c10 consumer p50 | pvb6 (i) | pvb6 (ii) | pvb6 consumer p50 |
|---|---|---|---|---|---|---|
| 1 (00–01) | 0.87 / 1.65 / 3.4 | 17 | 10.0 (ignition) | 1.36 / 3.47 / 5.5 | 24 | 4.43 |
| 2 (01–02) | 1.09 / 2.12 / 3.5 | 8 | 1.94 | 1.95 / 3.28 / 7.1 | 13 | 2.48 |
| 3 (02–03) | 1.50 / 3.06 / **63.1** (DJIG) | 13 | 2.78 | 2.04 / 5.03 / 58.1 (BSHM) | 18 | 1.44 |
| 4 (03–04) | 1.91 / 3.84 / **85.0** | 13 | 1.41 | 2.67 / 7.00 / 63.2 | 23 | 2.28 |
| 5 (04–05) | 2.01 / 4.67 / **91.0** (BSHM 53.8; its ZWD **197 m**) | 10 | 2.04 | 2.90 / 7.02 / 89.6 | 19 | 2.53 |
| 6 (05–06) | 2.14 / 6.15 / 83.6 | 10 | **4.37** | 2.91 / 7.40 / **160.2** | 31 | 4.72 |

  **The frozen reading is the DRIFT branch on both arms**: (i) rises ≈ +0.25 m/h from hour 2 on both (the anchored arm better at every hour —
  2.14 vs 2.91 and 10 vs 31 stations at hour 6 — but not stable), and the channel is the GROSS PER-LOS TAIL: the station code-rms max goes
  3.5 → 63 → 85 → 91 m as DJIG/G06-class lines of sight (§24.15 R5) are admitted, never acted on, and grow, until a whole station is
  poisoned (BSHM00ISR at hour 5: code rms 54 m, ZWD 197 m; on pvb6 its max reaches 160 m).  The consumer hovers at 1.4–2.8 m float on both
  arms and jumps to 4.4–4.7 m in hour 6 on both (a shared wire event, not the posture).  ⚠ Exe note: the inherited `_cm_pvb` control's (i)
  p50 1.79 was on the pre-ZP exe; the same posture on the post-ZP exe reads 1.95 (the fallback files ~25 more code rows per epoch; p90 / max
  / (ii) / (iii) identical) — every "vs PV-B" ratio in §24.14–§24.17 is conservative by that margin.
- **G4 (the 20:47 replay, seamless from 15:00 UT, `g4a` = c8b's posture vs `g4b` = PV-B): INCONCLUSIVE on this bed, not a mechanism
  failure.**  All four mechanical conditions hold at the event window — 13 `cta_decouple` (a 10-satellite `nis` trip at tow 334380, |w| to
  45), every trip a wire mark (348 = 348 run-wide), every one `cta_recouple … in_band=4` exactly four epochs later, 41 / 41 epochs
  emitted — and the wire through the window is 5× better than the control's (D_phase 0.168 vs 0.890 m, D_code 0.030 vs 0.145).  But the
  guard fires on 7.3 % of all epochs (57 of 780; the event cluster ranks 14th of 57), and the 20:47 excursion does not express on this
  61-station leveling bed at all (window `clock_std` p50 4.07 / 3.85 m against whole-window maxima 4.38 / 4.52; the best window epoch ranks
  143rd / 107th of 780 on both arms) — the documented event was a G01 pivot code-bias ratchet on a 305-station `carry_invariant` lineage.
  The consumer through the window is worse anchored (p50 4.26 vs 2.60 m; last 30 min 3.29 vs 2.05) with a 3.7× smaller largest step
  (0.115 vs 0.431 m) — the anchor buys smoothness and costs level there.  Per §24.17's rule a non-PASS keeps the anchor OFF on the field
  golden; **the posture card, if armed, ships the vehicle alone until G4 has a bed on which the event expresses** (a 305-station or
  `carry_invariant`-class replay, or a different known event on this bed).
**Where this leaves the field wire:** the vehicle + the clock anchor at the floor is a large, measured improvement at every hour, and it
is NOT a usable wire yet, for three named reasons in order of evidence: (1) **gross per-LOS errors are admitted at birth and no screen
acts** — field-blocking, it poisons stations over hours (§24.18); (2) the wire's within-arc random walk in the phase-bias / level channel
(0.3–0.9 m per hour, R7) — unopened; (3) a bulk code-residual drift of ~+0.25 m/h whose channel beyond the tail is unmeasured.

