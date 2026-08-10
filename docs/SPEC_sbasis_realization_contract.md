# The producer's S-basis realization contract (Odijk-2016 notation)

2026-08-09.  Companion to `docs/SPEC_clock_emit_rebase.md` §7 and
`docs/ssystem_theory_reference_2026-07-09.md`.  Purpose: state, in the notation
of Odijk et al. (2016, J Geod 90:15–44), (A) which S-basis the network producer
actually realizes (code-verified this date, not folklore), (B) what the emitted
wire records therefore MEAN, and (C) the **realization-transfer contract** — how
each per-epoch re-occurring rank deficiency is held to ONE realization over
time, which is the part Odijk's single-basis tables do not cover and where every
measured wire disease has lived.

Model class: the paper's Eq. (12) — undifferenced, uncombined, multi-frequency
phase+code, slant per-LOS ionosphere (+ SH-VTEC field constraint), CORS geometry
(station coordinates fixed, ν = 1: ZTD only ⇒ the regional type-0 deficiency is
absorbed by construction).  One deliberate deviation from the paper's notation:
our basis is per-(constellation, **signal**), not per-frequency j — channel
families (C/Q/X…) are first-class, which is what made the GPS+Gal SBC
completion necessary (see §A row 6).

## §A The realized basis — our analogue of Odijk Table 2

| Odijk rank deficiency | our constraint (the S-basis choice) | code anchor | count | S-line token |
|---|---|---|---|---|
| 1a — rx ↔ sat clocks (per constellation; paper size 1 via pivot rx or sat mean) | HARD pin of ONE satellite clock per constellation, target 0.0, **sticky** (RTA-1 keep-incumbent); receiver clocks/ISBs stay free local states.  A per-constellation member of the CC family — pivot-**satellite**, not CC-R's pivot receiver nor CC-S's mean.  (Alternative realization `clock_datum_sum_to_zero`: validated, does NOT graduate — F13.) | `NetworkSbasis.cpp:159–188` (`plan.clock_pivot`, Pin target 0.0; declared_dimension = #constellations) | 1 × #const | `<c>:<pivot>` per const |
| 1b/3a — sat hardware biases ↔ clocks (paper: IF sat code biases in the basis) | per-satellite **IF pin**: the two lowest distinct-frequency code signals, a₁+a₂ = 1 (the μ_IF combination enters the clock, IGS-compatible per the paper §4.1) | `NetworkSbasis.cpp:484–557` (`selectIfPinPair`) | m | `cbpin=` (consumer-pair trap: the pinned pair is lexical, NOT necessarily the consumer pair — `project_day129_pivot_cb_ratchet`) |
| 1b/3a remainder — per-(const,signal) code-bias gauge | **sum-to-zero over satellites** per (const,signal) channel — CC-S/Schaer-class zero-mean, the IGS DCB convention the paper names; TIME-LINKED under Card 2 (rows pin the CHANGE of the previous epoch's solved values ⇒ the realized lump stays the first-epoch one — the paper's "(1)" epoch index, implemented) | `NetworkSbasis.cpp:462–671` (`buildCodeBiasDatumRows`); Card 2 = `code_datum_time_linked` | per channel | `emit_cb`, `cb_datum` |
| 2a/2b — rx clocks/biases ↔ rx hardware biases | receiver clocks **white** (q = 1000 — the paper's own "usually unconnected" default); rx code biases: **zero-mean-over-receivers** datum (RXCB-DATUM) + loose physical bound | `rx_code_bias_datum` (+`_sigma_m`) | per (const,sig) | ref_rx field |
| 2b/3b — slant iono ↔ GF code biases (paper Table 7/8) | NOT separately constrained — the GF direction lumps into the iono exactly as Table 8 states; the SH-VTEC field + optional a₀₀ anchor is a soft constraint on the field, not a GF datum row | slant per-LOS states + `global_vtec_sh` | — | (none — see §B row 4) |
| 4/5 — phase biases ↔ ambiguities | **eq34 spanning-tree** datum (de Jonge 1998, cited by the paper for exactly this): HARD pins w = 1e10, target **0.0**, on the datum receiver's / tree ambiguities per (sat,signal) component; sticky/longevity/slip-aware election variants; `AmbDatum::PivotReceiverZeroAmbiguity` | `ApplicationNetworkAssembly.cpp:2219–2365` (pin_w 1e10, pin-to-ZERO — note: any AR design must use this, not the "float value" some older docs claim) | f(n−1)+fm class | `amb_datum`, `pdr=` |
| GPS+Gal channel-family extras (not in the paper — a consequence of per-SIGNAL bases) | per-epoch **SBC completion**: one minimum-constraint row per realized extra direction (PB↔iono channel-family trades), RHS = carried level; + KP loose CB prior | `NetworkSbasis::completeSbasisDimension` (`sbasis_complete`), `buildCodeBiasKaulaPriors` | 0–3/epoch | — |

Rank bookkeeping: `declared_dimension` + the null-space gate (tol-gated
eigencount, product withheld on mismatch) is the runtime assert of the paper's
Eq. (24) count — a stronger minimum-constraint verification than published
practice (Jul-1 audit verdict, re-verified through the SBC work).

## §B What the wire records mean — our analogue of Odijk Table 3

Under §A, the consumer should read (tilde = estimable function, not the
physical parameter; "datum(1)" = the held first-realization values under §C):

1. **C record (clock)**: dt̃ˢ(i) = [dtˢ(i) + d_IFˢ(datum)] − [dt_pivˢ-chain(datum)]
   − A_c — the IF-lumped satellite clock relative to the constellation's pivot
   realization, expressed in the **held emission frame** (A_c = the CLK-R
   accumulator; common to all sats of the constellation, absorbed by the user's
   per-epoch receiver clock).  It is a CORRECTION to the a-priori product named
   in the S line (`apriori_id`).
2. **P record (phase bias)**: the eq34-datum'd, emission-rebased per-(sat,signal)
   phase bias; the network datum receiver's ambiguity rides inside it, which is
   what makes the user's between-satellite DD ambiguities integer (paper §6.2)
   — proven end-to-end by the Q4 R-record experiment.
3. **B record (code bias)**: the per-(const,signal) zero-mean, IF-pin-referenced
   satellite code bias (≈ scaled DCBs, paper Eq. 28–33; j ≥ 3 signals estimable
   as "modernized DCBs" — our C2X/C5Q/C5X rows are exactly that class).
4. **Ionosphere (in-process supply / diagnostics)**: ι̃ˢ(i) = ιˢ(i) +
   d̄_r,GF(datum) − d_GFˢ(datum) — Table 8 with our datum: the receiver term is
   the **fleet-mean GF receiver code bias** (zero-mean-over-receivers basis).
   Measured: −5 ns fleet mean = the DEFECT-2 level offset
   (`project_extcb_defect2_rootcause`).  Estimable-but-biased BY CONSTRUCTION;
   the bias is only as steady as the fleet realization — which is why level
   tracking needs an external authority (item 0b), not another datum.

## §C The realization-transfer contract (the part Odijk's tables assume away)

The paper's estimable functions are defined w.r.t. ONE basis realization; its
§5.3/Fig. 3 shows which deficiencies re-occur per epoch when temporal
constraints are absent, and its Zhong-2015 discussion warns that solutions from
a CHANGED basis (e.g. a different zero-mean set) cannot be compared without the
connecting S-transformation.  Our q posture (rx clock white; sat clock q = 1e-3
≈ unconstrained at code-information level; sat CB q = 1e-10 stiff; sat PB
q = 1e-12 stiff + invariant DD carry) puts us on the Fig.-3 path where the
clock-gauge class re-realizes freely while the bias gauges are carried.  The
contract, per channel — every re-occurring gauge must be HELD to one realization
on the wire, and every genuine realization change must be MARKED:

| channel | re-realization cause | estimator-side continuity | emission-side S-transform (hold) | mark on genuine change |
|---|---|---|---|---|
| clock common mode (per const) | free per-epoch gauge (Eq. 41 class) + pivot handover | none (by q posture) | **CLK-R stage 1** `emit_clock_frame_hold` — census 613 → 1 | `E.datum_changed` (+leash re-anchor marked) |
| phase-bias gauge (per component) | eq34 datum re-realization (common fractional + per-sat integer·λ) | invariant DD carry | **NetworkEmitRebase** (+ PB emit filter for noise) | λ-scaled jump test → P disc / R |
| sat code-bias gauge (per channel) | membership change of the zero-mean set (the Zhong-2015 class), band-set change of the IF pin | **Card 2** time-linked datum (pins the change) | (measured unnecessary: B-channel census 0–3 common epochs/12 h, all at the marked handover) | structural set-change disc + `code_bias_disc_jump_test` |
| rx code-bias datum | present-receiver-set change of the zero-mean row | rx-CB carry (q = 1e-9) | S2H harvest de-mean (validated EXT; golden A/B pending) | (edge 3 — carried by the level campaign) |
| per-sat clock VALUE | — not a gauge: datum-invariant estimable content — | **missing** (Q3: the paper's assumed RW constraint, unimplemented at a defensible q) | falsified (SPEC_clock_emit_rebase §2 — the walk is not mediatable at emission) | `clock_disc_jump_test` (event-day only) |
| iono level ⊕ rx-DCB mean | fleet realization breathing | rx-CB carry memory (the ±6 TECU price) | not gauge at the consumer (elevation-mapped) — no hold is honest | — (item 0b authority is the fix) |

Two standing consequences:

- **A wire is interpretable only under a HELD realization plus marked events.**
  Post CLK-R, the internal product satisfies this for all three record channels:
  its discontinuity census and its mark set coincide (the 12 h survivor IS the
  marked handover).  Any new gauge-bearing channel must ship with its row in
  this table filled in — hold, or mark, or both; never neither (that was the
  clock channel's state for its first year).
- **What this contract cannot fix**: the last two rows.  The per-sat clock walk
  and the level authority are estimable-content problems (precision and
  anchoring of x̃ₛ), invariant under every S-transformation — Q3 and item 0b
  respectively own them.

## §D Verification status + known residues

- Code anchors in §A verified 2026-08-09.  The Jul-1 audit's header
  misdeclaration (GPS clock gauge) is FIXED in the current
  `include/filter/NetworkSbasis.hpp:17–25`; the pin-to-ZERO ambiguity targets
  stand (older producer-AR notes claiming "float value" targets are wrong).
- Vanish-return pivot signalling: covered by the A1 persistent last-pivot map
  under `network.pivot_vanish_return_signal` (merge-updated; datum_changed on a
  different-pivot return).
- The S line serializes the basis identity (`amb_datum`, `isb_datum`,
  `emit_cb`/`cb_datum`, realized pivots, `pdr=`, `discfrac=`, `apriori_id`,
  `cbpin=`, `hsig=`) — the consumer-side statement of §A.  Anything added to §A
  that a consumer must know to interpret the wire belongs on the S line too.
