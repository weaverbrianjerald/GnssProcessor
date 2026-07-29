# SPEC — BeiDou W6 datum: gate calibration + phase-anchor completion ("BD-DATUM")

**Handoff**: written 2026-07-22 by the session that reviewed
`docs/HANDOFF_cb_admission_card.md` (review verdict: findings confirmed against
the tree; planning accepted with the gate-before-prior ordering the user has now
ruled for). Self-contained — do not assume access to any prior conversation.
Read `CLAUDE.md` (build pattern, **Standing invariants**),
`docs/SPEC_cb_admission_card.md` + `docs/HANDOFF_cb_admission_card.md` (the
predecessor card: census + station-local quarantine + state-alloc phase co-gate,
all built, default-OFF, uncommitted) before starting. The ground rules of
`docs/SPEC_slip_qc_hardening.md` §1 apply here unchanged (default-OFF +
same-build byte-identity; Release-only data runs with a renamed exe and
name-scoped zombie kills; staged cadence — long/graded runs only on the user's
command; empirical thresholds; never hard-exclude measurements; run-end counters
prove wiring; additive unit tests; Doxygen + explicit CMake lists; no co-author
trailer). Cards state MUST criteria (binding) and CANDIDATE mechanisms
(non-binding — deviate with a stated rationale). If a MUST seems wrong, stop and
ask the user.

---

## 0. Evidence base and the necessity argument

### 0.1 Why this card exists — the four-piece emit path

The 305-station GPS+Gal+BDS W6 run (`_maxnet_305_gpsgalbds_d15_5ep_*.json`)
emits **zero epochs**: the `NetworkSbasis` null-space gate withholds every epoch
(observed near-null count ≠ declared S-basis dim 3). The CB-ADM investigation
decomposed the below-cutoff population into FOUR classes with FOUR different
owners — this is the reconciliation that makes the whole plan coherent, and it
must open any report on this card:

| class | measured (per-dump ≈, 305 arms) | nature | owner |
|---|---|---|---|
| declared clock gauges (`CLK\|G/E/C`) | 3, ev ≈ 1e-9..1e-16 rel | expected, pinned by datum | none (correct) |
| sparse-code confounds (pure-CB, CB+CLK+PB singles) | ≈ 3 (control) | **genuinely null** (~1e-11 abs) — 1–2-station (sat,signal) code; no gate value can fix | **CB-ADM quarantine + phase co-gate** (built; removes them: 7→0, 13→0) |
| weak-but-real BeiDou families | ≈ 3.5 (21 across 6 dumps, ev ≈ 1e-3..4.4e-3 abs vs cutoff 6e-3) | **observable, miscounted** — spread across a BDS-3 MEO's many signals | **Card DG (this spec)** — gate calibration |
| deep pure-PB orphans | ≈ 4 (24 across dumps) | genuinely null — admitted ~2-station signals with no reachable ambiguity anchor | **Card PA (this spec)** — phase-anchor / heal |

Neither the control (legacy gate) nor CB-ADM+co-gate emits on its own, and the
raw counts mislead (control 22/dump vs co-gated 33/dump — the co-gated arm looks
*worse*): control's 22 contain the sparse-code confounds that **no gate
calibration can clear** (they are true nulls), while the co-gated 33 are
dominated by the two classes this spec clears. The emit path is therefore
**CB-ADM + co-gate + DG + PA together** — CB-ADM is necessary-but-insufficient,
and this spec supplies the missing two pieces. Do not evaluate any piece by the
raw below-cutoff count alone; always decompose by class (Card AN).

### 0.2 The gate today (verified in tree, 2026-07-22)

- `NetworkEstimator::kNullSpaceTol = 1e-9` (`include/filter/NetworkEstimator.hpp:447`),
  applied as a RELATIVE cutoff `tol · max(ev_max, 1)` in **three consumers**:
  1. the withhold gate — `NetworkSbasis::assertNullSpaceRank(reduced_N_ss,
     declared_dim, tol)` at `ApplicationNetwork.cpp:3372` and the post-heal
     re-gate at `:3391` (**tol is already a call-site parameter**);
  2. the heal scan — `healNearNullPhaseBias` (`NetworkPhaseDatum.cpp:34`)
     selects PB columns spanning below-cutoff directions (`kSpanFrac = 0.3`);
  3. the estimability dump — `NetworkEstimator::dumpEstimabilitySpectrum`
     (`NetworkEstimator.cpp:723`), log-only, budget-latched (first ~6 processed
     epochs under `network.trace_estimability`).
- **The gate is an exact-equality test and throws in BOTH directions**
  (`NetworkSbasis.cpp:232-241`): observed > declared ⇒ "under-specified",
  observed **<** declared ⇒ "over-specified". Consequence: a tolerance lowered
  below a config's own gauge depth stops counting the declared gauges and
  **withholds a healthy config**. The armed value is therefore *per-config*,
  placed from that config's measured spectrum — never a global constant edit.
- **Precedent**: this calibration has happened once before, for the same reason
  — the comment block at `ApplicationNetwork.cpp:3345-3356` records the 1e-6 →
  1e-9 move ("the default 1e-6 sits above the weak-but-real directions and
  miscounts them as deficiencies; 1e-9 lands squarely in the gap"). BeiDou's
  weak-real layer sits ~3 decades deeper relative (≈1.7e-10 of ev_max ≈ 6e6)
  than the GPS+Gal-era weakest-real, so 1e-9 now miscounts again. The tolerance
  is a calibration parameter, not a constant — DG makes it one.
- Measured 305 spectrum (co-gated arm, first dump): 21 dirs at ev ∈ [1e-3,
  6e-3), 6 at [1e-6, 1e-3), 3 at [1e-9, 1e-6), 5 below 1e-9 (3 gauges + ~2
  uncharacterized). The gap between the weak-real layer (~1e-3 abs) and the
  gauges (~1e-9 abs) is **six decades wide**; `tol = 1e-12` ⇒ cutoff ≈ 6e-6
  sits mid-gap with ≥ 2 decades of margin on each side.

### 0.3 Why gate-calibration precedes the Kaula prior (the user's ruling)

The CB-ADM spec's own rejected-alternatives list killed loose-prior quarantine
because prior information sitting near the rank cutoff is fragile; a Kaula prior
lifting ~1e-3 directions past a 6e-3 cutoff has a factor-6 margin and shifts
every admitted OSB estimate plus the declared-dim accounting. Gate calibration
into a six-decade measured gap is robust, injects **no information** (the weak
families emit with honest large sigmas the M4 contract already lets the
consumer deweight), can only flip withheld→emitted (never breaks a config whose
observed already equals declared — but see the over-specified trap in §0.2,
which the per-config placement rule handles), and the INC-8
estimability-arm baseline machinery exists to prove byte-identity cheaply.
The Kaula prior remains Card KP, **conditional**, as the second lever.

Out of scope (do not bundle): the Jun-29 W6-OFF BeiDou clock-divergence mode
(re-graded only after this card emits, §6.4); the MW/EWL lane BeiDou arming
(`MwLaneTiers.hpp` `'C'`); pdp anchor-election threading (reserved — collides
with decomposition 8b; only unlocked if PA's heal proves insufficient).

---

## 1. Card AN — storm analyzer port (land FIRST; instrument before mechanism)

The CB-ADM investigation's classifier lives in a session scratchpad
(`characterize_storm.py`) and **will vanish** — the same lesson that forced
`grade_slip_agreement.py` into `scripts/`. Every falsifier below reads its
output, so it lands first.

**MUST**:
- A1. `scripts/characterize_storm.py`: inputs = one or two log dirs (two ⇒
  arm-vs-arm diff mode). Sources, in preference order: the Card-DG estimability
  CSV (D5 below) when present; else parse the `Network[estimability]:` WARN
  lines from `main_log.log` (the only source for archived runs — keep this
  fallback working).
- A2. Per dump: ev census below cutoff; per-direction family classification
  from the dumped top-5 spanning columns (classes: `pure_CB`, `pure_PB`,
  `pure_CLK`, and mixed combinations by which families exceed a weight
  threshold); per-constellation and per-satellite culprit tables; ev histogram
  by decade (the §0.2 shape must be reproducible from the tool alone).
- A3. **Unit discipline**: every printed count labeled per-dump or
  across-dumps; arm-vs-arm tables use one unit consistently (the HANDOFF's
  mixed-unit tables are the anti-pattern this fixes).
- A4. Cross-check once against the HANDOFF's recorded numbers (control
  135/258/33-class splits) from the archived logs, and state the result in the
  card report. `--json` output for downstream tooling.

---

## 2. Card DG — rank-gate tolerance calibration (`network.rank_gate_tol`)

**Where**: `include/filter/NetworkEstimator.hpp:447` (const becomes the
default); `src/core/filter/NetworkEstimator.cpp:723` (dump);
`src/core/filter/NetworkPhaseDatum.cpp:34` (heal);
`src/core/app/ApplicationNetwork.cpp:3372/:3391` (gate call sites — tol already
a parameter); `src/core/config/GnssConfigLoadNetwork.cpp` (parse).

**MUST**:
- DG1. New config `network.rank_gate_tol` (double), **default exactly 1e-9** ⇒
  byte-identical (same double through the same arithmetic; `kNullSpaceTol`
  remains as the default's single source). Loader validates > 0 and warns +
  clamps at ≥ 1e-16 (an accidental 0 must not turn the gate off).
- DG2. **One resolved value, all three consumers.** The gate, the heal scan,
  and the estimability dump read the same resolved tolerance (e.g. a
  `NetworkEstimator` member set once at construction; the free-function heal
  reads it through the estimator it already takes). Rationale: the dump must
  describe the gate's decision, and the heal must anchor only directions the
  gate counts as null — under the legacy shared value the heal would otherwise
  soft-anchor *weak-but-real* directions (prior information on real parameters,
  the exact fragility §0.3 avoids). No consumer keeps a private tolerance.
- DG3. **Measured-gap placement rule** (binding for every config that arms a
  non-default value): the armed tolerance must be justified from that config's
  own estimability dump — cutoff ≥ 2 decades above the config's gauge depth and
  ≥ 2 decades below its weakest-real layer, with the three numbers (gauge depth,
  weak-real floor, chosen cutoff) recorded in the card report. For the 305
  storm config: **1e-12** (cutoff ≈ 6e-6 in the measured gap). Guessing a value
  without a dump is a spec violation, per the §0.2 over-specified trap.
- DG4. Gate semantics untouched: exact equality, both-direction throw, relative
  cutoff `tol · max(ev_max, 1)`, declared-dim computation unchanged.
- DG5. **Machine-readable spectrum**: under `network.trace_estimability`, the
  dump additionally writes `network_estimability.csv` (per dumped epoch: tow,
  n_shared, declared_dim, ev_max, cutoff, then one row per reported direction:
  ev + top-5 `col=weight` pairs). Log lines unchanged. With the trace off:
  byte-identical; with it on: the new file is the only artifact delta
  (enumerate it in the gate table, Card-E precedent).
- DG6. Run-end counters: epochs gated / withheld-under / withheld-over /
  emitted, plus min/max observed near-null count over the run and the resolved
  tolerance — the "over-specified" case must be visible as its own number, not
  folded into generic withholds.
- DG7. Unit tests (additive): synthetic normals with planted spectra —
  equality-both-directions throw; default-tol byte-behavior (1e-9 path
  identical); armed-tol reclassification (a planted 1e-3-relative layer counted
  under default, passed under armed, gauges still caught); the three-consumer
  consistency (heal scan and dump agree with the gate on a planted spectrum).

**CANDIDATE** (record the rejection if kept out): automatic gap detection
(place the cutoff in the largest log-spectral gap below a ceiling). Rejected
for now — the manual measured-placement rule is auditable and the gate is too
load-bearing for an adaptive threshold; revisit only if per-config placement
proves operationally annoying.

**Falsifier (305 co-gated arm + DG armed, 5 ep ≈ 17 min)**: the weak-real class
(≈ 21 across dumps) moves above the cutoff (AN shows the [1e-3, 6e-3) bin
emptied from the below-cutoff census); the 3 gauges remain below; observed
count drops to gauges + deep residual only. The arm may still withhold (PA owns
the deep-PB class) — DG's falsifier is the *census shift*, not emission.

---

## 3. Card PA — phase-anchor completion for the deep-PB residual

**Where**: config arming first — `network.phase_datum_prioritized = true`,
`network.phase_datum_min_stations = 2` in the storm arm (both exist; the heal
`healNearNullPhaseBias` + re-gate recovery path at
`ApplicationNetwork.cpp:3385-3399` runs only under prioritized). Existing
counters `n_healed_epochs` / `n_healed_cols`.

**MUST**:
- PA1. The heal operates under DG's resolved tolerance (DG2 gives this for
  free) — with the armed cutoff at ≈ 6e-6 it targets only the deep nulls
  (orphan-class PB at ~1e-10, spanning weight ≈ 0.99 ≫ `kSpanFrac` 0.3), never
  the weak-real families.
- PA2. Wiring visibility: run-end heal counters already exist — verify they
  reach the run-end summary; add per-heal event rows (station-less: sat/signal,
  ev, span weight) under `trace_events` if not already emitted, so a healed
  column is attributable from the trace alone.
- PA3. The ~2 uncharacterized deep directions (§0.2: 5 below 1e-9 minus 3
  gauges) are characterized via AN on the DG-armed run and dispositioned in the
  report: PB-dominated ⇒ healed (PA closes them); CB-dominated ⇒ a genuine
  admitted-code deficiency — feed Card KP's trigger; anything else ⇒ a new
  finding, stop and report.
- PA4. **Falsifier — the milestone of the whole spec**: the 305 arm with
  CB-ADM + co-gate + DG + PA armed **emits > 0 epochs**, with the gate log
  showing the recovery path (healed > 0 → re-gate pass) or a clean pass, and
  AN confirming the remaining below-cutoff census = the 3 declared gauges.
  Zero mechanism invention: if emission still fails after DG + PA, STOP and
  report the residual census — do not improvise a fourth lever.
- PA5. If the heal proves insufficient (anchors flap epoch-to-epoch, healed
  count grows unboundedly, or PA4 fails on an anchor-reachability class), the
  pdp anchor-election threading is the follow-up — **reserved ruling** (it
  collides with decomposition 8b); spec it separately with PA's measurements,
  do not build it inside this card.

---

## 4. Card KP — Kaula/soft prior on admitted OSBs (CONDITIONAL second lever)

**Trigger** (build only if one fires, from DG/PA-armed runs): (a) PA3 finds
CB-dominated genuine deficiencies among the deep residual; or (b) the weak-real
families emit with sigmas too large to be useful downstream (define the bound
from the M4 consumer contract before judging — sigc-deweighted products that
never influence the consumer are "not useful").

**MUST** (if triggered):
- K1. Site: `NetworkSbasis::buildCodeBiasDatumRows` — one soft prior row per
  ADMITTED satellite code-bias column; σ from the physical OSB bound (tens of
  ns), stated in the report; default-OFF toggle (`network.cb_kaula_prior` or
  similar), byte-identical OFF.
- K2. **Anti-fragility placement** (inverting the rejected-alternative's
  failure mode): the prior's information contribution must land ≥ 2 decades
  ABOVE the armed cutoff of every config it ships in — compute and log the
  margin at startup; a prior that sits near the cutoff is the configuration
  the CB-ADM spec already rejected.
- K3. Declared-dim accounting audited: the clock-gauge directions carry ~zero
  CB weight (measured), so declared_dim must remain 3 — assert via AN on the
  armed run, and update the S-basis plan documentation if any prior row is
  datum-classed.

---

## 5. Card H — hygiene (rides the next CB-ADM/BD-DATUM commit)

- H1. Fix the stale comment at `ApplicationNetworkAssembly.cpp:859-869`: the
  phase side is NOT "co-gated to them [cbadm_reg_code_keys] below" — the
  co-gate lives upstream at state allocation and keys off the CENSUS-admitted
  set, not the per-epoch registered subset. Say exactly that, pointing at
  `MeasurementModel::ensureNetworkSharedStates`.
- H2. **Granularity-seam ruling (decided here, deliberately)**: the phase
  co-gate stays at CENSUS granularity. Per-epoch phase-state allocation
  churn would violate the anti-flap design and thrash eq34/carry/snapshot;
  the cost is a transient orphan window — an ADMITTED code key below 2 present
  stations for an epoch keeps its shared phase column while its code column is
  withheld. Make the window measurable: new run-end counter
  `n_cbadm_phase_orphan_key_epochs` ((key, epoch) pairs where an admitted code
  key was per-epoch-withheld while its same-signal phase key was shared).
  Measured no-op at 305 density; if a future sparse-tier run shows it nonzero
  and coincident with withholds, revisit with data.
- H3. HANDOFF erratum note: append a units clarification to
  `docs/HANDOFF_cb_admission_card.md` §2's A/B tables (per-dump vs
  across-dumps), or restate them unit-labeled in this card's report — either
  way the §0.1 reconciliation table above is the canonical presentation.

---

## 6. Validation harness

1. **Geometry rule (binding)**: efficacy claims run on the 305 storm config
   ONLY (`_maxnet_305_gpsgalbds_d15_5ep_*`); 50/120-station global subsets are
   the measured wrong test (all-constellation under-determination) and prove
   nothing either way. Iteration unit: 5-ep arm ≈ 17 min (3.2 min/ep + ~2 min
   ingestion). Stage a new armed config
   `_maxnet_305_gpsgalbds_d15_5ep_datum.json` = cbadm arm + `rank_gate_tol
   1e-12` + `phase_datum_prioritized/min_stations` + `trace_estimability`;
   the existing `_ctl` arm is the baseline.
2. **OFF gates**: all new toggles OFF ⇒ byte-identical vs the pre-change binary
   (a fast config suffices for the mechanism byte-id; the INC-8
   estimability-arm baseline covers the DG plumbing on the estim path).
   Suite green with the additive DG tests.
3. **Healthy-config guard**: the GPS+Gal 305 config (`_maxnet_305_gpsgal_d15_*`)
   with toggles OFF is byte-identical by (2); DG's armed value is NOT applied
   there (per-config placement, §2 DG3). If the user later wants a non-default
   tolerance on a healthy config, DG3's dump-first rule applies before arming.
4. **Graded emit leg (ON COMMAND — the original blocker's close-out)**: once
   PA4 emits, the BeiDou products become gradeable for the first time. On
   command, run the longer leg and grade datum-invariant quantities only,
   specifically: emitted BeiDou clock sanity vs the Jun-29 divergence
   signature (BDS |clock| abs-median was 28.6 km, worst 530 km, leaking ×3200
   into GPS — those statistics must land at the GPS/Gal metre class), GPS/Gal
   product quality unchanged vs the GPS+Gal-only baseline, and a held-out
   consumer sanity check (JPLM-class float). Report class-decomposed censuses
   (AN) per arm, never raw counts.
5. **Run mechanics**: Release-only, renamed exe per arm, name-scoped zombie
   pre-kill, LNK1104 ⇒ concurrent-build lock ⇒ retry; isolate exe+DLLs into a
   run dir (the measured exit-127 concurrent-build failure); copy `logs/`
   aside per arm before the next arm overwrites it.

## 7. Acceptance summary and sequencing

| Card | Type | Gate |
|---|---|---|
| AN (first) | tooling | reproduces HANDOFF numbers from archived logs; unit-labeled output |
| DG | config-gated shared-path | OFF byte-id + estim-arm baseline; armed: weak-real census bin empties, gauges still counted, over/under-specified counters visible |
| PA | config arming + visibility | heal targets deep nulls only; **PA4: 305 arm emits > 0 epochs** with census = 3 gauges |
| KP | conditional mechanism | trigger fires first; anti-fragility margin ≥ 2 decades; declared-dim audited |
| H | hygiene | rides the next commit; orphan-window counter live |

Sequencing: **AN → DG → PA (config) → combined 305 A/B (ctl vs datum arm) →
PA3 disposition → KP only if triggered → graded emit leg on the user's
command.** Commit logistics unchanged from the HANDOFF §8: hunk-level staging
on the shared TUs; nothing ships without the user's go.
