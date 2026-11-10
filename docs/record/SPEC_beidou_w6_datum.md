# SPEC — BeiDou W6 datum: gate calibration + phase-anchor completion ("BD-DATUM")

Written 2026-07-22 by the session that reviewed `docs/SPEC_cb_admission_card.md` §9–§13
(review verdict: findings confirmed against the tree; planning accepted with the
gate-before-prior ordering the user ruled for). Self-contained. Read `CLAUDE.md` (build
pattern, **Standing invariants**) and `docs/SPEC_cb_admission_card.md` (the predecessor:
census + station-local quarantine + state-alloc phase co-gate, all built, default-OFF,
COMMITTED; its former HANDOFF is now `docs/attic/HANDOFF_cb_admission_card.md`) first.
The ground rules of `docs/SPEC_slip_qc_hardening.md` §1 apply unchanged (default-OFF +
same-build byte-identity; Release-only runs with a renamed exe and name-scoped zombie
kills; staged cadence; empirical thresholds; never hard-exclude measurements; run-end
counters prove wiring; additive unit tests; Doxygen + explicit CMake lists; no co-author
trailer). **MUST** = binding; **CANDIDATE** = non-binding (deviate with a stated
rationale). If a MUST seems wrong, stop and ask the user.

---

## 0. Evidence base and the necessity argument

### 0.1 Why this card exists — the four-piece emit path

The 305-station GPS+Gal+BDS W6 run (`_maxnet_305_gpsgalbds_d15_5ep_*.json`) emits **zero
epochs**: the `NetworkSbasis` null-space gate withholds every epoch (observed near-null
count ≠ declared S-basis dim 3). CB-ADM decomposed the below-cutoff population into FOUR
classes with FOUR owners — the reconciliation that makes the plan coherent, and it must
open any report on this card:

| class | measured (per-dump ≈, 305 arms) | nature | owner |
|---|---|---|---|
| declared clock gauges (`CLK\|G/E/C`) | 3, ev ≈ 1e-9..1e-16 rel | expected, pinned by datum | none (correct) |
| sparse-code confounds (pure-CB, CB+CLK+PB singles) | ≈ 3 (control) | **genuinely null** (~1e-11 abs) — 1–2-station (sat,signal) code; no gate value can fix | **CB-ADM quarantine + phase co-gate** (built; removes them: 7→0, 13→0) |
| weak-but-real BeiDou families | ≈ 3.5 (21 across 6 dumps, ev ≈ 1e-3..4.4e-3 abs vs cutoff 6e-3) | **observable, miscounted** — spread across a BDS-3 MEO's many signals | **Card DG (this spec)** — gate calibration |
| deep pure-PB orphans | ≈ 4 (24 across dumps) | genuinely null — admitted ~2-station signals with no reachable ambiguity anchor | **Card PA (this spec)** — phase-anchor / heal |

Neither the control (legacy gate) nor CB-ADM+co-gate emits alone, and the raw counts
mislead (control 22/dump vs co-gated 33/dump — the co-gated arm looks *worse*): control's
22 contain the sparse-code confounds that **no gate calibration can clear** (true nulls),
while the co-gated 33 are dominated by the two classes this spec clears. The emit path is
**CB-ADM + co-gate + DG + PA together**. Never evaluate a piece by the raw below-cutoff
count; always decompose by class (Card AN).

### 0.2 The gate today

> ⚠ **ANCHOR REFRESH 2026-08-15, re-grepped.** Every `ApplicationNetwork.cpp:33xx` anchor
> this card was written against is **past EOF** — the INC-0..12 decomposition took that
> file to **2359 lines** and relocated the gate/heal into seams. The FACTS are unchanged;
> the superseded addresses are recorded here so an old citation resolves:
> `ApplicationNetwork.cpp:3372` (gate call) and `:3391` (post-heal re-gate),
> `ApplicationNetwork.cpp:3385-3399` (the prioritized-only heal+re-gate path),
> `ApplicationNetwork.cpp:3345-3356` (the 1e-6 → 1e-9 comment block),
> `NetworkEstimator.hpp:447` (`kNullSpaceTol`), `NetworkPhaseDatum.cpp:34` (heal impl).
> **Current sites**: gate = `NetworkSbasis::assertNullSpaceRank`
> (`NetworkSbasis.cpp:447`) driven from `NetworkSolveGlue.cpp:778`, post-heal re-gate at
> `NetworkSolveGlue.cpp:817`; heal = `healNearNullPhaseBias` (`NetworkPhaseDatum.hpp:107`,
> impl `NetworkPhaseDatum.cpp:24`) called at `NetworkSolveGlue.cpp:804`.
> `docs/SPEC_sbasis_realization_contract.md` §A records the same driver site as canonical.

*(Facts verified in tree 2026-07-22; anchors refreshed 2026-08-15.)*

- `NetworkEstimator::kNullSpaceTol = 1e-9` (`include/filter/NetworkEstimator.hpp:507`; the
  resolved value is a member set once at construction — `setRankGateTol` `:253`, resolved
  member documented at `:255` — which is DG2 as built), applied as a RELATIVE cutoff
  `tol · max(ev_max, 1)` in **three consumers**: (1) the withhold gate + post-heal re-gate
  (tol is already a call-site parameter); (2) the heal scan — `healNearNullPhaseBias`
  selects PB columns spanning below-cutoff directions (`kSpanFrac = 0.3`); (3) the
  estimability dump — `NetworkEstimator::dumpEstimabilitySpectrum`
  (**`NetworkEstimator.cpp:976`**; the card's `NetworkEstimator.cpp:723` is the 2026-07
  snapshot), log-only, budget-latched (first ~6 processed epochs under
  `network.trace_estimability`).
- **The gate is an exact-equality test and throws in BOTH directions** (the card's
  `NetworkSbasis.cpp:232-241` is now the `:447`–`:500` body): observed > declared ⇒
  "under-specified", observed **<** declared ⇒ "over-specified". Consequence: a tolerance
  lowered below a config's own gauge depth stops counting the declared gauges and
  **withholds a healthy config**. The armed value is therefore *per-config*, placed from
  that config's measured spectrum — never a global constant edit.
- **Precedent**: the same calibration happened once before, recorded in the comment block
  that documents the 1e-6 → 1e-9 move ("the default 1e-6 sits above the weak-but-real
  directions and miscounts them as deficiencies; 1e-9 lands squarely in the gap"). BeiDou's
  weak-real layer sits ~3 decades deeper relative (≈1.7e-10 of ev_max ≈ 6e6) than the
  GPS+Gal-era weakest-real, so 1e-9 now miscounts again. The tolerance is a calibration
  parameter, not a constant — DG makes it one.
- Measured 305 spectrum (co-gated arm, first dump): 21 dirs at ev ∈ [1e-3, 6e-3), 6 at
  [1e-6, 1e-3), 3 at [1e-9, 1e-6), 5 below 1e-9 (3 gauges + ~2 uncharacterized). The gap
  between the weak-real layer (~1e-3 abs) and the gauges (~1e-9 abs) is **six decades
  wide**; `tol = 1e-12` ⇒ cutoff ≈ 6e-6, mid-gap with ≥ 2 decades of margin each side.

### 0.3 Why gate-calibration precedes the Kaula prior (the user's ruling)

Loose-prior quarantine was killed by the CB-ADM spec's own rejected-alternatives list
because **prior information sitting near the rank cutoff is fragile**: a Kaula prior
lifting ~1e-3 directions past a 6e-3 cutoff has only a factor-6 margin and shifts every
admitted OSB estimate plus the declared-dim accounting. Gate calibration into a six-decade
measured gap is robust, injects **NO information** (the weak families emit with honest
large sigmas the M4 contract already lets the consumer deweight), and can only flip
withheld→emitted — never breaking a config whose observed already equals declared, subject
to §0.2's over-specified trap which the per-config placement rule handles. The INC-8
estimability-arm baseline machinery proves byte-identity cheaply. *(KP was scoped as the
CONDITIONAL second lever on this reasoning; it has since been built — §4.)*

Out of scope (do not bundle): the Jun-29 W6-OFF BeiDou clock-divergence mode (re-graded
only after this card emits, §6.4); the MW/EWL lane BeiDou arming (`MwLaneTiers.hpp` `'C'`);
pdp anchor-election threading (reserved — collided with decomposition 8b, since landed;
unlocked only if PA's heal proves insufficient).

---

## 1. Card AN — storm analyzer port (land FIRST; instrument before mechanism) — **LANDED**

`scripts/characterize_storm.py` exists (2026-08-15), ported out of a session scratchpad for
the reason that forced `grade_slip_agreement.py` into `scripts/`: every falsifier below
reads its output. Binding contract:

- A1. Inputs = one or two log dirs (two ⇒ arm-vs-arm diff). Sources in preference order:
  the Card-DG estimability CSV (DG5) when present; else the `Network[estimability]:` WARN
  lines in `main_log.log` — **the only source for archived runs, so keep that fallback
  working.**
- A2. Per dump: ev census below cutoff; per-direction family classification from the dumped
  top-5 spanning columns (`pure_CB`, `pure_PB`, `pure_CLK`, and mixed combinations by which
  families exceed a weight threshold); per-constellation and per-satellite culprit tables;
  ev histogram by decade — **the §0.2 shape must be reproducible from the tool alone.**
- A3. **Unit discipline**: every printed count labeled per-dump or across-dumps; arm-vs-arm
  tables use one unit consistently (mixed-unit tables are the anti-pattern this fixes).
- A4. Cross-check once against the recorded control **135/258/33-class splits** from the
  archived logs and state the result in the card report. `--json` output for downstream
  tooling.

---

## 2. Card DG — rank-gate tolerance calibration (`network.rank_gate_tol`) — **BUILT**

**Where** *(refreshed; see the §0.2 banner)*: `include/filter/NetworkEstimator.hpp:507`
(const becomes the default) + `:253`/`:255` (`setRankGateTol` + the resolved member);
`src/core/filter/NetworkEstimator.cpp:976` (dump); `src/core/filter/NetworkPhaseDatum.cpp:24`
(heal); `src/core/filter/NetworkSolveGlue.cpp:778` / `:817` (gate + re-gate, tol already a
parameter); `src/core/config/GnssConfigLoadNetwork.cpp:5715-5750` (parse + validation).
`network.rank_gate_tol` is live and the DG2 one-resolved-value contract is realized as the
estimator member.

**MUST**:
- DG1. `network.rank_gate_tol` (double), **default exactly 1e-9** ⇒ byte-identical (same
  double through the same arithmetic; `kNullSpaceTol` remains the default's single source).
  Loader validates > 0 and warns + clamps at ≥ 1e-16 (an accidental 0 must not turn the
  gate off).
- DG2. **One resolved value, all three consumers** (gate, heal scan, estimability dump) —
  e.g. a `NetworkEstimator` member set once at construction; the free-function heal reads it
  through the estimator it already takes. The dump must describe the gate's decision, and
  the heal must anchor only directions the gate counts as null — under a legacy shared
  value the heal would soft-anchor *weak-but-real* directions (prior information on real
  parameters, the exact fragility §0.3 avoids). No consumer keeps a private tolerance.
- DG3. **Measured-gap placement rule** (binding for every non-default armed value): the
  tolerance must be justified from that config's own estimability dump — cutoff ≥ 2 decades
  above the config's gauge depth and ≥ 2 decades below its weakest-real layer, with the
  three numbers (gauge depth, weak-real floor, chosen cutoff) recorded in the card report.
  For the 305 storm config: **1e-12** (cutoff ≈ 6e-6, in the measured gap). Guessing a value
  without a dump is a spec violation, per §0.2's over-specified trap.
- DG4. Gate semantics untouched: exact equality, both-direction throw, relative cutoff
  `tol · max(ev_max, 1)`, declared-dim computation unchanged.
- DG5. **Machine-readable spectrum**: under `network.trace_estimability` the dump also
  writes `network_estimability.csv` (per dumped epoch: tow, n_shared, declared_dim, ev_max,
  cutoff, then one row per reported direction: ev + top-5 `col=weight` pairs). Log lines
  unchanged. Trace off ⇒ byte-identical; trace on ⇒ the new file is the only artifact delta
  (enumerate it in the gate table, Card-E precedent).
- DG6. Run-end counters: epochs gated / withheld-under / withheld-over / emitted, plus
  min/max observed near-null count over the run and the resolved tolerance — the
  "over-specified" case must be its own number, not folded into generic withholds.
- DG7. Unit tests (additive), synthetic normals with planted spectra:
  equality-both-directions throw; default-tol byte-behavior (1e-9 path identical); armed-tol
  reclassification (a planted 1e-3-relative layer counted under default, passed under armed,
  gauges still caught); three-consumer consistency (heal scan and dump agree with the gate).

**CANDIDATE, rejected for now**: automatic gap detection (place the cutoff in the largest
log-spectral gap below a ceiling). The manual measured-placement rule is auditable and the
gate is too load-bearing for an adaptive threshold; revisit only if per-config placement
proves operationally annoying.

**Falsifier (305 co-gated arm + DG armed, 5 ep ≈ 17 min)**: the weak-real class (≈ 21 across
dumps) moves above the cutoff (AN shows the [1e-3, 6e-3) bin emptied from the below-cutoff
census); the 3 gauges remain below; observed count drops to gauges + deep residual only. The
arm may still withhold (PA owns the deep-PB class) — DG's falsifier is the *census shift*,
not emission.

---

## 3. Card PA — phase-anchor completion for the deep-PB residual

**Where**: config arming first — `network.phase_datum_prioritized = true`,
`network.phase_datum_min_stations = 2` in the storm arm (both exist; the heal + re-gate
recovery path, `NetworkSolveGlue.cpp:804`/`:817`, runs only under prioritized). Existing
counters `n_healed_epochs` / `n_healed_cols`.

**MUST**:
- PA1. The heal operates under DG's resolved tolerance (DG2 gives this free) — with the
  armed cutoff ≈ 6e-6 it targets only the deep nulls (orphan-class PB at ~1e-10, spanning
  weight ≈ 0.99 ≫ `kSpanFrac` 0.3), never the weak-real families.
- PA2. Wiring visibility: verify the existing run-end heal counters reach the run-end
  summary; add per-heal event rows (station-less: sat/signal, ev, span weight) under
  `trace_events` if not already emitted, so a healed column is trace-attributable.
- PA3. The ~2 uncharacterized deep directions (§0.2: 5 below 1e-9 minus 3 gauges) are
  characterized via AN on the DG-armed run and dispositioned: PB-dominated ⇒ healed (PA
  closes them); CB-dominated ⇒ a genuine admitted-code deficiency, feeding Card KP's
  trigger; anything else ⇒ a new finding, stop and report.
- PA4. **Falsifier — the milestone of the whole spec**: the 305 arm with CB-ADM + co-gate +
  DG + PA armed **emits > 0 epochs**, with the gate log showing the recovery path
  (healed > 0 → re-gate pass) or a clean pass, and AN confirming the remaining below-cutoff
  census = the 3 declared gauges. Zero mechanism invention: if emission still fails after
  DG + PA, STOP and report the residual census — do not improvise a fourth lever.
- PA5. If the heal proves insufficient (anchors flap epoch-to-epoch, healed count grows
  unboundedly, or PA4 fails on an anchor-reachability class), pdp anchor-election threading
  is the follow-up — **reserved ruling** (it collided with decomposition 8b); spec it
  separately with PA's measurements, do not build it inside this card.

---

## 4. Card KP — Kaula/soft prior on admitted OSBs — ⚠ **BUILT AND IN ROUTINE USE**

> ⚠ **STATUS CORRECTED 2026-08-15. KP is no longer conditional.**
> `NetworkSbasis::buildCodeBiasKaulaPriors` (`NetworkSbasis.hpp:632`, impl
> `NetworkSbasis.cpp:285`) is called from `NetworkSolveGlue.cpp:333`;
> `network.cb_kaula_prior` appears in five `src/` + `include/` files; CLAUDE.md describes an
> "SBC+KP recipe"; and `docs/SPEC_datum_graph_census.md` §3 / §6.4 treat KP-armed lineages
> as EXISTING (its `predicted only` cell IS the "KP-load-bearing map"). **Owed to this card,
> not yet recorded: which trigger below actually fired, and what the K2 margin measured on
> the armed run.** Until then the card documents the mechanism, not its justification.
> *(Loader site: `GnssConfigLoadNetwork.cpp:5674-5690`; the card's original
> `GnssConfigLoadNetwork.cpp:5671` still labels it "the CONDITIONAL second lever" — a
> comment worth refreshing.)*

**Trigger as originally scoped** (either would have justified building it, from DG/PA-armed
runs): (a) PA3 finds CB-dominated genuine deficiencies among the deep residual; or (b) the
weak-real families emit with sigmas too large to be useful downstream (define the bound from
the M4 consumer contract before judging — sigc-deweighted products that never influence the
consumer are "not useful").

**MUST** (binding on the shipped mechanism):
- K1. Site: `NetworkSbasis::buildCodeBiasDatumRows` — one soft prior row per ADMITTED
  satellite code-bias column; σ from the physical OSB bound (tens of ns), stated in the
  report; default-OFF toggle (`network.cb_kaula_prior`), byte-identical OFF.
- K2. **Anti-fragility placement** (inverting the rejected alternative's failure mode): the
  prior's information contribution must land ≥ 2 decades ABOVE the armed cutoff of every
  config it ships in — compute and log the margin at startup; a prior sitting near the
  cutoff is the configuration CB-ADM already rejected.
- K3. Declared-dim accounting audited: the clock-gauge directions carry ~zero CB weight
  (measured), so declared_dim must remain 3 — assert via AN on the armed run, and update the
  S-basis plan documentation if any prior row is datum-classed.

---

## 5. Card H — hygiene (rides the next CB-ADM/BD-DATUM commit)

- H1. ✅ **LANDED** (verified 2026-08-15). The stale comment the card cited at
  `ApplicationNetworkAssembly.cpp:859-869` — "co-gated to them [`cbadm_reg_code_keys`]
  below" — is corrected and now lives at `ApplicationNetworkAssembly.cpp:1609-1637`,
  stating that the co-gate lives upstream at state allocation
  (`MeasurementModel::ensureNetworkSharedStates`) and keys off the CENSUS-admitted set, not
  the per-epoch registered subset.
- H2. **Granularity-seam ruling (decided here, deliberately)**: the phase co-gate stays at
  CENSUS granularity. Per-epoch phase-state allocation churn would violate the anti-flap
  design and thrash eq34/carry/snapshot; the cost is a transient orphan window — an ADMITTED
  code key below 2 present stations for an epoch keeps its shared phase column while its
  code column is withheld. Make the window measurable: run-end counter
  `n_cbadm_phase_orphan_key_epochs` ((key, epoch) pairs where an admitted code key was
  per-epoch-withheld while its same-signal phase key was shared). Measured no-op at 305
  density; if a future sparse-tier run shows it nonzero and coincident with withholds,
  revisit with data.
- H3. Units erratum: append a per-dump vs across-dumps clarification to
  `docs/SPEC_cb_admission_card.md` §10's A/B tables, or restate them unit-labeled in this
  card's report — either way §0.1's reconciliation table is the canonical presentation.

---

## 6. Validation harness

1. **Geometry rule (binding)**: efficacy claims run on the 305 storm config ONLY
   (`_maxnet_305_gpsgalbds_d15_5ep_*`); 50/120-station global subsets are the measured wrong
   test (all-constellation under-determination) and prove nothing either way. Iteration
   unit: 5-ep arm ≈ 17 min (3.2 min/ep + ~2 min ingestion). Stage
   `_maxnet_305_gpsgalbds_d15_5ep_datum.json` = cbadm arm + `rank_gate_tol 1e-12` +
   `phase_datum_prioritized/min_stations` + `trace_estimability`; the existing `_ctl` arm is
   the baseline.
2. **OFF gates**: all new toggles OFF ⇒ byte-identical vs the pre-change binary (a fast
   config suffices for the mechanism byte-id; the INC-8 estimability-arm baseline covers the
   DG plumbing on the estim path). Suite green with the additive DG tests.
3. **Healthy-config guard**: the GPS+Gal 305 config (`_maxnet_305_gpsgal_d15_*`) with toggles
   OFF is byte-identical by (2); DG's armed value is NOT applied there (per-config placement,
   DG3). A non-default tolerance on a healthy config requires DG3's dump-first rule.
4. **Graded emit leg (ON COMMAND — the original blocker's close-out)**: once PA4 emits, the
   BeiDou products become gradeable for the first time. Grade datum-invariant quantities
   only: emitted BeiDou clock sanity vs the Jun-29 divergence signature (BDS |clock|
   abs-median was 28.6 km, worst 530 km, leaking ×3200 into GPS — those statistics must land
   at the GPS/Gal metre class); GPS/Gal product quality unchanged vs the GPS+Gal-only
   baseline; a held-out consumer sanity check (JPLM-class float). Report class-decomposed
   censuses (AN) per arm, never raw counts.
5. **Run mechanics**: Release-only, renamed exe per arm, name-scoped zombie pre-kill,
   LNK1104 ⇒ concurrent-build lock ⇒ retry; isolate exe+DLLs into a run dir (the measured
   exit-127 concurrent-build failure); copy `logs/` aside per arm before the next arm
   overwrites it.

## 7. Acceptance summary and sequencing

| Card | Type | Gate |
|---|---|---|
| AN (first) | tooling | reproduces the HANDOFF numbers from archived logs; unit-labeled output |
| DG | config-gated shared-path | OFF byte-id + estim-arm baseline; armed: weak-real census bin empties, gauges still counted, over/under-specified counters visible |
| PA | config arming + visibility | heal targets deep nulls only; **PA4: 305 arm emits > 0 epochs** with census = 3 gauges |
| KP | **BUILT** (was scoped conditional) | anti-fragility margin ≥ 2 decades; declared-dim audited. **OWED: which trigger fired + the measured K2 margin.** |
| H | hygiene | H1 landed; orphan-window counter still owed |

Sequencing as ruled: **AN → DG → PA (config) → combined 305 A/B (ctl vs datum arm) → PA3
disposition → KP only if triggered → graded emit leg on the user's command.** *(AN, DG, KP
and H1 have landed; the 305 A/B, the PA3 disposition and the graded emit leg — PA4, the
milestone of the whole spec — are still owed.)* Commit logistics unchanged: hunk-level
staging on the shared TUs; nothing ships without the user's go.
