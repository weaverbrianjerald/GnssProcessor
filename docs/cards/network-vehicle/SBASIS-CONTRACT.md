# S-basis contract: the realized basis, wire meaning and realization transfer

**STATUS**

- **id:** S-basis contract
- **topic:** network-vehicle
- **status:** reference
- **summary:** Reference for the producer's August 2026 S-basis in Odijk-2016 notation: the realized constraints, what each wire record means, the hold-or-mark transfer contract and the known residues.
- **dates in the text:** 2026-06-15 → 2026-08-15
- **knobs / posture:** `network.code_bias_gf_datum` (the slant GF pin, since retired), `network.sticky_pivot`, `network.pivot_vanish_return_signal`
- **hashes named in the text** (commit / exe / byte-cert): `8e5b04b` (anchor re-verification)
- **run dirs named in the text:** `runs/pc305_ggq_cbpin_verify_BEFORE_ambdatum`
- **source:** `docs/archive/SPEC_sbasis_realization_contract.md` (preamble), §A, §B, §C, §D

**Outcome.** The card states which S-basis the producer realized in August 2026 (pivot-satellite clock, per-satellite IF pin, sum-to-zero code-bias gauge, eq34 phase datum, slant GF pin), what each wire record means, and the rule that every re-occurring gauge is held or marked. The 2026-08-13 audit fixed two wire under-declarations (`cbpin=` gated on a QC knob; `amb_datum` 0 instead of 2 on eq34 products). Left open as written: the GF datum's run-end telemetry, fingerprint and re-aim mark, and a `NetworkSbasis.hpp` header edit (two clock-gauge ways named, three exist). The re-founding posture sets `phase_datum_eq34: false` ([program](../../refound/NETWORK_REFOUNDING_2026-09-14.md)) and pins GF at broadcast T_GD ([RF-0c](../../refound/RF-0c_gf_pin_2026-09-15.md)); `code_bias_gf_datum` is retired ([GAL](../../refound/GAL_gps_galileo_dual_2026-09-22.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_sbasis_realization_contract.md -->
# The producer's S-basis realization contract (Odijk-2016 notation)

Written 2026-08-09; **materially revised 2026-08-13** (audit against the working tree, plus the
code-bias GF datum, the eq34 seed-round work, and the `cbpin=` / `gfpin=` declaration fix).
Companion to `docs/SPEC_clock_emit_rebase.md` §7.

**2026-08-15 — this file also carries two absorbed documents**, appended as new lettered sections
so §A–§D are untouched: **§E — the S-system THEORY CANON**, from
`docs/ssystem_theory_reference_2026-07-09.md` (now `docs/attic/`); **§F — the 2026-06-15
USER-SIDE conformance audit**, from `docs/ssystem_conformance_2026-06-15.md` (now `docs/attic/`).
⚠ §F is a **user-side** (BSSD consumer, MLAMBDA, real-time SSR) audit with a different scope from
§A–§E, which are producer/network-side. Its **§F7 is the network's own design charter** — the
section `src/core/app/ApplicationNetwork.cpp:15` and `ApplicationNetworkSetup.cpp:27` cite as
"§7". All §F code anchors are **2026-06-15 snapshots that drift**.

**⚠ OPEN items live in this file.** §F9 carries **two OPEN P0** and **three OPEN P1** user-side
backlog entries, plus §F8.7's now-actionable `enforcePsd` defect; §A/§C/§D list the GF datum's
three unpaid residues. Only §F9's P2 (network side) is fully BUILT.

**Purpose:** state, in the notation of Odijk et al. (2016, J Geod 90:15–44), (A) which S-basis the
network producer actually realizes (code-verified, not folklore), (B) what the emitted wire records
therefore MEAN, and (C) the **realization-transfer contract** — how each per-epoch re-occurring
rank deficiency is held to ONE realization over time, which Odijk's single-basis tables do not
cover and where every measured wire disease has lived.

Model class: the paper's Eq. (12) — undifferenced, uncombined, multi-frequency phase+code, slant
per-LOS ionosphere, CORS geometry (station coordinates fixed, ν = 1: ZTD only ⇒ the regional
type-0 deficiency is absorbed by construction). Two deliberate deviations from the paper's
notation: (1) our basis is per-(constellation, **signal**), not per-frequency j — channel families
(C/Q/X…) are first-class, which is what made the GPS+Gal SBC completion necessary (§A row 7) and
what the family-GF card (`docs/SPEC_family_gf_datum.md`) exists to finish; (2) the ionosphere has
**two postures** realizing DIFFERENT bases — the single-layer posture constrains the field
(SH-VTEC, `estimate_iono_sh`) and leaves the geometry-free code direction implicit; the **slant
posture** (`network.code_bias_gf_datum`, landed 2026-08-12) leaves the ionosphere free per-LOS and
pins the GF direction as an explicit datum row. Rows marked *(slant)* apply only to the second.

## §A The realized basis — our analogue of Odijk Table 2

| Odijk rank deficiency | our constraint (the S-basis choice) | code anchor | count | S-line token |
|---|---|---|---|---|
| 1a — rx ↔ sat clocks (per constellation; paper size 1 via pivot rx or sat mean) | HARD pin of ONE satellite clock per constellation, target 0.0; receiver clocks/ISBs stay free local states. A per-constellation member of the CC family — pivot-**satellite**, not CC-R's pivot receiver nor CC-S's mean. Pivot priority: declared config pin → **sticky incumbent ONLY under `network.sticky_pivot` (default false)** → lexically first registered satellite. So the realized default is a **per-EPOCH lexical auto-pick**, and a lexically lower satellite rising IS a datum re-reference. Two constraint-realized alternatives, both `declared_dimension = 0` with an EMPTY pivot map: `clock_datum_sum_to_zero` (validated, does NOT graduate — F13) and `clock_datum_broadcast_anchored` (brdc_rts only; **no first validation run yet**). | `NetworkSbasis.cpp:159–188` (`plan.clock_pivot`, `Pin{clockColumn, 0.0}`, `declared_dimension = shared_datum.size()`); sticky default `GnssConfigLoadNetwork.cpp:4340–4341`; alternatives `NetworkSbasis.cpp:106–126`, `:136–151` | 1 × #const | `<c>:<pivot>` per const |
| 1b/3a — sat hardware biases ↔ clocks (paper: IF sat code biases in the basis) | per-satellite **IF pin**: the two lowest distinct-frequency code signals, a₁+a₂ = 1 (the μ_IF combination enters the clock, IGS-compatible per the paper §4.1). The pair is a pure function of the registered signal SET (strict total order: band digit, then full signal name) — deterministic but LEXICAL, not the consumer's convention pair. | `selectIfPinPair` `NetworkSbasis.cpp:23–50`, called at `:588`; the per-satellite IF-pin loop opens at `:579` | m | `cbpin=` |
| **2b/3b — slant iono ↔ GF code biases *(slant posture)*** | **per-satellite GEOMETRY-FREE pin** (`network.code_bias_gf_datum`). The paper's Eq. (30) row μ_GF = [−1, 1, 0, …]ᵀ/(μ₂−μ₁), target **0.0**, folded on the SAME pair the IF pin uses. IF and GF together span the pinned pair's whole 2-D subspace ⇒ d₁ = d₂ = 0 **exactly** — the paper's §5.1 "code biases are not estimable for the first two frequencies", realized as a datum rather than asserted. What survives is the modernized DCB d^s₁ − d^s_j, j ≥ 3 — the point of the third band. Target 0 is legitimate because d^s_GF is a gauge quantity the estimable parameters absorb (their Table 8); it is metre-scale, unlike a zero-target ambiguity pin. Loader interlocks are HARD ERRORS: requires `estimate_sat_code_bias`, refuses `estimate_iono_sh` / iono weighting / `frequency_mode: dual` (`all` warns). | `NetworkSbasis.cpp:746–780` (row weights `:752–753`, `zs.push_back(0.0)` `:758`, counters `:759`/`:777`); loader `GnssConfigLoadNetwork.cpp:2124–2125`, `:2218–2286` | m (dual-freq sats) | **`gfpin=`** |
| 2b/3b — same, *single-layer posture* | NOT separately constrained — the GF direction lumps into the iono exactly as Table 8 states; the SH-VTEC field + optional a₀₀ anchor is a soft constraint on the FIELD, not a GF datum row. | slant per-LOS states + `global_vtec_sh` | — | (none) |
| 1b/3a remainder — per-(const,signal) code-bias gauge | **sum-to-zero over satellites** per (const,signal) channel — CC-S/Schaer-class zero-mean, the IGS DCB convention the paper names; TIME-LINKED under Card 2 (rows pin the CHANGE of the previous epoch's solved values ⇒ the realized lump stays the first-epoch one). ⚠ *(slant)* under the GF datum this gauge sets a level only from **j ≥ 3**: for a channel inside every present satellite's pinned pair the row is satisfied identically by d₁ = d₂ = 0 and realizes nothing. | `buildCodeBiasDatumRows` `NetworkSbasis.cpp:508–927`; channel collection `:569–577`; s2z rows `:862–901`; `codeS2zTimeLink` `:145–167`; Card 2 = `code_datum_time_linked` | per channel | `emit_cb`, `cb_datum` |
| 2a/2b — rx clocks/biases ↔ rx hardware biases | receiver clocks **white** (q = 1000 — the paper's own "usually unconnected" default); rx code biases: **zero-mean-over-receivers** datum (RXCB-DATUM), realized as a per-DCB-key diagonal zero-mean prior at 1/σ² whose common mode over receivers IS the gauge | `rx_code_bias_datum` (+`_sigma_m`); `ApplicationNetworkAssembly.cpp:2697–2704` | per (const,sig) | **(none)** — it reaches the wire nowhere; only the warm-resume fingerprint records it as `rxcbdatum:<σ>` |
| 4/5 — phase biases ↔ ambiguities | **eq34 spanning-tree** datum (de Jonge 1998, cited by the paper for exactly this): HARD pins w = 1e10 on the tree's ambiguities per (sat,signal) component; `AmbDatum::SingleReceiverChain` (**wire value 2**; `PivotReceiverZeroAmbiguity` = 0 is the LEGACY single-pivot datum, `SumToZeroPerReceiver` = 1 is Fix A's ensemble datum). ⚠ **The wire under-declared this until 2026-08-13 — see §D.** Targets are **0.0 on a FIRST realization only**: `phase_datum_eq34_pin_round` defaults **true**, so an already-lit edge re-pins to λ·round(carried float/λ) — a ≈0-cycle frame step, not a re-realization at zero. Under `carry_invariant` pin-round is forced off (continuity moves to the invariant DD carry); under SEED-ROUND (`phase_datum_eq34_seed_round`) a cold pin targets λ·round(x_state/λ), \|y\| ≤ λ/2 by construction. Exception: under reattach-hygiene INC-3 a pin the election could not avoid placing on a FRESH host folds at w/scale² and decays to the hard pin. | `ApplicationNetworkAssembly.cpp:2229–2421` (`foldAmb` `:2256–2298`, `foldRpb` `:2301–2310`, RPB prior `:2360–2402`, row append `:2403–2420`; `R.np = n_hard` at `:2419` deliberately excludes the soft RPB rows); pin-round `NetworkChainDatum.cpp:409–425`, seed-round `:450–492`; σ-ramp `:579–586` | f(n−1)+fm class | `amb_datum`, `pdr=` |
| GPS+Gal channel-family extras (not in the paper — a consequence of per-SIGNAL bases) | per-epoch **SBC completion**: one minimum-constraint row per realized extra direction (PB↔iono channel-family trades), RHS = carried level; + KP loose CB prior | `NetworkSbasis::completeSbasisDimension` (`sbasis_complete`), `buildCodeBiasKaulaPriors` | 0–3/epoch | — |

Rank bookkeeping: `declared_dimension` + the null-space gate is the runtime assert of the paper's
Eq. (24) count — `assertNullSpaceRank` eigensolves the reduced shared normals, counts eigenvalues
below tol·max(λ_max,1), and THROWS on any mismatch (reporting under- vs over-specified); the driver
catches, tries the prioritized drop-and-heal re-gate once, and WITHHOLDS the epoch if it still
fails. A stronger minimum-constraint verification than published practice.
`NetworkSbasis.cpp:354–412`; driver `NetworkSolveGlue.cpp:712–766`.

> **⚠ Anchor status.** §A anchors were re-verified 2026-08-13 at `8e5b04b` (three of the five
> originally cited ranges had drifted and were corrected then). Spot-re-grepped 2026-08-15:
> `selectIfPinPair` `NetworkSbasis.cpp:23–50` and its call at `:588`, and
> `buildCodeBiasDatumRows` at `:508`, still hold; the row-1a range has drifted again
> (`buildSharedDatum` now opens at `NetworkSbasis.cpp:169`, `plan.clock_pivot[c]` at `:276`,
> `declared_dimension = shared_datum.size()` at `:281`, the two `declared_dimension = 0`
> alternatives at `:217` and `:242`), and the gate body is now `NetworkSbasis.cpp:447`–`:500`
> driven from `NetworkSolveGlue.cpp:778` with the post-heal re-gate at `:817`. The cited ranges
> are the 08-13 snapshot; the FACTS are unchanged.

⚠ **The gate cannot see over-pinning of estimable content.** It counts null directions; a row that
pins something real removes a direction that was never null and the count still matches. Only σ̂₀
and the residual counters can catch that — which is why the family-GF card refuses per-family IF
pins (the clock has already spent the IF direction, so a second family's IF combination is
estimable inter-family ISC, not gauge).

## §B What the wire records mean — our analogue of Odijk Table 3

Under §A the consumer should read (tilde = estimable function, not the physical parameter;
"datum(1)" = the held first-realization values under §C):

1. **C record (clock)**: dt̃ˢ(i) = [dtˢ(i) + d_IFˢ(datum)] − [dt_pivˢ-chain(datum)] − A_c — the
   IF-lumped satellite clock relative to the constellation's pivot realization, in the **held
   emission frame** (A_c = the CLK-R accumulator; common to all sats of the constellation, absorbed
   by the user's per-epoch receiver clock). It is a CORRECTION to the a-priori product named in the
   S line (`apriori_id`).
2. **P record (phase bias)**: the eq34-datum'd, emission-rebased per-(sat,signal) phase bias; the
   network datum receiver's ambiguity rides inside it, which is what makes the user's
   between-satellite DD ambiguities integer (paper §6.2) — proven end-to-end by the Q4 R-record
   experiment.
3. **B record (code bias)**: the per-(const,signal) zero-mean, IF-pin-referenced satellite code
   bias (≈ scaled DCBs, paper Eq. 28–33; j ≥ 3 signals estimable as "modernized DCBs" — our
   C2X/C5Q/C5X rows are exactly that class). ⚠ *(slant)* **under the GF datum the two pinned-pair
   channels' B records are identically the DATUM (0), not estimates** — IF+GF fix them exactly —
   and only j ≥ 3 carries content. A satellite tracked on just the pinned pair has NO estimable
   code bias left; that case is counted (`n_gf_only_pair`) precisely because "emitted nothing" and
   "estimated zero" are indistinguishable on the wire.
4. **Ionosphere**: ι̃ˢ(i) = ιˢ(i) + d̄_r,GF(datum) − d_GFˢ(datum) — Table 8 with our datum. The
   receiver term is the **fleet-mean GF receiver code bias** (zero-mean-over-receivers basis);
   measured −5 ns fleet mean = the DEFECT-2 level offset (`project_extcb_defect2_rootcause`). The
   satellite term changes status by posture: *single-layer* — d_GFˢ is an implicit lump the field
   absorbs; *slant* — d_GFˢ is an **explicit datum row pinned to 0**, so the estimable slant iono
   absorbs each satellite's true geometry-free bias exactly, per satellite, per epoch.
   Estimable-but-biased BY CONSTRUCTION; the bias is only as steady as the fleet realization —
   which is why level tracking needs an external authority (item 0b), not another datum.

   ⚠ **The pairing requirement, and the hole in it.** The producer's own startup message states
   the consequence: "the emitted ionosphere and code biases must be applied by a consumer AS A
   PAIR" (`ApplicationNetworkSetup.cpp:425–430`). **The wire cannot honour that literally, because
   it never carries an ionosphere.** There is no iono record in the product grammar; the only
   slant-iono channel is the in-process `pushSlantIono` supply (`NetworkEmitStore.cpp:641`), gated
   on `sh_model` — and the slant posture has no SH field, so on posture C that supply is inert too.
   A consumer therefore receives half the pair on every channel and must estimate the other half
   itself. That is workable (its own per-LOS iono absorbs the producer's GF convention
   family-consistently) and it is exactly why the posture must be DECLARED: `gfpin=` is the wire's
   statement that the B records are one half of a pair, so a consumer must not additionally apply
   an external absolute ionosphere over them.

## §C The realization-transfer contract (the part Odijk's tables assume away)

The paper's estimable functions are defined w.r.t. ONE basis realization; its §5.3/Fig. 3 shows
which deficiencies re-occur per epoch when temporal constraints are absent, and its Zhong-2015
discussion warns that solutions from a CHANGED basis (e.g. a different zero-mean set) cannot be
compared without the connecting S-transformation. Our q posture (rx clock white; sat clock
q = 1e-3 ≈ unconstrained at code-information level; sat CB q = 1e-10 stiff; sat PB q = 1e-12 stiff
+ invariant DD carry) puts us on the Fig.-3 path where the clock-gauge class re-realizes freely
while the bias gauges are carried. The contract, per channel — every re-occurring gauge must be
HELD to one realization on the wire, and every genuine realization change must be MARKED:

| channel | re-realization cause | estimator-side continuity | emission-side S-transform (hold) | mark on genuine change |
|---|---|---|---|---|
| clock common mode (per const) | free per-epoch gauge (Eq. 41 class) + pivot handover | none (by q posture) | **CLK-R stage 1** `emit_clock_frame_hold` — census 613 → 1 | `E.datum_changed` (+leash re-anchor marked) |
| phase-bias gauge (per component) | eq34 datum re-realization (common fractional + per-sat integer·λ) | invariant DD carry | **NetworkEmitRebase** (+ PB emit filter for noise) | λ-scaled jump test → P disc / R |
| sat code-bias gauge (per channel) | membership change of the zero-mean set (the Zhong-2015 class), band-set change of the IF pin | **Card 2** time-linked datum (pins the change; a pair change re-seeds and bumps rather than pretending continuity) | `clock_signal_basis_hold` holds the IF pair itself against the band-set cause, with latch/re-latch (`NetworkSbasis.cpp:506–543`). Emission-side hold measured unnecessary: B-channel census 0–3 common epochs/12 h, all at the marked handover | structural set-change disc + `code_bias_disc_jump_test`; a hold re-latch raises `CLK_BASIS/relatch` + a per-sat disc |
| **sat GF code-bias gauge (per satellite)** *(slant)* | IF-pin PAIR change — the GF row is built on the same `sig1/sig2`, so a channel entering/leaving the ≥2-station set re-AIMS the pinned geometry-free direction | **none** — the RHS is a hard-coded 0.0 with no Card-2 veteran branch (`NetworkSbasis.cpp:641`) | **none of its own**; `clock_signal_basis_hold` holds the PAIR and therefore the direction, indirectly | **none of its own.** A pair change raises `CLK_BASIS/relatch` and a per-sat CLOCK disc — recorded as a clock re-reference, which understates it: the code-bias datum moved too. `gfpin=` declares the POSTURE and re-declares at each contract broadcast, but a mid-run pair move is still not marked as a GF event. **OPEN.** |
| rx code-bias datum | present-receiver-set change of the zero-mean row | rx-CB carry (q = 1e-9) | S2H harvest de-mean (`rx_cb_harvest_demean`) — validated EXT through 12 h; golden-lineage A/B still pending, still default-OFF and fingerprinted | (edge 3 — carried by the level campaign) |
| per-sat clock VALUE | — not a gauge: datum-invariant estimable content — | **missing** (Q3: the paper's assumed RW constraint, unimplemented at a defensible q) | falsified (SPEC_clock_emit_rebase §2 — the walk is not mediatable at emission) | `clock_disc_jump_test` (event-day only) |
| iono level ⊕ rx-DCB mean | fleet realization breathing | **NOT the carry — EXCLUDED 2026-08-08** (S2H stage 3, 12 h: the carry's rolling common-mode memory is provably zeroed every epoch, 1440/1440 applied, and the wander persists at nearly full amplitude — control mean −9.60 sd 6.09 vs ON −12.21 sd 5.41). What both postures share is the per-epoch admitted GF data equilibrium and a FIXED a₀₀ anchor against a moving GIM global mean | not gauge at the consumer (elevation-mapped) — no hold is honest | — (item 0b authority is the fix) |

Three standing consequences:

- **A wire is interpretable only under a HELD realization plus marked events.** Post CLK-R, the
  internal product satisfies this for all three record channels: its discontinuity census and its
  mark set coincide (the 12 h survivor IS the marked handover). Any new gauge-bearing channel must
  ship with its row in this table filled in — hold, or mark, or both; never neither.
- **The GF datum shipped with neither**, and that is the entry above. It landed as a correct datum
  with no §C row, no wire token, no run-end counter and no fingerprint entry. The token half is
  fixed (§D); the mark half is open.
- **What this contract cannot fix**: the last two rows. The per-sat clock walk and the level
  authority are estimable-content problems (precision and anchoring of x̃ₛ), invariant under every
  S-transformation — Q3 and item 0b respectively own them.

## §D Verification status + known residues

- The Jul-1 audit's header misdeclaration (GPS clock gauge) is FIXED in
  `include/filter/NetworkSbasis.hpp:16–32`. ⚠ That header still says the clock gauge is realized
  "in one of **two** configurable ways" while `IsbDatum` has had **three** since 2026-07-11
  (`BroadcastAnchoredPerSatellite`); a header edit is owed.
- Vanish-return pivot signalling: covered by the A1 persistent last-pivot map under
  `network.pivot_vanish_return_signal` (merge-updated; `datum_changed` on a different-pivot return).
- **The S line, exactly.** Positional: `ref_rx, bias_sign, windup_id, amb_datum, isb_datum,
  emit_cb, cb_datum, disc_thresh_m, emit_clock_disc, apriori_id`; then the realized pivots
  `<const>:<sat>`; then, each written ONLY when armed, `gid= stale= cbjump=1 clkset=1 frame=
  cepoch= atx= oref= pdr= agek= discfrac= hsig= cbpin= gfpin=`. The `#` legend is grammar-complete
  and enforced against the writer by `HeaderLegendDeclaresEveryEmittedField`. Anything added to §A
  that a consumer must know belongs on the S line too.
- **`cbpin=` / `gfpin=` — FIXED 2026-08-13.** The `cbpin=` field, its parser and its contract
  compare had existed since B6, and a producer fill landed 2026-08-12 — but it read
  `this_epoch_if_pin_sel`, the hold-OFF disc-compare scratch, gated
  `!clock_signal_basis_hold && emit_clock_disc`. `emit_clock_disc` defaults false and appears
  **nowhere in the ft63 → pc305 field chain**, while every sim posture-C config arms it: so every
  FIELD product declared no pin while sim products declared fine, and the truth-grader (written to
  REFUSE rather than assume a pair) could not transform field B records at all. The defect class is
  **a product-contract declaration accidentally gated on a QC-channel knob**. The fill now reads a
  dedicated unconditional record (`this_epoch_if_pin_real`, written on BOTH the held and the
  re-selected path), and re-declares at every emit so a broadcast cannot carry a stale pair.
  `gfpin=` is new and declares the slant posture on the same per-constellation unanimity rule. Both
  are declaration-only: solve and emitted values untouched, and the S line is byte-identical when
  neither is declared.
- **What `gfpin=` proves, exactly.** PRESENCE is a positive assertion: every dual-frequency
  satellite of that constellation emitted in the declaring epoch took a GF row on the named pair,
  so the B records are the slant-posture estimable function. **ABSENCE is NOT proof of the
  single-layer posture.** It is also produced by (i) a per-satellite pair SPLIT, where unanimity
  refuses to describe a heterogeneous constellation with one field (the Galileo Q/X family case),
  and (ii) a constellation whose emitted satellites are all single-frequency, which take no IF or
  GF row at all. Both log a line naming the reason. Absence therefore means "read this as
  single-layer" — the safe direction to be wrong in, since a consumer that assumes single-layer
  applies one datum direction where two were folded and sees the discrepancy in its own residuals,
  whereas the reverse silently removes content that is really there. A grader must treat
  absence-with-B-records as REFUSE, not as a posture verdict. (The third historical failure mode —
  stamp-once, where a declaration made at the first emit went stale for the whole run — is closed:
  the declaration is rebuilt at every emit and assigned wholesale, so a constellation that stops
  qualifying DROPS its token at the next broadcast.)
- **Residues the GF datum still owes**: (1) **No run-end telemetry** — `n_gf_pin_sats` /
  `n_gf_only_pair` are referenced ONLY on the `Network[W6-cb]` first-N line
  (`NetworkSolveGlue.cpp:314/317`, capped by `spectrum_diag_budget = 6`), so a settled run reports
  the GF datum nowhere, while the loader's own message promises "the run-end count of such
  satellites is reported". Not merely a general-rule violation: the sibling RXCB-DATUM has exactly
  the missing line at `NetworkTeardown.cpp:2192–2211`, an armed-only run-end readout citing
  standing invariant #4 in its own comment ("the ONLY way to tell a live gauge from an
  armed-but-inert one is the fold count, so print it even (especially) when it is zero"). The GF
  datum deviates from an established in-repo pattern, and the fix is that function. (2) **Not
  fingerprinted** — `network.code_bias_gf_datum` is absent from the warm-resume snapshot
  fingerprint although it changes the realized basis; its sibling `rx_code_bias_datum` is present.
  (3) **No mark on a mid-run GF re-aim** — the §C row. (4) **`cb_datum` is a single-valued enum**,
  so the positional `cb_datum` field and `checkContract`'s comparison of it are tautologies; the
  code-bias datum is described by `cbpin=`/`gfpin=`, not by that field.
- **`amb_datum` was wrong on every eq34 product — FOUND AND FIXED 2026-08-13.** Same defect class
  as the `cbpin=` gap (the wire under-declares the realized basis), found by checking §A row 4/5
  against a real field product. `NetworkSolveGlue.cpp:388–394` flips the per-solve `decl` to
  `SingleReceiverChain` under `phase_datum_eq34` — but that object only feeds `buildSharedDatum`.
  The SERIALIZED declaration is `pending_contract.decl`, populated at
  `ApplicationNetworkSetup.cpp:998–1030`, which had a branch for `phase_datum_ensemble` (added by
  T3b, which repaired exactly this bug for the ensemble datum) and **none for `phase_datum_eq34`**.
  So every eq34 run emitted `amb_datum = 0` — `PivotReceiverZeroAmbiguity`, the legacy single-pivot
  datum it is not using — while carrying `pdr=` two fields later, a wire that contradicts itself.
  Evidence on disk: `runs/pc305_ggq_cbpin_verify_BEFORE_ambdatum/products.txt` reads
  `S,MBAR00UGA,1,nominal,`**`0`**`,0,1,0,0.05,0,…,pdr=POHN00FSM` on a config chain that sets
  `phase_datum_eq34: true`. Fixed by mirroring the SolveGlue precedence exactly (`ensemble` wins,
  then `eq34`). **This changes S-line field 5 from 0 to 2 on every eq34 lineage** — data records
  untouched. Safe for the in-repo consumers: the realtime/closed-loop consumer ADOPTS the first
  received contract as its expectation (`ApplicationRealtime.cpp:507–508`) rather than building
  `amb_datum` from its own config, so nothing newly mismatches, and it flags only a mid-stream
  CHANGE — what a genuine datum move should do. The warm-resume fingerprint keys on the config knob
  `network_phase_datum_eq34_` (`GnssConfigNetworkPosture.cpp:489`), not on the declaration, so
  resumes are unaffected. An EXTERNAL consumer that hardcoded 0 will now see a mismatch; that is
  the correct outcome — it was reading a false declaration. The three enumerators' wire integers
  are now frozen by `static_assert` (`tests/test_network_product_io.cpp`): they are bare positional
  ints in S-line field 4, so reordering the enum would silently re-interpret every stored product.
  **Append new realizations; never insert or reorder.**
- **Per-channel FAMILY (Galileo Q vs X) is NOT covered by this contract.** The measured field case:
  the DOY-129 roster partitions **185 pure-Q / 114 pure-X / 11 no-Galileo over 310 RINEX files,
  with ZERO OBSERVED bridging receivers** (re-measured on OBSERVED data 2026-08-13,
  `SPEC_family_gf_datum.md` §1). ⚠ The superseded "187 / 113" figure came from a **header** scan,
  and a census built that way reports **2 phantom bridges** — `KOUC00NCL` and `NRMD00NCL` declare
  C1C/C5Q/C7Q and observe **zero** of them across 22,564 Galileo satellite lines — which would read
  as NOT partitioned and silently disable the family-GF card. The census must derive from
  observation-derived state, never from headers. `selectIfPinPair` picks the Q pair, and rd 3b
  re-derives per family — 20 undeclared near-null directions, μ-patterned, and every epoch of
  `_pc305_gg_tri_slant_20m` withheld by the rank gate. Under a future per-family datum the two
  families share ONE S-system in the clock direction and have SEPARATE ones in the iono direction,
  so a consumer must not mix families' iono-coupled content. `gfpin=`'s value grammar already
  accepts the `'+'`-joined per-family form (`gfpin=E:C1C-C5Q+C1X-C5X`) and the parser round-trips
  it, but this producer only ever writes one pair per constellation and the grader refuses a
  multi-family value rather than grading the first family silently. Design:
  `docs/SPEC_family_gf_datum.md`.

---

# §E Theory canon

*Absorbed 2026-08-15 from `docs/ssystem_theory_reference_2026-07-09.md` (compiled 2026-07-09 from
a deep read of the S-system canon; now `docs/attic/`).*

