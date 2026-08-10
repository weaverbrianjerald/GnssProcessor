# SPEC — datum graph census (structural S-basis dimension, predicted not detected)

**Status**: SPEC'd 2026-08-04. Build in a fresh session.
**Origin**: NGS LASER Guide v2.7 slides 5-62/5-63 (PDF pp. 374–375): rank deficiency of
a fragmented network is **d·k over k disjoint components** ("29 independent networks ⇒
3×29 = 87"), and — the operative sentence — *"knowing the type of observations involved
could indicate the rank of A, avoiding the need to compute it."* Slide 1-4: LASER
handles disjoint networks automatically and warns about weakly-linked ones. Graph
machinery per the Deo (1974) reference.
**Memory context**: `project_network_ssystem_dia_assessment` (the Aug-2 SBC record —
REQUIRED reading: the GPS+Gal extras are per-satellite channel-family PB↔iono trades
tied to station-set splits, INTERMITTENT 0–3/epoch on 41 % of epochs, and the SBC+KP
recipe), `project_laser_theory_mapping`, `project_ft63_fast_tier`.
**Scope discipline**: default-OFF, OFF byte-identical, MEASURE-ONLY (predicts and
reconciles; never feeds the gate), fingerprint-SAFE.

---

## 1. Why — the dimension is currently detected spectrally, never explained

`Plan::declared_dimension` is structural-static — #constellations in PIN mode
(`include/filter/NetworkSbasis.hpp:193`), 0 under sum-to-zero — and the realized
dimension is measured **spectrally**: the null-space gate counts near-zero eigenvalues,
and SBC (`NetworkSbasis::completeSbasisDimension`, `:653`) derives the epoch's extras
from the same reduced normals. That machinery answers *how many*; nothing answers
*why*, and the Aug-2 incident showed the why is **graph topology**: Galileo C/Q-vs-X
tracking splits the station set per channel family, and a family whose code support
thins loses the b↔I anchor for its (sat, σ) phase-bias columns. LASER 5-63 is the
theorem form: fragmentation ⇒ predictable rank deficiency from the support structure
alone.

A structural census that PREDICTS the dimension from the epoch's registration sets and
reconciles against the spectral observation gives three things the spectral path
cannot: early warning in plain identities (which family, which stations, which sat),
an invariant-#4 `predicted-vs-observed` contingency that turns "the gate threw" into a
diagnosis, and — on the SBC+KP recipe where spectral extras are suppressed — a live map
of **where `cb_kaula_prior` is load-bearing** (predicted-but-lifted), which is exactly
the population the KP K2-margin WARN worries about without naming.

## 2. The two tiers

**Tier 1 — component multiplicity (LASER 5-63 verbatim).** Per constellation, union-find
over the station–satellite bipartite graph of ADMITTED observation support (edges =
this epoch's admitted (station, sat) rows). k components ⇒ each datum gauge of that
constellation is needed k times; the declared plan assumes k = 1, so **k > 1 predicts
(k−1)·d silent extra deficiencies** — the classic disjoint-network case (regional
outage, admission-screen mass action). Expected to read k = 1 essentially always;
the census exists so the day it doesn't is not silent.

**Tier 2 — channel-family code support (the Aug-2 mechanism).** Per constellation, per
satellite, partition the epoch's registered keys by receiver channel family (the
signal-code attribution that split the stations on Aug-2: X vs C/Q). For each
(sat, family) with **registered PB columns but zero registered code keys** in that
family (`registered_code_keys` / `shared_phase_keys`, built at
`src/core/app/ApplicationNetwork.cpp:1645-1650` and in scope for the whole epoch),
predict ONE PB↔iono trade direction. Topology predicts the **zero-support** subset;
thin-but-nonzero support (the Aug-2 "orphan" CB+CLK class, ev ≈ 1e-6) is spectral-only
by construction, and the census must not claim it — that asymmetry is part of the
measurement, not a defect (§4 INC-1).

## 3. Design

### INC-0: the seam

`include/filter/NetworkDatumGraph.hpp` (header-only accumulator: union-find, per-tier
prediction lists with identities) + `src/core/filter/NetworkDatumGraph.cpp` (emission +
run-end; manual `CMakeLists.txt` entry). Called once per epoch from the driver in the
FRAMEDIAG neighborhood, after registration is final and after the solve — inputs are
the registration sets, the admitted-row support, and the epoch's spectral observation.
O(rows·α(n)) union-find + O(registered keys) partition; **budget ≤ 0.5 ms/epoch at 63
stations**, no stride.

### INC-1: the reconciliation (the deliverable)

Per epoch, join predictions to the spectral observation — the null-space gate's
observed count and, when SBC is armed, the engage attributions (`sbc_engage` per-SAT
identities; SBC results surface in `NetworkSolveGlue.cpp` — if only counters exist
today, widen the SBC result to a small per-epoch struct passed to the seam, bounded,
armed-only). Three cells, each with identities:

- `predicted ∧ observed` — the census explains the extra (the Aug-2 trade class);
- `predicted only` — with KP armed: **KP-load-bearing** (the direction exists
  structurally, the prior lifts it above the cutoff) — the KP margin map. Without KP:
  over-prediction, a finding about the predictor;
- `observed only` — weak-support / spectral-only (the orphan class) — the honest
  boundary of a topological predictor.

**The census never feeds the gate, SBC, or the plan.** `declared_dimension` cannot
move (`NetworkSbasis.hpp:515` states why); acting on predictions is the successor card
(§8).

### INC-2: outputs

`network_datum_graph.csv` (per epoch per constellation: k, n_stations in each
component ≤ 4 listed + count, tier-2 predictions with (sat, family, station-count),
the three contingency cells); `DATUMGRAPH` events, rate-limited like PREFIT, on: k > 1,
any new `predicted∧observed` or `observed only` identity, and the first
`predicted only` per (sat, family) per era; `dg_*` counters in
`NetworkServiceCounters` reported by `NetworkTeardown` — a run must distinguish
"armed, k = 1 everywhere, zero predictions" from "never ran" (invariant #4).

## 4. Config

| knob | type | default | note |
|---|---|---|---|
| `network.datum_graph_census` | bool | `false` | master arm |
| `network.datum_graph_event_holdoff_ep` | int | `20` | event rate limit (PREFIT/MDB convention) |

Loader: next to the SBC block (`GnssConfigLoadNetwork.cpp:4892-4904`). WARN when armed
under sum-to-zero clock datum (tier-1's clock-gauge multiplicity statement changes
meaning; census still runs, the WARN names the reading). Fingerprint-SAFE.

## 5. Unit tests

New file `tests/test_datum_graph_census.cpp`, one suite.

1. Tier-1: a hand-built two-component station–sat graph reads k = 2 and predicts
   (k−1)·d; merging one bridging edge collapses it to k = 1.
2. Tier-2: an E03-style toy — family X stations lose their code keys while X-family PB
   keys stay registered — predicts exactly one direction with the right (sat, family)
   identity; restoring one code key clears it.
3. The orphan boundary: thin-but-nonzero code support predicts NOTHING (the census
   must under-claim, never over-claim).
4. Contingency accounting: the three cells partition every prediction/observation;
   nothing double-counted; `predicted only` labelled KP-lifted iff the KP knob is on.
5. OFF byte-identity (structural).

## 6. Field validation

1. OFF byte-identity; live wiring; cost within budget (invariants #4/#6).
2. **GPS-only control** (FT63 24 h class): k = 1 every epoch, zero tier-2 predictions,
   zero observed extras — and the run-end line proves "ran, saw nothing".
3. **★ THE KNOWN ANSWER — GPS+Gal WITHOUT KP** (the `ft63_gg12h` baseline class,
   SBC armed, `cb_kaula_prior` off): the spectral extras population (0–3/epoch, ~41 %
   of epochs, X/Q-family attributions) must land in `predicted ∧ observed` for the
   trade class, with the orphan class appearing as `observed only`. Coverage of the
   trade class is the headline number; the Aug-2 attribution says it should be high,
   and a low number falsifies the family-partition definition, not the card.
4. **The KP map** (SBC + KP recipe lineage): spectral extras ≈ 0 by design;
   `predicted only` becomes the KP-load-bearing map — report its (sat, family) roster
   and stability across the day next to KP's K2 margin.
5. Solve byte-identical in every arm; no product claims (nothing to grade — the census
   touches no estimate).

## 7. Cost

Union-find + set partitions over already-built epoch containers. If it measures above
2 ms/epoch the implementation is wrong — say so.

## 8. Deferral triage (invariant #3)

| deferred | field-blocking? | evidence |
|---|---|---|
| Feeding the prediction to the gate / SBC / plan (per-epoch declared dimension) | **INERT** — measure-only; the successor is solve-affecting and needs §6.3 coverage data first | — |
| Weak-support (near-zero) structural scoring | **INERT** — needs weights, not topology; the `observed only` cell measures exactly how much this omission costs | Aug-2 orphan class |
| Tier-1 on the DD/tie graph (baseline-map components) | **INERT** — `trace_topology` already ledgers tie-row edges; duplicate coverage in v1 would blur ownership | invariant #7 ledger |

## 9. Standing-invariant pre-flight

1. No response added/removed — measure-only. 2. Release-only field runs. 3. Deferrals
above. 4. `dg_*` counters + contingency cells are the telemetry; "k = 1, zero
predictions" is a distinct, printed outcome. 5. Nothing graded on the wire — no
estimate is touched. 6. Default-OFF byte-identical; same-build A/B. 7. No coupling
change; the census READS the topology invariant #7 instruments, it does not act on it.

## 10. What this card explicitly does NOT do

- It does not change `declared_dimension`, the null-space gate, SBC, PA, or KP.
- It does not claim to predict the weak-support class (§2, §8).
- It does not replace `trace_topology` — that ledger tracks tie-row coupling edges;
  this census tracks datum-rank structure. Different invariants, different files.

---

## 11. Build record (2026-08-04)

Implemented INC-0..2 + §5. Default-OFF, structurally byte-identical when unarmed
(the seam is not entered, no file is opened, no counter moves). **Not field-run —
every §6 gate is owed.**

### Files

| file | role |
|---|---|
| `include/filter/NetworkDatumGraph.hpp` | value types + the four testable pure functions + the seam decl |
| `src/core/filter/NetworkDatumGraph.cpp` | union-find, family partition, reconciliation, CSV, events |
| `tests/test_datum_graph_census.cpp` | one suite, 21 cases |
| `NetworkRunContext.{hpp,cpp}` | `dg_on` / `dg_event_holdoff` knobs; `dg_spec` hand-off + holdoff clock + per-era roster; `dg` tallies |
| `NetworkRunTraces.{hpp,cpp}` | `network_datum_graph.csv` header/stream/open/close |
| `NetworkSolveGlue.cpp` | the two spectral capture sites (SBC pre-completion; the gate fallback) |
| `ApplicationNetwork.cpp` | top-of-epoch `dg_spec` clear + the SB_FRAMEDIAG call |
| `NetworkTeardown.cpp` | run-end line + four wiring warnings |
| `GnssConfigTypes.hpp`, `GnssConfigLoadNetwork.cpp` | the two knobs, loaded next to SBC |

### Four deviations from the spec, all deliberate

1. **The support source is `modeled`, not the assembled row stacks.** §3 asks for
   "the admitted-row support". `classifyRow` scans every shared column of every
   row against a column-major `A_s`, and the prefit census measures **7.6
   ms/epoch at 63 stations** doing exactly that — 15× this card's 0.5 ms budget,
   for a graph that needs no residual, weight or Jacobian. The census instead
   walks `modeled[station]`, whose `observed` map is keyed by full RINEX-3 code
   and from which the admission screen has already removed every rejected
   observable: ~1300 map entries vs ~7500 rows × ~950 columns. Edges are gated on
   the station having FOLDED (`est_index_of >= 0`) and on the observable
   appearing in both `observed` and `modeled`. Residual gap (an observable that
   still fails to produce a row inside `computeMeasurementUpdate`) is named, not
   papered over.

2. **A second, weaker tier-2 predicate is measured alongside the spec's.** §2
   defines tier 2 on registered code **KEYS**. On the Aug-2 lineage the deficient
   directions carried live CB components (`PB:E03:L5X:0.35 CB:E03:C5X:0.35 …`) —
   the columns existed and it was their ROW SUPPORT that thinned — so the strict
   key predicate can be empty on exactly the run §6.3 grades. §6.3 anticipates
   that ("a low number falsifies the family-partition definition, not the card").
   Rather than run the gate blind, both are measured in one pass from the same
   partition: `zero_keys` (the spec's, and the ONLY one feeding the contingency,
   the events and the headline coverage) and `zero_support` (registered columns
   with zero admitted support — its own columns, own counters, own contingency,
   never mixed in). Support counts only observables backing a registered column,
   which makes `zero_keys ⇒ zero_support` exact.

3. **Both predicates require a code-bias LAYER.** With
   `network.estimate_sat_code_bias` off there are no code columns at all, the
   code observations pin slant iono directly, the trade direction does not exist
   — and the literal predicate would fire on **every cell in the run**. Held off
   per CONSTELLATION and counted (`cells_no_cb_layer`), with a loader WARN. This
   is a defect in the predicate as literally specified, found while implementing;
   the guard is the honest reading, not a softening.

4. **Two contingency partitions, not one 2×2.** The join is many-to-many (one
   direction spans several cells; one cell appears in several directions), so
   `pred_observed + pred_only` counts PREDICTIONS and `obs_only + n_obs_matched`
   counts DIRECTIONS. Collapsing them would require double-counting one side.

### Two limits a reader must know

- **A gate WITHHOLD skips the epoch before `SB_FRAMEDIAG`**, so the epochs whose
  datum actually failed are precisely the ones with no census row. The teardown
  prints `dg.epochs` against `n_gate_evaluated` and says so. On the §6.3 gate
  (SBC armed) the gate passes, so this does not bite there; on an SBC **refusal**
  it does.
- **Direction identities are SBC's top-six participants**, parsed from the same
  `detail` string its `sbc_engage` / `sbc_skip` events carry (so census and event
  agree by construction). A widely spread direction is identified by its heaviest
  six. Without SBC the reconciliation is COUNT-level and the trace says so
  (`obs_src=count`); tier 1 needs no spectrum at all, which is why `dg_on` is
  deliberately **not** AND-ed with `sbasis_complete`.

### Owed

Every §6 gate: OFF byte-identity A/B, the GPS-only control (`k = 1` all day, the
"ran, saw nothing" line), **the §6.3 known-answer coverage run** (`ft63_gg12h`
class, SBC armed, KP off — and it is the arm that decides between the two tier-2
predicates), the §6.4 KP map, and the §7 cost measurement against the 0.5 ms
budget (the teardown warns above 2.0 ms on its own).
