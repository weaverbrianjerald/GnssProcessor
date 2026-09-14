# RF-1 — ambiguities as float states with covariance

**Status:** DESIGN READ, 2026-09-14.  No implementation.  The manager decides the build from this document.
**Parent:** `docs/refound/NETWORK_REFOUNDING_2026-09-14.md` (the card order; RF-1 is the first replacement card).
**Baseline:** RF-0's frozen posture — the leveling vehicle as `config/test_suite/_pv_b.json` realizes it
(`carry_invariant: false`, `phase_leveling: true`, `cycle_slip.relevel_free_epoch: true`,
`carry_conditional_coupling` + `carry_coupling_gauge_project` on, `phase_leveling_local_block` (F3) **absent ⇒ off**,
`phase_leveling_q_amb` absent ⇒ **0.0**, `phase_leveling_var_floor_m2` absent ⇒ **0.0**,
`phase_leveling_max_gap_s: 900`), GPS-only dual, lanes off, seams off.
**Bars:** the two instruments of the re-founding decision and nothing else — the cold 6 h consumer, and the wire against
a reference per hour.  Everything else in this document is diagnostic.

---

## 1. What the ambiguity is today, in this estimator

### 1.0 The frame the whole vehicle sits in

The network producer has **no Kalman write-back**.  Each station's `StateManager` is constructed with five core states
(`pos(3) + clk + tropo`) and its state vector is written **once**, at setup
(`src/core/app/ApplicationNetworkSetup.cpp:571-574`); ambiguity and per-LOS iono columns are allocated later by
`MeasurementModel::ensureRequiredStates` (`src/core/measurement/MeasurementModel.cpp:289`, the ambiguity creation at
`:589-593` seeded by `StateManager::computeAmbiguitySeed`, `src/core/filter/StateManager.cpp:1276`).  That vector is the
**linearization point `x_lin`**; the kernel solves the correction `x_r` and the absolute estimate is `x_lin + x_r`.
A per-station `x_lin` shift is an EXACT solve no-op (proved 2026-08-01, memory `project_network_xlin_invariance`), so
the "seed" is a *frame*, not information: **an ambiguity column carries no state covariance, no process noise and no
prior of its own.**  Everything that makes it time-linked is the phase-leveling carry described below.

Per epoch the producer builds `N_rr = A_rᵀ W A_r` per station from **this epoch's rows only**, folds the carried priors,
Schur-reduces to the shared border, solves, back-substitutes.  That is a Kalman measurement update + predict in
**information form** (memory `project_network_kf_vs_information_form`); the carry marginalization is the predict.

### 1.1 The carried object

`struct LocalCarry` — `include/filter/NetworkRunTypes.hpp:425-482`, keyed in `ctx.carry.local_carry` by
`(station index, satellite, signal)`:

| field | what it is |
|---|---|
| `value_abs` | the **absolute** float ambiguity estimate (m) |
| `var` | last-solve posterior variance (m²) — the MARGINAL on the legacy diagonal path, the **pure conditional** `(N_rr⁻¹)_rr` under `carry_conditional_coupling` (RF-0's posture) |
| `last_seen_s` | continuous-GPS seconds of the last stash — the gap clock |
| `coupling` | Card-1's regression `M_r` on the FREE shared border (`CoupledShared`) |
| `blk` / `blk_idx` | F3's shared harvest cross-covariance block `PlHarvestBlock` (`:400-418`) — **null on RF-0** |
| `var_first` | the arc's FIRST-stash conditional variance, set once at birth, kept across re-levels, restarted only by erase-and-rebirth |
| `birth_ep` / `birth_s` | the arc's first-stash stamps (age gates; the sim slip oracle's window floor) |
| `relevel_s` | the last RE-LEVEL stamp — the young-arc inflate's age clock |

So the carried object is **a value plus a scalar variance plus a regression row onto the shared border** — i.e. a
per-column marginal, not a joint covariance over the station's ambiguity block.  The joint block exists
(`PlHarvestBlock`, `CouplingRowBuilder::buildLocalBlock`, `include/filter/NetworkCoupling.hpp:249-266`) but is gated on
`network.phase_leveling_local_block` (F3), which is **on in `golden_sim_tri_w6` and off on RF-0's field posture**.

### 1.2 The fold (PASS B, per station, per epoch)

`src/core/app/ApplicationNetworkAssembly.cpp`:

- the gate: `const bool do_amb_carry = (phase_datum_prioritized || !st.is_pivot) && !carry_invariant;` — `:3169-3170`;
- **gauge-pinned columns are never carried**: `networkIsGaugePinned_(...) ⇒ continue` — `:3236-3238`.  The elected
  anchor's ambiguity is instead **pinned to ZERO at `w = 1e10`** (`pin_y.push_back(target == 0.0 ? -piv_x(amb_idx) …)`,
  `:2833`; `double w = 1e10; // hard pin (exact gauge)`, `:2842`).  Inject and harvest partition on the same
  predicate, so a pinned column never gets a prior that fights its pin;
- the variance the prior uses: `double var_eff = cit->second.var + pl_q_amb * std::max(0.0, gap);` — `:3296`.
  **`pl_q_amb` defaults to `0.0`** (`src/core/config/GnssConfigLoadNetwork.cpp:3443-3445`, falling through to
  `NetworkQClass::LevelingAmbiguity`, whose table fallback is literally `0.0`,
  `include/config/GnssConfigTypes.hpp:951`).  `phase_leveling_var_floor_m2` also defaults to `0.0`
  (`GnssConfigLoadNetwork.cpp:3450-3451`) and is unset on RF-0.  **So the prior's variance is exactly last epoch's
  posterior, with no inflation of any kind — the zero-q level.**
- the row: `const double lambda = 1.0 / var_eff; const double eta = lambda * (lc.value_abs - x_lin(amb_idx));`
  — `:3214-3215` — accumulated into `inj` and folded by
  `est.addStationPrior(...)` at `:4396`.  Under `carry_conditional_coupling` (RF-0) the same ingredients instead build a
  **coupling row** — `+1` on the local column, `M_r` on the free shared border, RHS
  `y = (value_abs − x_lin) + Σ_j M_j·ŝ_prev,j` (`CouplingRowBuilder::buildCplRow`,
  `src/core/filter/NetworkCoupling.cpp:158-260`).

`addStationPrior` (`include/filter/NetworkNormalEquations.hpp:240-268`,
`src/core/filter/NetworkNormalEquations.cpp:566-610`) applies each `Λ_c` as a **rank-1 LDLᵀ update of the cached `N_rr`
factor**, leaving `N_ss` / `u_s` untouched — the fold is reduction-orthogonal and per station.  That is the property
that bounds RF-1's cost (§6).

### 1.3 The harvest (serial, post-solve)

`networkHarvestLocalCarries`, `src/core/app/NetworkRunContext.cpp:3891`ff, gated on `carry_warm`
(`phase_leveling_warmup_epochs`, default 6 **emitted** epochs, `GnssConfigLoadNetwork.cpp:3446-3447`):

- `xr = est.reconstructStation(est_idx, x_s);` `:4285`; `vard = est.conditionalLocalVarianceDiag(...)` `:4287`
  (the Card-1 path takes the **pure** conditional from `harvestCoupling` instead, `:4332-4341`);
- `const double value_abs = pa.x_lin + xr(pa.col);` — `:4326`;
- stash: `LocalCarry nlc{value_abs, var, pl_now_s, std::move(coupling)};` `:4581` → `local_carry[lkey] = …` `:4591`,
  with `var_first` set once at birth (`:4536-4541`), `birth_ep`/`birth_s` stamped by the first-stash-wins rule, and
  `relevel_s` stamped on CREATE or on FREE only.

With `q = 0` the recursion is exactly recursive least squares for a random constant: information telescopes, no
double-counting — **provided the carried object is the joint posterior of the block.**  It is not: `N` columns are
folded as `N` mutually independent rows whose errors share the station's clock / ZWD / rx-DCB / per-LOS-iono common
mode.  That over-count is stated in the codebase itself (the F1 comment at
`ApplicationNetworkAssembly.cpp:3170-3186`: "the B rows of ONE (station, satellite) LOS assert the SAME iono-direction
datum B times per epoch … formal σ collapses geometrically, the level freezes at `K_eff = (B+1)/(B−1)` epochs") and is
the mechanism of the ZWD/clock lock-in (memory `project_carry_datum_handover_theory`, vehicle 1).  Card-1
(`carry_conditional_coupling`, armed on RF-0) removes the **shared-border** half of the double count.  The
**local–local** half is F3's `buildLocalBlock`, and F3 is **off** on RF-0.

### 1.4 Birth

There is no birth prior.  `if (cit == local_carry.end()) continue;  // no carry yet (new arc)` —
`ApplicationNetworkAssembly.cpp:3275`.  The column solves **free** that epoch against that epoch's own rows, and the
harvest stashes whatever came out, at the conditional variance of the model **as folded**.  That variance is what the
prior will be at epoch 2, with no inflation.

The consequence is stated verbatim in the source (`ApplicationNetworkAssembly.cpp:3297-3305`, and
`tests/test_young_arc_inflate.cpp:8-16`): *a freshly born or re-levelled arc holds a CODE-quality value at a
PHASE-stiff variance, because the conditional it is read from is conditioned on the station's MATURE arcs.*  Two routes
make the harvested variance phase-stiff rather than code-honest at birth:

1. **band-scoped freeing** — one band freed while the sibling band's stiff carry still pins the LOS's iono means the
   freed band is determined at *phase* precision against the sibling (a geometry-free tie), inheriting the sibling's
   level error;
2. **the SH tie row** — `estimate_iono_sh: true` on RF-0 folds a prior on the LOS's slant iono, so a new arc's
   ambiguity is separated from its iono at the *tie's* stated precision, which §24.13's finding 2 shows is optimistic
   (25.8 % of settled iono rows carry **no** tie row at all —
   `estimation.ionosphere_weighting.elevation_cutoff_deg` 25 vs `processing.elevation_mask_deg` 15,
   `ApplicationNetworkAssembly.cpp:1080` (and `:912`) — and that population owns the whole metre-plus tail).

The existing partial mitigation is `network.phase_leveling_young_inflate_var_m2`
(`GnssConfigLoadNetwork.cpp:3491-3499`, default `0.0` = OFF, `include/filter/NetworkYoungInflate.hpp:55`), which
inflates the *inject* variance by `V0·exp(−age/τ)` keyed on `relevel_s`.  **It is not armed on RF-0.**

### 1.5 Slip / refloat — four distinct paths, none of which restores the birth variance

| path | site | what it does |
|---|---|---|
| **pending-reseed** (legacy) | `ApplicationNetworkAssembly.cpp:3263` | structurally DEAD for detector marks — the PASS-B `ensureRequiredStates` pre-pass consumes and clears the mark before the inject reads it (the measured level-walk root, `SPEC_slip_relevel_walk`) |
| **free-epoch latch** (`relevel_free_epoch`) | set at `src/core/slip/NetworkSlipQc.cpp:619` (and `:1311`, `:1866`, `:1963`, `:2141`) into `RelevelEpochState::free_ambs` (`NetworkRunTypes.hpp:638-660`, **one-epoch lifetime**); read at `ApplicationNetworkAssembly.cpp:3264-3270` (`++R.relevel_free_skips`) | **skips the inject for one epoch.**  The entry is NOT erased; the harvest at the end of that same epoch re-stashes it from that epoch's data, keeping `var_first` and `birth_s`, stamping `relevel_s` |
| **T4 attribution / T3 consensus refloat** | `src/core/filter/NetworkRaim.cpp:1028` (attribution) and `:1207` (consensus) | **erases the entry.**  The next harvest re-creates it with a fresh `var_first`, `birth_ep`, `birth_s` — an erase-and-rebirth |
| **gap cliff** | `ApplicationNetworkAssembly.cpp:3277-3288` (`++R.inject_stale_skips`) and the prune at `NetworkRunContext.cpp:4906-4918` | `gap > phase_leveling_max_gap_s` ⇒ not injected this epoch (so the arc solves free and is re-stashed), then erased by the prune if still stale.  A fleet crossing on one epoch is a silent mass re-level |

**Every one of these produces a level re-determined from ONE epoch, and none of them restores a birth-scale variance.**
That is the "re-seeding levels from one code epoch" the author's decision names.  Measured population on the field
(§24.19 R9): 904 `T4_attrib/attrib_arc`, 237 `T0_gap/arc_resume`, 205 `lli_slip`, 150 `if_slip`, 73 `mw_wl_slip`, 21
`refsat_reset`, 10 `we_refloat` over 6 h; the run-end reports **863 free-epoch latches / 572 inject-site skips** that
are *not individually traced* — a named instrument gap.

### 1.6 Handover / anchor re-election

Three mechanisms, and only one of them is a transformation:

- **`phase_anchor_carry_reref`** (armed on RF-0) — `networkApplyPhaseAnchorCarryReref`,
  `src/core/filter/NetworkPhaseDatum.cpp:825-842`: when a satellite family is re-referenced, it **erases every
  station's carry** for that `(sat, signal)`.  "The frame stepped, so the carried floats are stale; refloat next
  epoch."  A network-wide refloat of one satellite family at one epoch;
- **`phase_datum_handover_continuous`** (armed on RF-0) — the successor anchor is pinned at its **own carried
  `value_abs`** rather than at 0 (the ledger harvested at `NetworkRunContext.cpp:4020-4040` with the BORDER-EXACT
  `pureConditionalLocalVarianceDiag`).  Continuity, not a declared transform;
- **the exact S-transform, which exists in two places but not on this path** — the eq34 D4 carry shift
  (`src/core/filter/NetworkChainDatum.cpp:877-900`: `lc.value_abs += d;` with "**Variances untouched — this is an
  exact basis change, not a refloat**"), and the clock-pivot transform `networkHandoverStransform`
  (`src/core/filter/NetworkHandoverStransform.cpp:93-140`: `x ← x − c0·v_c` on the shared clock carry with an
  exactness self-check).  **`phase_datum_eq34: false` on RF-0, and `handover_stransform` is out of regime on a
  GPS-only lineage** (it requires `scoped_handover_response`, which is structurally inert with one constellation —
  recorded as an exception in `config/golden/golden_field_gps_dual.json:9`).

### 1.7 Death

`last_seen_s` only: the prune at `NetworkRunContext.cpp:4906-4918` erases every entry older than
`phase_leveling_max_gap_s`.  Its own comment: *"it reads ONLY `last_seen_s` — it selects on elapsed time and on nothing
else"*.  No quality criterion, no event, no re-validation on return.

### 1.8 One-sentence summary of §1

**Today the ambiguity is a per-epoch free parameter that is made time-linked by a zero-process-noise scalar prior
carrying last epoch's marginal variance, born with no prior at all from one epoch's data at a variance the model
reports rather than the value deserves, and returned to that same state by any of four un-instrumented reset paths.**

---

## 2. The evidence, mapped onto those mechanisms

| finding | where | the mechanism in §1 that produces it |
|---|---|---|
| **R1 — the level is NOT born at arc start, yet it GROWS over the arc.**  Fraction of arcs whose first-3-epoch level is within 0.3 m of settled: **0.39–0.46** (bar > 0.7); Spearman r(first epoch, settled) **0.17**, trimmed **0.150**; a random mid-arc epoch predicts the settled level better than the first (0.993 vs 0.978); **74–77 % of arcs have a level that GROWS**; arcs present at epoch 1 give r = 0.02 | §24.13 | §1.3 + §1.4 + §1.5.  The level is **not** a converging estimate of a constant.  Two contributions: (a) the local–local over-count (§1.3) makes the carried column absorb a *station-common* quantity asserted `n_LOS`× per epoch; (b) with `q = 0` and a variance that only shrinks, any real drift in what the ambiguity column **absorbs** (receiver phase bias, the station-common share of the iono/ZWD misfit, the un-tied sub-25° slant iono) has nowhere to go and ratchets into the level.  R1 is the *signature* of an over-stiff, over-counted carry, not of a level set at birth |
| **R7 — the wire's within-arc wander is a random walk or steeper, and it is neither code-driven nor the anchor's.**  `r(D_phase, anchor's code level) = 0.079`; `\|r(A)\| 0.307` against a wrong-satellite null of 0.310 (gap −0.003); `σ²(lag 10)/σ²(lag 1)` p50 **16.6**, 100 % above 5 (white ≈ 1, RW ≈ 10, drift ≈ 100).  G32: 0.06–0.17 m through five anchors in 24 min, **1.79 m on one 49.9° anchor held 152 epochs** | §24.15 R7 | §1.3 + §1.5.  A quantity that random-walks while the model says it is a zero-q constant is a quantity whose *carry* is wrong, not whose *data* is noisy.  The G32 contrast is the tell: the walk accumulates with **arc length**, not with handover count |
| **R9 — the wire's `\|ΔD_phase\|` steps sit on T4 refloats, not on raw detections.**  Pooled enrichment 4.29 near a re-level; **split by trigger** the raw T0/T1 detectors enrich **1.16 against a null of 1.02** (chance), while the post-fit **T4 refloat carries the whole 3.69**.  Per arc, level growth vs re-levels inside the arc r = 0.204 (13× the null, far below the 0.5 bar).  Rate elevation-driven (3.93 → 1.05 per 1000 code rows, 10–20° → 60–91°), 3.05× at the eight carry-less stations | §24.19 | §1.5.  **The slip DETECTORS are solve-independent and exonerated** (their counts are identical on every arm).  What costs is the **post-fit refloat's own erase** (`NetworkRaim.cpp:1028`) — the class that destroys a converged prior and re-creates it stiff.  R9's own reading is that the coincidence is *T4 reacting to* an excursion rather than creating one; the two are not separable on these traces, which is why RF-1's claim below is stated as a pre-registered test, not as a derivation |
| **R10 — 43 unsignalled satellite-clock steps > 2 m (to 16.7 m) on six of eight arms; 0 unsignalled PB steps in 590 k rows.  38 of 43 (88 %) at `dt = 0` on a `T4_attrib/attrib_arc` refloat of the same satellite; 3 on `PBGAUGE/handover_continuous`; `cta_recouple` 0 of 43** | §24.19 | §1.5 (T4 erase) + §1.6 (the anchor re-reference erase).  The satellite phase bias is held by the anchor's exact `w = 1e10` pin to zero (§1.2), so the only shared per-satellite parameter a single station's phase level can move is the **clock** — which is exactly what the split shows |
| **§24.20 B — 18 of 43 (41.9 %) of those steps were on the satellite's elected phase anchor; 25 (58 %) had the PB holding ≤ 0.5 m while the clock took the re-realization.  Both consumer-damaging steps (G17, 16.7 m) are ON-ANCHOR with the PB steady to 0.045 m** | §24.20 | §1.2 + §1.5.  An *anchored* `(sat, signal)` column is never carried, but the T4 erase is per-`(station, satellite)` and walks the station's **other** signals of that satellite — so the gauge station's non-anchored arc can be refloated, moving that station's contribution to the satellite's clock while the anchored signal's pin holds the PB.  This is the structural picture the anchor-election card (minimum age, elevation, no incumbent refloat without a declared handover) was pre-registered from, and it is **RF-2's**, not RF-1's |

**The one thing the evidence does *not* say.**  No read has shown that a *refloat causes* the clock step rather than
marking an excursion born elsewhere (R9's own sentence; §24.20 B's split falls between both thresholds).  RF-1 is
therefore designed so that its claim is falsifiable on the two instruments rather than assumed: if the wire's within-arc
D_phase does not fall, the walk is not the carry's and the next card is the level's *content* (what the ambiguity
absorbs), not its *vehicle*.

---

## 3. The alternative, made concrete for THIS estimator

### 3.1 What changes, in one paragraph

The ambiguity becomes a **float state of the station's local block with a carried (value, covariance) that is folded as
a proper prior**: an arc is born with an explicitly recorded, honestly-scaled birth prior; its information accumulates
over the arc from code **and** phase through the existing information-form recursion; the carried object is the
**joint** posterior over the station's carried ambiguity columns, not `N` independent marginals; a slip **drops the
prior back to the birth prior** — a refloat in the literal sense — instead of skipping one inject or erasing the entry;
and an anchor handover is a **declared S-transformation** of the carried values with the variances untouched, instead
of a network-wide erase.  `q` stays at 0.

Four of those five pieces already exist in the tree in some form.  RF-1 is mostly **re-pointing and completing**
existing machinery, not new estimation math — which is the main reason it is a cheap card.

### 3.2 The prior fold: exactly what changes in the normal equations

Today, per carried column `c`:

```
Λ_c = 1 / (var_c + q·Δt)                 with q = 0  ⇒  Λ_c = 1/var_c
η_c = Λ_c · (value_abs_c − x_lin_c)      ApplicationNetworkAssembly.cpp:3214-3215
N_rr += Σ_c Λ_c e_c e_cᵀ ,  u_r += Σ_c η_c e_c        (addStationPrior, rank-1 LDLᵀ)
```

with the Card-1 coupling row replacing the pure diagonal on RF-0 (`buildCplRow`, `NetworkCoupling.cpp:158-260`).

Under RF-1, per **station** and per epoch, over the set `C` of carried, present, non-gauge-pinned ambiguity columns:

```
Σ = P_prev|C  +  Q·Δt          (Q = 0 on the ambiguity block, see §3.5)
Σ = L Lᵀ  ;  fold the k unit-weight rows  L⁻¹·[ I_C | M_C | y_C ]
y_C = (value_abs_C − x_lin_C) + M_C ŝ_prev        (the Card-1 RHS, unchanged)
```

**This is `CouplingRowBuilder::buildLocalBlock` verbatim** (`include/filter/NetworkCoupling.hpp:249-266`): "the
off-diagonals are the HARVESTED conditional cross covariances `(N_rr⁻¹)_{c_i c_j}` read from the two entries' shared
`PlHarvestBlock` … Σ = LLᵀ, and the n rows `L⁻¹·[I | M | y]` are buffered as unit-weight multi-local `PendCplRow`
entries … **Exact for EVERY local-local direction given the border** — the per-LOS iono direction F1 corrects AND the
station-common rx clock / ZWD / rx-DCB directions it leaves over-counted."

So: **the "carried covariance folded as a proper KF prior" half of RF-1 is F3, which is built, unit-tested
(`tests/test_young_arc_inflate.cpp` exercises it at the real seam), and armed on the sim golden.**  What RF-1 changes
is that it stops being an optional knob and becomes the vehicle's definition, and that the block's provenance rules
(pair-miss zeroing when two entries were harvested at different epochs — `NetworkRunTypes.hpp:404-412`) become part of
the contract rather than a degradation path.

The harvest cost is already paid: the cross terms are "a GATHER, not a second solve" — column `a`'s
`N_rr⁻¹ e_c` solve is the one `stationRegression` already performs for `cond_var` and `row`
(`NetworkRunTypes.hpp:404-407`).

### 3.3 Birth: an honest prior instead of no prior

**Today:** no prior; free solve; stash at the reported conditional variance (§1.4).
**Under RF-1,** three changes, in increasing order of commitment:

**(a) Record the birth prior.**  `LocalCarry::var_first` **already is** the arc's first-stash variance, set once and
kept across re-levels, restarted only by erase-and-rebirth (`NetworkRunContext.cpp:4536-4541`,
`NetworkRunTypes.hpp:436`).  It is today read only by
`network.phase_datum_handover_max_var_ratio`.  RF-1 makes it the **object a drop restores** (§3.4).  This costs
nothing and is exactly right semantically: *the arc returns to what it was worth when it was born.*

**(b) Floor it.**  `network.amb_float_birth_var_m2` (default: the code-noise scale of the posture, ~`(1.0 m)²`, to be
fixed by the calibration in (c) — not a tuning constant to be swept).  Applied as
`var_birth = max(var_first, amb_float_birth_var_m2)` at the **birth stash** and again at every drop.  This is the guard
against the two routes of §1.4 that hand back a phase-stiff variance for a code-quality value — the band-scoped free
and the SH-tie-pinned iono — **without** touching the mature arcs (unlike a uniform `phase_leveling_var_floor_m2`,
which floors everything and was already available and unused).

**(c) Calibrate it, don't guess it.**  The instrument exists: `network_prior_residual.csv`
(`NetworkPriorResidual`, header at `include/logging/NetworkRunTraces.hpp:153-165`) reports, per family and label, the
prior-residual `σ̂₀`, the `n_gt_k` tail histogram and the per-row `(e, fold_var, post_var, gap_s)` winner.  The `pl`
family is fed from the harvest (`NetworkRunContext.cpp:4372`ff, with the `folded` predicate **reproduced** from the
inject rather than mirrored by hand).  **If the carried prior's variance were honest, `σ̂₀(pl) ≈ 1`.**  RF-1's birth
floor is set from the measured `σ̂₀(pl)` on the RF-0 baseline, restricted to arcs in their first `K` folds — and the
`carry_vce_apply` machinery (`NetworkCarryVceApply::kPl`, the α² factor read identically at inject and harvest) is the
already-built vehicle for making it adaptive later.  **That is a follow-on, not RF-1.**

**What RF-1 deliberately does NOT do at birth:** it does not fold a prior at the birth *epoch*.  The column must solve
free the epoch it is created, because a prior formed from that epoch's own data and folded into the same epoch is
data incest.  "An uninformative prior at birth" is, in information form, exactly today's behaviour — and it is
correct.  **The defect is not a missing birth prior; it is that the first CARRIED prior (epoch 2) is stamped with a
variance the value does not deserve.**  This distinction matters: it is why RF-1 is a small, exactly-specified change
and not a re-architecture.

### 3.4 A slip is a DROP of the prior — and only that

**Today:** four paths (§1.5), two of which (free-epoch latch, gap cliff) *skip one inject and re-harvest a stiff level*
and two of which (T4/T3 erase, prune) *delete the entry*.

**Under RF-1, one path:**

```
drop(station, sat, signal):
    lc.var        = max(lc.var_first, amb_float_birth_var_m2)   // restore the birth prior
    lc.coupling.clear();  lc.blk = nullptr;  lc.blk_idx = -1    // the border regression and the block
                                                               //   cross terms belong to the OLD value
    lc.relevel_s  = now                                        // the age clock restarts
    // value_abs, birth_s, birth_ep, var_first, last_seen_s are KEPT
```

and the prior **is still folded this epoch**, at birth strength.  Three consequences:

1. **The decoupling edge is bounded** (standing invariant #7).  Today a free epoch removes *all* information about that
   column for one epoch — an unbounded direction opens and the shared parameters re-balance into it, which is the
   mechanism by which a single station's refloat can move a satellite clock by metres.  Under RF-1 the drop removes
   information down to a *stated, bounded* level; the estimator's response is proportional.
2. **The re-coupling edge is re-validated by construction**: the arc re-earns its variance as information accumulates,
   and the prires census can see it happen (the `fold_var` trajectory).  No "first sighting" mis-read: `birth_s` and
   `birth_ep` survive, so the age gates, the sim slip oracle's window floor and the restored-arc sentinel
   (`LocalCarry::kBirthEpRestored`, `NetworkRunTypes.hpp:439-446`) all keep their meaning.
3. `value_abs` is kept because the drop is a statement about *confidence*, not about *identity*.  Keeping it costs
   nothing (a prior at birth variance is dominated by the epoch's data) and preserves a starting point for the
   lane-preserving refloat that `SPEC_lane_slip_response` owes — which is the natural successor card.

The **erase** is kept as a distinct, rarer action for the one case that means "this is a different arc": the gap prune
past `phase_leveling_max_gap_s` and an explicit re-admission beyond the readmit horizon.  It must stay an **event with
both edges counted**.

**What the detectors do is unchanged.**  R9 proved the raw T0/T1 detectors are solve-independent and enrich at chance
against the wire (1.16 vs 1.02): they stay exactly as they are.  What changes is only the *response* they invoke.

### 3.5 `q`

**`q` stays 0 on the ambiguity block.**  An ambiguity is a random constant punctuated by discrete slips, and `q = 0` is
exact recursive LS for it (memory `project_network_phase_leveling`).  A `q`-ladder was falsified as a fix for the
sibling lock-in (`runs/zwdabl_{q7,q6,q5,floor}`: settled bias unchanged-to-worse, the first-injection jump identical on
all four, and integers strictly worse — memory `project_carry_datum_handover_theory`), and the level-channel lesson is
the same shape (`CLAUDE.md` Amendment 6's corollary: do not fix the level by tuning a carry's memory model in either
direction).  **The knob `network.amb_float_q_m2s` exists in the design only so the default is explicit and the run
manifest records it; the card's posture is 0.0 and any nonzero value is a separate, evidence-bearing arm.**

If R1's "74–77 % of levels grow" survives RF-1 with `q = 0` and the joint block, that is the *result*: the growth is
not an artifact of the over-count, and what the ambiguity **absorbs** is genuinely time-varying — which is a content
card (the un-tied sub-25° slant iono, the receiver phase bias), not a vehicle card.

### 3.6 The S-basis: what is estimable, and what a drop can and cannot move

**The realized basis on RF-0.**  Per constellation a pivot-satellite clock Dirichlet pin; per `(satellite, signal)`
family an **elected anchor station whose ambiguity is pinned to ZERO at `w = 1e10`**
(`ApplicationNetworkAssembly.cpp:2833, :2842`) — Odijk Eq. 4; free receiver clocks / ISBs; the W6 code-bias datum.
The emitted satellite phase bias is therefore `b̃ˢ_σ = bˢ_σ + λ_σ N_{r*,σ}ˢ + …`, referenced to the anchor `r*`.

**Estimable / not estimable, unchanged by RF-1:**

- **DD ambiguities** (two stations × two satellites, same signal) are integer — the S-basis lumps are per-receiver and
  per-satellite constants that cancel over a closed DD cycle;
- **SD (between-satellite, one station) ambiguities** are biased by the receiver phase bias and are **not** integer;
- the per-LOS slant iono is estimable only as `ı̃ʳₛ = ıʳₛ + d_{r,GF} − dˢ_{GF}` (Odijk 2016 Table 8), which is why the
  DD-invariant graders (`scripts/simnet_dd_reref.py`, `simnet_amb_integer.py --ref-free`) are the correct instruments
  and raw wire steps are not (standing invariant #5).

**Why a prior fold cannot change any of this.**  A carried prior is a pseudo-observation on a column that *already
exists* in `A_r`; it is rank-**preserving**, not rank-adding.  The one way it could break the basis is by fighting an
exact gauge, and the inject already excludes gauge-pinned columns (`:3236-3238`) on the same predicate as the harvest.
**RF-1 must preserve that exclusion verbatim** — it is the single load-bearing line of the S-basis contract on this
path.

**What a DROP can move.**  Dropping a *non-anchored* column's prior removes information from a direction the anchor pin
does not fix.  With the PB pinned to the anchor and the satellite clock free, the network re-balances into the clock:
that is R10's 88 % / §24.20 B's 58 % "PB held, clock took it".  Under RF-1 the drop is bounded (§3.4), so the
re-balance is bounded — but it is **not zero**, and that is the honest limit of this card.

**What a declared S-transformation fixes, and what it does not.**  RF-1 replaces
`networkApplyPhaseAnchorCarryReref`'s network-wide erase (`NetworkPhaseDatum.cpp:825-842`) with the **exact carry
S-transformation already implemented twice in the tree**:

```
at a (sat, signal) anchor handover r_out → r_in, with both levels in the handover ledger
(ctx.carry.handover.value_abs, NetworkRunContext.cpp:4020-4040):

    Δ = value_abs(r_in, sat, signal)                       // the incoming anchor's carried level
    for every station r carrying (sat, signal):  value_abs_r −= Δ        // variances UNTOUCHED
    the incoming anchor's own column then reads 0 — an exactness self-check, exactly the
    sts_selfcheck_ok pattern of NetworkHandoverStransform.cpp:60-64
```

This is the eq34 D4 operator (`NetworkChainDatum.cpp:877-900`, "**Variances untouched — this is an exact basis change,
not a refloat**") applied to the phase-anchor family instead of the eq34 chain, with the clock-pivot transform
(`NetworkHandoverStransform.cpp:93-140`) as the structural model.  It **eliminates the 3-of-43 `PBGAUGE/
handover_continuous` class** and, more importantly, removes a whole-family refloat (every station, one epoch) from the
run's event inventory.

⚠ **It does NOT eliminate the 38-of-43 T4 class.**  A T4 refloat is not a frame change — there is no `Δ` to apply,
because the arc's own level is what is being doubted.  What bounds that class is §3.4's bounded drop, and what
*declares* it is `network.clock_disc_jump_test`, already validated on this vehicle (§24.20 A: 0 unsignalled steps,
5 / 8 marks in 6 h against 1144 / 904 refloats = 0.02–0.04 % of satellite-epochs, byte-identical solve, consumer
55.8 → 2.7 m at the event) and part of RF-0's frozen posture.  **The complete answer to R10 is RF-1's bounded drop plus
RF-0's marking rule plus RF-2's anchor election — three cards, and this one is only the first.**

⚠ **Two latent defects the transform must not re-open** (both already filed):
(i) D4 shifts `lc.value_abs` and the coupling terms' `s_prev` **together**, because a Card-1 coupling row's correct
`y`-change under a *known* frame step is the exact cancellation of the two moves — shifting one and not the other
injects the frame step as row bias (`NetworkChainDatum.cpp:852-866`).  RF-1's anchor transform must do the same;
(ii) the transform must be an **event with both edges** — `STRANSFORM/sts_applied` with identities and the exactness
self-check, per invariant #7 and the FT63 topology-ledger lesson.

---

## 4. What becomes redundant, and what must stay

### 4.1 Retired by RF-1 (config-only, on the RF-0 posture)

| mechanism | why it goes |
|---|---|
| **the free-epoch latch** `network.cycle_slip.relevel_free_epoch` (`NetworkSlipQc.cpp:619` etc.; `ApplicationNetworkAssembly.cpp:3264-3270`) | its entire purpose is "give the arc one epoch of freedom so the level re-draws".  Under RF-1 the drop *is* the response and is strictly better-behaved (bounded, no re-harvest at a stiff variance).  Keep the *latched-set* plumbing — the lane observatory, the phase-datum candidacy screen (`NetworkPhaseDatum.cpp:306`), the anchor-slip handover staging (`:817-820`) and the harvest's statistics guards all read it — and re-point it at the drop set |
| **the inject-site pending-reseed check** (`:3263`) | already structurally dead for detector marks; the drop set is the live predicate |
| **`phase_leveling_young_inflate_*`** (`GnssConfigLoadNetwork.cpp:3491-3499`) | it is a *time-decaying patch on the inject* for exactly the defect §3.4 fixes at the source.  Not armed on RF-0; it should stay unarmed and be retired if RF-1 wins.  Its `relevel_s` stamp and `NetworkYoungInflate::congruence` are reused |
| **`phase_leveling_los_scale`** (F1) | F1's `×B_eff` is an approximate correction for the per-LOS iono over-count that the joint block (§3.2) removes **exactly** — the F3 doc comment says so directly.  Off on RF-0; keep it off |
| **the `carry_invariant` family** (`network.carry_invariant*`, `NetworkDdPriorBuild.cpp:181` returns early when off; `networkInstallCarry` called only under `if (carry_invariant)`, `ApplicationNetwork.cpp:2342`) and its **D5 innovation gates** (`carry_invariant_innov_*`) | already the *other* vehicle and already off on RF-0.  RF-1 makes that permanent: one vehicle, two beds |
| **the lane layer's fold** (`lane_carry.*`) | already off on RF-0; the standing verdict is "MW/WL as conditioning — do NOT arm; net-harmful", now with the over-retention mechanism visible (CLAUDE.md Amendment 1) |
| **`phase_anchor_carry_reref`'s erase** | replaced by the declared transform (§3.6).  The knob name survives; its *semantics* change from erase to transform, which is a solve-affecting change and needs its own arm |

### 4.2 Must stay

| mechanism | why |
|---|---|
| **every raw slip DETECTOR** — MW (`mw_detect`), LLI, IF-phase TD (`if_phase_td` + `if_phase_td_ref_gate`), `mw_lli_corroborate`, `mw_evidence_refloat`, `gap_instrument` | R9 measured them **solve-independent** (identical counts on all three arms) and at chance against the wire (1.16 vs a null of 1.02).  They are right; only the response was wrong.  Standing invariant #1 also *requires* a wired response for any stiff time-linked quantity — RF-1 supplies a better one, it does not remove the requirement |
| **the gauge-pin exclusion at inject and harvest** (`:3236-3238`) | the S-basis contract (§3.6) |
| **`carry_coupling_gauge_project`** (Card 1f) and `carry_conditional_coupling` / `shared_carry_full_cov` | the shared-border half of the accounting; RF-1 adds the local–local half beside it, it does not replace it |
| **the clock anchor** (`clock_anchor_q: 2.9e-8`, the Q3 physical floor) | RF-0's posture; independently validated (code rms 1.79 → 1.09 m, consumer 0.95 m) and independently *exonerated* as a step source by R10 |
| **`clock_disc_jump_test`** | the M4 contract; the only thing that declares the residual re-realizations RF-1 bounds but does not abolish (§3.6) |
| **the RAIM pool and its σ̂₀** (`NetworkRaim.cpp:122-135`, `ambPool`) | the pool is `R.pl_amb` on a leveling lineage (`qc_amb` is the `carry_invariant` re-host).  It is the **admission by ambiguity partial**, i.e. the per-`(station, satellite)` post-fit phase residual max.  Under RF-1 this stays — but its *action* changes from erase to drop, and its statistic should be re-read: on a free local column the phase tension is absorbed by the column itself (`SPEC_raim_pool_rehost` §7.2/§7.3 measured `s_raw` p50 ≈ 0.02 with σ̂₀ pinned at its 1.0 floor on `carry_invariant`).  **On RF-1 the same question must be re-asked**, because a bounded-birth-prior column is *less* free than today's refloated one, which should make the pool *more* informative, not less — a pre-registered read, not an assumption |
| **`trace_prefit_census`, `trace_topology`, `rhs_attrib_on_trip`** and the run-end counters | invariant #4: the counters are the ON-path telemetry |

---

## 5. Cost at scale

**The local block is per station and stays per station.**  `addStationPrior` folds rank-1 updates into the station's
cached `N_rr` LDLᵀ factor and leaves `N_ss` / `u_s` untouched — "a per-station local increment is reduction-orthogonal
to every other station and to the shared null-space" (`include/filter/NetworkNormalEquations.hpp:244-248`).  The joint
block is a *station-local* object.  **Nothing in RF-1 is O(N²) across stations, and the shared reduction is
unchanged.**

**Measured dimensions** (§24.10's `network.trace_station_block`, 61 stations, GPS dual, the PL vehicle): WROC
`n_local = 29–32`, `rank = n_local` from epoch 1, condition number falling to 2.7e4 by epoch 20.  Structurally
`n_local ≈ 3·n_sat + 3` (per LOS: 1 slant iono + 2 ambiguities; plus clock, ZWD, rx code bias).  Carried ambiguity
columns per station `k ≈ 2·n_sat ≈ 16–24`.

| item | cost |
|---|---|
| harvest cross terms | **free** — a gather off the per-column `N_rr⁻¹e_c` solve `stationRegression` already pays for `cond_var` and `row` (`NetworkRunTypes.hpp:404-407`) |
| the joint fold | one `k×k` Cholesky + `k` triangular solves of `[I \| M \| y]`, `k ≈ 20` ⇒ `O(k³/3) ≈ 2.7 kflop` per station-epoch, `≈ 0.17 Mflop` over 61 stations.  Negligible beside the 2266 ms/epoch total the 63-station lane A/B measured |
| memory | one `k×k` double `PlHarvestBlock` per station-epoch, `shared_ptr`-owned and released when its last referencing entry is re-stashed: `61 × 20² × 8 B ≈ 195 kB` |
| the drop | `O(1)` per dropped column |
| at 305 stations | linear in stations: `305 × 20² × 8 B ≈ 1 MB`, `0.8 Mflop`.  Not the scaling problem — `applyLaneMeasurement`'s dense 2K×2K joint block was, and the lane layer is off |

**Owed measurement:** F3's actual field wall-clock at 61 stations.  It is armed on `golden_sim_tri_w6` (60 stations)
and has never run on the field posture.  The RF-1 A/B measures it directly (`network.trace_section_timers`).

---

## 6. Migration and tests

### 6.1 Migration

**Out of scope, by the re-founding decision:** seeds, fingerprints, warm-resume seams, snapshots.  RF-0's posture has
`state_snapshot_*` off and the WTZZ consumer cold; the decision cuts seams.  Record, do not act:

- `network.ambiguity_vehicle` is **solve-affecting**, hence fingerprint-relevant.  Because no lineage in RF-0 resumes,
  no migration card is needed now.  When seams are added back, the vehicle joins the datum-relevant config fingerprint at
  `NetworkRunContext.cpp:643-690` — one line, and the snapshot's `L`/`Z`
  record already carries `value_abs`, `var` and `var_first` (`makeRestoredLocalCarry`, `NetworkRunTypes.hpp:495`ff);
  `var_birth` must join it or a restored arc cannot be dropped correctly.  **Name this in the RF-1 doc now so the
  seam card does not rediscover it.**
- The goldens re-base **once**, when the posture card lands (the §24.11 decision).  RF-1 arms are copies of the RF-0
  config with one knob changed, per the golden-configs-first rule.

### 6.2 Unit tests — `tests/test_amb_float_vehicle.cpp` (new file, new suite; one suite per file)

1. **OFF byte-identity.**  `ambiguity_vehicle: "level"` reproduces the pre-card assembly exactly on the
   `test_network_inject` / `test_network_coupling` fixture pattern — the `inj` triples and the `PendCplRow` set
   compared element-wise.
2. **1/n.**  A synthetic 2-station block (one satellite, two bands, a fixed geometry, code σ 1 m / phase σ 3 mm):
   fold the carry over `n = 1..30` epochs under `float_kf` and assert `var(n) · n / var(1) ∈ [1−tol, 1+tol]` and that
   `value_abs(n)` converges to the true ambiguity.  **On the same fixture assert that the LEGACY path does NOT satisfy
   it** — it should collapse toward the `K_eff = (B+1)/(B−1)` floor the F1 comment predicts.  That contrast is the test
   that proves the joint block is doing the work.
3. **A dropped prior restores the birth variance.**  After `n` folds, drop; assert `var == max(var_first,
   amb_float_birth_var_m2)` exactly, that `coupling`/`blk` are cleared, and that the next fold's `Λ` equals the birth
   fold's `Λ` to the ULP.
4. **A drop is not an erase.**  `birth_s`, `birth_ep`, `var_first` and the restored-arc sentinel survive a drop; only
   an explicit erase (gap prune) restarts them.
5. **The gauge-pin exclusion holds under `float_kf`** — an anchored `(sat, signal)` column receives no prior on either
   the inject or the harvest side.
6. **The declared anchor S-transform is exact.**  Over a synthetic handover: every carried `value_abs` of the family
   shifts by the same `Δ`, every `var` is bit-unchanged, every coupling `s_prev` on a shifted PB column moves with it
   (the `NetworkChainDatum.cpp:852-866` latent defect), the incoming anchor's column reads 0 within
   `kStsExactTolM`, and the `STRANSFORM` event carries the identities.
7. **The drop set is the one predicate.**  The lane observatory, the phase-datum candidacy screen and the anchor-slip
   staging read the same set the inject consulted (the `free_ambs` contract, re-pointed).

Existing suites to extend rather than duplicate: `test_network_coupling.cpp` (the block build),
`test_young_arc_inflate.cpp` (the congruence properties — now structural), `test_network_phase_datum.cpp` (the
handover), `test_relevel_bridge_v3.cpp` / `test_gap_reentry_relevel.cpp` (the response paths).

### 6.3 Sim-first validation (the workflow rule: sim first, then field)

Bed: `golden_sim_tri_w6` restricted per RF-0 (GPS dual, `clock_anchor_q: 2.9e-8`, `clock_disc_jump_test: true`, lanes
off, F3 → structural under `float_kf`), 6 h cold.

| read | script | what it answers |
|---|---|---|
| **DD ambiguity error vs truth over the arc** | `scripts/simnet_amb_integer.py --ref-free` | does the float ambiguity converge toward its true integer as the arc ages?  Graded `e = dd_cyc_est − DD_true` in FULL cycles, ref-free (the §6.12 reference-satellite artifact) |
| integer-ness histogram | same, per signal + the WL/EWL rungs | which rung breaks, and whether it moves |
| DD fractional, truth-free | `scripts/simnet_dd_reref.py` | the field-comparable instrument |
| **the variance trajectory** (§7's diagnostic) | the new `network_amb_state.csv` | the direct test of §3.2 |

---

## 7. The build plan

### 7.1 The knob

```
network.ambiguity_vehicle : "level" (DEFAULT) | "float_kf"
```

String enum on the established loader pattern (`network.trace_dd_fractional_ref`,
`GnssConfigLoadNetwork.cpp:1491-1504`; `network.raim_erase_band_scope`, `:1512-1518`): unknown value ⇒ `log->error`
naming what it changes and fall back to `"level"`.  Enum in `include/config/GnssConfigTypes.hpp` beside
`NetworkDdFracRef`.  `"level"` is byte-identical to today (invariant #6).

Supporting knobs, all default-inert:

| knob | default | role |
|---|---|---|
| `network.amb_float_birth_var_m2` | `0.0` (= use `var_first` unfloored) | §3.3(b)'s floor |
| `network.amb_float_q_m2s` | `0.0` | explicit, manifest-recorded; **not** to be swept (§3.5) |
| `network.amb_float_declared_handover` | `false` | the anchor S-transform replacing the erase (§3.6) — solve-affecting, its own arm |
| `network.trace_amb_state` | `false` | the per-arc trace (§7.4) |

Loader interlocks: `float_kf` **requires** `phase_leveling` and `!carry_invariant` (hard error otherwise — one vehicle);
`float_kf` implies the joint block, so `phase_leveling_local_block` and `phase_leveling_los_scale` become
loader-refused-with-a-line under it (they are the mechanisms it subsumes); `phase_leveling_young_inflate_var_m2 > 0`
under `float_kf` is a WARN naming §4.1.

### 7.2 Seams touched (per the anti-regrowth decision tree)

| concern | file | change |
|---|---|---|
| carried state | `include/filter/NetworkRunTypes.hpp` | `LocalCarry` gains `var_birth` (the floored birth prior actually in force) and a `dropped_ep` stamp; `RelevelEpochState` gains the `drop_ambs` set beside `free_ambs` |
| per-epoch fold | `src/core/app/ApplicationNetworkAssembly.cpp` | the inject's vehicle branch: `float_kf` ⇒ the joint-block route unconditionally; the free-epoch skip (`:3264-3270`) becomes the drop's *fold at birth strength*; the stale-gap skip (`:3277-3288`) becomes the same |
| carried-prior algebra | `src/core/filter/NetworkCoupling.cpp` / `.hpp` | `buildLocalBlock` re-pointed: structural, not knob-gated, under `float_kf`; the PD fallback stays and **must be counted** (a silent vehicle change mid-run is the trap) |
| harvest / stash / prune | `src/core/app/NetworkRunContext.cpp` (`networkHarvestLocalCarries`) | `var_birth` set once at birth (floored); the drop applied here or at the response site, never both |
| slip response | `src/core/slip/NetworkSlipQc.cpp` | the five latch sites (`:619`, `:1311`, `:1866`, `:1963`, `:2141`) insert into `drop_ambs` under `float_kf` |
| post-fit QC response | `src/core/filter/NetworkRaim.cpp` | the T4 (`:1028`) and T3 (`:1207`) erases become drops under `float_kf`; the lane pre-erase capture (`lane_t4_pre`) keeps working because `value_abs` survives |
| handover | `src/core/filter/NetworkPhaseDatum.cpp` | `networkApplyPhaseAnchorCarryReref` (`:825-842`) gains the declared-transform branch, built on `NetworkChainDatum.cpp:877-900` and self-checked like `NetworkHandoverStransform.cpp:60-64` |
| traces | `include/logging/NetworkRunTraces.hpp`, `src/core/logging/NetworkRunTraces.cpp` | `network_amb_state.csv` |
| run-end telemetry | `src/core/filter/NetworkTeardown.cpp` | the drop/erase/transform counters beside the existing re-level clause (`:2081-2087`) |
| config | `include/config/GnssConfigTypes.hpp`, `include/config/GnssConfig.hpp`, `src/core/config/GnssConfigLoadNetwork.cpp` | the knob + interlocks + the summary line |
| tests | `tests/test_amb_float_vehicle.cpp` | new file; **add to `CMakeLists.txt` by hand** (no globbing) |

If any of this exceeds a thin branch in the assembly, it goes in a new seam `src/core/filter/NetworkAmbFloat.cpp`
(free function over `(NetworkRunContext&, const GnssConfig&, log, NetworkRunTraces&, …)`, alias preamble, owns no
state) with a manual `CMakeLists.txt` entry beside `NetworkCoupling.cpp` (line 223).

### 7.3 Commit order

1. **Knob + types + loader + interlocks + the OFF byte-identity gate.**  No behaviour.  Suite green, both smokes hash-identical, `make_golden_config.py --verify` PASS.
2. **`network.trace_amb_state`** — measure-only, byte-identical, armed on a twin of the RF-0 baseline.  **This lands and is READ before any mechanism**: it is the diagnostic that must move, and a pre-change baseline is required.
3. **The joint block, structural under `float_kf`.**  Solve-affecting; the first A/B.  Isolated so its cost and its effect are separable from the drop's.
4. **`var_birth` + the drop semantics** (slip response + T4/T3).  The second A/B.
5. **The declared anchor handover S-transform** behind `amb_float_declared_handover`.  The third A/B.
6. **Retirement** — config-only: the free-epoch latch, F1, the young inflate off; the ledger rows written.

Each step is default-OFF and byte-identical until armed (invariant #6).  Steps 3–5 are separately armable so a failure
attributes.

### 7.4 The diagnostic that must move — `network_amb_state.csv`

R1's statistic ("the fraction of arcs whose level is set at birth") was the right question asked of the wrong quantity:
it read the *code residual* per LOS arc because there is no trace of the carry itself.  There is none today — the
network's 40-odd traces (`NetworkRunTraces.cpp:29-71`) include no per-arc carry state, and the prior-residual census
(`network_prior_residual.csv`) is **aggregated per (family, label) per epoch**, so it cannot give a per-arc trajectory.

```
week,tow,station,sat,signal,
  value_abs_m,var_m2,var_first_m2,var_birth_m2,fold_var_m2,     // what was carried and what was folded
  n_folds,age_ep,age_since_relevel_ep,                          // the trajectory's abscissa
  folded,route,                                                 // 0/1; diag | block | coupling-scalar
  event                                                         // "" | birth | drop:<cause> | erase:<cause> | stransform
```

**Pre-registered reads** (RF-0 baseline first, then each arm):

- **(D1) the variance trajectory.**  Per arc, `fold_var(n)` against `n`.  Fit `log var = a − b·log n` over the first 30
  folds.  **RF-0 prediction: `b > 1`** (faster than `1/n` — the over-count).  **RF-1 bar: `b = 1.00 ± 0.15` at p50
  across arcs.**  This is the single most direct test of §3.2 and it is cheap.
- **(D2) the level trajectory** — R1 re-asked on the carry itself: the fraction of arcs whose `value_abs` at folds 1–3
  is within 0.3 m of its settled value (RF-0 reads 0.39–0.46 on the code-residual proxy), and the fraction whose
  `|value_abs|` **grows** monotonically over the arc (RF-0: 74–77 %).  **RF-1 bar: the growing fraction < 55 %**
  (chance for a converged estimate of a constant).  If it does not move, §3.5's alternative reading applies and the
  next card is the level's content.
- **(D3) the drop inventory.**  Drops per hour by cause, `fold_var` immediately before and after each drop, and the
  number of folds to return to the pre-drop variance.  **Invariant #4:** a `float_kf` run with 0 drops is a miswire,
  not a quiet run.
- **(D4) `σ̂₀(pl)` from `network_prior_residual.csv`**, split by arc age (first 5 folds / 5–30 / > 30).  An honest
  prior reads ≈ 1 in every bucket; RF-0 is expected to read > 1 in the young bucket.  This is also the calibration
  input for §3.3(c).

### 7.5 Gates

Standard: Release-only runs (invariant #2); `vs2026_build.ps1 -Config x64-Release`; full `gnss_tests` green;
both smokes (OFF and ON) hash-identical on the OFF path; `python scripts/make_golden_config.py --verify` PASS;
every arm a copy of the RF-0 config with **one** knob changed; traces to a run-scoped `logs/` (the stale-trace-dir
trap); a renamed exe copy for concurrent runs.

### 7.6 Pre-registered bars — the two instruments, as deltas against RF-0

The RF-0 numbers below are PV-B's (§24.11, field, 2 h) and are placeholders until RF-0 publishes its own 6 h baseline;
**the bars are written as deltas so they survive the re-base.**

| | instrument | RF-0 field reference | **RF-1 bar (field, 6 h cold)** |
|---|---|---|---|
| **I1** | cold consumer 3D, per-hour p50 | 1.19 m (last 30 min of 2 h); 0.55 m at the 2 h mark | **no hour worse than RF-0**; settled p50 **≤ 0.6 × RF-0**; the epoch of first sub-decimetre earlier or unchanged.  Target (the decision's): sub-decimetre settled |
| **I1b** | cold consumer 3D, per-hour p90 | — (RF-0 publishes) | **no hour worse than RF-0** |
| **I2** | wire vs CODE, within-arc `D_phase` p50/hour (`scripts/wire_vs_code_census.py`) | **0.95 m** | **≤ 0.5 m** (the §24.8 (iii) bar PV-B missed) **and ≤ 0.6 × RF-0 in every hour** |
| **I2b** | wire vs CODE, `D_code` and the clock LEVEL | 0.13 / 0.74 m | **not worse than RF-0** |
| **I2c** | unsignalled clock steps > 2 m (`scripts/wire_step_audit.py`) | **0** (with `clock_disc_jump_test` on) | **0**, and the mark count **not more than 2×** RF-0's (no mark storm) |
| **I2d** | marks per satellite-epoch | 0.022–0.035 % | **< 0.1 %** |
| **guard** | availability / emitted epochs | 720/720, 0 withheld | **within 1 %**, 0 S-basis withholds |

| | sim (6 h, the restricted sim golden) | bar |
|---|---|---|
| **S1** | consumer 3D per-hour p50 against the bed's truth consumer | not worse than RF-0 in any hour; settled ≤ 0.6 × |
| **S2** | wire vs truth, within-arc `D_phase` | ≤ 0.6 × RF-0 |
| **S3** | DD ambiguity error vs truth (`simnet_amb_integer.py --ref-free`), settled | med \|e\| **strictly smaller** than RF-0 on every rung it grades; **the arc-age slope must be negative** (the error falls as the arc matures) — that is the property "a float state with covariance" claims and the leveling vehicle does not have |
| **S4** | D1's `b` | `1.00 ± 0.15` |
| **S5** | D2's growing fraction | < 55 % |

**Frozen readings.**  I1 and I2 both cross ⇒ RF-1 is the vehicle and the goldens re-base on it (both beds, one posture).
I2 crosses and I1 does not ⇒ the wire is fixed and the consumer's remaining error is elsewhere (the §24 census already
points at the producer's own code residuals) — RF-1 is kept as a wire fix, on the author's go, and the consumer card is
next.  Neither crosses but S4/S5 do ⇒ the vehicle is *correct* and not *sufficient*: the level's content, not its
carry, is the defect, and the next card is the un-tied sub-25° slant iono / the receiver phase bias (§3.5's
alternative reading).  S4 fails ⇒ the joint block is not doing what its own documentation claims and the build stops at
step 3.

---

## 8. Risks, ranked

1. **★ The birth variance may not be derivable from the model as folded.**  RF-1 makes the carried prior honest about
   *phase noise*; it cannot make it honest about a *model* that is optimistic exactly where the level is wrong — the
   25.8 % of iono rows below the 25° tie cutoff that own the entire metre-plus tail (§24.13 finding 2), and the
   band-scoped free that ties a freed band to its sibling's error at phase precision (§1.4).  If that is where the
   level's error lives, RF-1 will produce a correctly-converging state with a wrong level, D1/S4 will pass, and I2
   will not move.  **Mitigation:** D4 (`σ̂₀(pl)` by arc age) is the direct measurement and is a *step-2*, measure-only
   read — it can refute the card before any mechanism is built.  This is the risk I consider largest.
2. **The bounded drop keeps a bad level alive.**  Today the T4 erase is the only path that fully discards a level.
   A drop that restores a code-scale variance still *injects*, and a level that is wrong by many cycles can re-lock at
   the new variance.  **Mitigation:** the erase survives for the gap/re-admission case, and the drop's cause is
   traced (D3) so a re-locked arc is visible as a repeated drop on the same key.  The lane-preserving refloat
   (`SPEC_lane_slip_response`'s owed build) is the real answer and is the natural successor.
3. **The joint block's fallback silently changes the vehicle mid-run.**  `buildLocalBlock` returns false on a
   non-SPD factor after the ridge retry and the caller falls back to per-column rows.  Under `float_kf` that is a
   *vehicle change on one station-epoch*.  **Mitigation:** count it, log it at WARN with identities, and make a
   fallback rate above a small threshold a run-end failure rather than a note.
4. **The declared handover transform re-opens the filed D4 latent defect** (`value_abs` moved without the coupling
   `s_prev`, injecting the frame step as row bias — `NetworkChainDatum.cpp:852-866`).  **Mitigation:** test 6 pins
   both halves; the exactness self-check is copied from the clock transform.
5. **Attribution risk in the campaign, not the code.**  Steps 3, 4 and 5 all move the same two instruments.  Arming
   them together would leave a win unattributable.  **Mitigation:** the commit order is also the arm order, one A/B
   each.

---

## 9. For the author

Three questions the design cannot settle from the tree:

1. **The birth floor's value.**  §3.3(b) proposes `max(var_first, amb_float_birth_var_m2)`.  Is the floor set from
   D4's measured `σ̂₀(pl)` in the young bucket (the empirical route, one extra read), or fixed a priori at the
   posture's code-noise scale (the simple route, one fewer variable)?
2. **Whether `value_abs` survives a drop.**  §3.4 keeps it (the drop is about confidence, not identity, and it
   preserves the starting point the lane-preserving refloat needs).  The alternative — drop the value too and let the
   epoch re-determine it — is closer to today and one line simpler, at the cost of that successor card.
3. **Whether the declared handover transform (step 5) belongs to RF-1 or RF-2.**  It is the same operator RF-2 needs,
   it fixes only 3 of R10's 43 steps, and it is solve-affecting.  Deferring it to RF-2 makes RF-1 purely about the
   carried prior; keeping it here makes RF-1's handover story complete.

---

## 10. The three sentences

**Today** the network ambiguity is a per-epoch free parameter made time-linked by a zero-process-noise scalar prior
that carries last epoch's marginal variance, is born with no prior at all from one epoch of data at a variance the
model reports rather than the value deserves, and is returned to that state by four un-instrumented reset paths — a
managed level, not an estimated state.

**Under RF-1** it becomes a float state of the station's local block whose carried joint covariance is folded as a
proper prior, whose variance falls as `1/n` over the arc because the fold stops asserting the station-common direction
once per line of sight, whose birth prior is recorded and floored, and whose slip response is a bounded **drop back to
that birth prior** — with an anchor handover expressed as a declared, exact S-transformation instead of a network-wide
erase.

**The largest risk** is that the birth variance cannot be made honest from the model as folded — the tie-less
sub-25° iono rows and the band-scoped free are both places where the estimator's reported precision is better than its
level deserves — so RF-1 could deliver a correctly-converging state carrying a wrong level, passing every internal
bar while the wire does not move; step 2's measure-only `σ̂₀(pl)`-by-arc-age read is pre-registered precisely so that
outcome can refute the card before any mechanism is built.
