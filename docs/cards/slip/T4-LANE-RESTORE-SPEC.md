# T4 lane restore: pend-and-restore v1 (value-only), spec only

**STATUS**

- **id:** T4 lane restore
- **topic:** slip
- **status:** open
- **summary:** Specified 2026-08-23, NOT BUILT: one epoch after a T4 erase, restore the pre-erase carried value where the lane observatory's δ corroborates the lane survived; sim-first gates G0 to G6.
- **dates in the text:** 2026-08-23
- **knobs / posture:** `network.cycle_slip.relevel_t4_lane_restore` (default OFF, conditionally fingerprinted; rider on `relevel_lane_snap`, requires `phase_leveling` and `relevel_free_epoch`)
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_lane_slip_response.md` §8

**Outcome.** The card specifies how to recover the one piece of information a T4 refloat discards, the arc's pre-erase carry, after the decide-time keep-gates were FALSIFIED below refloat-always. The erase stands; one epoch later the pre-erase value returns, at variance max(pre, free), only where δ is within min(2σ_null(el), 1.0 cyc) and the MW, GF and per-band checks pass. A breach of G2 (settled sim EWL at-own-integer must exceed refloat-always's 70.2 %) or G5 closes it DO-NOT-PROMOTE in favour of the 8(b) displacement drain. Not built ([OPEN_ITEMS](../../status/OPEN_ITEMS.md) section D); the re-founding has since cut lanes from scope ([slip topic](../../topics/slip.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_lane_slip_response.md -->
## §8 SPEC — the T4 apply leg: pend-and-restore v1 (value-only), `network.cycle_slip.relevel_t4_lane_restore`

**Status: DESIGN, adversarially synthesized 2026-08-23 from three candidate cards (pend-and-restore skeleton; grafts: tight-null gate + 1.0-cyc discrimination cap + prires family label + strike/cooldown from the lane-restore-row proposal; shadow-first truth-join calibration + ARMED-BUT-INERT line + T4-re-fire kill gate from the re-anchor proposal).  Executes SPEC_slip_relevel_walk §8.2's pend-and-replace shape on the T4 channel per §7.7's fired redesign trigger.  Default-OFF; solve-affecting ⇒ conditionally FINGERPRINTED (never flipped mid-chain).**

PREMISE ACCEPTED FROM THE §7.8 CENSUS: T4 removals are typically REAL (de-clustered el>30° med |δ| 2.94 cyc, 32.2% ≥ 5), so no decide-time keep-gate can be right — keep-always (54.0%) and corroborated-keep (59.6%) sit falsified below refloat-always (70.2%) and stay dead.  The only information refloat-always discards is the arc's pre-erase carry, and the only instrument fine enough to adjudicate it is the observatory's δ, available ONE EPOCH LATE.  Therefore: RESET FIRST (the T4 erase at `NetworkRaim.cpp:797`/`:915` stands untouched, interim free re-level and honest-loose emit exactly as today), then at the next harvest RESTORE the pre-erase carried prior only where δ corroborates the lane survived — a corroborated keep executed one epoch late, on evidence that is finer than the raw-step gates that falsified F4C (δ compares the converged pre-lane against an independent unbiased code re-level; the field per-event mode is 2–5 cyc against a 0.8–1.0-cyc gate, so the gate discriminates ~60%, not F4C's 1.5%).

### Mechanism

1. **CAPTURE** (epoch t, SB_RAIM, both erase sites): `LaneT4Pre` (`NetworkRunContext.hpp:996`) gains `var` (the about-to-be-erased `LocalCarry::var`).  Lifetime stays ONE epoch, cleared at the next harvest — the invariant-#1 bound unchanged.  A capture whose LOS is in THIS epoch's `free_ambs` (SB_RAIM runs after both slip drains, so the set is complete) is tagged detector-overlap and will void.
2. **MEASURE** (epoch t+1, appended to the existing T4-side observatory block, `NetworkRunContext.cpp` ~2774–2900): δ, band-digit resolve, freshness tests, and the §7.8 frame cross-guard union (`eq34_reref` + `phase_anchor_reref` + `datum_reref_sats`) are reused VERBATIM — the guard is already in the tree and spans exactly the capture→restore single epoch.  No persistent object exists, so there is no mid-life frame exposure by construction.
3. **DECIDE** — all must pass, else the free re-level stands and the refusal is counted:
   - el ≥ 30° (`refuse_low_el`; the null and intact fraction are characterized only there);
   - |ΔMW| ≤ 0.75 WL cyc (a raw step is a compound slip — reset-over-repair) and ΔMW formable;
   - GF veto, REFUSE-only (invariant-#1 pre-flight: an equal-in-cycles all-band slip is lane-silent but ΔGF = (λ₁−λ₂)·n ≠ 0): the qualified GF verdict must be measured AND not stepped.  Coarse evidence used to refuse more is not the F4C shape;
   - lane corroboration, elevation-mapped, TIGHT-end null with a discrimination cap: |δ| ≤ min(κ_L·σ_null(el), δ_cap), κ_L = 2.0, δ_cap = 1.0 cyc, σ_null(el) = max(floor_lane, c_lane·σ_P(el)), σ_P(el) = σ_P0/sin el (σ_P0 = 0.3 m config), c_lane = 0.76 m ÷ λ_lane per metre (= 0.88 cyc/m on WL(1,2), the tight end of the measured [0.88, 1.17] bracket — the dangerous direction is accepting drifted lanes).  floor_WL = 0.40 cyc (the measured O2 floor); floor_EWL = 0.059 cyc (metre-class reading — the TIGHT assumption; the O2 floor's λ-scaling is unresolved, and v1's truth join measures it: intact-population δ scatter at ~0.4 cyc on EWL ⇒ the floor recalibrates on truth data as a REGISTERED step, not a tuning pass).  Effective field gate ≈ 0.80–1.0 cyc; no restore is admitted where the gate could hide a > 1.0-cyc drift;
   - per-band corroboration: |C_pre,b − C_post,b(t+1)| ≤ 3.5·σ_P(el) per band in metres — bounds lane-silent band-common error at code resolution;
   - T0-repair void (§7.2 hazard): any phase-leveling repair on the (sta,sat) at t or t+1 ⇒ `refuse_t0_repair` (the repair adds dN·λ to value_abs while ΔMW carries the raw step);
   - detector/wind-up overlap void: the LOS in `free_ambs`/`free_cause` at t (from the capture tag) or t+1 ⇒ refuse.  ⚠ Keyed on SET membership, never on fill-site hooks — the tree has THREE fill sites (`NetworkSlipQc.cpp:422`/`:923`/`:1316`), the third being the wind-up-restart response (`kCauseWindup`), a MODEL step that is band-common in cycles and therefore BOTH MW- and lane-blind — δ cannot see it and must never adjudicate across it;
   - strike/cooldown: a restored LOS re-erased by T4 within 20 ep takes a strike; 1 strike ⇒ 600 s restore cooldown for that LOS (no restore↔erase limit cycle).
4. **APPLY**: full both-band carried-prior replace — `local_carry[k] = {value_abs: pre_value (shift 0 — a T4-pure event has no step to bridge), var: per the variance treatment, last_seen_s: pl_now_s, coupling: CLEARED}` — the constraint-7-sanctioned form (convention-pure: pre and free values are both the estimator's own state convention; no raw-MW convention enters, the v1-kill class structurally absent).  A third captured band outside the MW pair stays free and is counted (`refuse_third_band`).  The erased iono carry is NOT restored — it re-draws, by design (see hazards).

Knob: `network.cycle_slip.relevel_t4_lane_restore`, default false; rider on `relevel_lane_snap` (the observatory is the sensor; arming it also arms the raw-qualification pass); requires `phase_leveling` + `relevel_free_epoch` on SETTLED flags; dies under `carry_invariant`; warn-and-disarm on every unmet interlock; conditional fingerprint (seed and legs all carry it or none).  `SlipQcParams::t4_restore` registers in BOTH `anyConsumer()` AND `cardsArmed()` (the §7.4 double-registration trap, reason comment at the member).

### Variance — both edges (invariant #1 / #7)

DECOUPLING edge: unchanged — the T4 erase leaves no prior (iono carry erased with it); the interim epoch runs free and emits at honest code-class var, bounding what the decoupled arc can accumulate to one epoch of diffuse evolution.  RE-COUPLING edge: var_replace,b = max(pre_var_b, var_free,b(t+1)) — the restored value is NEVER re-injected tighter than the free posterior the data itself just produced.  The gate certifies the value only to gate width (≤ 1.0 cyc); pre-var re-injection would be invariant #1's stiff-prior shape re-created by the response.  v1 is therefore VALUE-ONLY benefit at honest weight: the converged level replaces the one-epoch code draw (field WL null 0.5–0.9 cyc median) and data re-adjudicates immediately — a wrong restore loses to data within epochs.  **Stated structural bound, not hidden:** a diagonal per-band carry cannot represent what the corroboration actually certifies (lane tight, bands code-loose), so v1's effect size is capped by construction; the forks that could lift the cap are pre-registered below and must be EARNED, not assumed.  Coupling cleared unconditionally (old-gauge pinning; retap/warm-resume precedent).  Pend state is not snapshotted — warm resume degrades to plain refloat, the conservative direction.

### Edges, counters, identities (invariant #7)

Edge 1 (decouple) = the existing T4 erase, already evented, untouched.  Edge 2 (recouple): one row per pend-attempt on the EXISTING `network_events.csv`, tier `LANE_APPLY` (LV-2 no-new-stream precedent): TOW, sta, sat, band pair, el, cause bits (T4A|T4C), δ, σ_null(el), ΔMW, outcome ∈ {applied, refuse_{low_el, mw_step, gf_step, gf_unmeasured, lane_gate, band_gate, t0_repair, overlap, third_band, cooldown}, void_{gauge, stale_pre, no_post, pair_changed, no_mw, unmeasured}}.  Closure printed at teardown, `*** MISWIRE ***` on failure: t4r_attempts == t4r_applied + Σ refuses + Σ voids.  Aliveness is channel-SHAPE, not closure (§7.4): refuse_gf_unmeasured ≈ 100% ⇒ the qualify pass did not run; applied == 0 on a slip-bearing armed run prints ARMED-BUT-INERT.  A 0%-or-100% acceptance rate is a miswire (F4C lesson).  Churn readout: t4r_reflagged (restores T4-re-erased within 20 ep, joined at the erase sites against a small aged map) + the strike census.  Applied restores carry a prires family label `t4_restore` so the σ̂₀ instrument set reads conditioned on the mechanism.

### Pre-registered validation — sim-first, each gate blocking the next

- **G0** Unit suite green (outcome ladder incl. every refuse/void, carry untouched on refusal — the `networkSlipBridgeReplace` precedent; gate algebra el-mapped; var monotonicity; T0 + wind-up voids; loader END-STATES).  OFF-path byte-identity: single-knob A/B, separate CWDs, `products.txt` + every solve CSV identical.
- **G1** Sim shadow smoke (~40 ep, `_camp60wu_f6smoke` posture + knob in decide-only mode): closure OK, both outcome classes populated, solve byte-identical in shadow.
- **G2 (HARD)** Sim apply arm, full 720-ep slipped wu93 (`_camp60wu_f7.json` = f6obs + apply): settled GPS EWL(2,5) at-own-true-integer **> 70.2%** (target ≥ 75%) and WL(1,2) **≥ 21.1%** (`simnet_amb_integer.py`, 2 486-truth-slip fixture).  Applied in the 1e2–1e3 class with rejects nonzero in ≥ 3 channels.
- **G3** Restored sub-population, graded separately via LANE_APPLY rows joined to truth (extend `simnet_lane_slip_age.py`): restored ≥ the matched refloat-only population; restored-lane error ≤ fresh-re-level error on ≥ 70% of applies; the join ALSO reports the intact-population δ scatter on EWL (the O2 floor λ-scaling measurement) and the would-be wrong-n* rate (the re-anchor revival input) — free, no extra run.
- **G4** Per-band at-own-integer unchanged in family (the restore must not masquerade as a per-band repair); ZWD hourly rms inside F1's 1.85–3.08 band; block-Baarda block-size histogram + datum-graph census bit-identical (the structural A/B invariant).
- **G5 (HARD)** t4r_reflagged < 10% of applies within 20 ep.
- **G6** Quiet-twin neutrality: applies rare (FA-driven only), every graded channel in the quiet band.

**FALSIFIED = G2 or G5 breach ⇒ card closes DO-NOT-PROMOTE**, alternative named: the 8(b) displacement-drain card (`SPEC_iono_carry_8b.md` §6.2), keeping the observatory + this card's refusal census as its per-event field instrument.  No tuning pass past a falsifier.

**FIELD (only on G2–G6 PASS):** first a fingerprint-SAFE SHADOW leg on the `_fld63` lineage (decide-census only, `products.txt` identical — sizes yield, el/δ mix, and verifies the ≥ 5-cyc census mass lands ENTIRELY in rejects: the mechanism must not restore what T4 is measurably right about).  Then the apply arm on a FRESH-convergence SEAMLESS 4 h lineage (conditional fingerprint — never a warm-resume mid-chain flip) vs the matched control: products MUST now differ (stated in advance — the first intended solve delta in this lineage); graded on datum-invariant instruments only — `field_lane_frac.py` / `field_lane_obs_grade.py`, prires/block-Baarda/congruence σ̂₀ conditioned on `t4_restore`.  Registered field falsifiers: the over-retention signature (prior-side σ̂₀ quieter WHILE data-side louder — the lane-63 A/B lesson) and a G5-class reflag rate.  T4 rate trajectory reported, not gated (the apply leg does not claim to flatten the drift ramp).  No default flip; SCOPED ledger entry only.

### Deferred alternatives — revival conditions, not graves

1. **Integer re-anchor** (the fraction-preserving snap onto the pre-erase lane grid, n* = round(δ)): DEFERRED, not falsified — its P_flip pricing distinguishes it from the killed §8.1 rounding shape, but it self-refuses on field GPS dual (WL 0.40-cyc floor ⇒ P_flip ≥ 21% at all el) and may act only on EWL rungs.  Revival: v1 PASS + G3's truth join shows wrong-n* ≤ 1% under a calibrated σ_dec(el) on sim EWL ⇒ build as a v2 rider, EWL-scoped; field WL stays refused unless G3 measures the O2 floor metre-class.
2. **Joint band-pair covariance carry**: the persistent per-epoch lane row is DEAD as proposed (self-referential double-count — the row and the per-band priors derive from one posterior; the over-retention signature by construction).  The principled version — a 2×2 joint block for the restored pair inside `LocalCarry`'s existing coupling machinery, expressing lane-tight/band-loose honestly — is a derivation card.  Revival: v1 passes G2/G5 but lands under target with t4r_reflagged < 3% (the value survives; the diagonal var starves it).
3. **Variance tightening** (restore toward pre_var + gate-risk pricing): earned only on v1's measured reflag + prires evidence, same trigger class as (2).
4. **Build-nothing / 8(b) routing**: if G2 fails, that IS the outcome — the census and the per-event refusal rows remain as the drift instrument.

### Hazards (named, each with its bound)

- **Amb↔iono exchange re-import**: T4 erases the iono carry WITH the level (`NetworkRaim.cpp:813` — the statistic reads their sum); restoring a displaced pre-ambiguity while the iono re-draws re-creates the postfit tension.  Bounded: displacement is band-DIFFERENTIAL (lane-visible), so the δ gate refuses most of it; the residual is G5's registered falsifier.
- **Lane-silent equal-cycle drift**: invisible to δ; GF veto + per-band gate bound it at code resolution; residual is a loose-var restore data re-adjudicates, counted by reflag.
- **Wind-up restart** (the stale-doc correction): lane- AND MW-blind model step; voided by free_cause SET membership at both epochs — never by line-site hooks.
- **Frame channels**: capture→restore spans one epoch under the §7.8 guard union; no persistent object ⇒ no mid-life exposure (the reason the lane-row shape was refused).
- **Half-cycle degradation**: a restore certified only to ≤ 1.0 cyc can sit near a half-integer and degrade at-own-integer while reducing |e| — visible only in G3's per-event truth join, which grades restored arcs as their own population.
- **Non-stationary field population**: the T4 ramp (911→2085/h) is the 8(b) drift accelerating; if 8(b) lands first, yield and intact fraction change — the field arm re-validates after any 8(b) landing.  The walk-dead sim fixture approximates the post-8(b) regime, so a sim PASS carries forward.
- **Yield starvation**: the field's 28% no-pre-carry starvation + refusals may leave t4r_applied in the 1e1 class/4 h — pre-registered as a CLOSE outcome (alive-but-inert, invariant #4), never a widen-the-gate outcome; the shadow census catches it before any solve-affecting arm.

### Surface (~450–550 LOC, existing seams, anti-regrowth tree respected)

`NetworkRunContext.hpp`: `LaneT4Pre` += `var` + overlap tag; strike/cooldown + reflag aged-map; ~20 counters (~50 lines).  `NetworkRaim.cpp`: var fill + overlap tag at `:797`/`:915`; reflag join at the erase sites (~20).  `NetworkRunContext.cpp`: restore block appended to the T4 harvest loop, reusing resolve/guards/δ verbatim (~140).  `SlipQcParams`: `t4_restore` in both gates (~10).  Config: knob + 8 sub-knobs + interlocks + fingerprint note (~50).  `NetworkRunTraces`/`NetworkTeardown`: LANE_APPLY rows + readout/closure/MISWIRE/ARMED-BUT-INERT (~50).  `tests/test_relevel_t4_restore.cpp` (new file, one suite, manual CMakeLists entry, ~250).  `scripts/simnet_lane_slip_age.py` + `field_lane_obs_grade.py`: LANE_APPLY truth join (~60 py).  Configs: `_camp60wu_f7.json` + field shadow/apply clones deriving from the standard armed set.
