# W6 — Network Satellite Code-Bias (OSB) Estimation: Scoping Document

**Date:** 2026-06-28
**Target:** `ProcessingMode::PPP_RTK_Network` (`src/core/app/ApplicationNetwork.cpp`,
`Application::runNetwork`)
**Goal:** estimate per-satellite, per-signal **code biases** inside the PPP-RTK network producer so
the engine emits its **own** satellite code-bias (OSB) product, instead of relying on an external
`.BIA` bias-SINEX product on the user side.
**Method:** 7-way parallel code recon + architect synthesis + 3 adversarial verifiers
(estimability / reuse / convention), each independently re-verified against the live tree. All three
verifiers returned **sound-with-fixes**; the substantive fixes are folded in below.

> **Anchor caveat.** `ApplicationNetwork.cpp` is being **concurrently edited** in another session — it
> grew ~43 lines in the gate region while this scope was being written. Line numbers below are *hints*;
> anchor by **symbol/function name**, and re-grep before editing. The verified anchors here match the
> tree as of this writing.

---

## 1. Verdict & recommended scope

**Network satellite code-bias estimation is feasible on the current arrowhead/Schur architecture, but
the system is rank-deficient as the code stands today. The single load-bearing prerequisite is an
explicit ionosphere constraint in the network path.** Everything else is comparatively mechanical.

The I/O and plumbing layers are **already built** — this is *not* an I/O gap:

- `NetworkEstimator` has `SharedParamKind::CodeBias`, `registerCodeBias`, `codeBiasColumn`, and a
  `toSolutionEpoch` arm that writes `code_bias_m[signal] = x_s(col)` (`NetworkEstimator.cpp:25,56,70,234`).
- `NetworkSatSolution::code_bias_m` exists (`NetworkProductEmitter.hpp`), `NetworkProductEmitter`
  pushes `SsrCodeBias`/`pushCodeBias` for non-empty maps (`NetworkProductEmitter.cpp:64-71`), and the
  on-disk `B,sat,signal,bias` record round-trips (`NetworkProductIO.cpp` write `:68-69`, read `:136-138`).
- The sign/unit contract is already pinned by tests (`tests/test_network_emit.cpp`,
  `tests/test_network_estimator.cpp`).

What is **missing** is the math that makes the satellite **DCB** (the geometry-free part of the code
bias) observable. With each station's slant ionosphere carried as a **free per-`(receiver,satellite)`
local nuisance** and **no iono constraint anywhere in `runNetwork`**, the satellite DCB sits in the
null space, co-linear with that local iono. The **IF (signal-common) part is intentionally
non-separable** — it is already absorbed into the estimable satellite clock the network produces
(sat-clock Jacobian `−1` on **all** signals, `MeasurementModel.cpp:1311-1318`). The existing force-off
comment block in the config loader (`GnssConfigLoad.cpp:2585-2600`) states this rationale precisely and
is why V1 deferred code bias.

### Recommended V1 slice — DO

1. **Promote the satellite code bias to a first-class shared-border column** for **GPS + Galileo
   only**, by lifting the existing fallback `sat_bias` state (`MeasurementModel.cpp` `sat_bias_term`,
   unit `+1.0` Jacobian on raw code rows, `:1262-1276`) into the network shared block.
2. **Add a per-station GIM-weighted iono pseudo-observation** — the load-bearing prerequisite — by
   **porting** the common-mode-free `R` construction from `Application::applyGimIonoUpdate` into a free
   helper that folds rows into each station's `A_r/w/y` block (see §3.5 for why it is *not* a verbatim
   reuse).
3. **Add a per-constellation satellite-DCB datum pin** to the S-basis (`NetworkSbasis::buildSharedDatum`),
   mirroring the clock pivot, and a per-satellite IF pin (see §4).
4. **Enable per-station receiver DCB/IFB as local nuisances in lockstep** (`estimate_ifb=true` for the
   network). They must land *with* the satellite column — either alone trips the rank gate.

### Recommended V1 slice — DON'T

- **No network thin-shell / spherical-harmonic VTEC estimator.** It introduces a *second* shared block
  and a much larger Schur border, violating the "stations couple only through the satellite border"
  invariant (`NetworkNormalEquations.hpp`). Defer to a future "network ionosphere product."
- **No GLONASS satellite code bias.** FDMA makes a single shared per-`(sat,signal)` OSB unphysical; the
  loader already restricts GLONASS to clock-only on the network side, and GLONASS receiver code delay
  is carried by an **orthogonal per-FCN-slot IFB** mechanism (`StateInitializer.cpp:118-132`, central
  slot `k=0` held as datum). Keep GLONASS code bias local. **Enforcement point:** gate the new
  shared-column registration in `ensureNetworkSharedStates` to skip GLONASS, or GLONASS code rows
  (which still carry the shared clock column) will acquire a spurious shared DCB column.
- **No iono-fixed mode for V1.** Holding slant iono at GIM with tight variance maximizes DCB
  observability but dumps 100% of GIM error into the DCB. Keep iono **weighted**.
- **No network AR, no change to held-fixed coordinates/orbits.**

---

## 2. Why estimate code biases

Today the user side consumes an external `.BIA` for satellite code biases. Two consequences:

1. **Frame conflict on the undifferenced user path.** The committed network direction is *undifferenced*
   USER processing (no BSSD), chosen specifically to dissolve the `.BIA` ↔ network-bias frame conflict.
   While code bias comes from an external `.BIA` and clock + phase bias come from the network, the user
   must reconcile two independently-referenced bias frames. A self-produced OSB referenced to the
   network's **own** estimable clock removes that reconciliation.
2. **Float quality / convergence.** A mis-referenced or stale code bias biases the code-determined part
   of the float (the ionosphere and receiver-clock states the code anchors), slowing convergence.

**What the OSB product actually delivers.** The IF part of the satellite code bias is *already* inside
the network clock, so the deliverable is **essentially the satellite DCBs (the geometry-free / between-
band part), referenced consistently with the network's IF clock.** Not an "absolute" code bias (which
is not an estimable quantity), but a DCB set in the *same datum* as the clock and phase bias the network
already emits — so one consistent product set drives the user float.

---

## 3. Estimability analysis (load-bearing)

### 3.1 The undifferenced uncombined code equation

For receiver `r`, satellite `s`, band `j` (geometry/clock/tropo lumped into `ρ`), in the L1-referenced
convention the code already uses (`MeasurementModel.cpp:1239-1241`, `α_j = f_L1²/f_j²`):

```
P_{r,s,j} = ρ_{r,s} + c·dt_r − c·dt^s + α_j·I_{r,s} + d_{r,j} + d^s_j + ε
```

- `dt^s` — satellite clock; **network shared state**, Jacobian `−1` on **all** signals
  (`MeasurementModel.cpp:1311-1318`).
- `I_{r,s}` — slant ionosphere; **free local nuisance per `(r,s)`**, lands in `A_r` via `splitStation`
  (`ApplicationNetwork.cpp` `splitStation`).
- `d_{r,j}` — receiver code bias (band `j`).
- `d^s_j` — **satellite code bias — the W6 target.**

### 3.2 The two confoundings

Split any code bias into its IF (signal-common) and GF (between-band) parts. With two bands the GF
observable is `P^GF = P_1 − P_2`:

```
P^GF_{r,s} = −(α_2 − α_1)·I_{r,s} + (d_{r,1} − d_{r,2}) − (d^s_1 − d^s_2)
```

- **IF ↔ clock (intentional, do not fight it).** The IF part of `d^s` enters with the *same*
  per-signal-constant signature as `c·dt^s`. The estimable clock absorbs it — deficiencies (a)/(b) of
  `docs/ssystem_conformance_2026-06-15.md`. Confirmed by the `−1`-on-all-signals sat-clock Jacobian.
- **GF ↔ iono (the problem).** The satellite DCB `(d^s_1 − d^s_2)` appears *only* in `P^GF`, alongside
  the free `I_{r,s}`. With `R×S` free iono unknowns and `R×S` GF equations, the iono soaks up the entire
  GF signal; the satellite DCB (one scalar per sat) and receiver DCB (one scalar per station) both lie
  in the null space, each confounded with the per-`(r,s)` iono level.

### 3.3 Where the deficiency surfaces (corrected from verification)

The iono is a **local** state, eliminated by the **Schur reduction** *before* the rank gate runs. So:

- The per-station local block `N_rr` is **not** made singular by adding a *shared* DCB column (the DCB
  lives in `A_s`, not `A_r`); the local iono stays estimable and **stations are not dropped at
  `addStation`**.
- The confounding annihilates the shared DCB direction inside the **reduced shared normals**
  `reduced_N_ss` (`NetworkNormalEquations::reduce`, which subtracts `N_sr Nrr⁻¹ N_rs`). The near-zero
  singular value appears there and is caught by `NetworkSbasis::assertNullSpaceRank`.
- Result: **the whole epoch is withheld by the shared null-space gate** (`kNullSpaceTol = 1e-9`,
  `ApplicationNetwork.cpp:1004`; gate call `:1036`) — the documented global zero-emit failure mode
  (`project_network_global_zeroemit_perf`). Do **not** chase a fix in `N_rr`.

The network applies **no** iono constraint today — a grep of `ApplicationNetwork.cpp` for
`applyGimIonoUpdate|applyPseudoObservation|gim_|IonoUpdate` returns nothing — so every `I_{r,s}` is
unconstrained, exactly as the algebra requires for the deficiency to bite.

### 3.4 The minimum constraint required

To separate the satellite-common DCB pattern from the per-`(r,s)` iono, you need a constraint that
**imposes between-satellite structure on the slant iono while leaving the per-constellation common mode
free** (so the receiver-DCB datum and the constellation-wide iono level stay unpinned by the iono
constraint and are instead pinned by the explicit S-basis). That is *exactly* the structure of
`Application::applyGimIonoUpdate`:

> `R = blkdiag over constellations of diag(var) + σ_c²·11ᵀ` — the all-ones block "leaves the
> per-constellation common mode unconstrained, so only the between-satellite structure of the GIM is
> imposed." (`Application.cpp:749-760`, verified verbatim.)

**Necessary but not sufficient (verification fix).** The GIM constraint breaks the GF↔iono confounding
for the **between-satellite** DCB pattern, but by construction it **leaves the per-constellation
common-mode DCB offset in the null space** (the `11ᵀ` block deliberately frees that mode). Therefore the
gate passes **only when the §4 per-constellation DCB pin lands in the same release** as the iono
constraint. GIM alone does not make the full DCB set estimable.

**Minimum-coverage precondition (verification fix).** Between-satellite structure requires **≥2
GIM-valid satellites per constellation per station** in the same common-mode block. A constellation with
only one GIM-valid satellite at a station gets a 1×1 all-ones block (adds nothing beyond its diagonal),
imposes no between-satellite structure, and that satellite's DCB stays confounded. Make minimum-2 an
explicit precondition.

### 3.5 Recommendation: GIM-weighted (ported), not thin-shell, not fixed

- **GIM-weighted (recommended).** Lowest effort, maximal structural reuse, and the common-mode-free `R`
  is precisely the structure needed. **Accuracy ceiling:** the estimated DCB is only as good as the
  GIM's *between-satellite spatial structure* (~few TECU → decimetres of slant delay), not cm-level —
  acceptable for a V1 OSB whose precise IF part is in the clock.
- **Thin-shell / SH VTEC (reject for V1).** More accurate, but a new cross-station shared subsystem.
  Architecturally heavy; violates the coupling invariant. Defer.
- **Iono-fixed (reject for V1).** Transfers all GIM error into the DCB. Only justified if the GIM is
  independently cm-accurate over the footprint, which it is not.

**Reuse correction — `applyGimIonoUpdate` is NOT verbatim-reusable.** It is an `Application` member that
operates on the singular members `state_manager_/kalman_filter_/gim_/...` and applies the constraint via
`kalman_filter_->applyPseudoObservation` (`Application.cpp:762`). The network path has **no per-station
KalmanFilter** — it folds pre-formed `A_s/A_r/W/y` rows into the arrowhead. **Port** the `R`-construction
+ per-sat row logic into a free helper that emits `H/z/R` as extra **tight-prior rows appended to the
station's `A_r/w/y`** (the same mechanism `splitStation` already uses for the pivot ambiguity pins). The
common-mode-free `R` (`Application.cpp:749-760`) is the reusable kernel; the KF plumbing is not.

**Two preconditions to verify before coding the iono slice:**

- `runNetwork` constructs **no** `gim_` today, and `applyGimIonoUpdate` silently no-ops on null `gim_`
  (`Application.cpp:689`). GIM load + `data_download.download_gim` must be wired into network startup.
- **Unverified and gating:** whether a GIM is even present in the `IGS_Pasadena 2024-129` fixture set.
  Confirm first — without it the slice is inert.

---

## 4. S-basis datum design

| Component | Status | Action |
|---|---|---|
| **Clock pin, per constellation** | exists (`NetworkSbasis::buildSharedDatum`, `clock_datum_satellite`) | unchanged |
| **Receiver clock datum** | exists implicitly (rx-clock common-mode gauge in `A_r`) | keep; rx-clock absorbs the IF rx-code-bias lump |
| **Satellite per-sat IF pin** | **does not exist** | **new: pin `μ₁·d^s_1 + μ₂·d^s_2 ≡ 0` per satellite** |
| **Satellite per-constellation DCB pin** | **does not exist** | **new: sum-to-zero per constellation (or pin a reference sat)** |
| **`declared_dimension` accounting** | computed in `NetworkSbasis::Plan` / `buildSharedDatum` | **grow by the new pin count** |

### 4.1 Satellite code-bias reference convention

Because the IF part is already in the clock, the pinned quantity must be the **IF combination of each
satellite's code bias, fixed to zero** — i.e. "the emitted satellite code bias is, by convention,
pure-GF/DCB; its IF lump lives in the clock." Pin per satellite:

```
μ₁·d^s_1 + μ₂·d^s_2 ≡ 0     (μ₁ = 1.0, μ₂ = f₁²/f₂²)
```

using the **same GF-IF algebra the receiver side uses** (`StateInitializer.cpp:160-173`). **Caveat
(verification fix):** that algebra exists **only on the receiver side** — there is no satellite
analogue, so this is **genuinely new code**, not a clean mirror. It is what keeps the emitted OSB
IF-clock-consistent and prevents the clock and OSB from **double-counting** the IF lump (Risk 4).

On top of the per-satellite IF pin, the **constellation-wide DCB offset** is left free by the
common-mode-free GIM `R` (§3.4), so it needs its own pin: **sum the per-constellation network satellite
DCBs to zero**, or pin one reference satellite's DCB to an external value (broadcast TGD / IGS DCB). This
choice sets the product's absolute reference and consumer compatibility (open question Q1).

### 4.2 Receiver code-bias datum — and reconciling the "disable receiver code bias" guidance

The standing guidance to *disable/absorb* receiver code bias is **BSSD-specific** — `BssdTransform`
zeroes the receiver-block code-bias columns. **The network producer is undifferenced
(`DifferencingMode::None`), so that zeroing does not apply.** On undifferenced rows with a *shared*
satellite OSB, the satellite OSB **absorbs the receiver code bias** unless the receiver DCB is an
explicit, separately-pinned state. Therefore:

- **Enable `estimate_ifb=true` for the network.** Receiver DCB/IFB then lands automatically in `A_r` via
  `splitStation` (no new routing), in the same IF/GF datum (`resolveReceiverBias`,
  `StateInitializer.cpp:82-174`).
- **Keep the convention:** rx-clock absorbs the IF rx-code-bias lump; the per-`(r,s)` slant iono absorbs
  the rx GF/DCB lump (the existing L1-referenced iono+DCB convention).
- **GLONASS:** if admitted, its receiver bias is the **per-FCN-slot IFB** (`StateInitializer.cpp:118-132`,
  `k=0` datum) — a *different* state family from the GPS/Galileo system DCB/IFB. Do not conflate.

### 4.3 declared_dimension & the null-space gate

The new pins add gauge directions: one per-constellation DCB offset (unless externally pinned) and the
per-satellite IF direction. **`declared_dimension` lives in `NetworkSbasis::Plan` and is set inside
`buildSharedDatum` as `shared_datum.size()`** — growing it means **pushing the new `Pin`s into
`plan.shared_datum`** there (it auto-updates), *not* editing the trace header (`ApplicationNetwork.cpp`
`declared_dim` CSV column at `:490` is only a label). `SbasisDeclaration` must also **record the
code-bias datum convention per constellation** (today `emit_code_bias` is a bare bool with no convention
field) so a consumer can cross-check code-bias compatibility — mirroring the existing clock/ambiguity
declaration.

**State-allocation discipline.** The gate counts genuine gauge directions only if there are no phantom
all-zero shared columns. `ensureNetworkSharedStates`' admission gates and `splitStation`'s dead-column
drop already enforce this for clock/phase bias; the new code-bias column **must** get the same admission
gate + a `pruneSatBiasStatesExcept` call (§5, Slice 2), or stale columns produce spurious null-space
dimensions.

---

## 5. Work breakdown (ordered, independently-landable)

> **Coupling note.** Slices 2 (estimator wiring) + 3 (datum) + 4 (iono) + 5 (rx nuisances) **cannot be
> validated in isolation** — enabling the satellite column without the iono constraint *and* the datum
> pins *and* rx-DCB trips the rank gate. They can be *coded* independently but land behind a single
> config gate flipped only when all four are in.

### Slice 1 — Config schema & process noise *(isolated; lands first)*
- **Files:** `include/config/GnssConfigTypes.hpp` — add `ProcessNoise::sat_code_bias` (today only
  `sat_bias`, `sat_phase_bias`, `sat_clock` exist, `:978-995`); add a `NetworkConfig` code-bias datum +
  enable field. `src/core/config/GnssConfigLoad.cpp` (`loadNetworkSection_`) — parse them, mirroring the
  existing `sat_bias_q` pattern. `src/core/config/gnss_config.json` — add the key.
- **Reuse:** the `sat_*_q` parse idiom.
- **Accept:** keys parse and round-trip; a deliberately-chosen default `q` (hardware delays are very
  stable → start near `sat_bias` `1e-10`, **not** the rx `code_bias` `1e-4`; justify, see Q4).

### Slice 2 — Promote sat code bias to a first-class shared column *(estimator wiring)*
- **Files:**
  - `MeasurementModel.cpp` `ensureNetworkSharedStates` — today registers **only** clock + phase bias
    (`addSatClockState`, `addSatPhaseBiasState`) and prunes them. **Add** `addSatBiasState` allocation
    **and** a `pruneSatBiasStatesExcept(required_keys)` call (state primitives exist —
    `StateManager.hpp:679,666,782`), with the **GPS/Galileo-only, raw-signal** admission gate.
  - `ApplicationNetwork.cpp` shared-column registration — call `est.registerCodeBias(sat,signal)`.
  - `ApplicationNetwork.cpp` `splitStation` — **NEW scatter arm**: today it scatters
    `getSatClockStates()` and `getSatPhaseBiasStates()` into `A_s` only; add an arm iterating
    `st.sm->getSatBiasStates()` → `est.codeBiasColumn(sat,signal)`. (This is *not* the "unchanged
    `splitStation`" of Slice 5 — that refers to the *receiver* DCB staying in `A_r`.)
- **Reuse:** `SharedParamKind::CodeBias`, `registerCodeBias`, `codeBiasColumn`, the `toSolutionEpoch`
  CodeBias arm — all exist (`NetworkEstimator.cpp:25,56,70,234`). `sat_bias_term` unit Jacobian on raw
  code rows (`MeasurementModel.cpp:1262-1276`).
- **Accept:** `codeBiasColumn(sat,signal)` returns a valid border index for GPS+Galileo raw-code rows;
  `toSolutionEpoch` populates `code_bias_m`; no GLONASS columns appear.

### Slice 3 — Satellite code-bias datum & declared_dim
- **Files:** `NetworkSbasis.hpp` (code-bias convention record; flip `emit_code_bias`); `NetworkSbasis::
  buildSharedDatum` (add the per-constellation DCB pin + per-satellite IF pin → `declared_dimension`
  grows automatically).
- **New:** the **satellite-side GF-IF split** (the IF pin math — no existing satellite scaffolding,
  cf. §4.1).
- **Accept:** with code-bias columns registered **and** the iono constraint of Slice 4 active, the
  null-space gate passes and `declared_dim` matches the pin count; emitted DCBs are reproducible across
  two runs. *(Without Slice 4 this slice cannot pass the gate — they validate together.)*

### Slice 4 — Per-station iono constraint *(the prerequisite)*
- **Files:** `runNetwork` — construct `gim_` at network startup (wire `download_gim` /
  `ionosphere_weighting`); in PASS B, **before** the Schur reduction, append GIM tight-prior rows to
  each station's `A_r/w/y` via the **ported** helper (§3.5); handle `q.valid==false` (exclude that
  satellite's code-bias contribution, **keep the station** — do not let the silent dead-column drop
  erase it).
- **Reuse:** the common-mode-free `R` kernel from `applyGimIonoUpdate` (`Application.cpp:749-760`),
  `IonosphereWeightingConfig`, `gim_->slantIonoDelay` (L1-referenced — matches the network convention,
  `MeasurementModel.cpp:1239-1241`). **NOT** the function body (no per-station KF).
- **Accept:** with the GIM loaded, per-station slant-iono states show GIM-structured between-satellite
  correlation and the satellite-DCB between-satellite pattern becomes estimable (gate passes once Slice
  3's common-mode pin is also present). **Pre-req:** confirm a GIM exists in the fixture set.

### Slice 5 — Receiver code-bias nuisances
- **Files:** set `estimate_ifb=true` in the network configs; in `GnssConfigLoad.cpp:2585-2600`, replace
  the unconditional force-off of `estimate_sat_biases_` / `promote_sat_code_bias_` with a gated
  activation (behind the network-scoped flag, Q2); consider per-station `initializeCodeBiases`
  (`StateInitializer.cpp`) seeding if the float won't converge unseeded.
- **Reuse:** `resolveReceiverBias` (`StateInitializer.cpp:82-174`), the `ensureRequiredStates`
  code-bias block, `code_bias_term`, and `splitStation` routing of the *receiver* DCB into `A_r` — all
  work on `DifferencingMode::None` rows unchanged.
- **Accept:** stations are **not** dropped by the rank gate with `estimate_ifb` on (proving the rx-DCB ↔
  sat-DCB split is identifiable); per-station rx DCB recovers a sane value.

### Slice 6 — Carry-forward, emit & trace *(mostly done; small additions)*
- **Files:** `ApplicationNetwork.cpp` — add a `CodeBias` arm to the D2 q-selection ternary (`:1070`:
  `q = (kind==Clock) ? q_clock : q_pb` — a third kind silently inherits `q_pb`, **a real bug**, Risk 5),
  defining `q_cb = getProcessNoise().sat_code_bias` near `:474`; add a `stash(CodeBias,...)` loop to the
  D2 stash; add a `CB` row to `network_solution.csv`.
- **Reuse — already complete, no change:** `NetworkProductEmitter` (`SsrCodeBias`/`pushCodeBias`,
  `:64-71`), `NetworkProductIO` (`B` record, `:68-69`/`:136-138`), the kind-generic `CarryPrior`.
- **Accept:** code bias carries epoch-to-epoch with the **correct** `q` (not `q_pb`); `B` lines appear
  in the product file and round-trip; `CB` rows appear in the solution trace.

### Slice 7 — Closed-loop validation *(see §6)*

---

## 6. Validation plan

1. **Closed-loop held-out consumer (primary).** Run the producer with W6 active → emit the product (now
   incl. `B` code-bias records) → feed it to our **own undifferenced user engine** on the held-out
   station and confirm the user float converges with the network OSB **and no external `.BIA`**. Success
   = held-out position/convergence matches or beats the `.BIA`-fed baseline. This directly tests
   IF-clock-consistency: a double-counted IF lump shows up as a position/clock bias on the consumer.
   - **BLOCKER (verification fix, was Q6).** `.BIA` and network OSB are applied through **two different,
     non-mutually-exclusive** mechanisms: `.BIA` via the `hasBiasEntry` state/modeled path; network OSB
     via the **ungated** `ssr_bias_term` on the consumer (`MeasurementModel.cpp:1335-1346`, gated off
     only for the *producer*). A consumer that loads **both** will **double-correct**. The W6 validation
     consumer config **must disable `.BIA`** (`require_bia=false` / `no_bia_dw`) so the network OSB is
     the sole code-bias source — **or** the producer must signal a "use-network-code-bias" flag that
     suppresses `.BIA`. Without this the closed loop silently double-corrects and the test is invalid.
2. **External-DCB cross-check.** Compare emitted per-satellite DCBs against IGS DCB / `.BIA` OSB. The
   *between-satellite* pattern should agree to the GIM-structure-limited level (decimetres of slant
   delay, §3.5); a constellation-wide offset is expected (the datum choice §4.1) and should match the
   pin, not zero. **Provide the explicit convention transform** (which signals, sign, how the
   constellation-wide offset maps from the network's L1-referenced/clock-absorbed-IF/sum-to-zero
   convention to the external product's) — otherwise the cross-check cannot tell a real bug from a
   convention mismatch.
3. **Real-data test-suite harness** (`project_realdata_test_suite`). Add a W6 manifest cell: A/B the
   held-out consumer, network-OSB vs `.BIA`-OSB, graded against the frozen site coordinate. Watch for
   the known-bad CIT100USA baseline-fixture ECEF (`task_2e62785b`) — don't let a bad truth coordinate
   mask a regression. Reference the existing `code_bias_m` round-trip tests as a cheap sign/unit anchor.

---

## 7. Risks & open questions (ranked)

### Risks (highest first)
1. **No GIM in the network path → the whole slice is inert.** `applyGimIonoUpdate` no-ops on null
   `gim_`, and `runNetwork` constructs none. Unverified whether the `IGS_Pasadena 2024-129` fixture even
   contains a GIM. **Gates everything — verify before coding Slice 4.**
2. **Coupled landing / rank gate.** The satellite column, iono constraint, per-constellation DCB pin,
   per-sat IF pin, and rx-DCB must all be present together; any subset trips `kNullSpaceTol`
   (`:1004`/`:1036`) — the whole epoch is withheld (§3.3). Land behind one config gate.
3. **declared_dimension miscount → global zero-emit** (`project_network_global_zeroemit_perf`). The
   estimability diag was noted as *not firing* there — do not trust it; verify the gate explicitly.
4. **IF double-count.** Emitting a full per-signal OSB **without** the satellite-side IF pin (§4.1) makes
   the clock *and* OSB both carry the IF lump → consumer double-corrects. The IF-pin math is genuinely
   new code (no satellite GF-IF scaffolding exists). Subtlest correctness risk.
5. **Wrong carry-forward q.** Omitting the Slice-6 q-arm silently inherits `q_pb` (`:1070`) — far too
   tight, producing a deaf, slowly-biased estimate that *looks plausible in the trace*.
6. **GIM error → DCB error.** Option-1 DCBs are only as good as the GIM's between-satellite structure;
   couples OSB quality to an external iono product the user may also use (correlated error). Acceptable
   for V1; document the ceiling.
7. **No-coverage / single-band station loss.** `q.valid==false` (no GIM) **or** single-frequency
   tracking for a `(r,s)` thins the GF rank; the silent dead-column drop then removes that contribution.
   Handle explicitly (exclude the code-bias rows, keep the station). Require **≥2 dual-band, GIM-valid
   satellites per constellation per station** (§3.4).

### Open questions (need a decision)
1. **Constellation-wide DCB datum:** sum-to-zero per constellation, or pin one satellite to broadcast
   TGD / IGS DCB? Sets the absolute reference and external interop. *Recommend sum-to-zero for
   self-consistency; revisit if external interop is required.*
2. **Activation toggle:** reuse the existing `promote_sat_code_bias_` / `estimate_sat_biases_` pair
   (force-disabled at `GnssConfigLoad.cpp:2597-2598`, but they **also** drive the single-receiver user
   path), or add a **network-scoped** `network.estimate_sat_code_bias`? *Recommend a network-scoped flag
   — no user-path side effects.*
3. **OSB granularity:** full per-signal OSB, or a per-satellite DCB(L1-L2) + per-signal offsets
   (mirroring the receiver `*_DCB` + per-band IFB)? The latter minimizes the shared-border dimension.
   *Lean toward the receiver-mirroring decomposition.*
4. **Default `sat_code_bias` q:** near `sat_bias` (`1e-10`) vs tighter — hardware delays are very stable;
   needs its own justification, not the rx `code_bias` (`1e-4`) value.
5. **`estimate_phase_clock` in the network:** the zero-emit error message names "phase-clock"
   (`ApplicationNetwork.cpp:1297`), but `loadNetworkSection_` never force-disables `estimate_phase_clock_`
   (parsed at `GnssConfigLoad.cpp:622`, default false; the force-off block touches only sat-side flags).
   Confirm the producer never needs it and **hard-force it off** for V1.
6. **Code-bias datum discontinuity/handover** *(verification fix — newly surfaced).* The phase-bias path
   has explicit discontinuity bookkeeping (`NetworkProductEmitter.cpp:73-90`, IOD/counter bumps consumed
   by `noteSsrDiscontinuity`); **code bias is pushed with none** (`:64-71`). The per-constellation DCB
   datum (§4.1) **re-references** as the sum-to-zero satellite set changes (sats rise/set) — a step in
   *every* emitted OSB simultaneously. Either keep a **stable reference set** over the broadcast interval
   or **signal the re-reference** to the consumer, mirroring the phase-bias path. Otherwise every code
   residual jumps on the consumer.
7. **Multi-GNSS ISB interaction** *(verification fix).* The per-constellation satellite clock absorbs the
   IF code-bias lump per constellation, and there is a per-system receiver ISB state. When the network
   adds an explicit per-constellation DCB pin, specify whether the receiver ISB datum must change —
   otherwise the constellation-wide DCB offset and the ISB can be co-confounded across systems.

---

## 8. Sources & provenance

- `docs/ssystem_conformance_2026-06-15.md` (§7 network plan; deficiencies (a)/(b)/(d); "promote the
  fallback satellite code-bias state to first-class").
- Live tree: `NetworkEstimator(.hpp/.cpp)`, `NetworkNormalEquations.hpp`, `NetworkSbasis(.hpp/.cpp)`,
  `ApplicationNetwork.cpp`, `MeasurementModel.cpp`, `StateInitializer.cpp`, `GnssConfigLoad.cpp`,
  `NetworkProductEmitter(.hpp/.cpp)`, `NetworkProductIO.cpp`, `Application.cpp` (GIM path).
- Memory: `project_ppp_rtk_network` (undifferenced-user direction), `project_satbias_alloc_ssr_blind`
  (per-signal gate keys), `project_network_global_zeroemit_perf` (zero-emit failure mode),
  `project_realdata_test_suite` (validation harness).

*All line numbers are hints against a concurrently-edited tree — anchor by symbol name and re-grep
before editing.*
