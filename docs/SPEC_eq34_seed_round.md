# SPEC — eq34 SEED-ROUND: first-sighting pin-to-round (Card 3 ignition fix)

*Drafted 2026-08-12 (SPEC_synthetic_minimum_network §12.8, HANDOFF_wsh_chain_slant
T1/T2).  Status: **BUILT + VALIDATED same day** (`gnss_processor_sr.exe`
6B1EF5D6…).  V1 OFF byte-identity: PASS (pinaudit rung, estimability + vtec
traces hash-identical to the pre-card build).  V2 `_simnet300_wsh_n50_seedround`:
**P1 spectrum identical to 10 digits; P2 a₀₀ 33.1 TECU at epoch 1 vs −18 780
without the card — the SH collapse is GONE, 15/19 emitted (the 4 skips are
rank-gate dips of the marginal 1.4×-above-cutoff L2W direction — the graded
posture keeps the rung-4 tolerance); P3 pin audit COMPLETE (1558/1558/1558 both
signals); P4 max |y| 0.1193 m ≤ λ_L2W/2, 2 target steps.**  The EWL TRIO
(`_simnet60_tri_slant_trio`: posture C + eq34 + carry_invariant + mw_wl.ewl —
the §13.6-blocked arm): **19/19 emitted, three complete chains (1748 = 19×92
per signal), max |y| 0.1256 ≤ λ_L5Q/2, 0 steps, and the truth-graded DD
integer structure is CLEAN (EWL 94.1 % at its true integer in a 20-epoch
ignition window; no km-scale injection anywhere)** — the §12.8 artifact is
closed and the unguarded slant posture reads healthy on truth.  Owed: the §9
unit-test file (tests/test_eq34_seed_round.cpp); V3 settled graded arm.*

---

## §0 One paragraph of context

The Eq-34 single-receiver-chain phase datum (`network.phase_datum_eq34`, Card 3)
pins one spanning-tree ambiguity per tree edge plus one RPB per component —
(n_c + m_c) pins, and the §12.8 audit proves every one of them lands.  The pin
TARGETS are the problem: **absolute zero on a first sighting**, which realizes
the datum frame at "all tree ambiguities ≡ 0".  That is a legal gauge choice
whose price is proportional to the phase−code lump the seeded float carries.
Field receivers keep that lump at metres and the field cold-starts the posture
healthy at 303 stations.  The synthetic fixture draws N ∈ [10⁵, 10⁶) cycles per
(satellite, band, station) (`src/sim/main.cpp` `ambN` — deliberately, for the
MW/NL exercise), the lump is 19–250 km, and the cold pins discharge km-scale
geometry-free tension into the per-LOS iono states: a₀₀ −16 000…−33 000 TECU
from the first epoch, SH health gate withholds every epoch, and the phase-
leveling carry can never warm because warming needs emitted epochs — the
degrade-during-withhold deadlock, third sighting.  This card replaces the
constant-zero cold target with **λ·round(x/λ) of the value the state actually
holds**, bounding the injected tension at λ/2 per pin instead of λN.

RINEX imposes **no** phase−code alignment guarantee — the receiver convention
the zero target silently relies on is exactly that, a convention.  The fix is
therefore producer robustness, not a sim accommodation (correct-architecture
directive); the sim is merely the first receiver class ever run against the
producer that exercises the freedom the format allows.

## §1 The defect, measured (all 2026-08-12, n = 50 W6+SH fixture)

| arm | posture | result |
|---|---|---|
| `_simnet300_wsh_n50_pinaudit` | eq34 + pd_pin_aware, default tol | rank gate **PASSES** (n_below 1 = declared_dim; L2W common mode lifted 9.78e-06 → 1.67e-04) — then **19/19 `sh_gate_withheld`**, a₀₀ −18 780 (ep 1) → −32 633 TECU (ep 2) |
| `_simnet50_wsh_eq34` (rung 2, re-read) | eq34, degree 4 | same signature all along: 19/19 SH-gate, a₀₀ −15 965 at its first epoch, "0 withheld by the observability gate" in its own run-end line |
| `_simnet300_wsh_n50_eq34kaula` | + `cb_kaula_prior` σ 3 m | unchanged (a₀₀ −19k/−33k) — a code-side prior does not bound a phase-frame discharge |
| rung 3 `_simnet50_wsh_sbc` | `sbasis_complete` | the same failure class, smaller dose: constrain the family cold at a garbage level (a₀₀ −6 292) |

Scale check that ties the mechanism shut: a doubly-pinned edge (each band's tree
elects independently) imposes GF pseudo-iono ≈ (λ₁N₁ − λ₂N₂)/(μ₁ − μ₂).  With
N ~ 5·10⁵ that is tens of km of L1 delay; a₀₀ = −19 000 TECU ≈ −3.1 km of L1
delay over the network — the observed magnitude, and nothing else in the posture
produces kilometres.

## §2 The rule this card changes

Current target selection (`NetworkChainDatum.cpp:402-418`, inside `recordEdge`):

```
target = 0.0                                        // first sighting
if (eq34_round && anchor_incumbent.count(amb))      // incumbent only
    if local_carry alive && λ valid:
        target = λ·round(carry/λ)                   // re-pin continuity
```

Three holes, one card:

1. **First sighting → 0.**  Cold start (every arc), and every genuinely new arc
   later.  This is the ignition defect of §1.
2. **Incumbent-without-carry → 0.**  The carry lookup misses ⇒ target silently
   falls back to 0 — the RE-ZERO trap.  On the fixture this is every epoch ≥ 2
   of a withheld run (the leveling carry never stashed), so the damage
   re-injects forever; under `carry_invariant` it is unconditional (the
   per-station phase-key carry is superseded, `GnssConfigLoadNetwork.cpp:2825-2835`).
3. **`eq34_round` is dead under `carry_invariant`** (`NetworkChainDatum.cpp:128-129`)
   — a Card-4 decision about RE-PIN continuity ("the frame is re-realized
   freely") that accidentally also forces the IGNITION target to 0.  Rung 6's
   cold failure is this hole plus hole 1.

**New rule** (armed by the new knob, default OFF):

```
target =
  λ·round(carry/λ)      if eq34_round && incumbent && carry alive     (unchanged)
  λ·round(x_state/λ)    else if seed_round && the ambiguity state EXISTS
  fold-deferred round   else if seed_round                            (state not yet allocated)
  0                     else                                          (exact legacy)
```

where `x_state` is the station StateManager's current absolute float.  Rounding
(not holding at the raw float) is deliberate: Card 3's documented property is
that every between-satellite reference is an **integer combination** across the
component, which requires integer-valued pin targets in the realized frame.
The residual pin innovation is then |y| ≤ λ/2 (≤ 12.2 cm on L2W) instead of λN,
and the fractional part lands in the per-(sat,σ) phase bias — the state whose
job that is.

## §3 Design

### 3.1 Knob

```cpp
bool network_phase_datum_eq34_seed_round_{false};                    // GnssConfigTypes
bool networkPhaseDatumEq34SeedRound() const;                          // getter
network.phase_datum_eq34_seed_round                                   // JSON, network section helper
```

Loader rules: refuse (warn + force off) without `phase_datum_eq34`.  It does
**NOT** require `phase_datum_eq34_pin_round` and is **NOT** gated on
`!carry_invariant` — the Card-4 zero-target rationale is re-pin continuity,
which this knob does not touch (an incumbent WITH a live carry still takes the
carry-round or plain-0 path exactly as today; seed-round fires only where the
target would otherwise be the cold 0 of holes 1/2/3).

### 3.2 Election side (`NetworkChainDatum.cpp`, `recordEdge`)

When the rule selects the seed-round branch:

- **State exists** (`stations[ri].sm->getAmbiguityIndex(amb) ≥ 0` — persistent
  StateManagers, so true for every arc alive in a prior epoch): compute
  `target = λ·round(x/λ)` in place.  All downstream bookkeeping
  (`new_prev_target`, pinmove, the shift walk) works unchanged on the concrete
  value; a target that steps between epochs (float crossed a half-integer) is a
  same-station pinmove = target difference — the existing known-step machinery
  (`NetworkChainDatum.cpp:419-448`) and, when the step is unknown, the existing
  reref/erase hygiene.  λ from `eq34_ppp_utils` exactly as the carry-round
  branch does.
- **State does not exist yet** (cold start epoch 1; a genuinely new arc — the
  pre-pass inside `computeMeasurementUpdate` allocates and seeds during PASS B,
  after the election): the election cannot read a value that has not been
  seeded.  Store λ in a new per-station map
  `NetStation::pinned_amb_seed_lambda[amb] = λ` (cleared with the other pin maps
  at election start), keep `pinned_amb_targets[amb] = 0.0` as the provisional
  entry, and mark the pin's frame step **unknown** (`known = false`) in the
  shift walk — for a brand-new arc nothing carried depends on it, and at cold
  start there are no carries at all, so the conservative reref is free.  In
  steady state this branch is rare by construction: the INC-1 hygiene walk
  prefers mature hosts, and every deferred pin here is one the walk already
  counts as an unavoidable cold pin (`hyg.cold_fallback_pins`).

### 3.3 Fold side (`ApplicationNetworkAssembly.cpp`, `foldAmb`, currently :2256)

By fold time the state exists and is seeded (`piv_x` is read at :2245, after
`computeMeasurementUpdate`).  When `amb_key ∈ st.pinned_amb_seed_lambda`:

```cpp
const double lam    = st.pinned_amb_seed_lambda.at(amb_key);
const double target = lam * std::round(piv_x(amb_idx) / lam);
pin_y.push_back(target - piv_x(amb_idx));        // |y| ≤ λ/2 by construction
```

and record `(amb_key, target)` into a new `StationAssembly::pin_seed_targets`
vector (produce-phase local, one writer per slot — the same discipline as
`pin_audit`).  The weight path (1e10 hard pin, INC-3 σ ramp) is untouched.

### 3.4 Realized-target feedback (serial consume)

Without feedback, next epoch's election would read `eq34_prev_target = 0` for a
pin realized at 12 345·λ and compute a huge phantom pinmove every epoch —
permanently re-referencing the (sat,σ) and keeping the leveling carry cold.  So
the serial consume (the same walk that sums `pin_audit`) drains
`R.pin_seed_targets` of **accepted** stations into
`ctx.carry.eq34_prev_target[{i, amb}]`, overwriting the election's provisional
0.  Ordering is already correct: election → produce → serial consume → next
election.  A station dropped by `addStation` contributes no feedback — its pin
did not land, and next epoch re-elects from scratch for it anyway.

### 3.5 What is explicitly NOT changed

- Pin COUNT and placement (the §12.8 audit's 1558 = 19×82 stays the invariant).
- The W5 null-space gate, `declared_dimension`, and the estimability spectrum —
  eigenvalues depend on H and weights, never on targets/RHS.  (Pre-registered
  as P1 below; a moved spectrum means a second variable is loose.)
- RPB pins (δ_{r1,σ} = 0): the receiver phase bias is seeded near zero — no
  lump lives there.
- The legacy `-piv_x` expression for exact-0 targets (the OFF golden hash
  guard at `ApplicationNetworkAssembly.cpp:2262-2267`): seed-round targets are
  nonzero and take the general form; target-0 pins keep the byte-exact legacy
  expression.
- Emission grammar, S-line, product contract: frame re-realizations at target
  steps are the existing eq34 reref events; no new event class.

## §4 Interactions and edge cases

| case | behavior |
|---|---|
| slip on a pinned arc | inherited response, two layers: the slip-aware election (Card 3/4) refuses the arc as a tree edge where the graph allows, and a slipped-then-refloated state re-seeds ⇒ seed-round re-rounds from the NEW float.  No stale-target carry exists (targets are recomputed every election) — invariant #1 satisfied structurally |
| gap / extend-only (I2) | a retained edge resuming from a gap prefers the carry-round path (pre-gap integer) exactly as today; seed-round is its fallback when the carry died with the gap |
| hygiene INC-3 σ ramp | orthogonal and composing: a fresh-host pin can be BOTH σ-ramped and seed-rounded (soft row, near-zero innovation) |
| `carry_invariant` | seed-round applies (the fix IS rung 6's missing piece); re-pin targets stay Card-4 plain-0 semantics only in the sense that no CARRY-based continuity is consulted — in practice every epoch re-rounds from the current float, which is continuous because the state is |
| steered/drifting receivers (a lump that RAMPS) | each λ-crossing steps the target ⇒ one reref of that (sat,σ) — bounded, counted (§5), and the arm that suffers it is visible in the counter, not silent |
| GLONASS | λ per (system, signal, **sat**) via the existing `getWavelength` call — FCN-correct by construction; no per-slot special case |
| `station_pd_pin_aware` | unrelated to targets (pins' WEIGHTS unchanged); keep arming it with eq34 per the loader recommendation |

## §5 Observability (invariant #4 — the counters ARE the feature's proof)

Run-scope tallies in `NetworkServiceCounters` (logged by `NetworkTeardown`
beside the pin-audit block):

- `seed_round_pins` — pins that took the seed-round target (election-side and
  fold-deferred separately: `seed_round_deferred`);
- `seed_round_max_abs_y_m` — the largest |pin innovation| any seed-rounded pin
  folded.  **The card's health assertion: ≤ λ/2 always.**  A value above λ/2
  is a logic error, full stop;
- `seed_round_target_steps` — same-station target changes between epochs (the
  reref pressure this knob generates; expected ≈ 0 on static receivers).

An ARMED run with `seed_round_pins == 0` gets the standing inert-warning
treatment (the eq34-hygiene block is the template).

## §6 Standing-invariant pre-flight

1. **Slip/jump response for stiff quantities** — no new carried quantity; the
   targets are recomputed every election from living state, and the slip path
   re-seeds before re-rounding (§4 row 1).
2. **Release-only data runs** — validation rungs below are Release, renamed exe,
   one CWD per arm.
3. **Deferral triage** — the fold-deferred path is IN scope (it IS the cold
   start); nothing field-blocking is deferred because field arming is not
   proposed (§8).
4. **Run-end counters** — §5; the pin-audit counters additionally certify count
   invariance for free.
5. **Datum-invariant grading** — the graded comparison (§7 V3) reads the
   standard §12.7 channels, all contrasts/SD/truth-transformed.
6. **Default-OFF, structural byte-identity** — OFF ⇒ the new maps stay empty,
   the fold branch is untaken, targets bit-identical (V1).
7. **Information-flow edges** — seed-round gates no information; the one event
   it can generate (a target step) discharges through the EXISTING reref
   machinery, which marks both edges (carry erase at the election, shared-PB
   marginalization at consume).  No new bare edge.

## §7 Validation plan (pre-registered, in run order)

- **V1 — OFF byte-identity, two postures.**  (a) `_simnet300_wsh_n50_pinaudit`
  re-run on the new build, OFF: `network_estimability.csv`,
  `network_vtec_sh_health.csv`, run-summary hashes identical to the e76b3c1
  run.  (b) the non-eq34 `_simnet300_wsh_n50_pin_el25b`: `products.txt`
  byte-identical (same-build discipline; §12.9 showed this comparison resolves
  at four digits even across instrumentation).
- **V2 — the ignition rung.**  `_simnet300_wsh_n50_seedround` =
  pinaudit config + the knob.  Predictions:
  - **P1** estimability spectrum IDENTICAL to the audit rung (λmax ≈ 1.19e5,
    n_below 1, L2W direction ≈ 1.67e-04) — targets cannot move eigenvalues;
  - **P2** SH gate never engages; a₀₀ in band from epoch 1 and settling toward
    rung 4's ≈ 31.5 TECU; ≥ 18/19 emitted (A4 skips allowed);
  - **P3** pin audit still COMPLETE (1558/1558/1558 per signal);
  - **P4** `seed_round_max_abs_y_m` ≤ 0.123 m (λ_L2W/2), target steps ≈ 0.
  Any prediction failing ⇒ stop and attribute before widening.
- **V3 — the graded arm.**  480 epochs, eq34+seed_round at DEFAULT tolerance,
  settled half, against the rung-4-tolerance graded arm (same build): every
  §12.7 channel within noise; the interesting readout is the **P record** —
  the L2W SD frac with the common mode datum-pinned rather than free (no
  direction pre-registered; both outcomes inform the emit-side story).
- **V4 — the deadlock exit.**  On V2's arm, confirm the leveling carry warms
  (carry_warm reached, `local_carry` populated) — the §12.8 deadlock is broken
  at the root, not worked around.

## §8 Field posture and non-goals

- **Default OFF everywhere, and NOT proposed for the FT63/field lineages.**
  Field lumps are metres; the zero target is fine there, and the no-promotion
  directive applies — validated ≠ armed.  SCOPED ledger entry: "arm only on
  receivers/fixtures whose phase−code lumps are large (sim W6+SH arms; any
  future receiver whose `seed_round_max_abs_y_m` on a probe run says so)."
- **Non-goal: fixing the sim's N draws.**  The large draws are load-bearing
  didactics (MW/NL), changing them breaks every fixture's noise realization,
  and the producer would stay fragile to a receiver convention RINEX does not
  guarantee.  The producer is the right side of the seam for this fix.
- **Non-goal: hold-at-float (no rounding).**  Breaks Card 3's integer-
  combination property; rejected in §2.
- **Non-goal: any change to `sbasis_complete`.**  Same failure class, different
  knob — its cold-engage hazard is recorded in §12.8.4 and stays a separate
  card if ever needed.

## §9 Build manifest (when built — no new files, no CMake change)

| file | change |
|---|---|
| `include/config/GnssConfigTypes.hpp` + `GnssConfig` getter | the flag |
| `src/core/config/GnssConfigLoadNetwork.cpp` | parse + the refuse-without-eq34 rule + summary toggle line |
| `include/filter/NetworkRunTypes.hpp` | `NetStation::pinned_amb_seed_lambda` (with the other pin maps), `StationAssembly::pin_seed_targets` |
| `src/core/filter/NetworkChainDatum.cpp` | target-rule rewrite in `recordEdge` (§3.2), clear the new map at election start |
| `src/core/app/ApplicationNetworkAssembly.cpp` | fold branch (§3.3), serial-consume feedback (§3.4) |
| `include/app/NetworkRunContext.hpp` | the §5 counters |
| `src/core/filter/NetworkTeardown.cpp` | the §5 run-end block |
| `config/test_suite/_simnet300_wsh_n50_seedround.json` | V2 rung |

Estimated diff ≈ 120 lines + config.  Unit-test seam: a `NetworkChainDatum`
target-selection case per rule branch (state-exists round, deferred, carry
precedence, OFF-legacy) in a new `tests/test_eq34_seed_round.cpp` — new file,
new suite name, per the one-suite-one-file rule (CMake entry required for the
test file only).
