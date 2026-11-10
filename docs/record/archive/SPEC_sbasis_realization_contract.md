<!-- FROZEN / READ-ONLY: the pre-split monolith, kept byte-for-byte. Never append here — new results go in the card file (see docs/INDEX.md). -->
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

## §E0 The corpus and the notation

| Tag | Source | What it gives us |
|-----|--------|------------------|
| **Odijk-2016** | Odijk, Zhang, Khodabandeh, Nadarajah, Teunissen (2016), *J Geod* 90:15–44, "On the estimability of parameters in undifferenced, uncombined GNSS network and PPP-RTK user models by means of 𝒮-system theory" (DOI 10.1007/s00190-015-0854-9 — paywalled; framework cross-checked against the 2011 algebra) | The exact estimability theory of **our** model: rank-deficiency enumeration, CC-R vs CC-S datums, S-transformations, user DD-integer, datum time-stability |
| **TK-2015** | Teunissen & Khodabandeh (2015), *J Geod* 89:217–240, "Review and principles of PPP-RTK methods" (DOI 10.1007/s00190-014-0771-3) | Six PPP-RTK mechanizations = **2 S-bases**; PPP-RTK is *relative* positioning; the UPD/FCB estimation-method **biasedness proof**; which part of the correction covariance the user actually uses |
| **Hou-Zhang-2025** | Hou & Zhang (2025), *J Geod* 99:3, "A generalized least-squares filter designed for GNSS data processing" | **TIME-domain** estimability — the rigorous replacement for the "engineering" large-variance init / fake dynamic models; two-transformation handover; NEQ reduction; flexible-window smoother |
| **AT-Teunissen** | Teunissen (2024, 2nd ed), *Adjustment theory: an introduction*, TU Delft OPEN | Foundations: BLUE/estimability, A-model/B-model duality, the datum model `E{y}=Ax, Bᵀx=0`, recursive/phased (Kalman) estimation, Lagrange/KKT minimum-constraint machinery |

Notation: `~` = estimable (S-basis-reduced) combination; `dt^s` satellite clock; `d^s_j`/`δ^s_j`
satellite code/phase bias (freq `j`); `z^s_{r,j}` integer ambiguity; `ι^s_r` slant iono;
`μ_j = λ_j²/λ_1²`; `μ_IF`, `μ_GF` the IF / GF combination row-vectors of `[μ,e]⁻¹`. `f` = #freq,
`n` = #receivers, `m` = #satellites, `k` = #epochs.

## §E1 The estimability spine, and the datum-invariance theorem

**Estimability condition** (AT Ch.1–2). A linear functional `θ = fᵀx` has a linear *unbiased*
estimator **iff `f ∈ range(Aᵀ)`**. In the undifferenced/uncombined model `A` is
column-rank-deficient, so raw per-satellite clocks / phase biases / code biases are **not**
estimable — only certain *combinations* are. "The satellite clock" does not exist as an estimable
number; only a datum-dependent lump does.

**Rank-deficient decomposition** (Odijk-2016 §2). `E{y}=Ax`, `rank A = r < n`, null space `AV = 0`.
Split `x = S·α̃ + V·β`: `α̃` = the `r` estimable functions, `β` = the `n−r` **inestimable**
directions = the minimum-constraint (datum) vector. The **S-transformation**

```
S = I − V [ (S⊥)ᵀ V ]⁻¹ (S⊥)ᵀ ,      S·V = 0,  S·S = S
```

is the idempotent oblique projector onto the estimable space along the null space; **its ROWS are
the estimable linear combinations** — literally what `NetworkSbasis` constructs. `S⊥` encodes which
directions the datum pins.

**Cross-datum consistency** (Odijk-2016 Eqs 9–11). For two datums `S, S'`: `S·S' = S`, `S'·S = S'`,
so `S'·x̂_S = x̂'_S` and `Q' = S'·Q·S'ᵀ`. You can convert any solution **and its covariance** from
one datum to another **without knowing which datum it came from** — the correct machinery for
comparing/converting products across our three clock datums (§A row 1a).

**The datum model** (AT Ch.5 §5.3) — the abstract skeleton of `NetworkSbasis`. Datum = constraint
`Bᵀx = 0`. Two-step solve:

```
Step 1 (ignore datum):  x̂_A = Q_xA Aᵀ Q_y⁻¹ y ,   Q_xA = (Aᵀ Q_y⁻¹ A)⁻¹
Step 2 (impose datum):  x̂   = [ I − Q_xA B (Bᵀ Q_xA B)⁻¹ Bᵀ ] x̂_A   ← the S-transform projector
```

Covariance becomes a **rank-b downdate** `Q_x̂ = Q_xA − Q_xA B(Bᵀ Q_xA B)⁻¹ Bᵀ Q_xA`, which is
**SINGULAR** (`Bᵀ Q_x̂ = 0`): the datum directions carry **zero variance** — conventions, not
measurements. For a single sum-to-zero gauge (`b=1`, `B=g`) this is exactly the rank-1 fold
`h hᵀ/β` with `h = Q_xA g`, `β = gᵀ Q_xA g` — the `g gᵀ/β` Schur fold already in the phase-ensemble
kernel. **⚠ The metric is `Q_xA`, NOT Euclidean** — even a plain `Σ = 0` constraint redistributes
covariance-weighted, not by arithmetic mean.

> ### 🔑 THE DATUM-INVARIANCE THEOREM — still in force
> Any estimable functional `fᵀx` (`f ∈ range(Aᵀ)`) is invariant in **value AND variance** under any
> admissible datum. Corollary: *the differential per-sat clock (~47 cm) and phase-bias (6–69 cm)
> wander are estimable quantities, hence provably unchanged by re-datuming.* No choice of
> clock/phase S-basis (s2z / bca / signal-hold / prioritized-anchor / ensemble) can move them. This
> is the first-principles proof of why every datum A/B came out NEGATIVE and why the "null-space
> leak" was correctly falsified. **The cure must alter ESTIMATION (weights / dynamics / model /
> transmitted covariance), not the gauge.** It is also the theory backing for
> `SPEC_pivot_visibility_gauge_defect.md` §4.1's "raw wire clock spread is not a verdict".

## §E2 The rank deficiencies, and what is estimable before any datum

Undifferenced/uncombined phase + code (Odijk Eq 12), per (r, s, j, epoch i):

```
E{Δφ^s_{r,j}} = g^sᵀ Δx_r + dt_r − dt^s + λ_j(δ_{r,j} − δ^s_j) − μ_j ι^s_r + λ_j z^s_{r,j}
E{Δp^s_{r,j}} = g^sᵀ Δx_r + dt_r − dt^s +   (d_{r,j} − d^s_j)  + μ_j ι^s_r
```

Ambiguities carry **no time index** (constant absent slips). Iono is either **vertical**
single-layer `ι^s_r = f^s_r · ι^s` (which makes DCBs estimable) or **slant** per-(r,s).

| Type | Coupling | Size |
|------|----------|------|
| 1a | receiver ↔ satellite clock | 1 |
| 1b | receiver ↔ satellite hardware (phase+code) bias | 2f |
| 2a | receiver clock ↔ receiver hardware bias | n−1 |
| 3a | satellite clock ↔ satellite hardware bias | m |
| 4 | receiver phase bias ↔ ambiguity | f(n−1) |
| 5 | satellite phase bias ↔ ambiguity | f·m |

**The exact count (Odijk Eq 24)** — what `declared_dimension` + the null-space gate implement at
runtime (§A rank bookkeeping):

```
# independent S-basis constraints  =  1 + 2f + (1+f)(n−1+m)               (Eq 24)
                    + (n−1+m)   if SLANT iono  (types 2b, 3b)
                    + ν         if REGIONAL (<~500 km, parallel LOS; type 0)
                    × (per-epoch multiples) for any parameter group WITHOUT a temporal constraint
```

**Datum-invariant even before choosing a datum:** the GEO row and the (vertical) ION row of the
null-space basis `V_net` are **ZERO** ⇒ **position/ZTD and vertical iono are estimable in absolute,
uncombined form**. Everything else (clocks, biases, ambiguities) is only a datum-dependent
combination. ⇒ **the consumer vertical error CANNOT be a position-datum artifact**; it must enter
through the biased clock/phase corrections.

**Code-bias / iono coupling** (Eqs 29–33, Table 8): on two frequencies only **one** code-bias DOF —
the geometry-free `d^s_GF` (a scaled DCB) — is estimable; the second band is a fixed multiple
(`d̃^s_j = μ_j·d^s_GF`, `j=1,2`). Modernized per-band DCBs (`j ≥ 3`) are estimable **only under
vertical iono**; under **slant** iono the GF code biases lump into the slant iono
(`ι̃^s_r = ι^s_r + d_{r,GF} − d^s_GF`) and dual-freq code bias is inestimable. This is the
estimability root of "SH-iono forces sat-code-bias, GLONASS excluded", and §A row 2b/3b is the
realization of exactly this row as a datum.

## §E3 CC-R vs CC-S, and where our features sit

- **CC-R** ("Common Clocks, pivot Receiver") — pin one receiver (clock, hardware biases, IF code
  biases, pivot-sat & pivot-rcv ambiguities). Estimable ambiguities are **exact integer double
  differences** ⇒ clean user AR. The reference jumps only when the pivot receiver changes (rare) ⇒
  **TIME-STABLE**. The *integer-ambiguity S-basis* `(Δx₁, d^ps, z₁^ps)` behind CC-1/DC/IRC/DSC.
- **CC-S** ("Common Clocks, mean Satellite") — pin the satellite **mean** via
  `Z_m = I_m − (1/m) e_m e_mᵀ`. Matches the IGS zero-mean-DCB convention. **But the reference set is
  TIME-VARYING** — the datum is silently redefined whenever a satellite rises or sets, so estimable
  clocks/DCBs **jump at every membership change**; Odijk names **Zhong et al. 2015**'s IGS DCB
  long-term variation as exactly this artifact. Ambiguities are **not directly integer** (they need
  the Eq-34 recovery, §E6). The *phase-bias S-basis* `(Δx₁, d^ps, δ^ps)` behind CC-2/UPD-FCB.

| GnssProcessor feature | Canon S-basis | Time-stability verdict |
|---|---|---|
| `clock_datum_sum_to_zero` (per-const `Σ dt^s = 0`) | **CC-S** / satellite-mean gauge | ⚠️ set-dependent — steps at every constellation-membership change; *theoretically the worst for time-stability* |
| `clock_datum_broadcast_anchored` (per-sat brdc anchors) | **inhomogeneous pin** `BᵀE{y} = b⁰` (external, dim-0, covariance-neutral) | ✅ set-**INVARIANT** — closest in spirit to CC-R's "jumps only on a rare reference change" |
| `clock_signal_basis_hold` (hold the IF-pin pair in time) | **fixed-clock-convention** — freezes `μ_IFᵀ d^s` across epochs | ✅ pins the code↔clock split in time |
| prioritized per-(sat,signal) **anchor pin** (phase bias) | **CC-R-like** integer-ambiguity basis; the `−z^s_{1,j}` inside `δ̃^s_j` | ⚠️ single point of failure; anchor handover steps the bias by whole cycles (defect-3 churn) |
| **ensemble** `Σ_r z^s_{r,σ} = 0` (phase bias) | **CC-S-like** phase-bias basis / **min-variance inner constraint** | ✅ anchor-free, handover-free — BUT needs the Eq-34 integer-recovery guarantee (§E6). **See the SUPERSEDED note in §E6.** |
| between-sat SD **pivot + handover** (`PivotManager`) | a choice of condition-basis `B` (`BᵀA = 0`) | handover = pure S-transform; any discontinuity beyond it is a code bug (defect 3) |

**Theory ranking for time-stability of the transmitted product:**
`broadcast_anchored ≳ signal_basis_hold > sum_to_zero`. A **product-domain** ranking; per the
datum-invariance theorem it does **not** change the consumer's estimable position.

## §E4 What the network transmits, and the correction covariance

- **Estimable satellite clock** (CC-R): `dt̃^s = [dt^s + d^s_IF] − [dt₁ + d_{1,IF}]` — the
  **ionosphere-free clock** (clock + IF code bias), referenced to an epoch-1 datum. Exactly the IGS
  IF-clock convention (cf. §B record C). Its **differential** (per-sat, non-common-mode) time drift
  is the consumer-unabsorbable part; the common-mode part is absorbed by the user receiver clock.
- **Estimable satellite phase bias** (CC-R): `δ̃^s_j = (fractional bias) − z^s_{1,j}` — it literally
  **carries an integer** (the pivot-receiver ambiguity). *That is the mechanism by which network
  phase biases give the user its integer property.* Re-anchoring shifts it by whole cycles ⇒
  defect-3 churn.
- **User ambiguity** = automatic **integer double difference**
  `z̃^s_{u,j} = [z^s_{u,j} − z^p_{u,j}] − [z^s_{1,j} − z^p_{1,j}]`. The **only** surviving
  network-datum footprint the user inherits is `[z^s_{1,j} − z^p_{1,j}]`; the user never needs to
  know the network pivot. ⇒ **PPP-RTK is RELATIVE positioning**, and an `n = 1` network collapses
  exactly to a single-baseline DD (TK-2015 §4.3, Eqs 69–72).
- **Iono must be transmitted UNDIFFERENCED per-satellite** (Table 11) — a vertical→slant mapping
  intervenes at the user, so between-satellite differencing it is inconsistent. (⚠ §B records that
  our wire carries **no** ionosphere record at all — the consumer receives half the pair.)
- **Integer preservation requires** (a) satellite phase biases are provided (else standard-PPP,
  non-integer and time-dependent, Eq 43); **AND** (b) network/user iono parameterizations are
  consistent, or DCBs are supplied — else DCBs lump into the user ambiguity and destroy integerness
  (Eqs 44–45).
- **The correction covariance the user actually uses** (TK-2015 §5.3, Eq 88): only the
  block-diagonal `diag(C_φ, C_p) ⊗ C_s` (× `1/n` if ambiguity-fixed). The rank-one `[e;e]`
  (clock/geometry) and `[−μ;μ]` (iono) parts lie in the user's own design range and are absorbed.
  **⚠ Caveat: a SINGLE-EPOCH, unbiased-correction result** — it lets us simplify the emitted
  stochastic model but does **not** cover time drift (defect 2).

## §E5 The four theory-grounded levers — all ESTIMATION-side

Three independent canon results converge: the §E1 datum-invariance theorem; BLUE S-basis invariance
(TK-2015 §4.1 — a rigorous network adjustment with *any* PPP-RTK datum gives the *same* user
position, so differences imply non-rigorous estimation, not a wrong datum); and our own NEGATIVE
datum A/Bs (s2z / bca / signal-hold) plus the falsified null-space leak. ⇒ **stop hunting the
gauge.**

**Lever A — datum TIME-stability / rigorous time-linking (Hou-Zhang-2025).** Is the transmitted
estimable combination **rigorously time-linked** (`z_k = z_{k−1}`, true small/zero process noise) or
**re-floated every epoch** under a diffuse large-Q prior? `z_k = z_{k−1}` with **true zero process
noise is the CORRECT model for a constant estimable function**; option (b) is the "engineering"
misspecification Hou-Zhang condemn (Fig 12: decimetre bursts on every re-track under KF, clean
under GLSF), and the large-variance init for newly-tracked satellites is the same anti-pattern.
**⚠ Do NOT compute a new ambiguity's predicted value from the current epoch (RTKLIB style)** — the
paper proves that reuses observations and breaks BLU.

**Lever B — full/block product covariance carry (Tienstra estimation-in-phases).** AT Ch.6 §6.6:
the producer→consumer split is **exact only if** Phase 2 receives the **full** Phase-1 covariance
`Qŷ_B1 = P_(QyB1)⊥ Qy`. Our **DIAGONAL** carry drops the cross-correlation among satellite
clocks/phase-biases — a **provable sub-optimality**: the consumer cannot correctly weight the
*correlated* differential wander it is fed. A/B: emit a per-satellite (or per-constellation)
**block** covariance. Good news (Eqs 160–164): the consumer's RAIM statistic needs only misclosures
+ their covariances, not raw `Qy`, so the product stream stays self-sufficient. *(Emit-path seams:
`NetworkProductEmitter` / `NetworkProductIO`; the gap is the "diagonal carry" item of
`project_network_ssystem_dia_assessment`.)*

**Lever C — decoupled phase clock (DC / IRC / DSC).** TK-2015 §2.3, §3.1: a phase clock decoupled
from the code clock (`δ̄^ps_IF` distinct from `dt̄^ps`) is a **rigorous, S-basis-consistent**
reparametrization. **Correction to the standing note "no separate code/phase clock per Odijk" — it
should be SOFTENED**: a decoupled clock adds no *information* and won't by itself fix defect 2, but
it is a **legal** datum and a genuine lever worth prototyping. It is not forbidden.

**Lever D — minimum-variance inner constraint (AT App.C Ex.3).** `min lᵀQ_y l s.t. Aᵀl = f` ⇒
`l = Q_y⁻¹A(AᵀQ_y⁻¹A)⁻¹f`: among admissible datums the **covariance-weighted inner constraint has
LEAST variance**. A single hard anchor pin dumps all datum variance onto one parameter; an
ensemble/inner constraint spreads it. Action: weight an ensemble gauge by parameter covariance
rather than by an unweighted mean.

*(Consistency check: the "coverage/float50" lever that cut differential clock wander 47 → 14 cm is
estimation-side — it changes which ambiguities are floated, hence the effective model/redundancy —
exactly the class the theory says can work.)*

**The absorbed reference's own ranked deep-dive list, kept because the ranking is the content:**
(1) **Lever B, product covariance carry** — the freshest, most concrete first-principles suspect;
emit per-sat/per-const block covariance and A/B the consumer vertical (the seams and the
"diagonal carry" gap above); (2) **Lever A, time-linking audit** — instrument whether the
transmitted datum quantities are `z_k=z_{k−1}` time-linked or re-floated, and replace the
engineering large-variance init with a data-derived (Hou-Zhang) init; (3) **the phase-ensemble
Eq-34 integer-recovery unit test** (§E6) — **it must pass before the ensemble datum can be trusted
or committed**; (4) **the §E7 correctness punch-list** — the rank-count assertion and the `sin²α`
meter are cheap, always-on, and bear directly on the 305-station non-PD stations; (5) **Lever C,
decoupled phase clock** — prototype emitting `δ̄^ps_IF` alongside the common clock, and A/B it only
if Levers A/B do not close the gap.

## §E6 The MANDATORY Eq-34 gate for any CC-S-like phase gauge

> ### ⚠️ MANDATORY correctness gate — still binding
> A CC-S-like gauge (the `Σ_r z^s_{r,σ} = 0` ensemble is one) has ambiguities that are **NOT
> directly integer** (Odijk-2016). They need the recovery transform (**Eq 34**):
> `z̈^s_{r,j} + Σ_{s≥2} z̈^s_{r,j} = [z^s_{r,j} − z^1_{r,j}] − [z^s_{1,j} − z^1_{1,j}]` (the CC-R
> integer double difference). Any claim that such a gauge "keeps the user DD integer" **IS** Eq 34.
> **Unit-test it explicitly:** run an ensemble-datum network and a CC-R (prioritized-anchor) network
> on the same data and assert the reconstructed user DD ambiguities are **bit-identical integers**,
> especially **across a handover epoch**. That is the pass/fail for the whole approach, and it
> **must pass before the ensemble datum can be trusted / committed.**

> ### SUPERSEDED BY MEASUREMENT — the ensemble gauge's *validation verdict* only
> The absorbed reference concluded that "the canon **strongly validates** the `Σ_r z^s_{r,σ} = 0`
> ensemble gauge" (min-variance per Lever D, anchor-free, handover-free, one common ensemble
> reference per satellite). **That verdict is SUPERSEDED, not the theory.** The ensemble PB datum,
> clock s2z and broadcast-anchored were subsequently **all A/B'd dead** in the field
> (`project_network_falsified_approaches`; §8.2 of `SPEC_pivot_visibility_gauge_defect.md` records
> the same list) because common-mode is consumer-absorbable, and that card's §4 measures the
> related prioritized-anchor posture as **NET-HARMFUL** on its fixture. The theoretical argument
> and the Eq-34 gate above **remain valid and binding**; the "strongly validates" reading does not.

## §E7 The ten-item correctness punch-list

1. **Rank-deficiency count assertion** (Eq 24). Assert
   `#S-basis constraints applied == 1 + 2f + (1+f)(n−1+m)` (`+ (n−1+m)` slant, `+ ν` regional) for
   the current `(f,n,m)`. Under-constrained ⇒ near-singular normal matrix (candidate for the
   305-station em25 non-PD stations); over-constrained ⇒ biased products. Cheap, exact, wire it
   into the epoch-1 estimability trace. *(§A's `declared_dimension` + null-space gate is the
   realized form.)*
2. **`sin²α` per-parameter estimability meter** (AT Ch.6, Eq 35): `sin²α = diag(N_reduced)/diag(N)`
   — **O(1) per parameter**, explicitly recommended by Teunissen "to detect singularities/rank
   defects". An always-on lightweight complement to the gated SVD (`trace_estimability`).
3. **Orthogonality / clean-datum test** (AT Ch.6, Eq 26): a datum column is a *pure re-gauge* iff
   `A₁ᵀ Q_y⁻¹ A₂ = 0`. If not, it leaks into the precision of every non-orthogonal estimable
   function (Eq 28). A concrete test for "pure S-transform" vs "perturbs the solution".
4. **`n = 1` network → baseline-DD regression** (TK-2015 Eqs 69–72). Drive the PPP-RTK consumer
   from a single-station `NetworkEstimator` run; the solution must equal the baseline-DD engine to
   within the absorbed iono term. A cheap deterministic test isolating product-contract /
   datum-reconstruction bugs from estimation issues.
5. **NO float-fraction UPDs** (TK-2015 §6, **Lemma 7**). Averaging `frac(·)` of **float**
   ambiguities is **provably biased** and breaks user integerness for `n > 1`. Emit phase biases
   from **ambiguity-resolved** quantities (Eq 106: `Σ_j frac(ẑ_{j,W})/n + â_{1,W}`), or restrict to
   the safe cases (single station, or fixed `ǎ_r`). Audit that we never ship float-fraction biases.
6. **Emit the SINGULAR product covariance** (AT Ch.5). If we transmit product covariance it must be
   the S-transformed **singular** `Q` (annihilated by `Bᵀ`), not the unconstrained `Q_xA` — else we
   tell the consumer a pure convention is uncertain.
7. **Innovation-based RAIM** (AT Ch.6, Eq 73). The detection statistic is `Σ v_kᵀ Q_v(k)⁻¹ v_k` on
   the **INNOVATION** `v_k = y_k − A_k x̂_{k|k−1}`, *not* the post-fit residual. Verify the
   clock-monitor/RAIM uses the predicted residual.
8. **Gate per-band OSB on the iono parametrization** (Odijk Table 8). Under slant iono dual-freq
   code bias is *not* separately estimable (lumped into iono); only `j ≥ 3` modernized DCBs
   survive. Emitting independent dual-freq OSB under slant iono is an over-parameterization — the
   estimability explanation for the GLONASS/SH-iono coupling. *(§A row 2b/3b now realizes this as
   an explicit GF datum row.)*
9. **Iono emitted UNDIFFERENCED per-satellite** (Table 11) — never between-satellite differenced.
10. **Datum re-derived over the CURRENT satellite set** on any membership change, modelled as
    Hou-Zhang's **two-transformation** handover (`Fᵀ` back, `F̃ᵀ` forward), carrying only the
    intersecting time-linked functions — no fabricated priors, no silent re-datum step.

## §E8 The estimation foundation (AT Ch.6 + Hou-Zhang-2025)

- **Measurement update.** B-form (Kalman gain) `K = Q Aᵀ(Q + A Q Aᵀ)⁻¹`, or A-form (information)
  `Λ_k = Λ_{k−1} + A_kᵀ Q_k⁻¹ A_k`. Numerically identical; the A-form maps to
  `shared_carry_full_cov` / `addSharedPriorDense` on the border. **⚠ Static-x caveat:** Ch.6 derives
  *only* the measurement update — no state transition, no process noise — so datum **TIME-stability**
  (the essence of defect 2) is **not** in Ch.6; it needs the dynamic layer.
- **Schur / border reduction** (AT §6.2, §6.4 Helmert): the arrowhead reduction behind
  `NetworkNormalEquations`. Reduction and gauge-fold **COMMUTE** in the bordered structure — the
  theoretical warrant for folding a sum-zero gauge as a rank-1 update on the reduced system from
  cached LDLᵀ factors.
- **Generalized LS filter** (Hou-Zhang-2025), the dynamic layer: only chosen linear functions
  `z = Fᵀx` carry a dynamic model, everything else (receiver clock, per-epoch iono, new-sat
  ambiguities) is re-estimated each epoch with **no** initial value and **no** large-Q;
  **two-transformation handover** (`Fᵀ` back, `F̃ᵀ` forward) rigorously carries only the
  intersecting time-linked functions when the satellite set changes ⇒ formalizes pivot handover as
  a *time-datum* operation and predicts a clean handover eliminates defect-3 churn; **null-space
  elimination** `x = S z̃ + G u`, `F̃ᵀG = 0`, projecting out nuisance `u` via `P⊥_{A^u}` — the same
  projector algebra as a minimum-constraint S-transform, source of the **23–32 % NEQ reduction**
  (eliminate iono + clocks per epoch); **flexible-window recursive smoother**
  `J^{z̃} = P^{z̃}(Φ̃^z)ᵀ(P^z)⁻¹` — collapses convergence to ~1 epoch and bridges rise/set without
  hard segment boundaries, a near-real-time complement to our full-arc RTS smoother.

---

# §F User-side conformance audit, 2026-06-15

*Absorbed 2026-08-15 from `docs/ssystem_conformance_2026-06-15.md` (now `docs/attic/`).* **⚠ Scope:
this is the USER side** — BSSD consumer, MLAMBDA, real-time SSR — audited on 2026-06-15, **not** the
network producer of §A–§E. **Every code anchor below is a 2026-06-15 snapshot and drifts.**
**§F7 is the network's design charter**, cited from code as "§7". *(There is no §F6: the absorbed
doc's §6 compatibility-audit table is folded into §F5, where it belongs with the enforcement
discussion.)*

**Method (for weight, not repetition):** 7 code-recon passes over the measurement model, state
schema, pivot machinery, corrections/bias path, ambiguity resolver, atmosphere and config; every
load-bearing code claim **adversarially re-verified** against source. One hypothesised gap (SSR yaw
fields unconsumed) was upgraded to **CONFIRMED**. Correction made during that pass and preserved:
the `markAmbiguityUnresolvable` / GLONASS exclusion lives around `MeasurementModel.cpp:186` (skip)
and `:650`, `:669` (mark) — **not `:629`**.

Primary sources: Odijk, Teunissen, Zhang (2011), "PPP-RTK platform performance based on
single-frequency GPS data", Curtin GNSS Research Centre (Eqs 1–9 read in full); Odijk et al. (2016)
*J Geod* 90(1):15–44 (DOI 10.1007/s00190-015-0854-9, paywalled; framework reconstructed from
established knowledge and cross-checked against the 2011 algebra); Teunissen & Khodabandeh (2015)
*J Geod* 89(3):217–240 (DOI 10.1007/s00190-014-0771-3); Baarda (1973); Teunissen (1985) zero-order
design / S-transformations.

## §F1 Verdict, and the one structural divergence

The **user side is a faithful, S-basis-INVARIANT realization of Odijk PPP-RTK — realized through
differencing operators and a frozen-pivot covariance pin, not through Odijk's explicit
minimum-constraint / estimable-bias parametrization.** The estimable functions the filter carries
(geometry, ZWD, slant-iono-lumped-with-DCB, DD-integer ambiguities) are the S-basis-invariant
observables of the 2016 model, related to it by a one-to-one S-transformation. The integers MLAMBDA
recovers are the same double-differenced integers `N_{1r}^{ps}` of Odijk Eq. 9. Conformance is high
and the architecture is correct **for a user engine**.

**The one structural divergence that matters:** the code never uses an explicit S-basis. Every
Odijk rank deficiency is disposed of by *differencing it away*, *importing it as a product
correction*, or *absorbing it into the float ambiguity* — never by an explicit minimum constraint.
That is a legitimate *implicit* S-basis (differencing **is** an S-basis choice). But it means **no
object in the code declares the convention it assumes** — which IF code-bias pair sits in the
clock, which ambiguity datum sits in the phase bias, which pivot. **Sufficient for a consumer; the
entire problem for a producer**, which must declare a convention the consumer can reproduce.
(§A–§D are that declaration, built since.)

## §F2 Per-deficiency conformance table (2026-06-15 anchors)

The three mechanisms are **differenced / imported / absorbed-into-ambiguity**; **NO row uses an
explicit minimum-constraint equation** — the defining structural fact.

| Odijk deficiency | Code mechanism | Anchor (2026-06-15) |
|---|---|---|
| **(a) Clock datum** | Receiver clock estimated (PPP) or **differenced away** (BSSD zeroes the column); satellite clock **imported** as the IGS IF clock | `BssdTransform.cpp:221`, `PppModel.cpp:1367` |
| **(b) Code-bias IF↔clock** | Satellite side absorbed into the imported IF clock; receiver side estimated as IFB/DCB states in the *same IF datum* via GF-IF coefficients (`μ1/dmu`, `μ2/dmu`) | `StateInitializer.cpp:103` |
| **(c) Phase-bias↔ambiguity** | **No phase-bias state anywhere.** Satellite phase bias **imported** (SSR/BIA); receiver phase bias **differenced away**; uncovered ⇒ absorbed into the float ambiguity + flagged `markAmbiguityUnresolvable` | `MeasurementModel.cpp:1160`, `StateManager.cpp:1103` |
| **(d) Iono↔DCB** | Estimated per-sat L1-referenced slant iono = iono + DCB lump (Eq. 3); pivot iono column zeroed in SD | `MeasurementModel.cpp:1098`, `BssdTransform.cpp:236` |
| **(e) Ambiguity↔phase-bias** | DD integers fixed by MLAMBDA; **ZD** state per-(sat,signal), AR differences SD→DD internally (`buildDdPairs`, elevation pivot); baseline holds explicit DD states | `AmbiguityResolver.cpp:386`, `:561`; `StateKeySchema.hpp:94`, `:256` |
| **(f) ISB datum** | GPS-hardcoded inter-system reference; ISB estimated per non-GPS system, differenced away in same-system SD | `StateManager.cpp:54`, `StateKeySchema.hpp:451` |

Supporting theory recap: the estimable **satellite clock** = true clock + IF satellite code bias
(+ regional ZTD lump); the estimable **satellite phase bias** = true phase bias + a code-bias lump
+ **the network pivot-receiver's non-integer ambiguity `M_{1,1}^{ps}`** — that hidden ambiguity is
exactly what makes the user's ambiguities come out as double differences (Eq. 9) even though the
user never sees the network pivot. Corrections are **S-basis-covariant** (TK-2015): the user
recovers integers *only if* it adopts an S-basis compatible with the network's — same satellite
pivot, same IF code-bias convention in the clock, same ambiguity datum and wind-up model in the
phase bias, same sign. The **three user models** in increasing iono-constraint strength (faster AR,
greater risk) are iono-**float**, iono-**weighted** (the regional workhorse) and iono-**fixed**.

## §F3 The implicit S-basis the code actually adopts, per mode

**PPP-RTK (BSSD on):**

- **Receiver datum** — fixed by between-satellite SD: receiver clock, phase clock, code biases
  (DCB/IFB) and ISB are all driven out by the differencing operator
  (`BssdTransform.cpp:221-235`). **Stronger than Odijk's "nominate receiver 1"**: SD eliminates the
  *whole* receiver-side block per constellation.
- **Satellite datum** — the **per-constellation pivot satellite** (`PivotManager`, GLONASS
  excluded). Pivot ambiguity / iono / sat-bias are the held quantities, realized as a **hard pin in
  TWO places**: covariance→0 via `freezePivotAmbiguityCovariance` /
  `freezePivotIonoAndBiasCovariance` (`PivotManager.cpp:849`, `:881`), and value→0 via BSSD
  column-zeroing (`BssdTransform.cpp:236–251`). *Two places realize "value 0, variance 0", not one.*
- **Clock/bias convention datum** — ***IMPORTED, not chosen by the code***: satellite clock = the
  IGS ionosphere-free code-bias-absorbed clock referenced to the BIA
  `SATELLITE_CLOCK_REFERENCE_OBSERVABLES` pair (`ProductParser.cpp:1003`); satellite code biases =
  per-signal OSB made IF-clock-consistent on the receiver side via GF-IF coefficients; satellite
  phase biases = per-signal OSB from SSR. **The code adopts whatever S-basis the product files were
  generated under** and re-expresses its receiver states to match (the GF-IF coefficient choice is
  the only active alignment).
- **Inter-system datum** — GPS, hardcoded.

**Baseline (DD):** satellite datum = per-constellation pivot (`DdPivotManager`, SNR-ranked);
receiver datum = the fixed reference station. DD removes *both* satellite-side product error and
receiver-side terms, so the baseline solution is **product-convention-INSENSITIVE** — the robust
fallback whose correctness does not depend on the §F5 compatibility contract.

**Versus Odijk's explicit S-basis:** Odijk fixes one parameter per deficiency and *retains the
estimable biases as named parameters you can print, broadcast and audit*. GnssProcessor fixes the
same dimensions but **discards** the satellite-side estimable biases into imported corrections and
**discards** the receiver-side estimable biases into the differencing operator. Estimable
observables are identical; the difference is that **Odijk's parametrization is invertible to the
conventions and the code's is not.**

## §F4 What is enforced ACTIVELY vs only NEGATIVELY

**The only actively enforced S-basis contract is GF-IF alignment** (`StateInitializer.cpp:103`):
the clock-reference observable pair is read from the BIA DESCRIPTION (`ProductParser.cpp:1003`) and
the receiver code-bias states are aligned to that IF datum. Correct and S-system-aware.
**Everything else is enforced NEGATIVELY**: a wrong pivot / datum / sign / wind-up convention
degrades to **failed AR validation ⇒ no fix** (ratio + bootstrap success rate ≥ 0.999 + per-DD
residual cap, `AmbiguityResolver.cpp:642`) — never to a silent wrong fix. Safe, but there is **no
positive cross-check**.

**Atmosphere.** Iono-float fully present (per-sat L1-referenced slant iono state, RW Q mapped to
slant by `1/sin²el`, `MeasurementModel.cpp:1098`). Iono-weighted present but **GIM-only**:
`applyGimIonoUpdate` injects the IONEX value as a per-sat soft pseudo-obs, `R` is
per-constellation `diag(var) + σ_c²·11ᵀ` with the all-ones block leaving the per-constellation
common mode (the iono datum) free (`Application.cpp:552`); config
`estimation.ionosphere_weighting.*` (`GnssConfigLoad.cpp:934`); σ→0 / σ→∞ sweeps the full
fixed↔weighted↔float continuum. Iono-**fixed** is absent as a first-class model. Troposphere: ZWD
RW + GMF/VMF3 + optional Chen-Herring gradients + a soft ZWD constraint anchoring the BSSD
[up, ZWD] null space (`GnssConfigLoad.cpp:893`) — correct.

**Integer AR is S-system-correct, but ⚠ PRODUCTION AR IS WL-ONLY.** The resolved quantity is the
genuine DD integer *provided* the satellite phase bias is removed as a known correction (Odijk
§3.2): receiver phase bias removed by **BSSD** (rows that cannot pair to a pivot are rejected
precisely because "the Kalman filter would then absorb the receiver clock and biases",
`BssdTransform.cpp:106`); satellite phase bias removed by the **known SSR correction**
(`MeasurementModel.cpp:1160`), or, where uncovered, the ambiguity is `markAmbiguityUnresolvable`
and excluded from LAMBDA — the code correctly **refuses to integer-fix a bias-contaminated
ambiguity** (`MeasurementModel.cpp:650`, `:669`); GLONASS correctly excluded (FDMA inter-channel
bias does not cancel in SD). Two caveats:

1. **The shipped configs set `"wl_only": true`** (`config/ppp_rtk_internal_AR.json:69`,
   `config/realtime_ppp_rtk_AR.json:123`) — MW-derived WL integers applied as Kalman pseudo-obs,
   **NL LAMBDA never searched**. The full WL+NL cascade and the per-band DD LAMBDA exist but are
   unexercised by live configs. **"Integer AR" today = WL-fixed, float-NL — be precise about this
   in convergence/accuracy claims.**
2. The integer property is decomposed in the uncombined L1/L2 domain (MW-WL + LAMBDA-NL), **not**
   via IF. The IF combination is float-only and AR-excluded (`integer_capable = false`,
   `MeasurementModel.cpp:1922`). No Standard-PPP IF→(WL,NL) integer path exists ⇒ the
   satellite-pivot / SSR-phase-bias machinery is the **only** route to integers.

## §F5 The compatibility audit — what AR silently trusts, and the confirmed gaps

For AR to be integer-valid the consumed satellite phase bias must (i) be generated under the same
per-satellite pivot ambiguity datum the user differences against, (ii) use a wind-up model the user
reproduces, (iii) match the IF clock's code-bias convention, (iv) match the assumed sign.

| Requirement | Enforcement |
|---|---|
| (iii) IF code-bias convention | **ACTIVE** — GF-IF alignment to the BIA reference pair (`StateInitializer.cpp:103`). *Risk:* if the clock product and the BIA OSB come from different providers with different IF reference pairs, alignment uses the BIA pair; **no cross-check of clock-product provenance**. |
| (i) Phase-bias ambiguity datum / pivot | **NEGATIVE only** — mismatch ⇒ failed validation ⇒ no fix. No positive check. |
| (ii) Wind-up / yaw model | **⚠ CONFIRMED GAP** — SSR `yaw_angle_deg` / `yaw_rate_deg_per_s` are *decoded* (`RtcmSsrDecoder.cpp:333`) but **read NOWHERE**; `computePhaseWindUp` derives its own yaw-steering attitude from sun/velocity geometry (`GnssCorrections.cpp:1426`). Consistency rests on both sides independently computing the same yaw — a known **eclipse / noon-manoeuvre AR hazard** and an S-basis-adjacent convention a network must eventually pin. |
| (iv) Sign | **EMPIRICALLY PINNED** to one provider (`kSsrPhaseBiasSign = kSsrCodeBiasSign = +1.0`, `MeasurementModel.cpp:1157`). **Onboarding a second SSR provider is a live hazard.** |
| Discontinuity reset | **ACTIVE** — `noteSsrDiscontinuity` forces `resetAmbiguity` on a bias jump (`RealTimeEngine.cpp:1082`). |

Net: a convention mismatch degrades to *no fix*, never a *wrong fix* — acceptable, but **the engine
is not self-describing about the S-basis it assumes.**

## §F7 Implications for the NETWORK side — the design charter

*(The section `src/core/app/ApplicationNetwork.cpp:15` and
`src/core/app/ApplicationNetworkSetup.cpp:27` cite as "§7".)*

Goal in S-system terms: estimate, from multiple stations, the estimable **satellite clock** (Eq. 2)
and estimable **satellite phase bias** (Eq. 4, carrying the pivot-receiver ambiguity lump) under a
**DECLARED S-basis**, and broadcast them so a user reproducing that S-basis recovers integers.
Orbits from the IGS stream are sufficient.

**Assets the architecture already provides (genuine reuse, not a rewrite):** (1) the measurement
model is **undifferenced + uncombined at its base** (`buildJacobianRow`), with differencing applied
as a *post-assembly* S-transform (`BssdTransform::apply`) — a network estimator runs on exactly
that substrate, it must simply *not* apply the receiver-side differencing and instead estimate the
satellite-side lumps on the undifferenced rows; (2) `StateManager`'s dynamic key→index blocks
already support per-(sat,signal) bias states (`makeSatBiasKey`, `sat_bias_indices_`), and the
*fallback* satellite code-bias state (fires only on missing product coverage,
`MeasurementModel.cpp:1128`) is structurally **the network's satellite-code-bias parameter in
embryo** — promote it from fallback to first-class; (3) the GF-IF decomposition
(`StateInitializer.cpp:103`) is the same IF/DCB algebra the network clock product must carry on the
satellite side — share one bias algebra across network and user.

**What was genuinely missing (three pieces) — and its OUTCOME.** *(2026-08 status: **all three are
BUILT and in production.** Kept as the charter because the code cites it.)*

1. **A multi-station joint estimator.** `BaselineMulti` is a *star topology* — N rovers each
   independently DD'd against one fixed reference, **no inter-rover correlation, no joint
   satellite-parameter estimation** (`GnssConfigTypes.hpp:84`). *Structural reason a network is
   required:* a single-receiver BSSD pipeline **CANNOT** estimate satellite phase biases —
   between-satellite SD on one receiver removes the receiver datum but leaves the satellite phase
   bias confounded with that station's pivot ambiguity. You need multiple stations (so the
   satellite-indexed bias is observed across receivers) **and** you must stop differencing the
   receiver side away. ✅ **BUILT** — `NetworkEstimator` / `NetworkNormalEquations`.
2. **An explicit satellite-PHASE-bias state** — existed nowhere in 2026-06. ✅ **BUILT** —
   `StateManager::getSatPhaseBiasStates`, PB keys in `StateKeySchema`.
3. **An explicit minimum-constraint S-basis** — the one S-system mechanism the codebase had *never*
   used. ✅ **BUILT** — `NetworkSbasis`, with a **runtime rank assertion** (§A rank bookkeeping).
   The **product emit path** is `NetworkProductEmitter` / `NetworkProductIO` with a declared **S
   line** (§D), and **network-scale pivot handover** machinery plus a persistent `last_pivot` map
   are live.

**The concrete path (as charted, and largely executed):** build the network estimator on the
existing undifferenced+uncombined `buildJacobianRow`, with satellite-side lumps as **explicit
estimable states** and an **explicit minimum-constraint S-basis** — for the network, deliberately
do the thing the user side avoids (add satellite phase-bias states, promote the satellite code-bias
state, add per-station receiver clock/bias/ambiguity states across a *stacked multi-station* normal
matrix). Apply Odijk's explicit S-basis as minimum constraints (fix reference-receiver
clock/biases, fix pivot-satellite-and-reference-receiver ambiguities, fix the ISB datum); doing
this on the network side does **not** perturb the (correct, differencing-based) user side.
**★ STRONGEST LEVERAGE — adopt the IGS-SSR clock+phase-bias S-basis as the broadcast convention**
so products are drop-in for the existing user consumer: the user side already applies SSR phase
bias and resets on `discontinuity_counter`, so if the network emits in that same convention/datum
the existing `SsrCorrectionStore` path is the validation harness, and **the network's correctness
criterion becomes operational and closed-loop: *"does our own user engine recover integers from our
own products on an independent station?"*** Reuse the GF-IF decomposition for the satellite-side
IF/DCB split so network and user share one algebra. `PivotManager` is reusable as the network's
satellite-datum selector, **but the network needs a SINGLE GLOBAL pivot per constellation, stable
over the broadcast interval** — not the user's per-receiver sticky-hysteresis pivot. The handover
S-transform (`applyCovarianceReReference`) is the right tool but must be applied network-wide and
reflected in the product datum.

## §F8 Honest divergences / risks (as of 2026-06-15)

1. **No explicit S-basis / minimum-constraint object** — legitimate for a user engine, but blocks
   self-description and is the structural prerequisite for §F7. *(Closed on the network side; still
   true of the user side.)*
2. **Phase-bias S-basis compatibility enforced only negatively** — no positive cross-check; the
   sign is pinned to one provider. **Second-provider onboarding is a live hazard.**
3. **SSR yaw fields decoded but unused** (`RtcmSsrDecoder.cpp:333` → read nowhere;
   `computePhaseWindUp` computes attitude independently). Eclipse-season AR hazard; wind-up is part
   of the phase datum a network must pin.
4. **Production AR is WL-only** (`wl_only: true`) — "integer AR" today is WL-fixed, not
   full-integer.
5. **No SSR-STEC iono-weighting path in real-time** — Odijk's fast-AR workhorse is GIM-only /
   post-proc.
6. **GPS-hardcoded ISB datum + GLONASS exclusion** — theory-faithful for a user engine; a
   multi-GNSS *network* product must decide and *declare* ISB/IFB conventions per constellation
   explicitly.
7. **§8.7 — frozen-pivot hard-zero pin vs soft constraint.** Equivalent in exact arithmetic; **in
   finite precision the handover S-transform `P ← T·P·Tᵀ` is where datum-transfer error
   accumulates.** The DD path has PSD/symmetrize guards (`DdPivotManager.cpp:208`).

   > **⚠ ANSWERED 2026-08-15 by reading the code — the 2026-06 question is RULED, and the true
   > state is sharper than either reading.** (a) **The user-path half is ruled deliberately
   > unguarded, with rationale, at the call site**: `include/filter/CovarianceReReference.hpp:22-29`
   > states that "the single-receiver user BSSD path (`PivotManager`) deliberately runs UNGUARDED —
   > at user scale the groups are small (~8–12 states) and handovers are rare, so FP error in
   > T·P·Tᵀ does not accumulate", mirrored at `src/core/filter/PivotManager.cpp:371-374`. So there
   > is nothing to "confirm" on that side. (b) **The network-path half is a stated MUST with ZERO
   > call sites — an actionable defect, not an audit question.** The same header says the
   > minimum-constraint NETWORK producer "MUST call `enforcePsd` after the transform", and
   > `enforcePsd` was extracted into the shared `CovReReference` namespace precisely so the network
   > producer could reuse it — yet a repo-wide grep finds exactly one non-test call site,
   > `src/core/filter/DdPivotManager.cpp:221` (the DD path). **The network producer never calls it.**
   > (c) ✅ **FIXED 2026-08-15 (serial cleanup pass).** Both citations were written bare as
   > "conformance doc §8.7" (`include/filter/CovarianceReReference.hpp:22`,
   > `src/core/filter/PivotManager.cpp:373`) and resolved only into `docs/attic/`; both now read
   > `docs/SPEC_sbasis_realization_contract.md §F8.7`. Comment text only — no semantics touched.
   > **(b) remains OPEN: the network producer still never calls `enforcePsd`.**

## §F9 The backlog, with its 2026-08 outcome

**P0 — unblock real-time PPP-RTK quality (user side) — BOTH STILL OPEN:**

- [ ] **SSR-STEC iono-weighting path.** `SsrCorrectionStore` carries orbit/clock/bias only and the
      SSR-to-STEC adapter is **explicitly unbuilt** (`RealTimeEngine.cpp:176`), so Odijk's
      *workhorse* weighted-iono model — the thing that enables fast/instantaneous AR — is
      **GIM-only in post-processing and entirely absent in real-time, directly capping real-time
      time-to-fix**. Add a STEC/iono push to `SsrCorrectionStore` and a real-time adapter feeding
      the existing weighted-iono pseudo-obs mechanism (`applyGimIonoUpdate` is the post-proc
      template). (§F4)
- [ ] **Decide WL+NL in production**, or document explicitly that production AR is WL-only by
      design. (§F4)

**P1 — S-basis hardening (makes the user side self-describing) — ALL STILL OPEN:**

- [ ] **Consume the SSR yaw fields in `computePhaseWindUp`**, or document that independent yaw
      modelling is the intended contract and gate AR through eclipse seasons accordingly.
      (§F5, §F8.3)
- [ ] **Positive S-basis cross-check at product ingestion**: verify the phase-bias product's pivot /
      ambiguity-datum / sign convention against the engine's assumptions; warn loudly on mismatch
      instead of relying on silent AR-validation failure. (§F5)
- [ ] **Cross-check clock-product provenance against the BIA IF reference pair** to catch
      mixed-provider IF-datum inconsistencies. (§F5 (iii))

**P2 — the network side. ✅ ALL FIVE BUILT AND IN PRODUCTION** (see §F7's outcome, and §A–§D for
what they actually realize). The five as the absorbed charter listed them, with their §F7 anchors:
explicit satellite phase-bias state — new `StateKeySchema` key + `StateManager` block mirroring
`sat_bias_indices_`, promoting the fallback satellite code-bias state to first-class (§7.2);
multi-station joint estimator — replace the `BaselineMulti` star topology with a stacked
multi-station normal matrix sharing satellite-side unknowns, estimating on
undifferenced+uncombined rows without receiver-side differencing (§7.1); explicit
minimum-constraint S-basis machinery — reference receiver, per-constellation global pivot
satellite, clock datum, ambiguity datum, ISB datum, and *record* the S-basis in the emitted product
(§7.3); the product emit path in IGS-SSR clock+phase-bias convention, wiring the existing user
consumer as a closed-loop integer-recovery test on a held-out station (§7); network-scale pivot
handover — a global per-constellation pivot stable over the broadcast interval (§7, §8.7).
**Do not read the P2 list as open work** — with one carried residue: that last item also asked to
"confirm `applyCovarianceReReference` conditioning matches the DD path's PSD/symmetrize guards",
and §F8.7 above records what that confirmation actually found (the network producer never calls
`enforcePsd`).
