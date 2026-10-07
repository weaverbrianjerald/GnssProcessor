# S2H: the S-consistent receiver code-bias harvest

**STATUS**

- **id:** S2H
- **topic:** code-bias
- **status:** closed
- **summary:** S-consistent rx-CB harvest VALIDATED as frame hygiene through 3 h; the 12 h level-tracking bar (within 4 TECU at h0 and h11) MISSED by both arms, so level authority moved to LV-6; default OFF.
- **dates in the text:** 2026-08-07 to 2026-08-21
- **knobs / posture:** `network.rx_cb_harvest_demean` (+ `rx_cb_harvest_screen_z`, default 8.0); default OFF, fingerprinted (`rxcbharv:`), forces LV-2 and LV-5 OFF
- **hashes named in the text** (commit / exe / byte-cert): `gnss_processor_s2h.exe` md5 `82B4450703C551109DCEFB0B6C212559` (the §28 build, superseded for the 12 h pair); exe MD5 `3979C756` (truncated in the text; 2026-08-21, both 12 h arms)
- **run dirs named in the text:** `runs/s2h_12h_{ctl,on}`, `runs/s2h_*`
- **source:** `docs/archive/SPEC_level_information_budget.md` §28, §31

**Outcome.** The question: does a per-epoch Λ-weighted de-mean of the harvested receiver code biases carry the frame at no fold-side cost and land the ionosphere level over 12 h. All 1 h and 3 h gates PASS. At 12 h the fold ceilings hold but the σ̂₀-compounding sub-gate FAILS (growth 2.255 vs the control's 1.831): cheap, not free. The ON arm read |a00 - GIM| of 13.6 and 9.6 TECU at h0 and h11; the level authority moved to LV-6 ([SPEC_level_authority_internal.md](../../SPEC_level_authority_internal.md), [QPR-ITEMS-5-8](../queues/QPR-ITEMS-5-8.md)). The harvest stayed default OFF as Edge-3 frame hygiene; since card [EXL](../../refound/EXL_exact_update_lah_promotion_2026-10-01.md) the goldens arm `carry_rx_code_bias_in_block`, whose loader interlock refuses it.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 28. Item 3 BUILT and validated through 3 h — card S2H, the S-consistent harvest (2026-08-07, overnight session)

Knob `network.rx_cb_harvest_demean` (+ `rx_cb_harvest_screen_z`, default 8.0); default OFF, fingerprinted
(`rxcbharv:`), **forces the superseded LV-2/LV-5 OFF**. Mechanism (§17 item 3's brief, implemented exactly):
at the serial post-solve harvest, AFTER the LV-4 erase pass and the gap prune (member set = the fold-eligible
set), per family, subtract the Λ-weighted mean of the harvested ABSOLUTE realizations from every carried entry
(λ = 1/var), with the **exact diagonal covariance co-transform (voters var − 1/Σλ, screened var + 1/Σλ =
diag(S·Q·Sᵀ), unit-tested against an Eigen brute force)** and the §16/12.1 robust peer screen on the μ vote.
Thin epochs apply NOTHING (identity, counted) — never a frozen μ.

**Two deliberate deviations from the LASER original, both load-bearing:** the de-mean is about the **DATUM
ROW's absolute-zero target** (de-meaning about `x_lin` is §13.2's measured failure), and it is **Λ-weighted**
(only the weighted mean zeroes the NET common-mode force; λ ≡ const reduces to the `GPS3710_test_PMINOLESS`
oracle case, which passes in-suite against the golden CSVs). Unit suite green at build time (case count is a
dated snapshot — §9).

Validation (binary `gnss_processor_s2h.exe` md5 `82B4450703C551109DCEFB0B6C212559`; control = the arm-32
posture re-run on the same binary, reproducing arm 32's hourly GIM diffs EXACTLY; full record
`CONFIG_promotion_ledger.md §A2` F6–F9):

- **OFF path**: products byte-identical across the rebuild (16 286/16 287 lines; the single diff is the `#`
  legend line documenting the new `hsig=` token).
- **20 ep smoke**: applied 20/20, n = 61, 0 thin/screened; μ smooth, no alternation.
- **1 h stability**: ALL FIVE gates PASS (a₀₀ sd 2.92; time-matched hour-1 fold fraction 0.425 vs 0.356 = +19
  %; prires rxcb 0.90× QUIETER; rms_phase +2.5 %; μ → +0.0000 settled — the projector's fixed point).
- **3 h**: ALL gates PASS — h2–3 rms_phase **9.60 vs 9.40 mm (+2.1 %**, flat with era); prires rxcb 4.43→4.95
  vs 4.91→5.55 (**0.89–0.90× QUIETER**, growth ratio = the control's, where LV-4 DOUBLED); folds **0.523 vs
  0.487**; step census statistically identical (**60 vs 59 key-steps, clock 0/0 both**) — the removed common
  mode is datum-null, invisible on the wire.

**The §24.5 vehicle question is answered on the fold side: a per-epoch post-solve transform carries the frame
with ZERO fold-side tax** (priors quieter, folds more frequent, data side flat — the over-retention signature
absent by construction AND by measurement).

Level, **DIAGNOSTIC** (invariant #5): time-matched hourly a₀₀−GIM −13.58 → −11.36 → −4.35 ± 1.75 (ON) vs -7.23
→ -5.52 → +0.44 (control); footprint mean −13.26 vs -2.76. With the carry's common-mode memory zeroed each
epoch the level sits at the per-epoch data+datum equilibrium — LOWER, toward the zero-mean convention,
converging upward at h2–3, with NO carry-off-class collapse (a stable ±1.75 band vs that mode's -9
TECU/EPOCH). DEFECT 2's convention offset is now a DECLARED frame property drained per epoch.

**OPEN, deliberately deferred: the 12 h tracking verdict** (|a₀₀ − GIM(t)| ≲ 4 at h0 AND h11) — whether the
a₀₀ anchor's authority (no longer outvoted by the carry, still outvoted by per-epoch GF data information)
lands the level, or whether §13.4 option 1 / item 0b (external receiver DCBs) must supply the data-derived
authority. Configs `_ext63_gps_dual_{20ep,1h,3h}_s2h_*.json` carry the pre-registered predictions; runs
`s2h_*` on disk. **NOT armed by default** (no-promotion rule).

---

## §31 The S2H 12 h tracking verdict (2026-08-21) — §28's deferred questions ANSWERED; the level authority question resolves onto LV-6

Run as staged (`_ext63_gps_dual_12h_s2h_{ctl,on}.json`, fresh seamless
lineages, one knob), on the session-certified exe MD5 3979C756… (the staged
comments name the Aug-8 session binary 82B44507 — superseded; BOTH arms share
today's build, so the same-build pair discipline holds).  1443 synced / 1440
emitted / 0 withholds, both arms.  Runs `runs/s2h_12h_{ctl,on}`.

- **P1 (fold-side gates at 12 h): ceilings PASS, expectation MISSED.**
  h2–12 postfit phase rms ratio **1.0010** (±5 % gate).  prires rxcb σ̂₀
  ratios **1.064** (h0–6) / **1.311** (h6–12) — inside the ≤1.5× gate, but
  the 3 h read's "~0.9× quieter priors" did NOT persist, and the
  σ̂₀-compounding sub-gate FAILS: ON growth h6-12/h0-6 = **2.255** vs the
  control's own **1.831**.  The S-consistent harvest is fold-side CHEAP but
  not fold-side free at 12 h.
- **P2: PASS.**  μ last +0.0000 m (mean |μ| 0.0040, max 0.4685), 0
  thin-skips, 693 peer-screen exclusions; wire step census identical (7/7
  disc marks); time-matched folds 0.5765 vs 0.5287 (ON ≥ ctl).
- **P3 — THE VERDICT: the second pre-registered branch.**  ON reads
  |a00 − GIM(t)| = **13.6 TECU at h0** and **9.6 at h11** (ctl 7.2 / 12.0)
  — neither arm within ±4 at both ends; both wander a −5..−20 band with the
  h6–7 dip to −19/−20 against the 08 UT GIM peak.  Per §28's own
  pre-registration: *"below −4 settled ⇒ the a00 anchor's 0.25 is still
  outvoted by per-epoch GF information and item 0b / a per-epoch-valued a00
  anchor is the designed next authority — that is a FINDING either way."*
  So: **the level card does NOT close on S2H**; S2H stands as the Edge-3
  frame hygiene it validated as (zero-mean by construction, P2's drains),
  and the LEVEL authority must be a per-epoch data-derived a00-direction
  value — which is precisely LV-6
  (`SPEC_level_authority_internal.md`), whose census stage was built the
  same session this verdict landed.  The convergence is independent: the
  staged experiment's outcome selects the card already in flight.
- **P4: footprint mean −17.00 / rms 18.58 vs global −12.21 / 13.35 —
  ≈1.4×, the §18 direction** (marginally under the 1.5–2× band).

Disposition: S2H keeps its standing (default-OFF, fingerprinted, Edge-3
validated; the 12 h read adds the compounding caveat to any future
promotion case).  The level-authority thread moves to LV-6's gates
(§4 there): census tracking first, fold second, with the fold's natural
EXT-route being L_cs as the per-epoch VALUE of the existing a00 anchor —
item 0b's design, internally sourced.
