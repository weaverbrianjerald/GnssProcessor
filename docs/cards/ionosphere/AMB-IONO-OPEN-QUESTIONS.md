# AMB-IONO open: the ambiguity/iono open-question register, items 1 to 44

**STATUS**

- **id:** AMB-IONO open
- **topic:** ionosphere
- **status:** closed
- **summary:** Register of the split over-count work (FIRST and items 1 to 44), reviewed on the float-vehicle posture: 13 done, 10 falsified, 12 superseded, 7 moot, 2 duplicates; one owed line (arms setting unread config keys) re-filed to OPEN_ITEMS.
- **dates in the text:** 2026-09-04 → 2026-09-10
- **knobs / posture:** `network.readmit_seed_horizon_s`, `network.readmit_coupling_marg_ep`, `network.cycle_slip.if_phase_td_ref_gate`, `network.phase_datum_handover_max_var_ratio`, `network.phase_datum_handover_max_var_m2`, `network.raim_arc_persist_epochs`, `network.cycle_slip.gap_reentry_relevel`, `network.carry_zwd_in_block`, `network.carry_zwd`, `network.phase_leveling_local_block` … (+14 more in the text)
- **hashes named in the text** (commit / exe / byte-cert): `18aa2854b2575658`, `9b59a2c`, `7311ad0`, `dd6c81c`, `4bb799a`, `966d4fd`, `5f1f422`, `95afa6d9` … (+3 more in the text)
- **run dirs named in the text:** `runs/ratio3h`, `runs/fldf3_pl2_bridge`, `runs/fldf3_on2_ctl_sb`, `runs/fldf3_on2`, `runs/golden_sim_tlink_off`, `runs/golden_sim_tri_w6_full`, `runs/golden_sim_campaign_plq7`, `runs/golden_sim_campaign_zwdblk` … (+2 more in the text)
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md` §7

**Outcome.** The register held 45 items (FIRST, 1 to 44) of the split over-count work, mostly about the leveling inject, the diagonal carry and slipped-bed probes that the [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) retired. Classified on 2026-10-13: 13 done (seed horizon, Card R, receiver code bias in block, clock disc, F3 field arm), 10 falsified, 12 superseded (float vehicle, held basis, gap re-entry, [ARL](../../refound/ARL_integer_ambiguity_ladder_2026-10-09.md)), 7 moot, 2 duplicates. One item stays owed (arms setting keys the loader never reads), re-filed to [OPEN_ITEMS](../../status/OPEN_ITEMS.md). Status reviewed 2026-10-13.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §7 Open questions (re-cut 2026-09-04, round 2; round-4 closures and items 15–21, §6.8)

**⚠ FIRST (2026-09-05, §6.9): the RE-COUPLING EDGE on the F3 block at satellite re-admission — BUILT the same day as `network.readmit_seed_horizon_s` + `network.readmit_coupling_marg_ep` (§6.10, pre-registered; arms running)** — the 24 h arm shows two ~4 h NETWORK-WIDE degradation episodes after the eclipse re-admission clusters (clean chords 0.19 → 0.58–0.68 cyc), absent from the quiet hours where F3 sits at the CRLB.  Build: at a satellite's re-admission (col_added after a gap ≥ N epochs / CTA first sighting), marginalize its shared terms out of every station's block regressors for K epochs (or register the re-admission as a Card-1g reinit source); counters at both edges; pre-registered PASS = no clean-chord degradation after the h16.6–18.9 cluster on `_nonint_losblock24h_hv`, settled quarter ≤ 0.2 cyc / ≥ 80 %.

1. ~~**The birth value.**~~ **CLOSED (§2.7)** — age 0 IS the free single-epoch absolute-code
   solve at the birth elevation (med|r0| tracks 0.6745·σ_P/(sin el·√Σμ²) within 15 % in every
   elevation bin; corr with the absolute-code proxy 0.62); the round-1 1.49 m / corr 0.58 was a
   tail-dominated 6-station rms.  Residual sub-item: ~30–40 % of the age-0 level not explained
   by the LOS's own 2-band proxy — station-common per-epoch rx-DCB code noise vs proxy
   attenuation (a 3-band proxy needs the L1C/L5Q pair rows, absent from `network_slip_raw.csv`).
2. ~~**Birth-spread dependence.**~~ **CLOSED (§3.1a; criterion measured §6.8.1)** — birth-frame
   inheritance (station-term slope 1.18); the DD-level-vs-birth-spread criterion reads flat under
   F1 (0.299 / 0.269 / 0.214 m) and F3 (0.070 / 0.047 / 0.052 m, slope −0.16 / −0.02 / −0.03) — PASS.
3. **L7Q** — the median OFFSET is CLOSED (§6.8): +1.01 (base) → −0.115 (F1) → **+0.017 / +0.009**
   (F3 / F3+hv), every band's settled med(e) +0.004 … +0.017 — it was the station-common frozen
   term.  What survives is a satellite-arc-mean-FRAME term in L7Q's iono part (M_t slope
   +2.7 … +6.4, R² 0.46–0.58 on 454 mid-run rows) invisible in its CRLB ratio (0.92); candidate
   the Galileo E5b satellite code bias (wire E:C7Q residual 0.53 m rms on every F arm).  Unquantified.
4. **RAIM pool admission of block rows** (`NetworkRaim.cpp:236` admits any row with |A_r| > 0.5,
   no `cpl_row_start` exclusion).  Round 3: T4 erases 288 → 0 under F1 (the coupling row's own
   standardized residual shrinks 1/√B_eff).  Round 4 (§6.8.4): under F3 the POST-FIT pool census
   is unchanged (n_pf_rows identical on all five arms) but the RAIM-attrib pool σ̂0 goes
   0.233 → 0.991 with |effect| p50 ARC 0.454 → 1.463 (max 4.07 < k = 6, 0 acted) and one ARC row
   at prominence 4.0e9 (a near-zero denominator) — the dense L⁻¹ rows sit in the T3/T4 pool at
   unit variance, admitted arbitrarily by the 0.5 test.  Still owed: tag block rows prior-class
   and exclude them (or grade the prior residual as its own tier), a per-column pool census so
   admission is READ not inferred, and the 4e9 denominator fix.
   **⚠ AMENDED 2026-09-09 (§6.16.4): the block rows are NOT the T4 driver.** E2 (F3 off, single
   knob, same bed) has T4 one-solve arc refloats **35 010 vs E0's 22 968** — turning the block rows
   OFF makes the T4 storm 1.5× WORSE, so the "F3 creates T4 work" reading is falsified and the
   admission item is now about pool HYGIENE (σ̂₀ / denominator / readability), not about the erase
   rate.  The erase rate's own agent is E1's persistence gate (item 22).
5. ~~**IF-TD reference gate: 1917 false refloats/day**~~ **CLOSED (§6.14, 2026-09-08)** —
   100 % false (the bed injects 0 slips), and the statistic is the reference satellite's own
   phase noise: σ 17.7 mm, FLAT in family size, 88 % station-local, against a fixed
   0.5·λ_IF = 53.48 mm bar = **3.02 σ**.  A fixture-realism artifact — the sim is 5×
   noisier than a real 300-station trace at matched elevation, where the same bar is ≈15 σ
   (hence 14 field flags vs 2482/day here).  The owed 2 h arm ran: Card R
   (`network.cycle_slip.if_phase_td_ref_gate`, armed in BOTH goldens) takes acting refloats
   **546 → 0** and the steady-state rate **0.504 → 0.000/epoch**, MW and the pool arm
   untouched, σ̂₀ unchanged.  The `nonint_*` ladder simply ran it OFF.  Of the two items filed
   in §6.14.5, **both are now FIXED (§6.14.6, 2026-09-08)** — the missing flag-rate tier (now
   `T1r`: 28 680 opp / 118 flags where T1 read 0, cross-checked against 118 `refsat_reset`
   events) and the reference satellite's structural zero deflating the pool scale (s_eff p50
   +15.3 %, solve byte-identical).  What stays OPEN is the physics: the pool floor still sits ON
   the (1,1) boundary, so the one MW-blind slip class is unreachable — and the sim bench cannot
   test a reference-satellite slip at all (zero elevation overlap).
6. **Run-level warmup** (`carry_warm = n_emitted ≥ 6`, `ApplicationNetwork.cpp:1450-1452`) —
   moot under F1 as predicted; the ignition cohort remains its own regime (item 8).
7. **What bounds the formal σ at 0.009–0.013 cyc?**  Round 2: the collapse has no fixed point
   for B ≥ 2; the 24 h value needs an additive ~1.5e-6 m²/epoch, i.e. the Card-1d
   `tu_var = M²·κ·q·Δt` with P_prev ~1e-6 m² (inferred — `cov_df(j,j)` is not traced).  A
   one-station dump of (M_clk, κ, tu_var) per row would pin it.
8. **Ignition cohort — now the LARGEST F3 residue** (§6.8.2).  Under F3 the per-chord clean
   first-inject reads 0.715 / 0.731 / 0.761 by elevation (the station-common share collapses at
   the honest rate) — but legs born at k = 0 with NO logged event (no cold-handover hit, no
   T1/T0 refloat) sit at 1.25–1.34× the population CRLB (0.843 F3 / 0.904 hv, n = 2085 / 5936)
   where mid-run-born legs sit AT it (0.72 / 0.64), and the T1/T0 history moves nothing (± 0.02).
   Candidates: the k ≤ 6 warmup solves before the first block exists (blocks start at k = 7),
   the k = 7 first-inject where rx-CB / clock / ZWD collapsed before any block, a frozen
   ignition-era level in the still-diagonal rx-CB carry.  Recipe: per-leg r(t) vs CRLB along the
   arc for k = 0 vs mid-run legs (`r4resid/arms/*.npz` carry everything needed) — fixed at k ≤ 7
   ⇒ a warmup / first-inject card; accruing ⇒ a carry-path card.  Never quote an ignition-epoch
   ratio as a B test.
9. **Gap re-births** (one effective prior row at the first re-inject, ratio 0.72–0.75, 429 arcs)
   — DEPRIORITIZED: under F3 rows with a re-birth leg sit BELOW the population bound (0.78 /
   0.66), so the edge is not measurable above the CRLB on this window.  The harvest / prune
   ordering read (`NetworkRunContext.cpp:3476-3486`, `NetworkSlipQc` post-gap) stays a code question.
10. ~~**F1's L1C first-inject 0.588 vs 0.707**~~ **CLOSED (§6.8.1)** — F3 reads 0.720 / 0.722 /
    0.728 / 0.722 on L1C / L2W / L5Q / L7Q (predicted ≈ 0.707): the share IS station-local.
11. **vf005 < vf001 in the iono part (1.48 vs 1.81 cyc)** — non-monotone in v_f, contrary to
    any smoother model; not pursued (the knob is falsified as a fix), recorded so nobody
    re-derives the floor.
12. ~~**Do `carry_zwd` and the rx-CB carry share the defect**~~ **RE-CUT into item 17.**  The
    ZWD is not carried on this posture (carried flag 0 / 21,600 rows), so F3's marginal ambiguity
    block is exact for it — measured honest (|err|/σ 0.94, §6.8.4); the rx-CB carry IS separate
    and diagonal — item 17.
13. **SPEC_synthetic §13.7's settled float 8–9.4 cyc (WORSE than single-epoch, K_eff < 1)** vs
    today's 2.7–3.8: did that arm run without `carry_coupling_time_update` (the SPEC_card1 §3
    poisoned-row signature)?  Unverified.
14. ~~**Wire grade of F1**~~ **CLOSED (§6.8.4)** — F1 and F3 wire + consumer grades landed on all
    five arms; the ledger rows carry the verdicts.
15. **The handover ceiling must be re-expressed on the honest-σ scale before F1 or F3 is armed
    on ANY lineage** (§6.8.3).  **BUILT 2026-09-05 as `network.phase_datum_handover_max_var_ratio` (§6.10, `runs/ratio3h` pre-registered).**  `network.phase_datum_handover_max_var_m2` = 1e-4 m² was sized to
    the over-counted σ: under any honest-σ carry 0/93 successors are continuous, 7 settled 4–15 m
    PB frame steps land on the wire, 1653 leveling carries are erased.  **Measured at 1e-2 m²:**
    F3 70/93 (all 23 refusals are arcs ≤ 52 min old in h0/h1; settled window 21/21 continuous,
    0 settled steps > 1 m, 408 erased), F1 67/93 (per-BAND refusals — L5Q 12 / L2W 7 / L7Q 4 /
    L1C 3 on arcs up to 3 h old, 9 mixed families, 4 settled > 1 m steps, consumer +0.43 m up
    bias — an INVALID pairing); neither reaches ≥ 90/93.  Candidates: a relative form
    (var ≤ k²·var_at_first_inject) or an age gate; either must keep the 378-σ unvalidated-target
    guard (SPEC_pivot_visibility_gauge_defect §5.0) demonstrably armed (inject an unvalidated
    target on the sim bed).  Prerequisite: trace the successor's conditional var and age per
    handover — `network_events.csv` carries the target only.  Interim: 1e-2 is the WORKING value
    for F3 arms, not a promotion.
16. **F3 telemetry gaps** (§6.8.5): no per-epoch breakdown of the marginalized-at-inject columns
    by reason (departed / skipped / freed / pinned — 8,534 lumped in the run-end line); the
    within-epoch RAIM refloat re-whiten path never exercised (T2 354 opportunities / 0 flags,
    uncertified on data); add both to `network_epoch_status.csv` before the field arm.
17. **rx-CB direction 2× over-count** (r3_5 design flag, now the named residue): the F3 block
    covers the ambiguity columns only while the rx-CB keeps its own diagonal carry, so the DCB
    direction is asserted twice (2×, not 25×).  DD-invisible above the bound at 3 h (mid-run rows
    AT the CRLB) and hinted only by the rx-DCB formal 0.089 (F1) → 0.067 (F3) = base's 0.066 m;
    the DCB truth join is definition-limited (0.85 m rms on every arm, corr 0.30–0.36 GPS, ~0
    Galileo) — a state-definition-aware grade is needed before calling it inert.  Fix = block ALL
    carried local columns of the station.
18. **F3's satellite clock lags F1** (settled rms 1.583 vs 1.437 m, p50 1.011 vs 0.832; G16 4.23 vs
    3.49, G09 2.78 vs 2.10, G18 1.88 vs 1.35) with a base-like URA (0.005 m, vf 1.1e5; F1 0.017 m)
    while every station-common channel is equal or better.  HYPOTHESIS: the exact stiff block
    re-asserts the carried ambiguities' retired-frame memory through the coupling rows that F1's
    blunt 3× loosening released.  Test: a3 / a5 + a per-satellite clock re-realization census keyed
    on the eclipse-readmission seam, on `nonint_losblock3h_hv`.
19. **Consumer east walk from the G12 re-emission epoch** (~267930): blk_hv E −0.15 → −0.32 m over
    the last 40 min with ZERO settled cold handovers, wire flat, n_sats constant — the round-3
    cold-handover explanation is FALSIFIED, the G12 half untested.  Test: replay from 267330 with and
    without G12, or a ≥ 6 h arm; grade the WIRE first.  Also open: los3h_hv's +0.43 m up bias (F1 +
    1e-2 only; the same families are cold on los3h with no bias); the consumer emits no reset /
    discontinuity counters on these arms.
20. **24 h F3 + hv arm owed** — pre-registered §6.8.6 (settled L1C 0.14–0.15 cyc / 84–86 %, WL
    ≥ 99 %, EWL 100 %, ZWD |err|/σ ~1; falsifiers > 0.20 cyc or < 80 %, |e| rising with age
    beyond 360, ZWD |err|/σ > 1.5); grade with `simnet_amb_integer.py` + `r4resid/crlb_pop.py` on
    the last 25 %, where 99.5 % of rows are mid-run-born.
21. **Field arm owed (FLD63 / FT63 lineage) with the same instruments** — F3 is sim-validated only.
    No truth there: grade on DD-fractional circR / med|f|, ZWD vs an independent ZTD, the PB-gauge
    census + seam-tools steps, the prires / block-Baarda / congruence σ̂0 families and
    `trace_prefit_census` (arm them on the next `_nonint` arms too — UNAVAILABLE on all five), and a
    consumer replay.  Fresh convergence (fingerprinted), wu93 side of the wind-up fix, item 15 first.
22. **BOUNDED T4 IDENTIFICATION — the dominant agent of the metre-class ZWD plateau (opened
    2026-09-09, §6.16.4).**  Starving the T4 isolated-arc erase (`network.raim_arc_persist_epochs:
    1000000`, E1) halves the ZWD truth rms (2.098 → 0.933 m), takes sat-clock contrast −66 % and
    code-bias −52 %, and flattens the D3 arc-age slope — but costs the consumer 46 % (settled p50
    1.751 → 2.559 m), a third of F3's integer gain (ref-free WL 81.5 → 65.4 %) and leaves the
    post-fit pool un-screened (σ̂₀(phase) 1.42 → 2.69, T3c 0.816).  **The knob is a PROBE, not a
    fix — ledger UNVALIDATED, do not arm.**  The build is Tier-C3 solution-separation confirmation
    of the median-polish identification, i.e. a BOUNDED mass-refloat: erase only what a separability
    test confirms.  Prerequisite named by `project_network_ssystem_dia_assessment`: median polish has
    no separability analysis today.
    **⚠ SUPERSEDED 2026-09-10 by the truth-graded T4 census (§6.17b) — bounded identification is the
    WRONG LEVER.**  This item assumed T4 erases healthy arcs and that a separability test would stop
    it.  Calibrated against same-epoch quiet arcs (the bed's ambient per-band DD error is **1.699 cyc**,
    so an absolute 0.5-cyc cut calls 83 % of untouched arcs "OFF"), the median erased arc is **2.59×**
    the ambient — T4 mostly confirms genuinely-off arcs.  Only **29 %** are indistinguishable from
    healthy, and the arc was already at that level 10 min earlier (grew in 44.5 % of cases — a coin
    flip).  What T4 is actually chasing is **its own previous re-level**: **48.4 %** of events fire
    within 15 min of one on the same (station, signal), the T4 → T4 chain runs 8.345 → **13.197** →
    14.082 cyc with the first erase making it WORSE in **64.4 %** of pairs, and a fixed-cohort profile
    goes k=0 3.031 → k=1 **4.192** → k=40 2.059 against a 1.653 floor it never reaches.  The disease
    is the re-level VALUE inheriting the station's corrupted per-epoch states — a station-wide
    positive-feedback loop, not an identification error.  Superseded by items 29 (the level-preserving
    ceiling, measured) and 33 (the split freeze).  ⚠ The knob itself stays a PROBE — ledger
    UNVALIDATED, do not arm; item 29's arms make that sharper, not softer.
23. **A LEVEL-PRESERVING gap re-entry response is unbuilt, and the two obvious candidates are spent
    (2026-09-09, `docs/SPEC_slip_qc_hardening.md` §12).**  The bare refloat
    (`network.cycle_slip.gap_reentry_relevel`) closes the defect — 88 → 0 bridged arcs, 129 freed,
    DD arc-wander tail 47 → 38 %, T4a −46 % — and still loses: float consumer +56 % settled rms,
    WROC00POL settled ZWD span 3.6 → 15.6 m, because each freed arc re-levels from ONE epoch of code
    (invariant #1's re-coupling edge, 129 re-seeds / 12 h).  Bridge v2 + v3 as built for detector
    slips (`relevel_delta_bridge` + `relevel_bridge_gf`, `runs/fldf3_pl2_bridge`) are FLAT on the
    same posture (52/282 pends replaced, 49 GF applies; consumer rms −1.5 % / p50 +17 %, ZWD span
    p50 0.797 → 0.904), so extending them to re-entries is not justified.  Design constraint: carry
    the level across the break at PHASE precision or do not act.  ⚠ The MW/LLI drain still has the
    identical live-state hole (`SPEC_slip_qc_hardening` §11 item 4) — a detector assertion on a
    not-yet-recreated ambiguity is silently inert there too.
24. **The ZWD-in-block card — LANDED and SCOPED-PROMOTED 2026-09-10 (§6.16.7d, four arms).**  The per-epoch ZWD has NO temporal prior
    and `carry_zwd` is blocked by item 17's block-coverage gap, so every erase / re-level that makes
    a young arc leaks into the wet delay: D3 interaction rms 4.264 m at arc age 0–60 s → 1.974 m at
    1200 s+ (E0), and the leak vanishes when the erases do (E1 flat 3.114 → 2.932).  The card puts the
    station's ZWD column INSIDE F3's marginal block, so the wet-delay direction is not left to absorb
    what a freshly re-levelled arc cannot explain (item 17's "fix = block ALL carried local columns
    of the station" is the same clause, one column wider).  Independent of items 22 and 23; grade it on the
    settled ZWD span (§6.16.1), not on the consumer.  **Outcome:** four arms (quiet sim Q, slipped sim Z,
    field-diagonal F, field-in-block F2).  The vehicle distinction is the whole card — the DIAGONAL
    row is the falsified double count (quiet sim consumer 0.723 m vs in-block 0.0877 m; field
    diagonal T3c 7 619 flags vs in-block 849 on the same data), so the in-block variant is not an
    optimisation of `carry_zwd`, it is the only correct way to carry it under F3.  Graded on its own
    channel as item 24 asked: fleet settled span p50 1.195 → 0.143 m on the field (F2), 1.590 →
    0.207 m on the slipped sim (Z), max 491.5 → 5.1 m.  The consumer came along anyway on the field
    (3D rms 4.034 → 2.926 m, −27 %; UP mean +2.834 → +0.072 m) and the QC tiers went QUIETER
    (T3 cells −31 %, T3c −18 %, T4a −20 %).  ⇒ ledger `network.carry_zwd_in_block` =
    **RECOMMEND_SCOPED** in regime `on("network.carry_zwd") and on("network.phase_leveling_local_block")`;
    the PAIR is a RECOMMEND_SCOPED candidate for `phase_leveling` + F3 lineages whose golden pin
    awaits the user's decision, and the loader now emits a log-only RECOMMEND for `carry_zwd` + F3 +
    in-block-OFF.  Still open on the field: a 1.86 m zero-mean UP wander plus ~1–1.8 m N/E (the
    horizontal got worse, +35 % / +46 %), unattributed.  **Same-build control DONE 2026-09-10
    (§6.17a): `runs/fldf3_on2_ctl_sb` — the control re-run on the HEAD exe — is BYTE-IDENTICAL to
    `runs/fldf3_on2` (`products.txt` sha256 `18aa2854b2575658…`, plus `closed_loop_trace.csv`,
    `network_zwd.csv` and `network_flag_rates.csv`), so the F2 A/B is same-build by proof and every
    number above stands as a single-knob-pair contrast.**  This item is now CLOSED.
25. **Tool debt / telemetry (2026-09-09).**  `scripts/simnet_plateau_decompose.py` raised inside
    `np.percentile` on an arm with ZERO T4 events (exactly E1) — guarded 2026-09-10 at
    `scripts/simnet_plateau_decompose.py:172-175` (empty jump population now prints `n=0` and skips
    the summary); the E1 numbers in §6.16.4 were produced with a scratchpad copy carrying the same
    two-line guard.  Also: E1's `network_flag_rates.csv` / `network_run_summary.csv` were lost to a
    box reboot, so its T3 / T3c rates are RECONSTRUCTED from `logs/network_raim.csv`
    (`count(spiked==1)` = T3 flags, distinct `(tow,sat)` = T3c opportunities, distinct with
    `n_sta_spiked ≥ min_sta` = T3c flags — validated exactly on three runs where both sides exist),
    and its T4a tier count is NOT recoverable.
26. **BOUNDED IDENTIFICATION, SECOND HALF — a station-level response the network's own screens can
    already drive (opened 2026-09-10, §6.16.7d; PREMISE AMENDED same day by Arm F2).**  Item 22 named the
    first half (Tier-C3 solution-separation confirmation of the median-polish proposals, so a
    mass-refloat is bounded by a separability test rather than by a polish residual).  ⚠ **The premise, stated honestly.**  The
    misattribution storm this item was opened on — **T3c 1 105 → 7 619 flags (6.9×), T3 cells
    22 713 → 60 222 (2.7×), T4 one-solve refloats 3 899 → 5 156**, with the h5 = 7.36 m / h7 =
    5.14 m consumer excursions — was measured on Arm F, which the loader had silently reduced to the
    **DIAGONAL** `carry_zwd` vehicle.  Arm F2 ran the real in-block vehicle on the F3 posture and
    every one of those tiers went the OTHER way (T3c 1 038 → 849, T3 cells −31 %, T4a 1 187 → 952,
    T4 refloats 2 159 → 1 750).  **So there is no field misattribution storm; that storm was the
    diagonal row's double count.**  What still motivates this item is (a) **E1** — the T4-erase half
    of the sim plateau, where removing the erases flattens the D3 interaction (3.114 → 2.932) and
    accounts for half the ZWD sink — and (b) the **sim clock relocation** under in-block ZWD
    (contrast 8.05 → 11.70 m, iono⇄ZWD 2.46 → 3.03 m on the slipped bed): the tension the pinned wet
    delay no longer absorbs has to be identified and bounded somewhere, and on that bed it is not.  The station-congruence
    rider has been screaming about that station since §6.16.2 (**t = 2.2e6**, d = (−418, −144,
    −771) m ENU, `reported=1`) and is **measure-only**: there is no quarantine response, so the
    identification the network already has never acts.  Build, both edges (invariant #7): a
    station-level quarantine/deweight driven by the congruence screen, with a release policy, plus
    the Tier-C3 confirmation of item 22 — validated on BOTH beds with ZWD-in-block armed, since
    that posture is what exposes the residuals in the first place — and grade it on the SIM clock
    contrast and on all three consumer components, not on the field's T3c count, which is already
    quiet.  ⚠ **No longer a prerequisite for `network.carry_zwd_in_block`** — Arm F2 promoted that
    knob to RECOMMEND_SCOPED in regime `carry_zwd` + F3 on 2026-09-10 without it.  It remains the
    prerequisite for arming the **PAIR** as a golden default, and for the sim bed at all.
    ⚠ **RE-STATED 2026-09-10 (§6.17b/§6.17g): DEAD as a STATION-QUARANTINE motive.**  Half (b) of
    this item asked for a station-level quarantine driven by the congruence screen, on the theory
    that a station fault read as satellite faults was driving the field's consumer excursions.  The
    field residual census says the tropospheric channel is CLOSED after the in-block fix (fleet ZWD
    deviation sd 21.5 → 4.9 mm; the site's own ZWD explains 1.8 % of the UP variance) and the
    remaining residual is ONE slow axis at el 49.8° / az 9.3° pointing into the site's constellation
    void, with every block-level "driver" collapsing under an autocorrelation-corrected n (p
    0.24–0.37).  There is no station to quarantine on the current field posture.  What survives of
    this item is the SIM clock relocation under in-block ZWD, and item 22's half is now superseded
    outright (see there).  Any future quarantine card must first show a station whose exclusion moves
    a truth-graded channel — §6.16.2's roster A/B already showed exclusion only RELOCATES the sink.
27. **CONSUMER-SIDE WL/NL CASCADE ON THE UNDIFFERENCED MODEL — the AR track's step 1 (opened
    2026-09-10, §6.16.7b).**  The closed-loop consumer cannot fix per band and the blocker is its
    OWN float, not the wire: after 12+ h at a cm-level static position on the quiet truth bed the
    per-band float σ is **0.321 / 0.368 / 0.422 / 0.439 cyc** (L1C / L2W / L5Q / L7Q, p50 over
    h12–24 from `logs/ambiguity_trace.csv` joined to `truth_ambiguities.csv`), so ILS chooses
    between near-equiprobable candidates — **2 879 attempts, 0 fixes, ratio p50 1.01 (max 1.26),
    `ratio_battery` refusing 2 596 of them**.  With a free per-LOS slant ionosphere and no cascade,
    per-band ambiguities are undetermined by construction; the GF direction is code-determined.
    Build: MW-WL float (iono-free, σ ≈ 0.05 cyc over hours) → fix WL → NL from the IF float, on the
    undifferenced consumer model.  The machinery exists for the BSSD/user paths but `ar_wl_only` is
    INERT under `consumer_undifferenced`.  ⚠ Until this lands, **no per-band fractional grade is a
    statement about the wire** — neither the consumer's fix rate nor `simnet_grade.grade_phase`'s
    P-record SD fractions (confounded twice: the iono-direction gauge of §6.16.6 AND the float
    weakness measured here).  Step 2 of the track is an anchor-aware TRUTH grader on the WL/IF
    combinations of the P records; step 3 (network integer-leveled pins) stays behind both.
    ⚠ **REPLACED 2026-09-10 — the ORDER FLIPS: the WIRE comes first, and the cascade as specified is
    DEAD ON ARRIVAL (§6.17c/§6.17d).**  Step 2 was built (`scripts/consumer_float_truth_join.py`,
    `scripts/simnet_wire_integer_consistency.py`) and it answers step 1's premise.  (i) The
    consumer's lanes are already PRECISE **within an arc** — GPS EWL 0.026 cyc, Galileo EWL 0.015,
    WL 0.05–0.08, NL 0.053 — so noise is not the blocker; but **every** channel's per-arc offset sits
    at the uniform null (p50 |wrap(arc mean)| 0.22–0.30, only GPS EWL concentrated at 0.173).  That
    is a BIAS, and more integration cannot remove it.  (ii) The cascade's WL would come from
    `computeSatMwBias`'s MW SD wide-lane, which subtracts *the wire's own* satellite MW biases — the
    same biases producing those offsets — so it would lock onto WRONG integers with a high ratio:
    exactly the Jul-30 user-path failure (`project_pppar_user_control`, FAIL-UNSAFE).  (iii) Graded
    against truth directly, the WIRE is at the uniform null on all 12 channels, the consumer inherits
    it exactly (unwrapped slope +0.888, Pearson +0.936 on `W = dC − dP`), and the offset is a stable
    per-(sat, band) gauge (within-tenure circular std 0.024–0.036 cyc) that is **an arbitrary
    receiver's float ambiguity** — `round(−dP/λ)` matches SOME station's truth integer 50–75 % of the
    time and the CURRENT anchor's 0–3 %.  **⇒ build order: (1) make the wire integer-consistent
    (design-doc Phase 2 diagnostic re-run with a truth-gradeable trace → Phase 3 integer pins,
    item 30); (2) only then the consumer cascade.**  The cascade stays a real gap — it is what makes
    a good wire fixable — but it cannot be validated, or safely armed, before the wire carries
    integers.
28. ~~**Quantify the `code_datum_time_linked` + `code_bias_gf_datum` "inconsistent gauge" pair on a
    real arm**~~ **CLOSED 2026-09-10 (§6.16.7e; `docs/SPEC_family_gf_datum.md` §2.1 addendum).**
    Single-knob 24 h A/B on the quiet golden (`runs/golden_sim_tlink_off` vs
    `runs/golden_sim_tri_w6_full`, same exe): OFF is **worse** — consumer settled rms 0.0762 →
    0.1013 m (+33 %), code-bias truth rms 0.1482 → 0.4182 m (max 0.52 → 3.70 m, concentrated on
    E:C7Q and G:C5Q), clocks and ZWD flat.  The pair injects no measurable tension on this bed and
    time-linking is load-bearing for the consumer: **keep it armed; the loader WARN stays
    informational.**  Not closed for the FIELD (no field A/B), and not a statement about a bed where
    the veteran set and the pinned set diverge — §2.1's rank-1 dependency is exact only when they
    match.
29. **A LEVEL-PRESERVING SLIP RESPONSE IS ONLY HALF A FIX — the ceiling is measured, and it does not
    move the plateau (opened and answered 2026-09-10, §6.17e; `docs/SPEC_slip_qc_hardening.md` §13).**
    `network.cycle_slip.oracle_relevel` shifts `LocalCarry::value_abs` by the TRUE integer jump at
    every detector actuation instead of freeing the arc — the sim-only CEILING of the engineering
    card (`SPEC_lane_slip_response`'s EWL/WL-MW jump estimator), so the card could be priced before
    it is built.  The mechanism is exact: unrepaired fraction |e(t₀) − e(t₀−30)|/|δ| **p50 0.005**,
    |e(t₀)|/|δ| **0.070**, holding at 0.078 after 150 s, latency-0 on 1 124 of 1 161 repairs.  And
    every pre-registered gate FAILS: ZWD 2.098 → **1.779 m** (target ≤ 0.5), ambient per-band DD |e|
    1.65 → **2.43 cyc (WORSE)**, T4 events 7 656 → **13 562 (+77 %)**, consumer p50 flat, ref-free
    WL flat at 81.5 %.  Adding the E1 T4 starve makes it **worse still** (ZWD 1.819, consumer p50
    3.781, WL 35.4 %) even though the ceiling instrument itself IMPROVES there (repairs 1 161 →
    1 549, `no_carry` fall-through 3 149 → 1 396).  ⇒ **do not build the engineering card as a
    plateau fix.**  It may still be worth building for the field's own reasons (slip-response
    quality), but it must be graded on a channel that is not the plateau.  Two residual defects of
    the instrument itself, both biasing the ceiling DOWNWARD and worth fixing if it is re-used: the
    **25.6 % detected-but-KEPT** class (1 587 unexplained `oracle_keep` rows whose arcs sit at
    |e|/|δ| ≈ 0.89 — the window or the (station, sat, signal) keying missed them), and the 5.9 %
    SILENT class the detectors never saw.
30. **THE WIRE'S INTEGERS ARE THE AR TRACK'S CRITICAL PATH, and the emission-side shortcut is
    FALSIFIED (opened 2026-09-10, §6.17d/§6.17f).**  `network.emit_pb_gauge_level` estimated the
    per-(sat, signal) common fractional part from the carried stations and added it back at
    emission.  It actuated (76.0 % of P records levelled, mean |δ| 0.65 cyc) and FAILED 4 of 5
    science gates: the wire's SD fraction did not move on any of 12 channels, the float consumer got
    **+36 % worse** (0.0761 → 0.1037 m rms), and the wire's within-tenure circular std ROSE 1.1–1.9×.
    Two root causes, both general lessons: (i) the premise is only weakly true — the carried
    per-station SD fractions have R p50 **0.572** at n ≥ 5 against ≈ 0.41 for pure noise at n = 6,
    and the carry covers a median of **6 of 61** stations, so 75 % of innovations are rejected before
    the estimator sees them; (ii) an **unwrapped EWMA is an integrator** — per-epoch steps are tiny
    (p99 0.039 cyc) but half of all families walk more than a full cycle in 24 h and G03/L1C reaches
    **−7.63 cyc = −1.45 m** of injected bias under one unbroken reference.  Any future emission-side
    gauge needs a bounded/wrapped δ̂ with per-epoch authority and a much higher concentration bar.
    ⇒ **the corrected build order is the design doc's own**: Phase 2 (the producer's DD search,
    `NetworkProducerAr` / `network.ambiguity_resolution.*`) re-run on the quiet golden — it was
    falsified only on the pre-F3 bed at near_int 34 %, and the quiet bed now carries 87 % ref-free
    DDs at the integer — then Phase 3 integer pins replacing the float-valued `w=1e10` pivot pins,
    then the consumer cascade of item 27.  **Prerequisite: Phase 2 is LOG-ONLY today.**  A trace
    (`network_producer_ar.csv`: per-DD float value, fixed value, ratio, and the pair identity) plus a
    truth grader against `truth_ambiguities.csv` must exist before its fixes can be believed — a fix
    count is not a grade (`project_pppar_user_control`).  The branch
    `worktree-agent-a7e2586003a605548` (commit `9b59a2c`) is NOT merged and stays as the vehicle for
    item 31.
31. **FIELD EMISSION CONTINUITY — pre-registered, in flight (opened 2026-09-10, §6.17g/§6.17h).**
    The field's emitted CLOCK is smooth (within-tenure deviation from a 30-min local-linear fit p50
    **2.6 cm**, only 3.9× the COD final clock's own floor) but the emitted PHASE BIASES **step by
    metres**: pc_IF p50 0.46 m, p90 2.3, max 10.5; **69–92 %** of that variance is within-tenure
    steps > 0.5 m; 1 012 pc_IF steps > 0.25 m in 6 h = 5.3 per satellite per hour, unmarked by
    construction (the per-epoch cross-satellite median is removed first), i.e. invariant #7 edges
    with no event.  The 5 min after a PBGAUGE (349) or PHASE_ANCHOR (382) handover carry **6.9 m rms
    against 1.4 m elsewhere (×5)** — the "continuous" handovers cold re-realize because the
    successor's variance exceeds `handover_max_var`.  Every such step trips the 0.25 λ discontinuity
    test ⇒ a consumer ambiguity RESET, which is why the field consumer never holds an arc.  Card:
    `network.emit_pb_gauge_mode: continuity` — a per-family emission offset that absorbs the frame
    step (0.10–40 cyc) with **no integer content** (the satellite's true bias is constant; the gauge
    is arbitrary and common to every station's product), so it is a different mechanism from the
    falsified item-30 levelling and its failure modes do not carry over.  Arms
    `_fldf3_on2_zwdblk_pbcont` vs `_logctl`; pre-registered: consumer resets −70 %, pc_IF
    within-tenure deviation 0.46 → ≤ 0.10 m, consumer 3D 2.93 → ≤ 2.0 m.  Note `emit_pb_filter` (the
    v1 continuity KF) is structurally OFF on the leveling vehicle — it is nested under `emit_rebase`,
    which is `carry_invariant`-only — so this is not a re-run of that.
32. **FIELD INSTRUMENT GAPS that blocked this morning's work (opened 2026-09-10).**  Three, all
    cheap, all measured as missing on the DOY-129 chain: (a) **`logging.enable_ambiguity_trace`** is
    off, so the field has no consumer ambiguity series and §6.17c's arc-offset decomposition has no
    field analogue (the field has no truth, but the per-arc offset STRUCTURE is truth-free); (b)
    there is **no consumer ZWD trace**, so §6.16.7c's height ⇄ consumer-ZWD correlation had to come
    from a separate log-only arm; (c) **`network.emit_clock_disc`** is unarmed, so the per-satellite
    CLK_STEP / CLK_ANCHOR rows are structurally empty and §6.17g's clock smoothness had to be
    measured truth-free off the wire.  Arm all three on the next field lineage — (a) and (b) are
    trace-class and fingerprint-SAFE; (c) is the companion the AGU-26 amendment 5 named for
    `network.clock_datum_broadcast_anchored` and still has no first validation, so without it every
    product-wide clock jump is silent to the consumer.
33. **THE (ρ, I) SPLIT FREEZE — the level's uncertainty along the split direction (opened
    2026-09-10, §6.17e; supersedes item 22's mechanism).**  With every T4 erase starved AND every
    detected slip repaired to its true integer, the slipped bed gets WORSE, not better: ambient
    per-band DD |e| **1.65 → 6.09 cyc**, ZWD 0.933 (E1 alone) → 1.819 m, ref-free WL 65.4 → 35.4 %.
    The per-band DD errors are equal in CYCLES while the lanes stay clean (E0 ref-free L1C 0.896 /
    L2W 1.086 / L5Q 0.980 / L7Q 0.816 against EWL **0.032**, WL **0.141**), and an error that is a
    constant ε cycles on every band is a (range, iono) split, not an ambiguity error: solving
    `δρ + γ_b δI = ε λ_b` on the L1/L2 pair gives (δρ, δI) = (0.1069, 0.0834)·ε metres and predicts
    L5's own per-band error to **0.6 %**.  ⇒ the station's zero-q levels are frozen around a
    SELF-CONSISTENT mis-split of range against ionosphere, which no per-arc response and no
    identification tier can undo — and T4's erases, destructive as §6.17b shows them to be, were also
    the only mechanism periodically re-randomising it.  **The lever is the LEVEL's uncertainty along
    the split direction.**  Candidate build: a per-LOS ionosphere carried IN-BLOCK, i.e. the same
    vehicle distinction the ZWD card paid for in §6.16.7d, one column per LOS instead of one per
    station.  ⚠ **Re-read `docs/SPEC_iono_carry*.md` and `project_iono_carry_8b_campaign`'s FOUR
    falsifications before building it** — a DIAGONAL per-LOS iono row is precisely the shape those
    falsified, and §6.16.7d's finding is that the diagonal vehicle IS the double count; if the
    in-block vehicle is what makes it work, that must be shown on the quiet bed first (the B9/Q1
    protocol), not assumed.  Second candidate, cheaper to falsify: a truth-consensus level oracle
    that shifts a whole station's levels to the truth-consistent split rather than one arc at a time
    — if THAT does not collapse the plateau, the freeze is not in the levels at all.
34. **THE LEVELS' MEMORY IS NOT THE LEVER — FALSIFIED ×4 (opened and closed 2026-09-10, §6.18a).**
    The first candidate against item 33's (ρ, I) split freeze was the levels' own stiffness: soften
    the zero-q prior and let the split move. Two knobs, four single-knob arms off E0 on the slipped
    bed. `network.phase_leveling_q_amb` 1e-7 / 1e-6 m²/s (`runs/golden_sim_campaign_plq7` / `_plq6`)
    HALVES the T4 churn (22 968 → 12 348 / 12 477 arc erases, −46 % — the first direct evidence that
    the erase tier is sensitive to level stiffness) and still leaves ZWD truth rms at 1.93 / 1.96 m
    (E0 2.098) while the truth |residual| MEDIAN gets WORSE (p50 0.123 → 0.256 / 0.450 m)
    and the consumer
    pays **+57 %** (settled p50 1.751 → 2.76 / 2.75). `network.phase_leveling_var_floor_m2` 1e-4 /
    1e-3 m² (`_plvf4` / `_plvf3`) is NEUTRAL on every channel (ZWD 2.089 / 2.060, consumer 1.85 /
    1.89, T4 22 524 / 21 429). Both knobs are non-monotone in their own value across a decade, i.e.
    past the point where the prior's scale sets anything. **Reading the data supports:** a re-levelled
    arc is code-quality in VALUE but phase-stiff in the block, so it is inconsistent with geometry
    until ~20 min of geometry change refines it, and T4 at E0's rate (21 erases per station-hour)
    erases it first — a limit cycle. Softening the prior lets the level MOVE but gives it nothing to
    move TOWARD while arcs keep restarting. ⇒ **do not re-run this ladder**; both knobs now carry
    KEEP_OFF ledger rows with these numbers, regime `on("network.phase_leveling")`.
35. **T4 ERASES ARE THE CRUDE CURE FOR THE ANCHORS' WALK — and the consumer feels the PHASE-clock,
    not the clock (opened and answered 2026-09-10, §6.18b/§6.18c).** The `network.raim_arc_persist_
    epochs` ladder K = 1 / 10 / 40 / 1e6 on the in-block posture splits the bed cleanly in two.
    **Producer, monotone with K:** ZWD truth rms 1.188 → 0.977 → 0.931 → **0.818 m**, satellite-clock
    datum-free contrast **11.79 → 2.66 m (−77 %)**, clock variance factor −92 %, T4 arc erases
    29 613 → 0. **Consumer, monotone the other way:** settled p50 1.085 → 2.296 → 2.727 (×2.5) and
    2.320 at K = 1e6 — the pre-registered gate (ZWD ≤ 0.5 WITH consumer rms ≤ 2.0) fails at every K,
    and no K tunes out of an inversion that holds across the whole range. The wire grade says why:
    **within-tenure WANDER rises monotonically with K** (Galileo L1C 0.249 → 0.793 cyc, GPS L1C
    0.507 → 0.944) while per-epoch JITTER is FLAT (~0.07 cyc), step size is flat (p50 0.21–0.28 cyc)
    and **handover counts are identical in every arm** (PBGAUGE 279 / PHASE_ANCHOR 297) — K changes
    the CORRELATION of the steps, not their number or size — and the motion is a genuine 2-parameter
    (range, iono) walk, R² 0.993–0.996, |dr| ≈ 1.2 cm/30 s, SHAPE invariant in K. Arc-level-removed,
    the clock channel is flat at 5–12 cm across the whole ladder (**the 4.4× clock gain is ALL arc
    LEVEL**) while the arc-removed PHASE-clock DEGRADES on every Galileo band (L1C 0.143 → 0.277 m).
    The consumer's degradation is a settled BIAS, not noise (3D mean 1.97 → 2.23–2.57 m, worst at K = 40;
    UP mean −0.22 → −1.95 m; wander and jitter both FALL), and the within-run causal test is decisive: the
    correlation of |error| with ARC AGE **flips sign**, −0.335 at K = 1 (error decays after a reset)
    to +0.20…+0.42 at K ≥ 10 with the per-age-bin mean growing inside each run. ⇒ **the erases chop a
    continuous walk into steps the consumer flushes by resetting.** Two carry-forward rules: (a)
    `sat-clock contrast` is NOT a consumer-relevant metric by itself — grade a consumer-facing change
    on the per-satellite PHASE-clock (C + P); (b) the fix is neither T4-off nor a K, and not bounded
    identification (item 22, superseded) — it is stopping the walk (item 37).
36. **FIELD EMISSION CONTINUITY — MECHANISM VALIDATED, PROMOTION KEEP_OFF ×2; the defect is the
    TRIGGER, not the plumbing (item 31's first round, answered 2026-09-10, §6.18d).** Three field
    pairs and one quiet-sim twin. The step-suppression half passes everywhere: consumer PB-disc
    resets −91 / −95 / −93 % (field v1 diagonal / field v1 in-block / field v2) and −93.4 % on the
    quiet sim, the emitter's own |ΔP| > 0.25 λ jump test −98 / −99 / −97 %, pb_L1C unmarked step rate
    0.671 → 0.000 /sat/h, and **emission-only by proof on every arm** (all "P," lines stripped ⇒
    byte-identical products, max |ΔC| = 0.0). On the correct in-block posture v1 also takes the field
    consumer's **UP rms 1.862 → 1.131 m (−39 %)**, passing the pre-registered UP sd gate, so the
    field vertical chain now reads `on2` 3.71 → in-block 1.86 → + continuity 1.13 m. **What blocks it
    is the trigger.** `dev()` is linear, so dev(arm) = dev(ctl) + dev(STAIRCASE), and the staircase is
    measured: 14 887 firings in the settled window at one-epoch |jump| p50 **0.026 m** against a
    0.10-cycle trigger of **≈ 1.9 cm** — it is absorbing routine phase-bias NOISE (~23 firings per
    family per hour), each absorption a permanent offset, so the emitted level RANDOM-WALKS (p50
    18.8 m of accumulated per-satellite excursion in 6 h) and the consumer's N rises +25 % on a
    near-horizontal due-south channel 84° off the geometric null. A datum artefact was tested and
    REJECTED (the per-epoch trim datum is QUIETER in the arm). **v2 pend-and-confirm is a REGRESSION**
    (3D 2.93 → 4.47 m, UP rms 3.46 with a −0.921 m/h ramp, G3 0.844 m) for a reason that is not a
    tuning question: the emitted P offset equals `delta_cont_cyc` EXACTLY on every row **including the
    42 386 PENDING rows** (p50 = p99 = max = 0.000000), **96.8 % of the offset motion happens on
    non-confirmation rows**, and 68 % of confirmations are force-resolved at the 13-epoch rail — it
    gated the LABEL, not the WIRE. The revert path is correct (190/190 restore to the bit) and fires
    on 4.1 % of pends. ★ **And the walk is a property of the TRIGGER, not of the field's event
    content:** on the quiet sim with ZERO truth slips the accumulated |δ_cont| still reaches p50
    28.6 cyc = 5.4 m on L1 over 24 h, ~9× slower per hour only because 96.2 % of its candidates are
    sub-min against 72.3 % on the field. ⇒ **v3 = hold the EMISSION (not the label) for the
    confirmation window, or raise the trigger well above the noise floor / gate it on a declared
    re-reference** — in build. Owed and untouched by v2: the 196 rail pass-throughs (> 40 cyc) in 15
    epochs, 9 with no handover marker, whose sim analogue is 24 of 26 DECLARED — undeclared
    network-wide frame motion is a FIELD defect (invariant #7's output edge), not a property of the
    gauge. Rider, from the 2×2's control column: **the in-block ZWD posture itself doubles-to-
    quadruples the wire's phase-bias churn** (resets 1 158 → 2 322, unmarked pb_L1C steps 0.671 →
    2.925 /sat/h, pc_IF deviation 0.230 → 0.464 m) — the wire-side price of §6.16.7d's vertical, and
    its own card.
37. ~~**THE IONO-IN-BLOCK CARD**~~ — **BUILT, GRADED ON BOTH BEDS, KEEP_OFF (opened and closed
    2026-09-10; build §6.18c, results §6.19a / §6.19a.1).** The card ran exactly as pre-registered and
    the pre-registration FAILED on the slipped bed: ZWD truth rms 1.188 → 1.133 m (−5 %, gate ≤ 0.5)
    and the consumer REGRESSED, settled p50 1.085 → 1.592 m (+47 %, gate ≤ 0.6), with the mechanism
    demonstrably live (671 535 iono columns carried, **0 diagonal fallbacks**). The B9/Q1 protocol
    PASSED — on the quiet 24 h bed the arm is NEUTRAL (consumer 3D rms 0.0875 → 0.0890 m, +1.7 %; ZWD
    truth 5.6 mm both; 2.75 M columns, 0 fallbacks) — which settles a second question worth keeping:
    **the four `project_iono_carry_8b_campaign` falsifications were the DIAGONAL row's over-count, not
    "carrying iono"**, exactly as §6.16.7d found for ZWD. The one channel that moved the right way is
    the INTEGERS (L1C settled ref-free med|e| 1.77 → 1.32 cyc, −26 %; share rounding to the true
    integer 21 → 25 %) — carried forward as evidence for the AR track, not as a reason to arm this.
    Rider: on the quiet bed the extra carried direction per LOS gives the post-fit consensus more to
    flag (T4 arc erases 159 → 408 with ZERO truth slips). Ledger row `network.carry_iono_in_block`
    (+ `_q_m2`, `_max_gap_s`) = **KEEP_OFF**; branch `worktree-ionoblk` (`7311ad0`) UNMERGED. The
    original pre-registration, for the record: `network.carry_iono_in_block` on branch
    `worktree-ionoblk` (from `dd6c81c`): the I half
    of the frozen (ρ, I) split carried INSIDE the F3 local block with process noise — one column per
    LOS where the ZWD card is one per station. The motive is now measured rather than argued: with the
    erases starved the anchors' levels WALK along a 2-parameter (range, iono) direction at R² 0.99,
    and the consumer integrates it (item 35). Pre-registered: slipped-bed ZWD ≤ 0.5 m **and** consumer
    settled p50 ≤ 0.6 m, quiet-bed consumer within +10 % of the golden. ⚠ **The B9/Q1 protocol is
    mandatory here** — `project_iono_carry_8b_campaign` falsified a DIAGONAL per-LOS iono row FOUR
    times, and §6.16.7d's finding is that the diagonal vehicle IS the double count; the quiet bed must
    show the in-block vehicle neutral BEFORE any slipped-bed claim, exactly as the ZWD card did. ⚠ And
    grade it on the PHASE-clock and all three position components, not on the satellite-clock contrast
    (item 35(a)) and not on the vertical alone (§6.16.7d's horizontal regression).
38. **A BRANCH/WORKTREE EXE SILENTLY IGNORES EVERY CONFIG KEY IT DOES NOT PARSE (opened 2026-09-10,
    §6.18d; `feedback_branch_exe_unknown_knob_silent`).** The PB-gauge worktree branched from
    `4bb799a`, so it lacked `966d4fd` (`network.carry_zwd_in_block`), `9963959` and `5f1f422`. Its exe
    passed an OFF-path byte-identity gate against HEAD on the QUIET GOLDEN smoke — which arms none of
    those — and the field pair `_fldf3_on2_zwdblk_{pbcont,logctl}` then ran with the sub-knob **dropped
    without a word**: no "ZWD in block" run-end clause, the diagonal carry instead, and every
    pre-registered ABSOLUTE baseline off the wrong posture. The JSON loader has no strict mode: it
    reads the keys it knows and ignores the rest. Three rules, all cheap: (a) before launching an arm
    on a branch exe, `git merge-base --is-ancestor <commit-of-every-armed-knob> <branch>` for every
    knob the arm arms beyond its golden; (b) grep the smoke console for the run-end clause or manifest
    echo of every armed knob — invariant #4, counters are the ON-path telemetry, and an absent clause
    means the knob was dropped; (c) **the `logctl` hash rule** — a log-only twin of a main-tree run
    must reproduce that run's `products.txt` sha256 exactly, and a mismatch is a posture defect, not
    noise. It was rule (c) that caught this one (`95afa6d9…` ≠ `c48b9a5c…`), and rule (c) that
    certified pair 2 (`logctl2` = `c48b9a5c…`, exact). Byte-identity on ONE posture proves nothing
    about knobs that posture does not arm.
39. **`network.serialize_disc_counters` IS OFF ON THE FIELD CHAIN — file consumers get no
    discontinuity signal at all (opened 2026-09-10, §6.18e).** The DOY-129 lineage emits the 6-field
    P record (`P,sat,signal,bias_m,sig_int,wl_int`) with no `phase_disc` column, while the wire is
    stepping at **~5.3 pc_IF steps per satellite per hour above 0.25 m** (205 above 1 m in 6 h, up to
    26 satellites in one epoch), driven by T4 RAIM-attribution refloats (`NetworkRaim.cpp:888-893`;
    r +0.546 |z| 8.7 on the control arm, +0.268 |z| 4.4 in-block, every other tier at noise). The R
    retro-bump fired **68 times against ~1 000 steps**. The closed-loop consumer only resets because it
    reads the in-process store counter; a FILE consumer of the same product is blind. That is
    invariant #7 with a bare OUTPUT edge, and it also made G2 of the continuity card UNMEASURABLE on
    its own terms (the grade had to fall back to the emitter's |ΔP| > 0.25 λ proxy). **Arm it on the
    next field template**, alongside item 32's three trace gaps (`logging.enable_ambiguity_trace`, a
    consumer ZWD trace, `network.emit_clock_disc`) — all of them trace/serialization-class, none of
    them solve-affecting.
40. **THE SLIPPED-BED PROBE LEDGER IS CLOSED — the station frame WALKS under arc churn, and every
    response that reduces re-levelling makes the consumer worse (opened and closed 2026-09-10,
    §6.19a).** Eleven config-only and card-level probes against the in-block control
    (`runs/golden_sim_campaign_zwdblk`: ZWD truth rms 1.188 m, consumer settled p50 1.085 m, T4 29 613
    arc erases): oracle exact repair, oracle + E1, level q ×2, var floor ×2, persistence K = 10/40/1e6,
    `raim_k_sigma` 4, iono-in-block, young-arc inflation ×2, false-alarm-only keep, receiver-clock q.
    **All FAIL both gates** (ZWD ≤ 0.5 m *and* consumer p50 ≤ 0.6 m). The single reading that predicts
    the whole table: the station frame — its stiff zero-q levels together with its per-epoch ZWD,
    receiver clock and slant iono — walks (measured at 1.2 cm/30 s range-like, §6.18c), so an arc is
    consistent with it only if periodically re-levelled INTO it. ⇒ the erase-rate family is BRACKETED
    (k = 6 / K = 1 is its consumer optimum; more erasing is noise, less is drift), the level's
    stiffness is not the lever (item 34), the response's ACCURACY is not what the consumer buys from it
    (item 35 + §6.19b), and the only thing that ever helped is a temporal prior on the ρ direction
    (ZWD-in-block, −43 %). ⚠ Method riders for anyone re-using this table: the T4 opportunity
    denominator is identical (645 406) in every slipped arm, so the erase counts are directly
    comparable; and p50 vs rms disagree on this bed by design — the young-arc arm has the table's
    second-best ZWD rms and its worst ZWD p50 and consumer rms, so a p50-only ladder reads its tail as
    a win. WHAT IS STILL OPEN: a prior that makes the frame observable epoch-to-epoch in more than one
    direction (item 42 is the next member), and integer constraints (item 44). Both go to the EVENT
    harness (item 43) first.
41. **FALSE-ALARM RE-LEVELS ARE A RESYNC MECHANISM — "tighten the detectors" is dead as a card
    (opened and closed 2026-09-10, §6.19b).** The slipped bed's detectors fire on nothing as often as
    on something — **2 371 of 4 708 slip-tier events have no truth slip in the window (MW 1 132,
    IF-TD 1 239)**, ≈ 6.4 false re-levels per station-hour against 6.8 true slips, while detection
    itself is fine (94.0 % of the 2 486 truth slips detected, 3 % silent). The isolating arm
    (`oracle_true_slip: free` + `oracle_false_alarm: keep`, `runs/golden_sim_campaign_zwdblk_orafa`,
    commit `3dd1284`) KEEPS 6 804 false alarms and frees every true slip as today — and the consumer
    gets **2.7× worse** (settled rms 2.389 → 4.46 m, p50 1.085 → 2.92) with **62 % more T4 erases**.
    ⇒ on this bed the false re-levels are doing the resync work that keeps arcs inside a walking frame,
    so a better detector would remove them and leave the walk. Together with §6.17b (T4 mostly
    re-erases the damage its own previous re-level caused) this closes BOTH accuracy-of-response
    levers — item 22's bounded identification and this one. ⚠ Counting basis: the census counts EVENTS
    per (station, satellite) (~2 300 per 6 h); the oracle counts per-signal ACTUATIONS (6 804). Never
    mix them in a rate. ⚠ Still OPEN and NOT addressed by this: the fixture's phase noise is ~5× the
    field's and the MW thresholds are field-empirical, so this false-alarm rate is a property of THIS
    BED (§6.14.3), and the same probe on the field is unowned.
42. **THE RECEIVER CLOCK HAS NO TEMPORAL PRIOR ON THE PRODUCER — card
    `network.carry_rx_clock_in_block`, pre-registered, in build (opened 2026-09-10, §6.19d).**
    `filter.process_noise.clock_bias_q` is **INERT** on the network producer: the per-station receiver
    clock is a per-epoch LOCAL parameter — estimated fresh, marginalized, never carried — so the knob
    has nothing to act on, and an arm that sets it is armed-and-inert (`_zwdblk_rxq` `products.txt`
    byte-identical, `bba9a7f2865070bc…`; T4 counter identical to the digit). Meanwhile the object it
    would model is real and simple: the simulator's own receiver-clock truth is a deterministic drift
    plus a WHITE increment — mean step +2.997e-08 s / 30 s (≈ +0.2995 m/s) with the step's sd
    1.0055e-09 s = **0.301 m**, second-difference sd 1.436e-09 s = √2 × that. Build: carry the receiver
    clock INSIDE the F3 whitened local block (the ZWD card's sibling; the fourth member of the
    (amb, iono, ZWD, clock) block), q ≈ 0.09 m²/epoch, in-block or nothing, counted diagonal fallback,
    the same loader interlocks, fingerprinted, OFF byte-identical, B9/Q1 quiet-bed neutrality BEFORE
    any slipped-bed claim. ⚠ **Design risk: the drift.** A plain RW on the ABSOLUTE clock fights a
    9 m/epoch ramp, three decades above the 0.30 m increment — carry the RESIDUAL about a per-epoch
    re-linearized `x_lin`, or make the state a (bias, drift) pair. A stiff prior carrying a stale value
    across the ramp is standing invariant #1. ⚠ Validate on the EVENT harness (item 43) first.
43. **THE EVENT HARNESS IS THE DEVELOPMENT LOOP NOW — USER DIRECTIVE (opened 2026-09-10, §6.19f;
    `feedback_event_harness_over_daylong_runs`).** Develop on a CONVERGED SNAPSHOT + ONE INJECTED
    EVENT + 40–60 epochs (1–2 min at 60 stations), not on day-long storm beds (23–90 min per arm).
    §6.19a is the argument: eleven storm arms produced eleven aggregate numbers and no mechanism,
    because every discriminator in it is a single-event question answered only statistically with the
    events interacting. Four pieces, three half-existing: (a) a converged snapshot library (quiet sim
    6 h / 12 h, field 6 h) on the RTB-7 warm-resume vehicle — correctness closed by
    `project_seam_continuity_card`, and ⚠ every seam card graded with a COLD-CONSUMER REPLAY, never the
    seam-epoch diff; (b) a producer-side fault injector `network.inject.*`, default OFF, byte-identical
    unarmed, each event scheduled at an absolute TOW and counted at BOTH edges (invariant #7):
    `slip`, `outage` (**the class the simulator has never had — the reason §12's gap-poison card is
    field-only**), `anchor_handover`, `arc_erase` (a forced T4-style one-solve refloat on a named arc),
    `sat_clock_step`; (c) a leg generator (resume → arm one injection → 40–60 epochs); (d) a per-event
    grader (arc level trajectory vs truth, the station's ZWD/iono/clock response, the emitted C and P
    step, the consumer's reset and re-convergence) — every instrument exists, the per-event framing does
    not. Branch `worktree-evharness`, building; nothing merged. **The rule: storm beds are for FINAL
    validation of a fix whose event-level behaviour is already understood, and never more than 2–3
    concurrent 60-station arms** (the box was oversubscribed at 5–6 on 2026-09-10 and everything ran
    2–3× slow). Item 42's card is the first to be built this way.
44. **THE PHASE-2 PRODUCER-AR DIAGNOSTIC IS GRADED — NO-GO FOR PHASE 3, AND THE BLOCKER IS THE
    CARRY-BLIND COVARIANCE (opened and answered 2026-09-10, §6.19e).** `runs/golden_sim_netar` (the sim
    golden + `network.ambiguity_resolution.{enable,trace,…}`, quiet 24 h truth bed, exit 0, product
    byte-identical by construction — Phase 2 is read-only). Against the arm's own pre-registration:
    fixed epochs **86.8 % PASS**, P1 trace rows **852 260 joined / 0 unjoinable PASS**, searched-subset
    |e| p50 **0.198 cyc FAIL** (gate ≤ 0.10), and CORRECT-fix rate among kept **85.52 % FAIL** (gate
    ≥ 99 %) — a **14.48 % wrong-fix rate, 14× the 1 % threshold the pre-registration named as the
    carry-aware trigger.** Every one of the 200 largest-|e| wrong fixes is off by **exactly ±1 cycle**
    (|e| 0.90–1.10 cyc) at a formal σ median of **0.091 cyc** — **median |e|/σ = 10.4** — accepted at
    ratio 14.0 and `p_succ` **1.0000**; the run-end clause names the cause in one word, `carry-blind`.
    ⭐ **The diagnostic tell: correctness gets WORSE as the run settles** (93.28 % h0–12 → 85.52 %
    settled) while the gate's ratio p50 RISES (11.31 → 14.01) — a covariance shrinking faster than the
    error it describes, which is what a carry-blind conditional block does on the phase-leveling
    vehicle. The search is not being refused by its gates; it is fixing onto a float that has not
    arrived (fixability ceiling 72.4 % settled, so plain rounding would be wrong on 27.6 % of the
    tightest DDs the cap chose). Galileo is worse than GPS on both (correct-fix 79.6 / 81.7 % vs
    90.4 / 90.3 %). ⚠ The graded population ROTATES — 213 / 261 reference-satellite and 242 / 313
    reference-station switches over 2 869 epochs (§6.12) — so quote whole-population rates, not
    per-band splits off this trace. **NEXT, and it is a prerequisite, not an option:
    `network.carry_aware_covariance` applied to the Phase-2 DD block, re-graded with this same
    instrument.** Cost to budget for the re-grade: `netar` is **2 152 ms of a 3 984 ms epoch (54 %)** at
    60 stations. The DO-NOT-BUILD verdict on producer DD-*fixing* (M5) is untouched — this is a
    diagnostic search plus its grader, and it stays log-only.

---

