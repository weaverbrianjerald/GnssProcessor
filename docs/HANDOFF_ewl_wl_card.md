# HANDOFF — EWL-first tri-frequency cascade (M5 card WL-2)

**Implements** `docs/SPEC_ewl_wl_cascade_card.md` (Fable). Opus implementing, Fable verifies.
Default-OFF / byte-identical. **Status: Increment 1a (FOUNDATION) DONE + verified (Opus, Jul-15 eve).**

---

## Increment status

| # | Scope | State |
|---|---|---|
| **1a** | Config surface + detector gate + **third-band carry exclusion** | **DONE + verified** |
| **1b** | EWL statistic lane (§3.2): (2,5) rows, FFR fix, integer-hard fold, station-1 quarantine | **DONE + smoked** |
| **2a** | WL inject **primitive** on the carry driver + 1-epoch D5 exemption (§3.3) | **DONE + force-fire tested** |
| **2b** | Inject **wiring** in ApplicationNetwork (bridge → FFR/closure → hold rows + post-solve inject) | **DONE + smoked (Opus, Jul-16)** |
| **3** | WL phase bridge (§3.4) — new GF_b accumulator (the WL target source) | **DONE + unit-gated (Opus, Jul-16)** |
| 4 | Dual-freq MW(1,2) fallback (§3.5) + telemetry (§6) | after Fable's A/B |

**Telemetry (§6)** landed with 2b (per-lane run-end line + counters); the MW(1,2)
fallback (§3.5) is the only remaining lane and is deferred to after Fable's 15-ep A/B
(the bridge path is the v1 target — see §11 Q6).

## 2a — WL inject primitive (DONE + force-fire tested)

- `NetworkInvariantCarryDriver::injectCarriedChord(group, chord, new_z, new_var)` — edits the
  carried DD value (correction frame; row/col cross-terms zeroed → PSD) and marks the chord
  D5-EXEMPT for the NEXT `installPosterior` via a driver-internal one-epoch set `gate_exempt_`
  (cleared after every install; also cleared in `reset()`). Both install paths (serial +
  parallel) skip the σ̂₀ feed + flag for an exempt chord. Read half: `carriedChordValue(...)`.
  Chose the driver-internal set over threading `ArcInput.gate_exempt`/`BlockMeta` — same
  one-epoch semantics, far less surface.
- **Force-fire tests (`tests/test_network_invariant_carry_driver.cpp`, both green; suite 741/742,
  the 1 a pre-existing skip):** `InjectLaneTargetEditsCarriedValue` (edit + read-back + the
  not-1:1 false paths); `InjectD5ExemptionSuppressesReTap` (control-vs-exempt — the SAME 8-m-off
  carried DD flags the D5 gate WITHOUT the exemption and does NOT with it). Satisfies invariant #1
  (stiff assertion with a wired + tested event response).
- OFF byte-identity re-confirmed (the inject is unreachable until 2b wires it; `gate_exempt_`
  stays empty ⇒ the carry is byte-unchanged).

## 2b + 3 — LANDED (Opus, Jul-16) — the WL lane closes the F6 loop

EWL-fixed cycle → **bridge** WL target → **hold row** (when the carry already agrees) OR
**inject** (post-solve, when it disagrees). Three seams, all wired + gated default-OFF:

1. **Bridge accumulator (detector).** `MwSlipDetector::updateBridgeArc` — a LEAN per-arc Welford
   (`BridgeArcStats` + `bridge_stats_`) of `GF_b = λ_WL·(φ1−φ2) − λ_EWL·(φ2−φ5)` (phase-only,
   ZD, meters; the raw-cycle identity of `φ_WL−φ_EWL`), fed once per obs inside `detectLargeMwResets`
   (right after the elevation gate / `has_lli`), with the same F1 lag as `arcZdMean`, a gap reset,
   a gross-jump refloat (`kBridgeJumpResetM`=0.4 m, > iono residue, < a 1-cyc WL slip), and slip
   response from the SAME funnel (`resetSatellite`/`resetSatelliteSignal`/`resetAll` erase the
   matching triple; LLI re-warms). Armed only via `setBridgeEnabled(wl_inject && ewl)` ⇒
   byte-identical OFF. Accessor `arcBridge(sat,sig1,sig2,sig5)` → lagged mean/var/λ_WL/λ_EWL.
   NOTE realised: `GF_b` is ZD (no SD pivot needed — biases cancel in the closed DD cycle), which
   is why it is far simpler than the MW `arcZdMean` reconstruction. **No slip DETECTOR of its own**
   — the funnel + the §3.4 bridge-σ guard are the wrong-integer backstop.
2. **EWL-integer exposure (layer).** `NetworkMwWlLayer::build` gains an optional
   `std::vector<FixedEwlRow>*` out-param: per FFR-fixed row it emits `{block, row, round(z_abs)}`.
   The caller maps `(block,row)` → chord `ArcKey` via `carry_driver.blockChordsForTest(block)[row]`.
   OFF / WL-1-float path emits none (byte-identical).
3. **Bridge + inject wiring (ApplicationNetwork, `buildCarryDdPriors` + post-`installPosterior`).**
   Per fixed EWL cycle: walk the carry-block row's arcs, sum `Σc·GF_b`, form
   `z_WL = (Σc·GF_b + λ_EWL·N_EWL)/λ_WL`, `w=round`. Guards: bridge σ < `bridge_max_sigma_cyc`,
   FFR (σ<`wl_fix_sigma_cyc`, |frac|<`wl_fix_frac_cyc`), MW(1,2) closure (|z_mw12−w| ≤ 0.5+3σ_mw12),
   `lane_max_abs_xlin_m` quarantine. Decision is stateless (re-derived each epoch) — no cross-epoch
   cycle-identity persistence needed (the per-arc detector stats + the per-epoch EWL round ARE the
   state). Then split by the carried lane vs `w`:
   - **agree** (|w−(A1−A2)| ≤ `inject_disagree_min_cyc`, pre-solve): fold an integer-hard **hold row**
     (WL correction frame, σ=`fix_weight_sigma_cyc`) on (col1,col2), grouped by source block ⇒ WL↔WL
     joint. Its innovation is ~0 ⇒ no F6 fight.
   - always **stash an inject plan** (group ids, chord, `w`, `z_lin1/2`, λ1/2); applied POST-`installPosterior`
     against the FRESH posterior: read both bands' carried chords (`carriedChordValue`; either missing
     ⇒ not-1:1 ⇒ `skip_basis`), `A1=(z1+z_lin1)/λ1`, `A2=…`, `s=A1+A2`; if `|w−(A1−A2)| > inject_disagree_min`
     ⇒ `A1'=(w+s)/2`, `A2'=(s−w)/2`, `injectCarriedChord(group_L1,chord, A1'·λ1−z_lin1, var)` + L2. The
     primitive marks each moved chord D5-EXEMPT for the next install internally (§3.3 storm guard).
   Per-lane run-end telemetry (§6): rows_seen / cycles_decided / holds / injects (mean |Δlane|) /
   skips {bridge-missing, bridge-wide, FFR-unfixed, closure, quarantine, not-1:1}.

**Unit gates (Debug, all green; suite 753 run / 752 pass / 1 pre-existing skip):**
- `tests/test_mw_bridge.cpp` (NEW, 7 gates): GF_b hand value + λ table, disarmed-never-accumulates
  (invariant #6), lagged maturity, gap refloat, gross-jump refloat, the full reset funnel (per-signal
  any-of-3-bands / per-sat / full), LLI re-warm.
- `tests/test_network_mw_wl_layer.cpp` (+3): FFR fix exposes a `FixedEwlRow`; unfixed / float lane
  expose nothing.
- `tests/test_network_invariant_carry_driver.cpp` (+1): the inject lane-target arithmetic — lane hits
  the integer, NL sum preserved to 1e-12 (§7.2 "primitive exactness").

**Smoke (10-ep 50-sta GPS-only, `_wlver_50sta_10ep_wlinj.json`, Release, ~21 s) — ARMS + FORCE-FIRES,
NO STORM (iono-de-biased, §3.4 v2):** EWL 98/98 fixed (unchanged from 1b ⇒ no regression). WL lane: 98
EWL-fixed rows bridged → **29 WL cycles decided** (mean bridge R 0.0040 cyc², σ≈0.063), **19 hold rows
folded, 9 injects** (mean |Δlane| = **3.56 cyc** — the carried lane ~3.6 cyc off the truth, the F6
code-seed wall, moved onto the integer). Guards firing: 33 no/young bridge arc, 0 no-iono, 4 bridge-σ,
32 FFR-unfixed, **0 MW(1,2) closure** (the de-biased integers agree with the iono-free MW — the decisive
check that the de-bias is correct; v1 without de-bias had 9 closure blocks + only 5 decided), 0
quarantine, 0 not-1:1. **D5 = 381 flags (< 393 EWL-only ⇒ no storm; the injects/holds even stabilise
it slightly).** No warnings/errors/NaN. New exe `gnss_processor_wlinj_run.exe`; new config
`_wlver_50sta_10ep_wlinj.json`. Nothing committed.

**Still owed to Fable (§7.3/§7.4, graded — NOT run by Opus per QUICK-ONLY):** the end-to-end force-fire
(inject a synthetic mid-arc slip → assert the EWL fix + WL hold evaporate next epoch, the re-tap
re-triggers a fresh inject, and NO wrong integer is ever asserted; `scripts/mw_census.py` truth) and the
15-ep A/B vs the Triple-OFF control (D5 flag rate at control level; injected-cycle KF-WL circR
integer-clustered; station-1 quarantined). The smoke already shows D5 at control level at 10 ep.

## Fable 15-ep A/B review (Jul-16) — seams PASS, force-fire PASS, ONE blocking finding

**Verdict: increments 1a–3 are structurally sound and the F6 loop is closed at the WL
lane — but NOT yet armable: the EWL lane carries a reproducible ~1.9× D5 elevation
(F-A below) that must be attributed + cured first.  Keep default-OFF.**

**PASS list (all re-verified on a fresh Release build, exe `gnss_processor_fable_ab.exe`):**
- Suite 754 run / 753 pass / 1 pre-existing skip.  OFF byte-identity vs the pre-WL-2
  exe (`gnss_processor_f6_run.exe`): `network_solution.csv` + `network_dd_fractional.csv`
  BIT-IDENTICAL (invariant #6) — after ALL of 2b+3's shared-file edits.
- Code seams (personal single-pass): inject NL-preservation + correction frame exact;
  D5 exemption symmetric serial/parallel, consumed at the next install (install runs
  once per ACCEPTED epoch, outside the RAIM block ⇒ the one-epoch semantics hold);
  per-build hygiene RAIM-safe (`wl_inject_epoch.clear()` per build); bridge accumulator
  lag/gap/jump/funnel correct; `iono_coeff` = −0.435 = β_WL−β_EWL ✓; guard order +
  counters complete; decisions statistic-gated throughout.  No defects found.
- **§7.3 force-fire PASS** (synthetic +5 cyc L2W slip, KRGG00ATF G06 @ep 8, config
  `_wlver_50sta_15ep_ff.json`, edited RINEX in the session scratchpad): affected EWL
  fixes EVAPORATE (147→144 rows, +69 no-arc skips during re-warm), bridge gross-jump
  refloat fires, cycles RE-FIX post-warm at the physically shifted integers (+5 EWL /
  −5 WL — the −33.6 m bridge jump and +5 EWL integer compose exactly), closure stays at
  the same 2 unrelated blocks ⇒ **zero wrong integers**; D5 649→658, no storm.
  Invariant #1 satisfied end-to-end.
- WL lane at 15 ep: 147 bridged → 50 decided (σ≈0.063), 31 holds, 20 injects
  (mean |Δlane| 27.6 cyc — bigger than the smoke's 3.56 because sub-quarantine
  clock-seed stations carry ~1.16 cyc lane offset per meter of clock seed; ties to
  memory `network-coldseed-clock-leak` — arming `cold_clock_seed` should shrink it),
  closure backstop live (2 blocks), 0 not-1:1.  **WL lane is D5-neutral-to-stabilizing**
  (EWL-only 679 → +inject 649).

**F-A (BLOCKING for arming): EWL lane D5 elevation, ~1.9× the fair control, steady
state.**  15-ep legs (50 sta, all this build): OFF 369 / CTL(mw_detect, lanes off) 342 /
EWL-only 679 / EWL-only+mwdet 653 / EWL-soft(row σ 0.1) 659 / full-ON 649 / ON+mwdet 650.
So: NOT the WL lane, NOT the missing coarse funnel, NOT row softness at 0.1 — and the
10-ep smoke's "detector-construction artifact" explanation is REFUTED by the fair
control (which builds the same detectors).  The elevation appears with `ewl_enable`
(1b) and persists ~57 flags/ep vs control ~23/ep in ep 11–15.  Two live suspects:
(i) EWL-row↔carry tension transmitted through the N5 + L5-phase + per-epoch-iono chain
(NOTE the units trap: the row functional is in BAND-cycle units, so σ_row 0.01–0.1 is
comparable-to-stiffer than the carry's 0.06–0.085 — a truly-soft discriminant needs
σ ≥ ~0.3–0.5); (ii) the third-band carry-exclusion basis change itself.  NEXT
INCREMENT: an F6-dump-style per-chord D5 attribution (which (group, chord) flags, per
epoch, with EWL-cycle-membership tagging) + re-run these legs (configs all exist:
`_wlver_50sta_15ep_{off,ctl_mwdet,ewlonly,ewlonly_mwdet,ewlsoft,wlinj,wlinj_mwdet,ff}.json`).
**F-A RESOLVED (Fable, Jul-16 same-day): attributed + cured; the 15-ep gate now PASSES.**
Instruments built (both default-inert): `network.mw_wl.d5_dump` (per-flag CSV
`logs/network_d5_flags.csv` with group/chord/station/amb/σ + adjacency tags; driver
gained the aligned `lastInnovFlagSigmas()` accessor) and `network.mw_wl.ewl_fold_rows`
(the discriminant).  Findings:
- **Fold-off collapses D5 to control:** EWL-only 679→**348**, full-ON 649→**358**
  (control 342) — with the ENTIRE cascade intact (147/147 EWL statistic-fixed, 50 WL
  decided, 33 holds, 24 injects — MORE injects than before: cleaner lanes).  The EWL
  rows' solve-side fold was pure cost: the bridge/inject read the DETECTOR statistic,
  never the posterior, so nothing downstream needed the fold.
- **The dump shows the tension was BORDER-MEDIATED and global, not local:** only 6% of
  the 649 flags sat on EWL-touched (station,sat) pairs, and 24% were on
  secondary-channel groups (L2L/L2X/L1W/L1X) the EWL machinery never touches — 147
  σ0.01 rows perturbed the shared clock/PB border enough to trip D5 network-wide.
  This is why no local discriminant (mw_detect, row σ 0.1) moved it.
- **Landed:** `ewl_fold_rows` default = FALSE (decide/FFR-fix/expose, fold nothing);
  loader warns when set true (study shape); EWL run-end line says "rows FOLDED/NOT
  folded".  Re-verified at the new default: suite 753/754, OFF byte-identity
  BIT-EXACT, ON D5 358, force-fire re-run intact (144 decided, evaporate/re-fix, D5
  362, no storm).  The §7 pass criterion "D5 at control level" is now MET.
- **WL-3 implication (for the tight-coupling discussion):** the refold-against-stiff-
  carry failure class is now MEASURED and confirmed — evidence FOR the (B)
  lane-coordinate carry as the durable v2 (continuous lane tightening, dual-freq
  maturity, honest covariances) — but the ACUTE case is cured; WL-2's current shape
  passes the quick gate without the refactor.  (B) remains HELD, now unhurried.

- **F-B RESOLVED (Fable, Jul-16): `network.mw_wl.lane_trace` (default-OFF, log-only)**
  — per epoch, EVERY primary cycle's POST-install carried WL lane + frac + tags
  (decided w / held / injected) to `logs/network_wl_lane_trace.csv`; grader
  `scripts/grade_lane_trace.py` (constrained = chords ever held/injected; control =
  the rest of the SAME run).  **First direct measurement of the WL-2 lift (15-ep,
  `_wlver_50sta_15ep_lanetrace.json`): control population circR 0.019 / med|f| 0.251
  (exactly uniform — the structural gap live); constrained population circR 0.343 /
  med|f| 0.137 / P(<0.15) 0.52 (pooled last-5, n=54).**  The §7 "circR lifting ≥ OFF"
  criterion: MET.  Per-epoch view shows the discrete-correction wobble (clean epochs
  med|f| 0.01–0.09 alternating with churn epochs ~0.26) — the roughness the WL-3
  lane-coordinate carry (SPEC_lane_carry_card.md) exists to smooth.

## SPEC §11 — implementer answers (Opus, Jul-16; complements the scout answers below)

- **Q2 (bridge iono): v1 FALSIFIED by Fable — v2 iono de-bias LANDED (Jul-16).** The v1
  Welford-priced-only form is wrong: the Welford variance prices the GF_b *temporal drift*, but a
  STABLE differential iono (constant over the arc) has ~0 variance yet biases the WL determination
  by ≈ −0.50 cyc per meter of DD iono. A near-integer bias then *slips the FFR |frac| gate* and
  injects a wrong integer. **Fix:** the bridge subtracts the network's own DD-iono estimate —
  `z_WL = [Σc·GF_b − C_iono·Σc·Î₁ + λ_EWL·N_EWL]/λ_WL`, with `C_iono = f_ref²(1/(f1 f2) − 1/(f2 f5))`
  (f_ref = GPS L1 = the estimator's iono-state frame; exposed by `arcBridge().iono_coeff`) and
  `Î₁` = the per-(station,sat) slant-iono state (`getIonosphereStates`/`getStateVector`, the same
  x_lin prior the ambiguity F5 term uses). **NOT variance-priced:** the local *marginal* iono
  variance (`getStateVariance`) is dominated by the ABSOLUTE/common-mode uncertainty that CANCELS in
  a closed DD cycle, so `Σc²·var` grossly over-states the DD-iono error and gated the bridge to zero
  cycles at 10 ep (measured) — defeating its ep~4-6 advantage. Instead the residual DD-iono ESTIMATE
  error (small — common-mode cancels) surfaces in `|frac|` (FFR self-protects) with the iono-free
  MW(1,2) closure as backstop. **Smoke delta (10-ep):** decided 5→**29**, MW(1,2) closure blocks
  **9→0** (the de-biased integers now agree with the iono-free MW — the decisive check), 32 FFR-unfixed
  (frac self-protection), D5 381 (< 393 control, no storm). The exact joint-DD-iono covariance / the
  §3.4 iono-column fold is a v3 refinement if the frac+closure protection proves insufficient in the A/B.
- **Q3 (primitive placement):** SPLIT, as scouted. Low-level `injectCarriedChord` on the driver
  (edits `carried_[b].z`, reseeds the diagonal, sets the internal `gate_exempt_`); the bridge
  assembly + FFR/closure + hold-row build live in `ApplicationNetwork::buildCarryDdPriors`. The 1:1
  cycle-support check is cheap: the chord `ArcKey` is byte-identical across the two bands' groups, so
  `carriedChordValue(group_L1,chord)` + `(group_L2,chord)` IS the check (either false ⇒ not 1:1 ⇒
  `skip_basis`). No cross-basis reconcile needed for v1.
- **Q4 (hold rows vs re-inject):** hold rows earn their keep — they fold ONLY when the carry already
  agrees with `w` (innovation ~0), so they cost nothing and pin the lane between injects/across benign
  re-taps. The A/B should still measure holds-OFF (fold nothing, inject every disagreeing epoch): the
  smoke shows 4 holds + 5 injects, so at 10 ep the two mechanisms are comparably loaded and the
  holds-OFF cost is a real question for Fable.
- **Q5 (WL-1 float-row disposition):** KEPT as a default-OFF diagnostic (`mw_wl.enable`); the loader
  already warns if it is armed together with `wl_inject_enable`. Not folded into the lane counters —
  the float-row telemetry is a distinct forensic surface (`f6_dump`) and retiring the toggle would
  lose the A/B baseline.
- **Q6 (dual-freq inject maturity):** DEFERRED. v1 is bridge-only (the WL lane is inert on a cycle
  with no tri-freq bridge — 33 "no/young bridge arc" skips in the smoke are exactly the dual-freq /
  immature arcs, correctly not injected). The MW(1,2) σ(n)≈0.5–0.7/√n law (census) means n≈25–50 ep
  to FFR-grade, which the 15-ep harness cannot reach anyway; §3.5 waits for Fable's graded call.

## 1b — EWL statistic lane (DONE + smoked)

- `NetworkMwWlLayer` extended (lane-generic): `Config.{ffr_fix,fix_sigma_cyc,fix_frac_cyc,
  fix_weight_sigma_cyc,max_abs_xlin_m}`; `build()` computes z_abs/z_lin separately, does the
  **FFR fix on the DETECTOR statistic** (σ=√R_raw, frac=z_abs−round; §3.0.1) → integer-hard row
  (zhat=round(z_abs)−z_lin, Pz=fix_weight², contribs cleared); **station-1 quarantine** (skip a
  cycle if any arc |x_lin|>`max_abs_xlin_m`); `Counters.{rows_fixed,skip_quarantine}`.
- `ApplicationNetwork` `buildCarryDdPriors`: shared setup (`revmap`/`arc_signals`/`primary`/`lam`/
  `lin_of`) hoisted to `if (mw_wl_on || ewl_on)`, plus a `band5_of` map; a second `ewl_layer` +
  `ewl_resolver` pairs each primary-cycle arc as (L2=primary sig2, L5=band5) with `arcZdMean(sat,
  L2,L5)` and λ_EWL; both lanes read the PURE carry blocks then append (WL-1 first). Run-end
  telemetry line + accepted-install accumulation.

**Smoke (10-ep 50-sta GPS-only, `_wlver_50sta_10ep_ewl.json`, Release):**
- **98 EWL rows, ALL 98 INTEGER-FIXED via FFR** over 3494 primary cycles (mean R 0.0001 cyc²).
  Skips: 6226 non-primary, 3069 missing band (L5 sparse — only a station subset tracks it), 254
  no MW(2,5) arc, 0 young, **73 x_lin quarantine (station-1 caught)**. **The uncarried-lane thesis
  holds: EWL fixes by ep 3.** ✓
- D5 = 393 (vs Triple-OFF control 216) — NOT a storm (the F6 stiff-WL failure was 1628). The delta
  is largely an artifact: the EWL run builds MW detectors (refloats 193→368) while the card-OFF
  control builds none. A cleaner control (mw_detect ON, ewl OFF) is a §7.4b graded item; for the
  smoke the fix result is the pass. New configs: `_wlver_50sta_10ep_{ewl,tripleoff}.json`.
- OFF byte-identity re-confirmed after the 1b edits (the WL-1 + carry OFF path is unchanged).

## 1a — what landed (all `network.mw_wl.*`, default-OFF)

- **Config** (`GnssConfigTypes.hpp` `MwWlConfig`): `ewl_enable`, `ewl_min_arc_epochs`(2),
  `ewl_fix_sigma_cyc`(0.10), `ewl_fix_frac_cyc`(0.20), `wl_inject_enable`,
  `wl_fix_sigma_cyc`(0.10), `wl_fix_frac_cyc`(0.15), `bridge_max_sigma_cyc`(0.15),
  `inject_disagree_min_cyc`(0.75), `lane_max_abs_xlin_m`(1000) [station-1 quarantine].
  `fix_weight_sigma_cyc`(0.01) reused as the hold-row σ.
- **Loader** (`GnssConfigLoad.cpp`): parse + clamp; mutex (both lanes require `carry_invariant`,
  fail-loud); bridge-needs-`ewl_enable` warn; `mw_wl.enable`+inject double-drive warn;
  `fix_enable` REPLACED-by-statistic-gated warn; per-lane ON summary lines.
- **Detector gate** (`ApplicationNetwork.cpp` ~1136): builds the per-station MW detector under
  `ewl_enable || wl_inject_enable` too.
- **Third-band carry exclusion** (`ApplicationNetwork.cpp`, `buildCarryDdPriors` group loop):
  `ewl_carry_exclude` + `isUncarriedThirdBand(cons,signal)` (GPS band '5', Galileo band '7')
  skip those ambiguities from the carry `GroupInput` — they STAY in the eq34 election/datum
  (shared_phase_keys/pins built upstream, untouched). This is the §3.1 crux: the EWL lane's
  uncarried leg has no stiff prior ⇒ its rows win with no F6 fight.

## 1a — verification (50-sta 15-ep `_wlver` harness, Release)

- **OFF byte-identity:** `_wlver_50sta_15ep_off.json` on the new build vs the pre-WL-2 exe
  (`gnss_processor_f6_run.exe`) — `network_solution.csv` AND `network_dd_fractional.csv`
  **bit-identical** (invariant #6). ✓
- **Exclusion has effect:** `_wlver_50sta_15ep_ewl.json` (ewl_enable only) drops the carried
  set **1030→912 DD** (−118) and births 1414→1271 — the L5 chords no longer carried; run clean
  (15/15 emitted). ✓  (New exe: `gnss_processor_wl2_run.exe`.)

## Scout answers to SPEC §11 (verified reads, Jul-15 eve)

- **Q1 (Triple band scope):** ONE key — `processing.frequency_mode` (`dual|triple|all|independent`,
  getter `getFrequencyMode()`). Band table `PppModel.cpp:73-93` getBandMap: GPS Triple/All={L1,L2,L5},
  Galileo Triple={E1,E5a,E5b}, All adds E6,E5(AltBOC). The estimator's uncombined selector
  `selectUncombinedSignals` (`PppModel.cpp:1011-1078`) loops ALL bands, no 2-band cap; each phase →
  an ambiguity `G##_L5Q` (`MeasurementModel.cpp:352`). **The `_wlver` configs already set
  `"frequency_mode":"all"` ⇒ GPS N5 columns already exist** (fixture has C5Q/L5Q at ABMF/ADIS/AIRA,
  Galileo C5Q/C7Q/C8Q). MW detector band scope is SEPARATE (`mw_frequency_mode`, Mirror inherits KF).
  **GOTCHA:** `estimation.lock_signals_to_clock_ref` (default false) pins the estimator to the 2 BIA
  clock-ref bands and IGNORES frequency_mode — confirm configs leave it false. `signal_min_station_fraction`
  can drop a sparse L5 network-wide.
- **Q1b (carry exclusion seam):** confirmed clean — the carry driver is fed `GroupInput` explicitly
  in `buildCarryDdPriors`, so a caller-side band filter drops L5 from the carry while eq34 keeps it.
  Verified byte-identical OFF + the −118 DD effect ON.
- **Q3 (inject placement):** split. Low-level primitive on `NetworkInvariantCarryDriver` editing
  `carried_[b].z(i)` (mirror the `carriedDdForTest` private-access pattern); resolution seam in
  `ApplicationNetwork` reusing the WL-1 `revmap`/`arc_signals`/`primary`/`lam`/`lin_of` machinery.
  **`carried_[b].z` is CORRECTION-frame** (absolute DD = `z(i)+Σcoeff·x_lin`, the F5 term — any
  integer target must account for x_lin). Groups independent but `ArcKey(rx_vertex,sat_vertex)` is
  byte-identical across signal groups (rx=station idx, sat=per-sat kSatBase id) ⇒ match arcs on
  ArcKey; but a cycle that's a chord in L1 may be a tree edge in L2 (§3.3 v1 = 1:1 cycles only).
  Insertion point: right after `installPosterior` (edit the fresh `carried_`, next epoch's
  buildDdPriors projects it). **D5 exemption:** do NOT reuse `carry_slipped_ambs` (it refloats =
  discards the injected value) — add a PARALLEL `carry_gate_exempt` funnel: new `ArcInput.gate_exempt`
  → `BlockMeta.chord_gate_exempt` → `continue` past the flag test in installPosterior's D5 loop.

## Station-1 pre-flight (§7) — ROOT-CAUSED: benign clock-seed artifact

`256540 m / c = 0.856 ms`. The network has NO SPP/WLS receiver-clock pre-seed
(`ApplicationNetwork.cpp:1169` clock_bias=0; no WlsEstimator use); `StateManager::computeAmbiguitySeed`
(`StateManager.cpp:1211-1273`) subtracts the clock STATE which is 0 at first sight, so the whole
~0.856 ms clock offset lands uniformly in station-1's ambiguity SEEDS (G01/G10/G15/G16/G27). The
batch solve then splits it (clock→256 km, amb→normal), so it's **benign for the ordinary solve** and
cancels in DDs — but it ill-conditions the correction-frame LANE rows (the probe: station-1 cycles
failed to converge even at 100× authority). **v1 fix = quarantine** (`lane_max_abs_xlin_m`=1000): the
lane builder skips any cycle with an arc whose |x_lin|>threshold, counted. (Root fix — an SPP clock
pre-seed — is a general, byte-identity-breaking change, deferred.) The quarantine wiring lands in 1b
with the lane rows.

## Key seams for 1b (EWL lane) — reuse, don't fork

- Extend `NetworkMwWlLayer` (or a sibling) to emit rows on a CONFIGURABLE band pair (the EWL (2,5)
  instead of the primary (1,2)) + an FFR fix decision (statistic-gated per §3.0.1 — read the DETECTOR
  stat `arcZdMean(sat,sig2,sig5)`, NEVER the posterior) + integer-hard fold post-fix (Pz→
  fix_weight_sigma²). The uncarried lane ⇒ ordinary rows, NO inject, NO D5 exemption (§3.2).
- Second resolver in `buildCarryDdPriors` for the (2,5) pair: the primary-pair logic
  (`ApplicationNetwork.cpp` ~6644) picks the two lowest bands = (1,2); the EWL resolver instead pairs
  band-2 col with band-5 col at each (rx,sat). `arcZdMean` is pair-generic (`MwSlipDetector.cpp:179`)
  so MW(2,5) stats exist once L5 is in the MW detector scope (mw_frequency_mode Triple/All).
- Apply the `lane_max_abs_xlin_m` quarantine skip in the eligibility loop (counted).
- Telemetry: per-lane run-end counters (§6).

## Anchors / exes / configs
- New exe `build/x64-Release/gnss_processor_wl2_run.exe` (WL-2); reference `gnss_processor_f6_run.exe`
  (pre-WL-2, byte-id baseline). Configs: `_wlver_50sta_15ep_{off,on,f6,probe,ewl}.json`. Diagnostics
  `network.mw_wl.{f6_dump,debug_r_scale}` kept (default-OFF, loud warn). Nothing committed.
- **2b+3 (Jul-16):** exe `build/x64-Release/gnss_processor_wlinj_run.exe` (WL inject smoke, renamed
  copy per the concurrent-build rule); config `_wlver_50sta_10ep_wlinj.json` (= the `_ewl` 10-ep smoke
  + `wl_inject_enable:true`). Touched: `include/slip/MwSlipDetector.hpp` + `src/core/slip/MwSlipDetector.cpp`
  (bridge accumulator + reset hooks), `include/filter/NetworkMwWlLayer.hpp` + `.cpp` (FixedEwlRow
  exposure), `src/core/app/ApplicationNetwork.cpp` (detector-gate `setBridgeEnabled`, the WL bridge/hold
  lane in `buildCarryDdPriors`, the post-`installPosterior` inject, run-end telemetry), `CMakeLists.txt`
  + `tests/test_mw_bridge.cpp` (new), `tests/test_network_mw_wl_layer.cpp` (+3), `tests/test_network_invariant_carry_driver.cpp`
  (+1). All default-OFF / byte-identical when `wl_inject_enable=false`. Nothing committed.
