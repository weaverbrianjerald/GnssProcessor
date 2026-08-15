# SPEC — EWL-first tri-frequency cascade (M5 card "WL-2")

**Status:** BUILT and quick-gated (Jul-16 2026), then measured DEAD on a global network — the DOY-129 305-station field verdict, **§12**. Default-OFF, byte-identical when off. The (B) loose-WL carry refactor was HELD here and later BUILT as WL-3 (`docs/SPEC_lane_carry_card.md`). Absorbed the field report 2026-08-15 (was `docs/REPORT_mwwl_conditioning_day129.md`, now under `docs/attic/`) — §12 is that record.

> **Family disposition banner.** The whole MW/WL lane family — WL-1, WL-2 (this card), WL-3, UWL-1, LF-1 — is under the standing verdict **"MW/WL as conditioning — do NOT arm; net-harmful"** (CLAUDE.md), reinforced by §12's measurement that **≈83 % of gate-admitted WL integers on a 305-station network would be WRONG** (≈64 % after the closure backstop). A RECORD, not an open build order; arming state lives in `docs/CONFIG_promotion_ledger.md`.
> **Provenance.** Every `runs/<name>` path below resolves under `D:\GnssProcessor_archive\2026-08-12\runs\` (archived off C: 2026-08-12).

**One sentence:** fix the extra-wide lane from single-epoch-class MW(L2,L5) / MW(E5b,E5a) statistics within ~3 epochs of arc birth (the third band deliberately uncarried, so those rows win with no fight), bridge phase-only from the fixed EWL to a fast wide-lane integer decision, and INJECT the WL integer into the Card-4 carry through a lane-targeted reseed primitive (set the cross-band difference, preserve the NL).

**Supersedes / relates:** inverts `SPEC_mw_wl_card.md` §3.5 (Phase D "EWL cascade, after B") to the FRONT of the ladder; implements the F6 fix (`SPEC_mw_wl_card.md` §15 + the probe; memory `mw-wl-f6-fix-directions`) as an NL-preserving gated reseed — option (C)'s trigger with the refloat replaced by a lane-targeted value edit; (A) is unnecessary (probe) and (B) is HELD as the fallback (built later as WL-3); demotes WL-1's Phase-A float rows to a diagnostic (§3.0); replaces WL-1's Phase-B posterior-gated fix surface (never implemented) with statistic-gated fixing.

---

## 1. Motivation — F6 + the tri-frequency escape (do not relitigate)

- **F6 (proven, `SPEC_mw_wl_card.md` §15):** the Card-4 carry time-links ∇ΔN₁ and ∇ΔN₂ in SEPARATE per-(const,signal) groups, each stiff (σ≈0.06–0.09 cyc by ep 11). Two independent per-band DDs pin WL = ∇ΔN₁−∇ΔN₂ at σ≈0.085 cyc — at the code-seeded (garbage) value, since no WL information ever entered (the Jul-15 structural gap). Any per-epoch WL information — the 0.22-cyc MW pseudo-obs, or an exact joint fold — is outvoted; the residual pressure trips D5 (369→1628) and shreds arcs.
- **The `debug_r_scale` probe (Opus, Jul-15 eve):** at 100× WL-row stiffness the healthy cycles SNAP to z_mw within 0.05 cyc at the FIRST arming epoch — so fold composition was never the blocker (no joint block needed) and birth-seed quality is irrelevant (a loose lane converges from garbage seeds the moment a strong measurement arms). But the stiff-row-vs-stiff-carry override CHURNS (D5 1628→1779): value transfer by row battle sprays carry innovations. Binding consequences: the WL entry mechanism must avoid the battle entirely (§3.3), and station-1 cycles — which failed to converge even at full authority — are a REAL independent bug (§7 pre-flight).
- **Why three frequencies dissolve the gap (tri-freq sats):** two bands give 2 phase equations against {N₁, N₂, I} — underdetermined, so the WL is code-limited and needs the cross-epoch accumulator the per-epoch producer lacks. With a third band and the EWL integer fixed, 3 phase equations meet {N₁, N₂ (N₃ tied), I}: the WL becomes determinable through PHASE at iono-model precision, per epoch, no code averaging.
- **The λ-protection asymmetry (the load-bearing subtlety):** the MW(L2,L5) OBSERVABLE is λ_EWL-protected — narrow-lane code noise divided by 5.861 m gives single-epoch DD σ ≈ 0.05–0.12 cyc, so its integer is trustworthy almost immediately. But CARRIED per-band values are never λ_EWL-protected: a lane formed from two stiffly-carried bands is ε_a/λ_a − ε_b/λ_b with per-band seed errors ε ~ 0.3–2 m (code noise + the −2I iono sign-flip) over λ ≈ 0.25 m ⇒ multi-cycle garbage held at σ≈0.085 — the F6 wall, for ANY lane whose bands are BOTH carried. Consequences: (a) the third band is NOT carried in v1 (§3.1) so the EWL lane has no stiff opposition — its rows simply win; (b) the WL lane (L1, L2 both carried, NL continuity requires it) cannot be entered by rows at any stiffness and gets the INJECT primitive (§3.3). The third frequency's contribution is TIME: trustworthy WL targets at ep ~4–6 via the bridge instead of ep ~25–50 via plain MW averaging.
- Wavelengths: GPS L2−L5 λ = c/51.15 MHz = **5.861 m**. Galileo E5b−E5a λ = c/30.69 MHz = **9.768 m** (every FOC sat, quad-freq). Galileo E5b−E5 / E5−E5a (AltBOC, 15.345 MHz) λ = **19.54 m** — a luxury rung; the fixture data DOES carry L8Q, but 9.768 m is already single-epoch-trivial. GPS L5 flies on the IIF/III subset (~⅔ of the fleet); dual-freq GPS sats take the §3.5 fallback.
- **MEASURED (Jul-15 eve, `scripts/ewl_census.py` — estimator-free star DDs, raw RINEX 2024-129, 309 sta, uniform channel groups):** GPS EWL (C2W/L2W×C5Q/L5Q, 186 sta): per-epoch arc noise σ = 0.027 cyc; SINGLE-epoch DDs med|f| = 0.042, P(|f|<0.15) = 0.94, circR = 0.88; n=2 → 0.96/0.90; n=30 → med|f| 0.020, circR 0.97. Galileo EWL (7,5): n=1 circR 0.97, P(<0.25) = 1.00. Galileo 19.5 m pairs: n=1 med|f| ≈ 0.012–0.014, P(<0.15) = 1.00. Reference lanes on the same data confirm the asymmetry: GPS WL (1,2) n=30 circR 0.55, Gal WL (1,5) 0.49 — code-limited, exactly the gap this card exists to bypass. The §3.2 defaults (n_lag ≥ 2, frac < 0.20, σ < 0.10) are comfortably inside these margins.
- **MEASURED, bridge arc (Jul-16, `ewl_bridge_oracle.py` — nearest-neighbor DDs bucketed by baseline, EWL fixed at n=2 then phase-bridged):** EWL gate skips 5–8 % at EVERY span (iono-free ⇒ span-independent ✓). The UNCORRECTED bridge beats code-MW only inside ~150 km (n=1 med|f| 0.124 vs 0.209 at <50 km) and plateaus iono-bias-limited (≈0.5 cyc/m × ~0.2 m DD iono) — never FFR-grade; ≥400 km it is uniform. Hence §3.4's mandatory iono correction. Closure-guard flag rate 2 % local → 37 % far. Caveat: 2024-129 is a solar-max stress set — quiet-iono margins will be wider; design to these numbers anyway. ⚠ **§12.3 measured this same bridge on the real 305-station roster and found it geometrically DEAD — the short-baseline regime this bullet documents is 0.3 % of that network's lane cycles.**

## 2. Estimability

Identical invariance class to WL-1 §2: every quantity constrained is a closed-cycle (DD) functional on the (station,sat) arc graph — receiver and satellite bias lumps cancel by balanced signs; no pivot, no second S-system; gauge directions have zero component along the rows ⇒ the W5 null space is untouched (FIREWALL inherited; §7 re-proves mechanically for the (2,5) rows). The INJECT is estimator-side but equally legal: it edits carried z's — themselves estimable DD functionals — by setting one invariant linear combination (the lane difference) to an integer while leaving the complementary combination (the phase-determined NL/sum) untouched. That is a measurement-update-like act on invariants inside the network's one S-system, not a datum act. Frame discipline carries over unchanged: ZD-consistent `arcZdMean` per pair, mixed-BIA-frame cycles skipped, per-cycle channel consistency (§3.1).

## 3. Mechanism

### 3.0 Two architectural principles (F6 lessons, binding)

1. **Decision source:** every integer decision (round, FFR, inject trigger) reads DETECTOR statistics — never the float posterior. The posterior's WL/EWL fracs are carry-contaminated (F6: deceptively tight σ passes the gate on garbage values).
2. **No rows against stiff-carried lanes:** folding pseudo-obs — at ANY stiffness — against a lane whose bands are both stiffly carried either loses (float) or churns (stiff, the probe). On such lanes statistics act ONLY through the inject primitive + a post-inject hold row (innovation ~zero by construction — nothing fights). On UNCARRIED lanes (the v1 EWL, §3.1) ordinary rows are fine and win cleanly. WL-1's float layer stays in the tree as a default-OFF diagnostic (`mw_wl.enable`), useful for A/B forensics; the loader warns if both it and `wl_inject_enable` are armed.

### 3.1 Third-band inputs (mostly already built)

- **Detector:** `buildUncombinedPairs` already forms ALL cross-band pairs, frequency-descending (f1 > f2, the signed-Δf convention), with same-channel code matching (`matchChannel`). Under `mw_frequency_mode = Mirror` the band scope follows the KF; Triple/All widen it. The per-arc stats map is keyed `sat_sig1_sig2` and `arcZdMean` is pair-generic with the F1 lagged snapshot — so MW(2,5)/(7,5) statistics exist as soon as the bands are in scope. **No new MW math.**
- **Estimator:** the network solve must PROCESS the third band (the EWL row needs a live N₅ column): network configs move to FrequencyMode Triple for GPS (+Galileo where enabled). This grows the local blocks (~+⅓ ambiguity columns for GPS) — perf note §8. The known L5 receiver-ATX phase inflation is irrelevant to EWL integer content; keep L5 out of NL-bearing rows.
- **The third band is NOT carried (v1, deliberate):** the (const, third-band-signal) groups are excluded from the invariant-carry driver (they still participate in the eq34 election/datum as any signal does — §11 Q1b). N₅ stays per-epoch loose, so the EWL lane never acquires an unearned stiff prior: EWL rows win cleanly against per-epoch code, no churn, no inject needed on that lane (§1 probe logic). Carrying the third band (for its own NL content) is a later card, gated on this one. ⚠ The exclusion is `ewl_carry_exclude` — §12.8 for its warm-resume hazard (it changes carry MEMBERSHIP with **no fingerprint check**).
- **Pair selection per constellation (hardcoded v1 table, not user knobs):** GPS EWL = (band 2, band 5) on the estimator's chosen channels; Galileo EWL = (band 7, band 5); BDS/QZSS deferred; GLONASS excluded (FDMA, never in the carry). Per-cycle channel discipline as WL-1: every arc's statistic on the SAME channels the estimator uses at that station; mixed-channel cycles skipped + counted (the mw_census DCB-residue lesson, now on three bands). *(Generalized to a per-constellation tier table by UWL-1, `docs/SPEC_uwl_card.md` §2.)*

### 3.2 EWL statistic rows → integer decision (ep ~3)

Ride the Card-4 chord basis exactly as WL-1: for each primary-sig1 fundamental cycle whose arcs all expose the constellation's EWL pair, assemble z_EWL = Σc·MW̄₂₅/λ_EWL (cycles), R from the lagged Welford variances (+ shared-arc covariance within a component, the F2 form), correction-frame re-expressed per F5 against the (col₂, col₅) linearization values. Eligibility: n_lag ≥ `ewl_min_arc_epochs` (default 2 ⇒ first decision at ep 3 under the F1 lag).

**§3.2 CORRECTION (measured, Jul-16 F-A attribution — the rows are NOT folded in v1):** folding the fixed EWL rows into the solve proved to be pure cost: the WL cascade consumes the integers from the DETECTOR statistic (never the posterior), and 147 σ0.01 integer rows perturb the shared clock/PB border enough to elevate D5 ~2× NETWORK-WIDE (679 vs control 342 at 15 ep; 24 % of flags on secondary-channel groups the EWL never touches — border-mediated, not local). With the fold removed (`ewl_fold_rows=false`, the v1 default) D5 returns to control (348–358) with the cascade fully intact. The EWL lane is therefore DECISION-ONLY in v1: statistic → FFR → integer → bridge. Folding (this section's original design, for N₅ conditioning / future L5-NL work) is retained behind the `ewl_fold_rows` study knob and must come back, if ever, at honest softness or inside the (B) lane-coordinate carry. *(§12.8 lever 3: that D5 cost was measured in a HEALTHY network; the trade is worth re-taking in a sick one.)*

**FFR on the statistic row:** fix when σ_row < `ewl_fix_sigma_cyc` (0.10) AND |frac(z_EWL)| < `ewl_fix_frac_cyc` (0.20). Rounding is stateless — re-derived each epoch from the (stable, slip-reset) statistic, with a hysteresis margin (once consistent, keep while |z_EWL − N_fix| < 0.35) and a `fix_flips` counter. A slipped member arc resets its stats ⇒ the gate fails ⇒ the fix evaporates by construction — the per-epoch re-derivation IS the un-fix wiring (force-fire test mandatory, §7).

### 3.3 The WL inject primitive (the F6 fix on the carried lane)

The WL lane is the one lane whose two bands are BOTH stiffly carried, so the probe's verdict applies with full force: a float row loses, a stiff row transfers the value but churns (D5 1628→1779), and a raw refloat (option (C) as re-tap) throws away the NL the carry spent epochs earning. The remaining NL-preserving, battle-free entries are (B) — the lane-anisotropic carry basis, HELD here, **later BUILT as WL-3** (`docs/SPEC_lane_carry_card.md`) — and the v1 choice:

`injectLaneTarget(cycle, lane, target_int, stat_var)`: given a fundamental cycle and a band pair (a,b), adjust the carried DD values so the cycle's lane difference ∇ΔN_a/λ_a − ∇ΔN_b/λ_b equals the integer target while the complementary sum (the phase-determined content) is PRESERVED exactly. Mechanics: the two bands live in SEPARATE carry groups, and v1 applies the primitive only when the cycle maps 1:1 in both groups (same chord arc, identical cycle support under each group's tree — the common case when both bands' graphs coincide); non-1:1 cycles are skipped + counted (`inject_skip_basis`), and the general cross-basis solve is the v2 extension. Solve per band pair in cycle units: with w = target (cycles) and s = current sum preserved, z_a′/λ_a − z_b′/λ_b = w and z_a′/λ_a + z_b′/λ_b = s. Covariance: reseed the lane direction's uncertainty to the statistic's honest variance; the two groups cannot represent the cross-band correlation (independent blocks), so the HOLD ROW (below) supplies the cross-group stiffness the carry cannot.

- **D5 exemption, one epoch:** the inject deliberately moves each band's carried value by up to ~half the garbage gap — per-epoch code-limited z_free will disagree at a few σ. The affected chords enter a `lane_injected` exemption set for that epoch (mirrors the `carry_slipped_ambs` funnel plumbing); D5 resumes normally next epoch. Without this the inject re-creates the storm it cures.
- **Trigger (stateless, self-limiting):** inject when the lane's FFR passes AND |statistic − carried lane value| > `inject_disagree_min_cyc` (0.75). Post-inject the disagreement is ~0 so it cannot re-fire; a later slip/re-tap regrows it and legitimately re-triggers — option (C)'s gated reseed, minus the NL destruction.

**Hold row:** after a successful inject, fold the lane integer as a hard row (σ = `fix_weight_sigma_cyc`, 0.01, soft-hard — rollback-able) on the cycle's (col_a, col_b) columns, re-derived each epoch while the FFR + hysteresis hold. Its innovation is ~zero, so nothing fights; it pins the lane between injects and across benign re-taps. Integer transport across basis re-expression is automatic where v1 applies (1:1 chords; W unimodular).

### 3.4 WL bridge statistic (tri-freq lane 2, ep ~4–6)

New per-arc accumulator in the detector, mirroring `MwArcStats` (Welford + F1 lag + slip reset from the same funnel): the phase-only geometry-free bridge GF_b = φ_WL − φ_EWL (meters; both raw phases, ambiguities constant along the arc). Cycle assembly in the layer: Σc·GF_b + λ_EWL·∇ΔN_EWL^fixed = λ_WL·∇ΔN_WL + (β_EWL−β_WL)·Σc·Ī₁ + ε_φ, with β_EWL−β_WL ≈ 0.435 ⇒ the iono residue is ≈ 0.50 cyc per meter of cycle-DD iono (arc-window mean); amplified phase noise ≈ 0.08 cyc. So z_WL^bridge = [Σc·GF_b + λ_EWL·N_EWL]/λ_WL is a WL determination whose error is IONO-limited, not code-limited: for adjacent-station cycles (0.05–0.3 m DD iono) that is 0.03–0.15 cyc — FFR-grade within a few epochs, vs n≈25–50 for plain MW(1,2). **⚠ That "adjacent-station" premise is what §12.3/§12.7 falsify at network scale.**

- Requires the cycle's EWL integer (§3.2/§3.3) — the bridge lane arms only on EWL-fixed cycles.
- **Iono handling is MANDATORY, not optional (measured, Jul-16 oracle):** on the 2024-129 fixture (solar max, 2 days before the G5 superstorm) the UNCORRECTED bridge carries a ~0.5·DD-iono cyc bias that plateaus at med|f| ≈ 0.07–0.12 even on <50 km nearest-neighbor cycles (P(<0.15) only 0.60–0.81; implied DD iono ~0.2 m), and is uniform garbage beyond ~400 km — Welford pricing alone cannot reach FFR-grade. Therefore assemble the cycle statistic IONO-CORRECTED: subtract (β_EWL−β_WL)·Σc·Î₁ using the estimator's LAGGED per-LOS iono posterior (decision-side only; no row fights any carry), with R gaining the iono posterior variance. The Welford σ still prices residual scatter; `bridge_max_sigma_cyc` stays; `bridge_span_max_km` optionally gates by cycle station-span. Fallback: WL injects source from plain MW(1,2) at maturity (§3.5). ⚠ **§12.3 measured the de-bias NET-HARMFUL where the bridge worked** (gated wrong-integer rate <100 km: 0.0 % raw → 4.0 % phase-levelled code → 12.5 % stale arc-start seed).
- WL decision + inject: FFR σ < `wl_fix_sigma_cyc` (0.10), |frac| < `wl_fix_frac_cyc` (0.15) on z_WL^bridge → `injectLaneTarget(cycle, (1,2), N_WL)` + hold row. Cross-check (integrity, free): plain MW(1,2) must agree within its own σ — disagreement counts + blocks the inject (`inject_skip_closure`). ⚠ **§12.4: that closure backstop cannot catch the dominant wrong-by-ONE error** (tolerance 0.5+3σ ≈ 1.16 cyc at n=10), and the companion σ gate is STRUCTURALLY BLIND because a constant de-bias error contributes zero Welford variance.

### 3.5 Dual-freq fallback (GPS non-L5 sats)

Plain MW(1,2) statistic matures at σ ≈ 0.5–0.7/√n cyc ⇒ FFR-grade at n ≈ 25–50 epochs. Same FFR + inject + hold path, no bridge. Partial fixing propagates through the cycle graph: tri-freq fixes constrain mixed neighbors' search space (no special mechanism in v1 — the hold rows do it through the solve). ⚠ **NOT IMPLEMENTED — see the §4 `wl_inject_enable` row.**

### 3.6 NL endgame (unchanged, the original design intent)

With WL injected+held, the carried per-band DDs are WL-consistent; NL is the phase-determined direction the stiff carry already transports superbly. The netar probe (ils_max_dim capped) reads the constrained posterior for NL fixability — no new mechanism in this card; grading tells us when NL is reachable.

## 4. Config surface (all `network.mw_wl.*`, `loadNetworkSection_`)

| key | default | meaning |
|---|---|---|
| `ewl_enable` | false | Master for the EWL lane (stats scope, rows, statistic-gated fix; no inject — the lane is uncarried). Requires `carry_invariant`. OFF ⇒ byte-identical. |
| `ewl_min_arc_epochs` | 2 | Lagged Welford n before an EWL cycle is decision-eligible. |
| `ewl_fix_sigma_cyc` / `ewl_fix_frac_cyc` | 0.10 / 0.20 | FFR gate on the EWL statistic row. |
| `ewl_fold_rows` | false | Study knob: fold the fixed EWL rows into the solve. Default FALSE = the lane is DECISION-ONLY (§3.2 correction). |
| `wl_inject_enable` | false | Master for the WL lane (bridge on EWL-fixed cycles). Requires `carry_invariant` **and** `ewl_enable`. ⚠ **DOC + LOADER DEFECT: both this row and the loader message advertise an "MW(1,2) fallback" when `wl_inject_enable` is set without `ewl_enable`. That path DOES NOT EXIST** — the code reads `wl_inject_on = ewl_on && nmw.wl_inject_enable && !lane_on` (`src/core/app/ApplicationNetwork.cpp`, verified §12.2). Nothing arms §3.5. A code defect as well as a doc defect; fixing it means either implementing §3.5 or deleting the promise from the loader. |
| `wl_fix_sigma_cyc` / `wl_fix_frac_cyc` | 0.10 / 0.15 | FFR gate on the WL statistic (bridge or MW12). |
| `bridge_max_sigma_cyc` | 0.15 | Residual-honesty guard: no WL fix from a bridge statistic wider than this (post iono correction). |
| `bridge_span_max_km` | 0 | Optional cycle station-span gate on bridge-sourced fixes (0 = off). ⚠ §12.7 makes this the ONLY structural defence a 305-station roster has. |
| `inject_disagree_min_cyc` | 0.75 | Inject trigger threshold on \|statistic − carried lane\|. |
| `fix_weight_sigma_cyc` | 0.01 | Hold-row σ (key reused from WL-1; soft-hard, rollback-able). |

Deprecations: `fix_enable` / `fix_sigma_cyc` / `fix_frac_cyc` (WL-1 Phase-B posterior-gated surface, never implemented) — loader warns REPLACED by the statistic-gated keys above. `mw_wl.enable` (WL-1 float rows) is diagnostic-only; warn if armed together with `wl_inject_enable`.

## 5. Lifecycle / slip response (Invariant #1 pre-flight)

- Rows and fixes are RE-DERIVED each epoch from live (lagged) detector statistics — a slipped/reset arc fails eligibility next epoch and its fixes/holds vanish; no stiff assertion survives its evidence. Force-fire test per lane (§7.3).
- The inject writes carried VALUES — but the carry's existing lifecycle already guards those: detector slips funnel via `carry_slipped_ambs` (chord re-tapped fresh), D5 covers fine slips, and a re-tap regrows the inject trigger (§3.3), which re-seeds. The one NEW wiring is the one-epoch D5 exemption on inject; it must be force-fire tested (inject → assert exemption consumed → assert D5 live next epoch).
- EWL fix flip (statistic wandering across a rounding boundary): hysteresis + the `fix_flips` counter; a flip triggers a re-inject only through the normal disagreement gate.
- Coordinate with Card-4 unchanged: a chord refloat does NOT invalidate MW/bridge statistics; a DETECTOR slip invalidates both.
- ⚠ **Warm-resume gap (measured, §12.5b):** `MwSlipDetector` has NO serialize/restore, so on a warm resume every arc restarts immature. Any warm-start arm must allow ≥ `min_arc_epochs` (default 10) before the cascade can act.

## 6. Telemetry (Invariant #4)

Run-end lines per lane: cycles eligible / EWL rows decided / fixed / flips / unfixed (by reason) / injects (count, median |Δ| cyc) / holds active at end / skips {not-primary, missing band-3 column, missing MW pair, young arc, mixed frame, mixed channel, basis-not-1:1, closure-disagree, bridge-too-wide} / mean R per lane. Per-build counters accumulate on the ACCEPTED install only (WL-1 precedent). Config echo gains the new keys. The `f6_dump` instrument stays and gains lane columns; its row/block caps are `f6_max_rows` (8) and `f6_all_blocks` (false) — §12.7.

## 7. Validation plan (Fable; QUICK-ONLY per user directive Jul-15) — and its outcome

All on the `_wlver_*` 15-ep 50-sta GPS-only harness + unit gates. **NO long/graded runs until the user commands.**

1. **Byte-identity OFF** (same-build; all new toggles false). **TWO OFF baselines are required** because FrequencyMode Triple changes the estimator by itself: (a) the existing dual-freq config byte-identical vs the pre-card exe; (b) a Triple-freq config with the card OFF as the NEW control for every ON A/B.
2. **Unit battery:** (2,5)/(7,5) row math + λ table; bridge assembly hand value (incl. the λ_EWL·N_EWL term); primitive exactness (lane hits target, sum preserved to 1e-12, values transport under a basis re-expression); FFR + hysteresis; D5-exemption consume/expire; closure-disagree block.
3. **Falsifier FIRST (force-fire):** synthetic slip mid-window on a member arc — the EWL fix and WL hold must evaporate next epoch, the re-tap must re-trigger a fresh inject, and NO wrong integer is ever asserted (external truth: `scripts/mw_census.py` extended to (2,5) star DDs).
4. **15-ep A/B (the gate):** ON (ewl+inject) vs Triple-OFF control. PASS = (a) EWL fixes by ep ~3–4 on the L5 subset, WL injects by ep ~6–10, zero closure violations; (b) D5 flag rate at OFF level (the storm is the failure mode this card exists to avoid); (c) KF-WL circR on injected cycles integer-clustered (≫ OFF's uniform); (d) the station-1 x_lin pathology quarantined or fixed first (below).
5. Optional same-window 100/150-sta scale point.

**Outcome:** the card passed its quick gates on the 50-sta GPS harness (D5 358 at 15 ep, constrained circR 0.343 — the numbers WL-3 then improved on, `SPEC_lane_carry_card.md` §6). It was never graded long. **§12 is the only field measurement of it, and it is negative.**

**Pre-flight fix — ✅ LANDED (was blocking).** Station index 1's ~256 km ambiguity linearization seeds (`SPEC_mw_wl_card.md` §15) made correction-frame lane rows ill-conditioned for its cycles, and the probe upgraded this from cosmetic to real: station-1 cycles failed to converge even at 100× row authority (off by 8–14 cyc). **ROOT-CAUSED (Jul-16):** stations[1] = JPLM00USA (SEPT POLARX5, free-running clock ≈ 0.8557 ms at t=0; position ruled out, 0.45 m). `StateManager::computeAmbiguitySeed` (`StateManager.cpp:1211` — re-grepped 2026-08-15; the card's original `StateManager.cpp` ~1269 is stale) subtracts `clk_term = x(clock)`, which is **0 at the first network epoch** (no code-clock pre-solve exists on the network path), so the full c·dt_rcv lands UNIFORMLY in every ambiguity seed. Cold-seed-only (mid-arc reseeds subtract the converged clock correctly); **EVERY unsteered receiver in the fleet has this pro rata.** **Fixed by `cold_clock_seed`** (pre-seed a robust per-station code-clock estimate into x(clock) before the first ambiguity loop): `x_lin1_m` 269 352 m → **57 m** at station 1 (§12.6). CLAUDE.md's standard armed set lists it under cold-start hygiene as *"the 256 km x_lin cold-start leak fix — mandatory for MW-WL work"*; the quarantine workaround is obsolete. **Open sub-question that survives:** the affine argument says the float solve absorbs the seed benignly, yet the probe saw non-convergence at full authority — verify what ELSE the cold seed contaminates (co-cold iono seeds are the prime suspect).

## 8. Perf notes

Triple-frequency processing is the real cost (+⅓ GPS ambiguity columns, more measurement rows; Galileo more) — measure with D0/P0b at 50 sta before any talk of 305. Detector pair count grows with bands (all-pairs) — Mirror/Triple scope bounds it; stats are O(pairs) trivial. Lane rows are ≤ #chords each, 4-term rows — the fold absorbs them like DD priors (WL-1 measured small). `max_rows_per_epoch` extends per lane if needed (count drops — no silent caps). ⚠ **The measured cost at 305 stations is elsewhere and it is large** — §12.6, and `SPEC_lane_carry_card.md` Appendix LF-1.

## 9. Deferrals (triaged per Invariant #3)

- **Field-inert:** Galileo 19.54 m AltBOC pairs (9.768 m already single-epoch; data has L8Q when wanted — later promoted by UWL-1 increment 5); BDS/QZSS lanes; iono columns in bridge rows (v2; Welford σ + guard price it in v1); general non-1:1 cross-basis inject (counted; 1:1 covers the common case); EWL→WL→NL netar coupling changes (read-only consumer); GLONASS (excluded upstream).
- **Field-blocking if skipped:** the D5 inject exemption (without it the card re-creates the F6 storm); the force-fire battery (§7.3 — stiff assertions with untested un-fix violate Invariant #1); the station-1 seed fix (§7 pre-flight — now LANDED as `cold_clock_seed`); the Triple-OFF control baseline (§7.1b — without it every A/B confounds band scope with the card).

## 10. Verified anchors (Jul-15 eve tree — ⚠ LINE NUMBERS ARE STALE)

⚠️ `src/core/app/ApplicationNetwork.cpp` was decomposed (INC-0..12) from ~14.7k lines to ~2.4k. The `ApplicationNetwork.cpp ~6562–6693` anchor below is DEAD. **Prefer the §12.2 anchors** (`NetworkMwWlGlue.cpp`, `GnssConfigLoadNetwork.cpp`, `NetworkDdPriorBuild.cpp`, `MeasurementModel.cpp`) — verified 2026-07-28 — and re-grep before editing anything.

- Detector all-cross-band pair builder, frequency-descending, same-channel code matching — `MwSlipDetector.cpp` ~437–606; band-scope knobs `mw_frequency_mode` (Mirror/Dual/Triple/All) + `mw_channel_mode`.
- Stats keyed `sat_sig1_sig2`; `arcZdMean` pair-generic, F1-lagged, ZD-consistent (`MwSlipDetector.cpp` ~179–234); MAD threshold already EWL-aware ("L5X/L8X at λ ≈ 19.5 m", `MwSlipDetector.hpp` ~842).
- WL-1 substrate to extend (not fork): layer `NetworkMwWlLayer.{hpp,cpp}` (row build, F2 shared-arc Pz, F5 correction frame); wiring `ApplicationNetwork.cpp` ~6562–6693 (revmap / arc_signals / primary pair / λ cache / `lin_of`); `DdPriorBlock::Term` general rows + fold `ũ += G·M·(z_free−ẑ)` (`NetworkNormalEquations.hpp` ~349, ~379).
- Carry driver owns cycle algebra + `installPosterior` + D5 + the slip funnel (`NetworkInvariantCarryDriver.hpp`) — the primitive and exemption live there.
- Fixture data (2024-129, sampled ABMF/ABPO/ADIS/AGGO/AIRA/AJAC): GPS C5Q|X/L5Q|X everywhere sampled; Galileo C5Q/C7Q + C8Q/L8Q present. `_wlver_50sta_15ep_*` = the harness (incl. the `_f6` and `_probe` ON variants); diagnostics `network.mw_wl.f6_dump` + `network.mw_wl.debug_r_scale` (probe-only, loud warn, NOT production).
- F6 evidence: `SPEC_mw_wl_card.md` §15; memory `mw-wl-f6-fix-directions` (includes the probe result + the held (B) scope sketch, built as WL-3).

## 11. Open questions for the implementer

1. **Triple band scope:** what `getAllowedBandDigits` returns per constellation under Triple, and the Galileo third-band choice for the ESTIMATOR (E5b vs E6) vs the EWL pair (7,5) — they need not coincide; the resolver just needs live columns for bands 5 and 7. **Q1b — third-band carry exclusion seam:** confirm the (const, third-band) groups can be dropped from the carry driver's GroupInput list while still passing through the eq34 election/datum normally, and that OFF byte-identity is preserved when `ewl_enable=false` under a Triple config (the §7.1b control).
2. **Bridge iono — ✅ ANSWERED TWICE.** (a) Jul-16 oracle: Welford-only pricing does NOT suffice, correction is mandatory (§3.4). (b) **WITH FIELD DATA, §12.3: the bridge's iono term is LOCAL and does not survive a global network's chord geometry** — z_WL = N₁₂ − 0.5049·I₁_dd[m], so a 0.1-cycle lane needs DD ionosphere to 0.198 m against 1.6–3.4 m on this roster's chords = **8–17× the entire error budget**. `bridge_span_max_km` is therefore the only structural defence, and §12.7 says it would exclude 99.7 % of the lane cycles.
3. **Primitive placement:** driver-level API vs ApplicationNetwork seam; how the 1:1 cycle-support check reads both groups' bases cheaply.
4. **Hold rows vs re-inject:** shipped with holds. *(Moot on the WL-3 shape — the idempotent lane SET replaced injects entirely, `SPEC_lane_carry_card.md` §10 deviation 3.)*
5. **WL-1 float-row disposition:** keep `mw_wl.enable` as diagnostic — **DONE, §3.0.2.**
6. **Dual-freq inject maturity:** *(§12.4 measured MW(1,2) at 1.6–1.9 % wrong at EVERY baseline bucket over n = 5…60 — baseline-independent but NOT FFR grade for a stiff carry.)*

---

# 12. DOY-129 305-station field verdict (measured 2026-07-28)

*Was `docs/REPORT_mwwl_conditioning_day129.md` (now `docs/attic/`), absorbed 2026-08-15. Question: can the MW/WL cascade CONDITION the DOY-129 305-station network against the SH detonation?*

**Verdict: no — and the ordering question has a third answer.** The cascade is neither a conditioner nor a victim of the SH-detonation disease. It is **decoupled** from the disease (its de-bias never reads the solve) and **geometrically dead** on a 305-station global network (its only estimator-facing constraint is manufactured by a phase bridge whose error budget is 8–17× smaller than the DD ionosphere on the chords this network actually forms). Arming it as shipped would write **wrong-by-one-cycle widelane integers into the stiff `q_z = 1e-4` carry at a 27–71 % rate** — a standing-invariant-#1 violation, not a stiffening.

## 12.1 The disease does not touch the data, or the cascade's information  *(REPORT §1)*

Estimator-free census of all **305 roster stations**, raw RINEX, GPS, 17:30–21:30 UTC (481 epochs at 30 s), spanning the 19:26:00 (tow 329160) SH detonation, the 19:34 epicentre spike and the 20:47 ratchet.

| band | dI_med (m/30 s) | dI_p99 | icres_med (m) | icres_p99 | LLI/ep | MWjump/ep | EWL med\|f\| | EWL circR |
|---|---|---|---|---|---|---|---|---|
| quiet 17:35–19:00 | 0.0178 | 0.202 | 1.82 | 34.7 | 28 | 88 | 0.024 | **0.969** |
| pre 19:00–19:21 | 0.0175 | 0.277 | 1.54 | 28.1 | 31 | 90 | 0.024 | **0.979** |
| **detonation 19:26–19:31** | **0.0176** | 0.289 | **1.27** | 29.8 | 31 | 89 | **0.024** | **0.976** |
| spike 19:31–19:40 | 0.0173 | 0.218 | 1.29 | 31.5 | 34 | 94 | 0.020 | 0.975 |
| ratchet 20:47 | 0.0175 | 0.220 | 1.80 | 26.8 | 35 | 101 | 0.026 | 0.978 |

`dI` = |Δ slant ionosphere from PHASE| per epoch (network median over ~3060 LOS). `icres` = |code-minus-phase ionosphere| after per-arc levelling — **the error the code-only SH field actually eats**.

- **No data-side insult at 19:26.** Phase TEC rate flat to ±2 % across four hours; the code-side ionosphere residual is *lower* during the detonation (1.27 m) than quiet (1.82 m); LLI and MW-jump rates unchanged. Confirms the state-endogeneity verdict from `runs/raim_cold_event` **from a completely independent instrument** — observations rather than filter state.
- **The cascade's extra-widelane information is pristine and unmoved:** EWL(2,5) DD median |frac| 0.019–0.029 cyc (0.020–0.026 across the five bands above), circR 0.969–0.979, unchanged through every event. L5 present at **293 / 305** stations (297 declare it — 4 are the header≠data trap); ~52 % of LOS carry L5 (18 of 32 GPS sats).

⇒ the **"cascade is a victim of the SH disease" hypothesis is FALSIFIED at the source**.

## 12.2 The only estimator-facing constraint is manufactured by the bridge  *(REPORT §2)*

Structural audit, all claims file:line-verified and adversarially re-checked. ⚠ **These anchors are as-of-2026-07-28**; `ApplicationNetwork.cpp` has since been decomposed to ~2.4k lines, so re-grep the symbol, not the number. (Verified 2026-08-15: the `wl_inject_on` expression is unchanged, now at `ApplicationNetwork.cpp:775`; the `ewl_fold_rows` gate at `NetworkMwWlGlue.cpp:1018`; the EWL-fixed walk gate at `NetworkMwWlGlue.cpp:453`; the loader's MW(1,2)-fallback message at `GnssConfigLoadNetwork.cpp:6423`; the lane_carry-requires-ewl_enable fail-loud at `GnssConfigLoadNetwork.cpp:6569`; the frozen arc-start iono seed at `MeasurementModel.cpp:1459-1467`. The as-of-2026-07-28 numbers below — `NetworkMwWlGlue.cpp:1004`/`:439`/`:160`, `GnssConfigLoadNetwork.cpp:2714-2720`/`:2588-2592`/`:6324-6328`, `ApplicationNetwork.cpp:575`, `MeasurementModel.cpp:414`, `NetworkDdPriorBuild.cpp:206-208`/`:606`,`640` — are kept as the audit's own record and are STALE.)

| link | finding |
|---|---|
| EWL rows → normals | **Never.** `if (nmw.ewl_fold_rows) …` — default **false**; the EWL lane is decision-only (`NetworkMwWlGlue.cpp:1004`). |
| WL decision source | **Bridge only.** The walk is gated `(wl_inject_on \|\| lane_fix_on \|\| lane_bridge_on) && !ewl_fixed.empty()` (`NetworkMwWlGlue.cpp:439`). |
| an iono-free WL arm? | **Not constructible.** `lane_carry.enable` *requires* `mw_wl.ewl_enable` (`GnssConfigLoadNetwork.cpp:2714-2720`, fail-loud), and `bridge_meas=false` suppresses only the raw-GF *increment*, not the decision. **THE LOADER LIES:** its promise of an "MW(1,2) fallback" when `wl_inject_enable` is set without `ewl_enable` (`:2588-2592`) describes a path the code does not implement — `wl_inject_on = ewl_on && …` (`ApplicationNetwork.cpp:575`). |
| bridge ↔ SH circularity | **None.** The de-bias reads `x_lin(iono_idx)` — a **frozen, arc-start, code-derived seed** (`MeasurementModel.cpp:414`, never written back) — not the solve. `networkMwWlInject`'s signature takes no estimator, no `x_s`, no `sh_model`. |
| ambiguity → SH coupling | **Live within one epoch.** Ambiguity and slant-iono share `N_rr`; carried DD priors fold post-Schur through `Gacc += N_sr · Y` onto the SH shared rows, with `M = (Pᶻ+B)⁻¹` — so stiffening *amplifies* the SH perturbation. Magnitude not measured. |

The conditioning **mechanism** therefore exists. What does not exist is a clean way to drive it: the disease-immune rungs (EWL, MW(1,2)) are deliberately kept out of the normals, and the one rung that reaches the normals is the iono-sensitive bridge.

## 12.3 The bridge is a short-baseline instrument, and this network has no short baselines  *(REPORT §3)*

Bridge oracle, 183-station (W,Q)-channel group, truth = whole-window MW(1,2) arc median, n = 10 trailing epochs, **at the detonation epoch 19:26:00**:

| baseline bucket | DDs | med \|DD iono\| | **bridge integer hit** | de-biased hit | MW(1,2) hit |
|---|---|---|---|---|---|
| < 100 km | 68 | 0.19 m | **0.971** | 0.868 | 0.985 |
| 100–400 km | 219 | 0.48 m | **0.945** | 0.685 | 0.968 |
| 400–1500 km | 682 | 0.71 m | **0.628** | 0.613 | 0.957 |
| 1.5–4 Mm | 896 | 1.63 m | **0.287** | 0.257 | 0.933 |
| 4–12 Mm | 243 | 3.35 m | **0.091** | 0.123 | 0.897 |

The mechanism is arithmetic: `z_WL = N₁₂ − 0.5049 · I₁_dd[m]`, so a 0.1-cycle lane needs the DD ionosphere to **0.198 m**. On global chords the unmodelled DD ionosphere is 1.6–3.4 m — **8–17× the entire error budget**. **This answers `SPEC_ewl_wl_cascade_card` §11 Q2 with data: the bridge's iono term is LOCAL, and it does not survive a global network's chord geometry.**

**De-biasing does not rescue it — it destroys the one regime where the bridge worked.** Gated wrong-integer rate at 19:26 with three de-bias inputs (the third proxies what the code actually subtracts, `x_lin(iono_idx)` = a frozen arc-start code seed, modelled as the code ionosphere ~2 h earlier):

| bucket | raw bridge | de-biased, phase-levelled code | de-biased, **stale arc-start seed** |
|---|---|---|---|
| < 100 km | **0.0 %** | 4.0 % | **12.5 %** |
| 100–400 km | **1.4 %** | 23.2 % | **17.6 %** |
| 400–1500 km | 28.2 % | 25.4 % | 29.0 % |
| 1.5–4 Mm | 59.7 % | 71.9 % | 65.7 % |
| 4–12 Mm | 91.3 % | 75.0 % | 95.8 % |

The de-bias input's own error exceeds the bias it removes wherever the bias was small, so it is **net-harmful at short baselines and no help at long ones**. *(Proxy caveat: the shipped path de-biases per sample inside the Welford with a `computeIonosphereSeed` value; the proxy is directionally the same but the magnitudes are indicative, not exact.)*

**Roster geometry** (305 stations): nearest-neighbour median **232 km** and **68 %** of stations have a neighbour within 400 km — but only **0.85 % of the 46 360 station pairs** are under 400 km, and the all-pairs median is **8341 km**. Chord selection is therefore worth ~10× in bridge yield, and today it is not geographic: `tree_edge` comes from the eq34 pinned-ambiguity election (`NetworkDdPriorBuild.cpp:606,640`), not from proximity.

## 12.4 What actually reaches the stiff carry: the wrong-integer injection rate  *(REPORT §4)*

Applying the **shipped** FFR gate `|z − round(z)| < 0.15` (`network.mw_wl.wl_fix_frac_cyc`):

| bucket | bridge admits | **of admits, WRONG** | \|err\|=1 | \|err\|≥2 | after MW(1,2) closure | MW(1,2) admits | MW WRONG |
|---|---|---|---|---|---|---|---|
| < 100 km | 85.3 % | **0.0 %** | 0.0 % | 0.0 % | 0.0 % wrong | 61.8 % | 0.0 % |
| 100–400 km | 47.0 % | **1.9 %** | 1.0 % | 1.0 % | 1.0 % wrong | 47.9 % | 1.9 % |
| 400–1500 km | 30.5 % | **34.1 %** | 29.8 % | 4.3 % | **27.5 % wrong** | 57.5 % | 1.8 % |
| 1.5–4 Mm | 31.8 % | **69.8 %** | 42.8 % | 27.0 % | **51.1 % wrong** | 55.0 % | 1.6 % |
| 4–12 Mm | 29.6 % | **91.7 %** | 23.6 % | 68.1 % | **71.4 % wrong** | 48.1 % | 1.7 % |

Three things this establishes. (1) **The gate's admit rate saturates at ~30 %** on long baselines — matching, to within measurement noise, the *independently derived* prediction from the code: the frac gate is periodic, admitting within ±0.297 m of every multiple of 1.980 m, i.e. a **30 % duty cycle** on the iono-error axis. Theory and measurement agree. (2) **The dominant error is wrong-by-ONE cycle**, exactly what the ±1.98 m periodicity predicts for 0.7–1.6 m of DD ionosphere — and the shipped MW(1,2) closure backstop (tolerance `0.5 + 3σ` ≈ **1.16 cyc at n = 10**) **cannot catch it**; after the closure, 27–71 % of surviving admissions are still wrong. The companion σ gate is **structurally blind**: a *constant* de-bias error contributes zero Welford variance (the code records this falsification itself at `NetworkDdPriorBuild.cpp:206-208`). (3) **The iono-free MW(1,2) statistic is ~40× more reliable at long baselines** — 1.6–1.9 % wrong at *every* bucket, baseline-independent — **and it is the rung that cannot decide.**

**This is disease-independent.** Repeating at a quiet epoch (18:30:00) gives bridge-wrong **0.0 / 1.0 / 32.7 / 59.2 / 87.3 %** — the same. The bridge does not fail *because* of the detonation; it fails on geometry, all day.

MW(1,2) arc-length sweep (hit rate, worst→best bucket): n=5 0.81–0.94 · n=10 0.90–0.99 · n=20 0.91–1.00 · n=30 0.95–1.00 · n=60 0.96–1.00. It **plateaus near 0.96** rather than falling as √n theory predicts — a systematic per-arc floor. *(Caveat: the truth is a long-arc MW(1,2) median, so MW's own rate is optimistic by correlation; the bridge's is not. The two agree at <100 km, which cross-validates the truth.)*

## 12.5 Two mechanism findings worth keeping (both cross-card)  *(REPORT §5, items (a)/(b))*

**(a) The D5 storm ERASES the SH deviation pins.** `carry_slipped_ambs` erases `sh_dev_carry` per (station, satellite), and those entries supply `r.innov` / `r.var` for the SH pin rows. A 92 %-re-tap seam therefore mass-reverts every per-LOS SH deviation pin to the cold seed (dev 0 at full representativeness σ) — **a direct, solve-affecting mechanism for the SH field going code-determined**, independent of the lane machinery. A concrete causal link for `project_day129_gate_policy` defect 2.

**(b) The out-of-band memory is real WITHIN a run, and ABSENT across a resume.** When D5 re-taps a chord, everything the lane put *inside* the carry is erased (`d5GateHalf` / `laneFixErase`), but the decision source — the per-station `MwSlipDetector` Welford accumulators — is untouched, so a re-tapped chord can be re-pinned at full strength in **one epoch**. Precisely the capability the 92 %-re-tap collapse needs. **But `MwSlipDetector` has no serialize/restore**, so on a warm resume every arc restarts immature. Any warm-start arm must allow ≥ `min_arc_epochs` (default 10) first.

**Cost:** arming `lane_carry` flips the carry into joint 2K pair blocks, and `resolve_dropping()` re-runs `buildCarryDdPriors()` on every RAIM adapt pass (up to `kMaxAdapt = 5`) — so the cascade is **up to 6× more expensive during a detonation**, exactly when it is least able to help.

## 12.6 Estimator-side smoke and cost (305 sta, cold at 19:00)  *(REPORT §5b)*

`runs/mwwl_cond/casc` — guards_cont leg shape + `ewl_enable` + `uwl_enable` + `lane_carry.enable` + `bridge_sample_debias`, 8 Eigen threads, stopped after epoch 3.

- **Cost: 88.6 then 75.7 s per epoch.** `solve-glue` dominates at 33.7–35.7 s; `dd-build` only 0.6–5.9 s; `install-post` 9.1–11.8 s — the lane bill lands on the joint-2K carry stages, as the audit predicted, not on dd-build.
- **Clean incremental measurement of `lane_carry`** (same build/threads/epochs): `ewl_enable` + WL-1 float layer **without** lane_carry (`runs/mwwl_cond/geom`) runs **56.2 / 48.6 / 44.3 / 47.6 s/ep**; adding it takes it to 88.6 / 75.7 ⇒ **lane_carry alone costs 1.6–1.9×**, the joint-2K pair-block flip. (The anchor's 45–52 s/ep is cascade-OFF but at 20 threads — not a clean comparator.)
- ⚠️ **A cold 305-station start at 19:00 emits NOTHING**: the SH gate engages at the first epoch (`a00 −1406.9 TECU, rms_all 352.74`) and the run ends *"0 epochs emitted, 0 skipped … of 2285 synced"*. **Cold micro-windows are engagement/cost probes only.**
- **Lane population: 2317–2328 primary cycles/epoch** in the `G L1C+L2W [PAIR]` group (2652 arcs) — but ~2888 arcs stay in **residual per-band** groups (L2L 1054, L2X 726, L1W 553, L2S 244, L1L 206, L1X 90, L1C 15), i.e. **roughly half the arc population is outside the lane pair basis** at 305 stations (the UWL card's `skip_basis` at scale).
- `decided = held = injected = 0` for all 4645 traced rows — correct for a cold epoch 1–2 (EWL needs n≥2, MW increments n≥10).
- **Campaign scheduling law, measured on the same box** (the durable part of the session machine-state note): the 305-sta anchor `gnss_processor_gcont` ran healthy at **~43–52 s/ep** (ep 0115 at 21:33) on 20 threads / 33 GB, against these arms' 8 Eigen threads / ≈48 GB, on a 20-core / 137 GB machine — both fit, but they share cores. At ~45 s/ep the anchor needs **≈ 8 h to reach ep 0721** (19:00 content, the warm-start anchor point) and ≈ 11 h to reach 21:29:30, so warm-start arms off that history are not available before roughly **05:30–06:00**.

**Run-end counters** (`runs/mwwl_cond/geom_ccs`, 305 sta, 14 epochs, `cold_clock_seed` armed) — estimator-side confirmation of §12.1 and §12.2:

- **WL-2 EWL lane: 3372 rows decided, 3357 INTEGER-FIXED via FFR, mean R = 0.0001 cyc²** over 32 850 primary cycles inspected — and the counter line says it outright: *"rows NOT folded into the solve"*. Near-perfect at 305-station scale, **contributing nothing to the estimate**.
- WL-1 float layer: **4399 WL rows folded, mean R = 0.0984 cyc² (σ ≈ 0.314 cyc)** — the honest MW code-noise level, matching the census.
- **`cold_clock_seed` works and the guards_cont lineage lacks it**: `x_lin1_m` **269 352 m (269 km) → 57 m** at station 1. Arm it for any lane work here (memory `project_network_coldseed_clock_leak`; §7 pre-flight).

## 12.7 ★ Lane-cycle geometry — MEASURED, and it closes the argument  *(REPORT §5c)*

The F6 dump was hard-capped at 8 rows of one block (`nrow = std::min(wb.rows.size(), 8)`, `NetworkMwWlGlue.cpp:160`). Two log-only knobs now lift it — `network.mw_wl.f6_max_rows` (default 8) and `network.mw_wl.f6_all_blocks` (default false), both reproducing the legacy dump exactly.

> **RETRACTION (kept so the error is not re-made):** an earlier revision inferred a "99.7 % `x_lin` quarantine" from that hard 8. **That was WRONG** — arming `cold_clock_seed` fixed `x_lin` by four orders of magnitude and the dump still returned exactly 8. Retracted.

**The structural prediction this measurement confirmed** (REPORT §5b): the lane trace records the chord `(station, sat)` but the cycle closes through the eq34 tree, whose path it does not record. Structural inference — **the Eq-34 datum is a single-receiver chain, ONE datum receiver per (constellation, signal) component, so a chord's cycle closes back through that datum receiver, making the effective DD baseline global (roster all-pairs median 8341 km), i.e. §12.3's bottom bucket.** That per-component granularity is what makes the inference transferable to other rosters.

Uncapped run (`runs/mwwl_cond/geom_full`, 305 sta, `cold_clock_seed` armed): **939 cycles, 4410 arcs.** Cycle span = max pairwise station distance inside the cycle (the ionosphere lever arm), against the buckets §12.3/§12.4 measured the bridge in:

| bucket | share of lane cycles | bridge WRONG % (§12.4) | after MW closure |
|---|---|---|---|
| < 100 km | **0.1 %** | 0.0 % | 0.0 % |
| 100–400 km | **0.2 %** | 1.9 % | 1.0 % |
| 400–1500 km | 7.5 % | 34.1 % | 27.5 % |
| 1.5–4 Mm | 19.6 % | 69.8 % | 51.1 % |
| **4–12 Mm** | **72.6 %** | **91.7 %** | **71.4 %** |

Span distribution: min **19 km**, p10 **1644**, **median 7433 km**, p90 **10 756**, max **12 658**. **92.2 % of lane cycles are ≥ 1500 km; only 0.3 % land in the < 400 km regime where the bridge works.** Cycles span 2 / 3 / 4 distinct stations (**671 / 209 / 59**) — a third are multi-station tree paths, i.e. longer lever arms still.

**Folding the measured per-bucket wrong-rates against the measured cycle population:**

> **≈ 83 % of gate-admitted WL integers on this network would be wrong**, and **≈ 64 % still wrong after the MW(1,2) closure backstop.**

The star structure is visible too: **`BELE00BRA` appears in 679 of 939 cycles (72 %)** — the eq34 datum receiver for that component, so cycle baselines are essentially (station → datum receiver) distances: the single-receiver-chain inference above, confirmed. **Which is why lever 1 (geographic chord selection) is the highest-value fix: the population is not inherently global, it is global because the datum tree is a star.**

## 12.8 Ordering verdict, ranked levers, and the do-not-arm recommendation  *(REPORT §6)*

**The disease must be addressed first — but not for the reason the framing assumed.** The cascade is decoupled from the SH field; it is not poisoned by the detonation. It cannot condition this network because its WL-manufacturing path is *geometrically* dead here, and it would be actively harmful if armed. Fixing the disease would not by itself make the cascade a conditioner; the geometry fix is independent and must come first. Ranked levers:

1. **Geographic lane-chord selection.** The bridge is 0–1.9 % wrong below 400 km and 68 % of the roster has a neighbour within 400 km. Preferring proximity in the pair/chord election would move most cycles into the working regime. **Largest single win; no new estimator maths.**
2. **An MW(1,2)-decided WL (§3.5 / increment 4), hard-FFR-gated.** Iono-free and baseline-independent, but **1.6–1.9 % wrong at n=10–60 is NOT FFR grade for a stiff carry** (invariant #1) — it needs a gate that reaches ≲1e-4, i.e. long arcs plus the closure, or it must enter as a soft prior rather than a set.
3. **Re-measure the `ewl_fold_rows` trade under a SICK network.** Folding was rejected on a D5 cost measured in a *healthy* one (679 vs 348). The EWL is the only integer information that survives a detonation intact (circR 0.976 through it); the trade should be re-taken where it matters.

**Recommendation on warm-start arms: do NOT arm `network.mw_wl.*` / `network.lane_carry.*` on a DOY-129 305-station resume.** The treatment is predicted — from measurement, not speculation — to inject wrong-by-1 widelane integers into the stiff carry across most of the DD population. **Arming on a RESUME additionally changes carry MEMBERSHIP** (`ewl_carry_exclude` drops every GPS L5 ambiguity) with **no fingerprint check**, where a shape mismatch is a silent per-block skip that still logs "BRANCH-FAITHFUL". If a field arm is wanted, the informative one is lever 1's A/B, not the stock cascade.

## 12.9 Instruments left behind  *(REPORT "Instruments left behind")*

- `scripts/mwwl_event_census.py` — `parse` / `analyze` / `oracle`. Estimator-free, windowed, cached (re-analysis is seconds). `oracle` grades any lane against a long-arc MW truth, bucketed by baseline, and reports the shipped-gate wrong-integer injection rate.
- `scripts/mwwl_cond_arm.py` — derives `ctl` / `lane` / `casc` arms from an anchor leg's own `leg.json`; documents the loader interlocks in its header.
