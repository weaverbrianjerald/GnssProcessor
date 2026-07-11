# CARD 3 — Eq-34 phase-bias datum: per-(receiver,signal) receiver phase-bias states + single-receiver / de Jonge chain datum

*Fable-specced 2026-07-10, from the Eq-34 gate result (memory
`project_eq34_ensemble_noninteger`, adversarially verified in
`project_eq34_verification_dd_frac_blocker`) and the S-system canon
(`docs/ssystem_theory_reference_2026-07-09.md` §3–§6). Design-only handoff —
NOT built. Every file:line below was re-verified against the tree on
2026-07-10 (uncommitted state on top of d003698).*

## The defect (why no existing datum can ever give a user integer DDs)

A PPP-RTK user receives only the emitted satellite phase bias
`δ̃_s = b_s + ref_s` (physical fractional bias + datum reference). Its float
ambiguity absorbs `M_{u,s} − ref_s`; the between-satellite SD it must fix is
`integer − (ref_s − ref_p)`. **Integer-fixable iff `D_sp = ref_s − ref_p ∈ ℤ`
for every satellite pair** (unit-test-proven:
`NetworkEstimatorTest.Eq34UserIntegerRecovery_CCRIntegerEnsembleFractional`,
tests/test_network_estimator.cpp:309–407). All three built datums fail this at
global scale:

1. **Prioritized anchor** (`network.phase_datum_prioritized`; election at
   src/core/app/ApplicationNetwork.cpp:3961–4007, pin fold at 4267–4306):
   pins ONE elected receiver's ambiguity to 0 per (sat,signal) at weight 1e10
   (4300–4302). With **no receiver phase-bias states in the model**, the
   receiver's fractional phase bias `δ_r` is lumped inside its ambiguity, so
   `ref_s = M_{anchor(s),s} = z + δ_{anchor(s)}`. Same-anchor satellite pairs
   are integer; **cross-anchor pairs carry
   `δ_{anchor(s)} − δ_{anchor(p)} ∉ ℤ`** — and cross-anchor pairs are
   structurally forced at global scale (no receiver sees all satellites).
2. **Ensemble** (`network.phase_datum_ensemble`; gauges built at
   ApplicationNetwork.cpp:5020–5076, folded via
   `NetworkEstimator::setPhaseSumZeroGauges`, NetworkEstimator.hpp:500–502):
   `ref_s = mean_r M_{r,s} = k/R` — non-integer **by construction** (worse in
   production: per-sat visibility subsets, so receiver fractionals leak
   between satellites). `toSolutionEpoch` already forces
   `signal_integer = false` for it (src/core/filter/NetworkEstimator.cpp:599–604).
3. **Legacy single pivot** (`stations.front().is_pivot`, 3842–3845): integer
   `ref_s` but globally rank-deficient (one receiver cannot see all sats — the
   SPOF the other two were built to escape).

Nothing in the tree implements Eq-34 recovery (verified: the only Eq-34
artifacts are the unit test above and comments). **Card 3 = make `ref_s`
integer-consistent across ALL satellites while staying full-rank at global
scale.** The mechanism (Odijk 2016 §4/§6; TK-2015; de Jonge 1998): give the
receiver fractional phase bias its OWN state so it can no longer lump into
the emitted satellite bias, then fix the enlarged gauge with a
single-receiver datum generalized by chain (spanning-tree) pivoting.

## The theory in two lines

With explicit per-(receiver,signal) phase-bias states `δ_{r,σ}`, the phase
row is `… + δ_{r,σ} − δ^s_σ + z^s_{r,σ}` (all in meters; `z ∈ λℤ` now truly
integer-valued). The gauge group per (constellation,signal) connected
component is `(a_r, b_s)`: `δ_r → δ_r + a_r`, `δ^s → δ^s + b_s`,
`z^s_r → z^s_r − a_r + b_s` — dimension `n_c + m_c`. Fix it with:
`δ_{r1,σ} = 0` (one receiver) + `n_c + m_c − 1` **ambiguity pins on the edges
of a spanning tree** of the bipartite (receiver × satellite) observation
graph. Then `b_s = δ_{r1} + Σ(pinned integers along the tree path)`, so
`ref_s − ref_p ∈ λℤ` for **every** pair — including satellites the datum
receiver never sees (each chain hop contributes only an integer, because the
hop receiver's fractional bias is now a separate estimable state, not part of
the pin). Full-visibility regional case degenerates to the Odijk Eq-34 star
basis (CC-R): pin all `z^s_{r1}` + one `z^p_r` per other receiver.

## Design

### D1. New states — per-(receiver,system,signal) phase bias

- **StateKeySchema** (include/filter/StateKeySchema.hpp — add section 4c
  between the receiver code-bias keys (§4, :357–508) and sat-bias keys):
  format **`"<system>_RPB_<signal>"`** (e.g. `"GPS_RPB_L1C"`,
  `"Galileo_RPB_L5Q"`), constant `kRpbInfix = "RPB"`, `makeReceiverPhaseBiasKey` /
  `parseReceiverPhaseBiasKey` / `isReceiverPhaseBiasKey` following the §4
  IFB pattern (underscore family; `_RPB_` infix is not producible by
  `makeIfbKey`/`makeDcbKey`, so the code-bias namespace stays disjoint).
- **StateManager**: new map `receiver_phase_bias_indices_` mirroring the
  sat-phase-bias family (`getSatPhaseBiasStates`/`addSatPhaseBiasState`/
  `pruneSatPhaseBiasStatesExcept`, include/filter/StateManager.hpp:983–1012):
  `addReceiverPhaseBiasState`, `getReceiverPhaseBiasStates`,
  `getReceiverPhaseBiasIndex`, `pruneReceiverPhaseBiasStatesExcept`. No
  process-noise plumbing needed in v1: the network solve is a per-epoch
  absolute solve; `δ_{r,σ}` is deliberately **re-estimated fresh each epoch**
  (the receiver-side wander sink), while the integers stay time-linked
  through the existing phase-leveling ambiguity carry. This is exactly the
  Hou-Zhang-correct split (theory ref §8).
- **MeasurementModel / StateInitializer** (network branch only, toggle-gated):
  allocate `RPB(sys,σ)` when the station builds ≥1 usable phase row on an
  ungated shared (sat,σ) (rule D3c), and add coefficient **+1** on the RPB
  column of every such phase row (same sign as the ambiguity column it splits
  — today the ambiguity carries `z + δ_r` with +1). `splitStation` routes any
  non-position, non-shared state column into `A_r` automatically
  (ApplicationNetwork.cpp:2315–2320, 4237), so the estimator kernel needs
  **zero changes** for the state itself.

RPB states are **LOCAL (A_r) in v1** — each is observed by exactly one
station, and local placement keeps the border size, the D2/C1 carry, and the
Card-1 coupling machinery untouched (see D6 for the border-promotion v2 and
the coupling-row analysis the border variant requires).

### D2. The datum — chain election replacing the anchor election

New master toggle `network.phase_datum_eq34`. Wiring reuses the ensemble
precedent exactly: the local redefinition
`phase_datum_prioritized = config || ensemble || eq34`
(ApplicationNetwork.cpp:2291–2293) buys all the SPOF-cure machinery (no
pivot-first `i_lo`, every station carries its arcs, union shared set, 3261,
3842–3960) for free; eq34 then branches at the election (replacing
3961–4007) and at the pin fold:

Per (constellation, signal), each epoch, over the **ungated** keys (the
`phase_datum_min_stations` prune at 3915–3947 runs first, unchanged):

1. Build the bipartite graph: vertices = present stations with a usable
   (sat,σ) ambiguity (`usable_keys`, 3862–3873) ∪ satellites; edges = live
   (r,s) arcs. Union-find the connected components (O(#arcs·α), trivial).
2. **Per component, elect the datum receiver r1**: sticky incumbent while
   still present in the component (mirror `anchor_incumbent`, 3987–3993, and
   the RTA-1 emit-baseline rule — advance at emit only, 5414–5428), else the
   config preference `network.phase_datum_eq34_receiver` when present, else
   the highest coverage-ranked candidate (reuse `cand` ranking, 3962–3971).
3. **Build the spanning tree**, sticky-first: keep every incumbent tree edge
   whose station still tracks its satellite; grow the remainder by BFS from
   r1 preferring high-coverage stations and high-elevation arcs (`sat_el`,
   3862–3866). Determinism: stable sort by (coverage desc, station name,
   sat, signal) as in the existing election.
4. Record the pins:
   - r1's RPB pin: new per-station `pinned_rpb_keys` (set of `"sys_RPB_σ"`),
     folded as a 1e10 row on the RPB column **in r1's own block** — the
     same load-bearing invariant as the ambiguity pins (4258–4266).
   - one ambiguity pin per tree edge, in the tracking station's own block:
     generalize `pinned_amb_keys` (std::set, memory `NetStation`) to
     `std::map<std::string,double> pinned_amb_targets` ("sat_signal" →
     target). Pin fold change at 4272–4302: `pin_y = target − piv_x(amb_idx)`
     (today `target ≡ 0`), weight 1e10 unchanged.
   - **Pin target** = 0 on first sighting of the edge; on a re-attach /
     re-pin of an already-lit (sat,σ), `target = λ·round(x̂_prev/λ)` of the
     new edge station's carried float (`network.phase_datum_eq34_pin_round`,
     default true). Any integer target is a valid gauge; rounding selects the
     integer that makes the frame step ≈ 0 cycles (see D4) — a wrong-side
     rounding is still a legal frame, never an error.
5. `shared_phase_keys` = the ungated union (as under ensemble, 3959–3960);
   `this_epoch_anchor` is repurposed to record the tree (per key: its edge
   station) for the emit-site handover detection (D4).

Clock datum, code-bias IF-pin + s2z datum (5078–5115, 5385–5448), and the
SH-VTEC machinery are **untouched and independent**. No phase s2z rows exist
under eq34 (`buildEnsembleGauges` stays ensemble-only, 5020–5076; its RAIM
re-invoke at 6482 is a no-op).

### D3. Rank / redundancy bookkeeping

- Per (const,σ) component with `n_c` stations, `m_c` satellites: states
  +`n_c` (RPB), pins = `(n_c + m_c − 1)` tree edges + 1 RPB = `n_c + m_c` =
  the exact gauge dimension (minimum constraint; vs prioritized's `m_c` pins
  / `m_c` gauges — both minimal, eq34 adds the receiver split).
- **All pins are local, folded pre-reduce** ⇒ the W5 gate's
  `Plan::declared_dimension` is UNCHANGED (still #constellations in PIN clock
  mode / 0 in s2z or brdc-anchored mode; NetworkSbasis.hpp:167–185,
  `assertNullSpaceRank` :366). The RPB↔ambiguity gauge is entirely
  within-block (no shared column), so it must make `N_rr` singular unless
  pinned — rule D3c below guarantees a pin.
- (c) **RPB allocation rule**: a station allocates `RPB(sys,σ)` iff it has
  ≥1 phase row on an UNGATED shared (sat,σ) this epoch — i.e. iff it is a
  vertex of the D2 graph. A spanning tree touches every vertex, so every
  such station carries ≥1 tree-edge pin, which together with the component
  chain fixes its `a_r` ⇒ `N_rr` stays PD. A station whose σ-keys are ALL
  gated allocates no RPB and keeps legacy lumping (its ambiguities absorb
  `δ_r`) — consistent, and its keys have no shared column to poison.
- Hub stations (high tree degree) carry many 1e10 pins →
  `network.station_pd_pin_aware` (median-|pivot| scale) should be
  RECOMMENDED-ON with eq34 (the TWTF/VOIM relative-rank lesson, memory
  `project_network_pdp_gate_pinaware`).
- `healNearNullPhaseBias` (5610, invoked from the gate-failure catch at 5755)
  stays as the reactive net for the same rare orphan class as today (a pin's
  `foldAmb` misses via `getAmbiguityIndex < 0` / dropped H column,
  4274–4277).
- Cheap always-on check (theory ref §7.1): assert per-(const,σ)
  `#pins == n_c + m_c` at election time; log with the existing `total_pins`
  ledger column (5006).

### D4. Handover rules (the datum-receiver-disappears clause)

Modeled on the sticky clock pivot (`sticky_pivot`, NetworkSbasis.hpp:306–324,
ApplicationNetwork.cpp:5414–5428) and the prioritized anchor's emit-side
signalling (7480–7502). All incumbent state (r1 per component, tree edge
set) advances **at the emit site only** — a withheld handover epoch leaves
the incumbents unchanged (the P0-1 rule).

- **Tree edge lost** (edge station stops tracking the sat / is non-PD
  dropped): re-attach the satellite via a new edge to any in-tree tracking
  station (highest rank). The emitted `δ̃_s` steps by an **exact integer
  number of cycles** (chain path difference — the receiver fractional no
  longer leaks, unlike prioritized handovers); with pin-to-round the realized
  step is the |·|-minimizing integer, typically **0 once the floats are
  honest** ⇒ frequently NO wire discontinuity at all. Signalling: rely on the
  emitter's replay-safe 0.05 m bias-jump test
  (NetworkProductEmitter.hpp:145–149) — **never `markSatDiscontinuity`**
  (not reconstructable on NetworkProductIO replay; the verified lesson at
  7480–7484). Add a `PHASE_DATUM eq34_reattach` trace event gated on
  `network.trace_events` (mirror 7490–7493).
- **Component split** (cut station leaves): the r1-less fragment elects a new
  datum receiver; its satellites re-reference by a common fractional
  (`δ_{r1'}`) + integers. Within-fragment `D_sp` stays integer;
  **cross-fragment pairs are non-integer** — unavoidable (no data connects
  them) and transient at global scale. Jump test signals the steps; v2 may
  emit a component id (D5).
- **Datum receiver disappears**: elect successor (sticky rule order:
  config pin → incumbent → coverage rank); ALL of the component's biases
  step by a common fractional + per-sat integers. Between-sat `D_sp` still
  moves **only by integers** (the common `δ_{r1}` cancels) — a materially
  softer event than a prioritized cross-anchor handover. Trigger an S-line
  rebroadcast (extend the `due || datum_changed` condition at 7537–7546 with
  the eq34 root-handover flag) and let the jump test bump the per-key discs.
- **Carry hygiene on any b_s re-reference of (s,σ)**:
  - phase-leveling local carries for (s,σ) at every station are stale by the
    same integer: v1 = ERASE them (the `local_carry.ERASE` refloat pattern) +
    counter; v2 = SHIFT by the known `λk`. With pin-to-round the realized
    step is usually 0 — gate the erase on |step| > threshold, mirroring how
    the emitter's jump test gates the disc bump.
  - D2/C1 shared carry: skip the re-referenced PB keys from the carried
    prior for that epoch (mirror the scoped `datum_changed` skip at
    5916–5966 / 6084–6089).

### D5. Product-contract change

- `SbasisDeclaration::AmbDatum` (include/filter/NetworkSbasis.hpp:92–93)
  gains a third enumerator: `SingleReceiverChain` (Eq-34 / de Jonge). Set at
  the decl site (ApplicationNetwork.cpp:5393–5394 pattern).
- **Wire**: the S contract serializes `amb_datum` as an int
  (write src/core/products/NetworkProductIO.cpp:356, read :237 —
  `static_cast` accepts any int — compat check :654), so **no new record
  type and no format bump**: value 2 rides the existing v2 S line, and an
  old consumer's `verifyContract` flags "amb_datum differs" exactly as
  designed. Optional new S-line token `pdr=<station>` (component-0 datum
  receiver, diagnostics only — Odijk §6: the user never needs the network
  pivot; absent = unset, replay/back-compat safe like `gid=`).
- **`signal_integer` = TRUE** under eq34 for CDMA sats: thread a
  tri-state/enum through `toSolutionEpoch` instead of the current bool
  (call site ApplicationNetwork.cpp:6891, logic
  NetworkEstimator.cpp:599–604; the GLONASS `'R'` exclusion stays — FDMA is
  out of scope for eq34, its keys keep legacy float treatment).
- Warm-resume fingerprint gains `phdatum:eq34` (mirror `phdatum:ens`,
  2768–2774) so a toggle flip cold-starts; `generation_id` (RTA-3) then
  marks the datum break for live consumers. `contract_rebroadcast_s`
  re-broadcasts the new declaration periodically with no further change.

### D6. Card 1/1d/1e coupling-row interaction

v1 (RPB local) — the safe default:
- Tree-edge pinned ambiguity columns are exactly the class Card 1e already
  handles: the harvest keeps pinned columns with `p_prev = 0` (the
  gauge-amputation fix); nothing new.
- The coupling rows regress local ambiguities on the SHARED border only;
  the RPB column is marginalized inside `N_rr` automatically, so each row's
  `cond_var` honestly inflates by the receiver-bias wander — conservative,
  correct, zero code.
- The RPB gauge has **no shared-column footprint**, so `tuGaugeRestore`'s
  per-(constellation × kind) groups (4480–4495) and the `couplingQ` ladder
  (4443–4453) are untouched.

v2 (promote RPB to the shared border — only if the D2-style rigorous
time-linking of `δ_{r,σ}` proves necessary, i.e. if per-epoch re-estimation
is shown to inject receiver-side wander the ambiguity carry cannot absorb):
- New `SharedParamKind::RcvPhaseBias`; `couplingQ` gains a `q_rpb` entry
  (config, default = `q_pb`); the D2/C1 fold carries it like any border key.
- **tuGaugeRestore caveat**: the restore assumes gauge weights are uniform
  within (constellation × kind). The RPB gauge direction spans (one RPB
  column, ONE station's ambiguities) — per-STATION, not
  constellation-uniform — so RcvPhaseBias columns must be **excluded from
  the restore** exactly as VtecSh is (4483, 4489) or grouped
  per-(station × signal). Get this wrong and every shrunk row observes the
  RPB gauge → W5 withholds (the Card-1e failure mode again).
- Cost: border grows by ~#stations × #signals (≈ +1200 at 305-sta ⇒ ~8× the
  O(n³) border solve) — the reason v1 stays local.

### D7. Config (repo toggle conventions, CLAUDE.md)

In `GnssConfig` private members + Doxygen getters (pattern:
GnssConfig.hpp:2216–2295), parsed in `loadNetworkSection_`
(GnssConfigLoad.cpp:4073–4101 block):

```
network.phase_datum_eq34               bool   false   master; supersedes phase_datum_prioritized (info log, like ensemble at 4349-4358)
network.phase_datum_eq34_receiver      string ""      preferred datum receiver (component 0); empty = coverage auto-pick
network.phase_datum_eq34_pin_round     bool   true    re-pin target = λ·round(float/λ); false = always 0 (A/B)
```

Mutual exclusion at load (mirror the fail-loud pattern at 4273–4277 and the
mutex warns at 4163–4187):
- `phase_datum_eq34 && phase_datum_ensemble` → **throw** (two different
  realizations of the same gauge; neither subsumes the other).
- `phase_datum_eq34 && phase_datum_prioritized` → info: eq34 supersedes
  (the ensemble precedent).
- `phase_datum_eq34 && phase_datum_hold_sigma_m > 0` → warn INERT (the hold
  is ensemble-only stopgap; eq34 needs no hold — its frame is pinned hard).
- `phase_datum_min_stations` and `station_pd_pin_aware` compose with eq34
  (gate runs before the graph; recommend pin_aware ON in the banner).

Default-OFF byte-identity is structural: with the toggle off, no RPB state
is ever allocated, `pinned_amb_targets` is empty, the election branch never
runs — hash-prove against the frozen golden artifacts per repo convention
(the `_golden_carrylive_*` configs, docs/GOLDEN_carrylive_baseline.md).

## Tests (gate; tests/test_network_estimator.cpp style)

Extend the `makeTruth`/`buildStation` fixture family with (a) a per-receiver
fractional phase bias `d_r` added to every M (the test at :310–312 documents
its absence today), (b) a visibility mask, (c) RPB local columns + chain pins.

1. **`Eq34ChainDatumUserDdInteger`** — R=4, S=5, no receiver sees all sats,
   `d_r ≠ 0`, integer `z`. Assert `fracDist(ref_s − ref_p) < 1e-6` for ALL
   pairs, including pairs with different chain paths. Negative control in the
   same test: the prioritized election on the same fixture yields ≥1
   fractional cross-anchor pair (mirrors the CC-R-vs-ensemble structure of
   the test at :309).
2. **`Eq34ReattachStepsByExactInteger`** — two solves; drop sat-s3's edge
   station between them; assert the emitted bias step ∈ λℤ, and with
   pin-round == the |·|-minimizing integer (≈0 when the float frac is small).
3. **`Eq34DatumReceiverHandoverBetweenSatInteger`** — remove r1; assert all
   `D_sp` still integer (common fractional cancels) though absolute values
   moved.
4. **`Eq34ComponentSplitLocalIntegrity`** — disconnected graph: assert
   within-component `D_sp` integer; cross-component pairs fractional
   (documents the accepted limitation).
5. **Rank**: `assertNullSpaceRank` passes with clock-only declared dim on the
   chain fixture; per-(const,σ) pin count == `n_c + m_c`.
6. Config-parse regression (mutex throw, inert warns) + suite green + OFF
   byte-identity SHA vs the golden baseline.

## Falsifier (field — the only grading that counts)

The producer's own datum-invariant DD-frac meter (`network_dd_fractional.csv`,
`scripts/grade_ddfrac.py`) CANNOT grade Card 3 — DDs cancel any datum. The
eq34-specific instruments are consumer-side:

1. **Synthetic known-integer product** (verification step 2 of
   `project_eq34_verification_dd_frac_blocker`): synthesize a product file
   with known integer frame via NetworkProductIO, feed the closed-loop
   consumer (`consumer_undifferenced=true`), assert its float DD ambiguities
   converge to those integers and MLAMBDA actually executes (guard the
   σ-gate sentinel: ratio 0.00 ⇒ the search never ran —
   AmbiguityResolver's early return, the C7 misread). This doubles as the
   acceptance harness for the later self-AR build
   (`project_network_ar_producer_scope`).
2. **A/B prioritized vs eq34** on the golden 50-sta 2024-129 config, held-out
   consumer: grading = the CONSUMER's float DD fractions of the emitted
   products cluster at integers (near_int ≫ 30% uniform-expectation;
   mean|frac| ≪ 0.25) — NOT producer-internal metrics, NOT fix counts alone
   (the σ-gates at GnssConfigLoad's 0.20 m closed-loop cap /
   `ar_max_position_sigma` independently zero fixes).
3. Secondary: eq34 handover steps on the wire are ∈ λℤ (post-process the
   product file's per-key jumps) and disc-bump counts drop vs prioritized
   (~145/h anchor handovers today).

## Sequencing (include verbatim in any planning)

**Field grading of Card 3 is meaningless until the ~04:00 UTC divergence of
the 12 h live-carry instrument is resolved**
(docs/GOLDEN_carrylive_baseline.md "Open question": `_golden_carrylive_tu_12h`
diverges at hour 3.90; leading hypothesis = the PL var floor sets the arming
time) — while the network floats sit ~63 cycles off with a drifting carry, no
datum can make consumer DD fractions cluster. The spec is written NOW so
implementation (D1–D7 + unit gates 1–6, all divergence-independent) can start
immediately; the field falsifier runs after the divergence fix, on the golden
config lineage. Downstream: an eq34-integer product is the precondition the
self-AR pipeline (Phase 2 log-only block, ApplicationNetwork.cpp:6740–6900)
was missing — integers to re-pin only exist in an integer-consistent frame.

## Known-open (could not be resolved from code/memory)

1. **Per-signal vs per-band trees**: gauges are per (sat,signal), so D2
   builds per-signal trees; rarely-tracked channels (Galileo C5Q vs C5X
   class) fragment into small components → more datum receivers → more
   cross-component non-integer pairs. `phase_datum_min_stations ≥ 2`
   mitigates; a band-level chain with per-channel offsets is a v2 question.
2. **RPB observability at 1-signal stations**: a station tracking one σ has
   RPB fully coupled to its clock/iono through a single row family; the tree
   pin fixes the gauge, but the estimable split may be weak (near-null but
   real, the drop-and-heal class). Whether `station_pd_pin_aware` suffices
   or RPB needs a loose σ≈1 m soft prior (the GLONASS local-anchor pattern,
   4330–4358) is an empirical call.
3. **Erase-vs-shift** of phase-leveling carries on re-reference (D4): erase
   is safe; shift needs the realized integer plumbed from the emit site back
   into the carry stash — deferred to measurement.
4. **Whether `this_epoch_anchor`/`emitted_anchor` reuse is clean** for tree
   edges (one station per key holds, but a key's pin can live at a station
   that is not "the anchor" semantically) — naming/plumbing detail for the
   implementer.
5. v2 border promotion of RPB (D6) is specced but its trigger condition
   (receiver-side wander measurably poisoning the per-epoch re-estimate) has
   no instrument yet.
