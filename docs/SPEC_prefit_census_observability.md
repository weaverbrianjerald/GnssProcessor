# SPEC — prefit census + structural fuse + topology ledger + flight recorder

**Status**: SPEC'd 2026-08-01 (the session that root-caused era-2); built and field-gated the
same day (§15–§18).  Known-answer gates (§12) from `runs/ft63_leg19_rhsat`.
**Memory context**: `project_ft63_fast_tier` (era-2 root cause + the rhs_attrib instrument),
`docs/SPEC_deweight_release_hygiene.md` (the cause-level fix; §12 declares the interaction discipline).
**Scope discipline**: every increment default-OFF, OFF structurally byte-identical, NONE
fingerprinted (observability / forward-only policy — the adaptive_iono_q / eq34-hygiene precedent),
so shared-snapshot replay legs stay A/B-legal.
**Provenance**: every `runs/<name>` path resolves under `D:\GnssProcessor_archive\2026-08-12\runs\`.

---

## 1. Motivation — the blind-spot stack this card closes

Era-2's injector — a steady **+1.88e5 m** prefit code innovation on (BSHM00ISR, G07), folded at
w≈2 every epoch for minutes — was invisible because:

1. Only POSTFIT σ̂₀ is monitored (P1-4).  Free ambiguities + free local states launder
   inconsistency out of postfit by construction (the P1-4 log line says the phase pool is
   identically zero).
2. `raim_within_epoch`, the one pre-solve row screen, is carry_warm-gated — inert with leveling OFF.
3. Estimation-topology changes (the deweight gate re-folding a station's SH-tie rows) were not
   events; the only trace was an aggregate counter.
4. No state-birth acceptance: a fresh single-band arc was seeded ~188 km wrong and nothing checked
   it.  INC-3 is its tripwire and locator.
5. Attribution tooling had to be BUILT mid-investigation.  It now exists
   (`network.rhs_attrib_tow`, filter/NetworkRhsAttrib, closure 5e-11); INC-5 fires it
   automatically at the moment of any future discharge.

Failure class: **(absorbing DOF) × (topology valve) × (persistence)**.  This card is the
symptom- and observability-side defense; SPEC_deweight_release_hygiene is the cause-side fix for
the deweight valve; §13 governs the remaining valves.

## 2. INC-0 (prerequisite): hoist the row classifier

`classifyRow` + the shared-column family/label pass lived in an anonymous namespace in
`src/core/filter/NetworkRhsAttrib.cpp`.  Hoist BOTH into `include/filter/NetworkRowClassify.hpp`
(header-only or + .cpp with manual CMake entry), consumed by NetworkRhsAttrib AND every increment
below — one classifier, one truth; the census and the attribution must never disagree about what a
row is.  Classes unchanged: `obs_code`, `obs_phase`, `obs_other`, `iono_sh_row`, `pivot_pin`,
`local_pin_row`, `cpl_row`.  Regression gate: re-run §12's attribution known-answer leg after the
hoist and require byte-identical attribution output.

## 3. INC-1: prefit innovation census (`network.trace_prefit_census`, bool, default false)

**Metric — station-common-mode-removed, NOT raw** (user-raised, load-bearing).  A raw |y| of 188 km
is not inherently absurd: receiver clock offsets/steps are ms-scale, and URUM/INEG on this very leg
take clean 1 ms steps (|y| ≈ 3·10⁵ m on EVERY row of the station for one epoch, absorbed exactly by
the white clock state).  The census metric is therefore per-station per-row-type robust-centred —
effectively a between-satellite single difference, receiver-clock-free by construction:

- Per station, per type t ∈ {code, phase, other}: m_t = median of that station's type-t y_i
  (require n ≥ 4, else skip the type this epoch and count the skip).
- s_i = |y_i − m_t|·√w_i.  BSHM G07 stays at s ≈ 2.6·10⁵ (its station's code median is ~10 m); a
  whole-station clock step cancels to ~0.  `iono_sh_row` rows carry no receiver clock — raw y, no
  centring.
- DATA classes only (`obs_code`, `obs_phase`, `obs_other`, `iono_sh_row`); pins/coupling excluded
  (a healthy pivot pin runs s≈1e7 by design and would drown the census).
- ⚠ **SUPERSEDED DESIGN INTENT** — this said the census "rides the SAME row walk
  `networkPostfitSigma0` already does".  **As landed it is its OWN seam** (§16 deviation 1, with
  the three reasons); it must run when `trace_postfit_sigma0` is OFF, because it is the always-on
  tripwire and P1-4 is not.  It still reads `w`, `y` from `StationAssembly`, and the median pass is
  one extra O(rows) sweep per station.  (`networkPostfitSigma0` now begins at
  `src/core/filter/NetworkFrameDiag.cpp:171` in a 367-line file; the spec's
  `NetworkFrameDiag.cpp:264-337` has drifted independently.)
- Emit per class into `network_epoch_status.csv`: max_s, p99_s, n_s_gt_10, n_s_gt_1000.
- When max_s > `prefit_event_sigma` (double, default 1000): ONE `network_events.csv` row (tier
  PREFIT, event `prefit_exceedance`) naming (station, sat, class, s, y, m_t, w), rate-limited to one
  event per (station, sat, class) per `prefit_event_holdoff_ep` (default 20) so a parked cell logs
  its identity persistently but not 120×/hour.

This is the always-on tripwire: era-2 would have printed `PREFIT obs_code BSHM00ISR G07 s=2.6e5`
every epoch of the park.  **Scope note**: the census flags magnitude; it cannot distinguish "parked
garbage" from "storm extreme on a well-checked cell".  That discrimination is the companion
redundancy card's cross-flag (`docs/SPEC_redundancy_census.md`).  Census = free trigger;
redundancy = targeted verdict.

## 4. INC-2: structural fuse (`network.prefit_fuse_sigma`, double, default 0 = off)

DATA-class rows only, on the INC-1 centred metric — the centring is what makes the fuse safe
against legitimate receiver clock events.  Also skip rows with w ≥ 1e6 (pin/constraint weight
regime) and skip when the station's type-median had n < 4.  If s_i > S_fuse:

- w_eff = w_i · (S_fuse / s_i)² — a continuous cap so the row contributes at most an
  S_fuse-equivalent standardized load.  NOT a drop (prefer-fallback directive): order-preserving,
  smooth, sign information retained.  Single-frequency cells are NOT excluded as a class — a 10³σ
  common-removed excursion is ≳1800 TECU/30 s even at code noise, physically impossible ionosphere,
  so the cap cannot bite genuine single-freq storm dynamics.
- Counter + PREFIT-tier event `prefit_fused` (identity + original s).
- Optional composition: `network.prefit_fuse_require_uncheckable` (bool, default false) — fuse only
  rows the redundancy census marks r < ε, i.e. act only where the misfit is invisible to every
  residual test.  Requires the redundancy census at stride 1 or on-trigger escalation; the loader
  WARNs if required-uncheckable is set without it.
- Placement: in the produce phase where `R.w` is finalized, BEFORE `est.addStation`
  (ApplicationNetworkAssembly / NetworkSolveGlue row-build); the implementing session verifies the
  single site where all data rows' weights are last touched.

SOLVE-AFFECTING when armed (unlike every other INC): full falsification-arm discipline in §12, OFF
path structurally byte-identical.  It is NOT robustness tuning — Huber/deweight own the 3–10σ
regime; the fuse exists for the 10³σ+ regime where a "measurement" is a wiring/seed/model fault.
It also bounds the discharge magnitude of ANY future parked-garbage release, whatever the valve.
Default when armed: 1e3 (era-2's cell sat at 2.6e5; the storm sea tops out ~40σ — three decades of
margin each side).

## 5. INC-3: seed-birth tripwire (`network.seed_birth_check_sigma`, double, default 0 = off)

Measure-only.  At the epoch a per-LOS state is BORN (fresh ambiguity / fresh slant-iono for a
(station, sat) absent last epoch — detectable in the census walk via a ctx-side presence map, or by
reusing the AnchorHygiene age map when armed; do NOT depend on hygiene being armed), apply the
INC-1 census to the born cell's rows at the tighter `seed_birth_check_sigma` (default
recommendation 100).  Event tier PREFIT, `seed_birth_exceedance`, with the seed values (amb x_lin,
slant x_lin) in the detail — a correct self-seeded birth has s ≈ 0 by construction, so ANY fire
localizes a seed-path defect to an exact epoch and site.

Deliverable beyond the guard: the locator for the open **188-km seed co-defect**
(SPEC_deweight_release_hygiene §6).  ⚠ **SUPERSEDED BY §17 GATE 6: it is NOT a seed-path defect.**
INC-3 fired at the arc's first epoch and the trace against products showed the initializer
faithfully seeding a **corrupt C1C pseudorange**; the class was handed to
`docs/SPEC_measurement_admission.md`.  What INC-3 bought stands — it dates the onset to the arc's
FIRST epoch, ruling out mid-arc poisoning, so neither the seed card nor the hygiene card's INC-2
park bound owns it.

## 6. INC-4: topology ledger (`network.trace_topology`, bool, default false)

Estimation-topology changes become EVENTS with identities (tier TOPOLOGY in `network_events.csv`;
no new stream):

- Per station: SH-tie row-set presence delta (the deweight valve — count of `iono_sh_row` rows vs
  previous epoch; event on any change, with the station's gate weight in the detail).  Era-2's
  entire story was one such event ("BSHM +1 tie row, w 0→0.25") that nothing logged.
- Per class per station: data-row count deltas beyond satellite rise/set churn — emit only when a
  (station, sat) cell's row-KIND set changes while the sat stays present (band gained/lost,
  code-only↔code+phase), reusing INC-3's presence map.
- Shared-column registry deltas: columns added/removed vs previous epoch (label + reason when
  cheaply known), so CB:G07:C1C-class admission flips become visible.

Ctx-side previous-epoch structure summaries live in `NetworkCarryState` (counts + small maps, NOT
matrix copies).  Run-end totals per event kind (invariant #4).

## 7. INC-5: flight recorder (`network.rhs_attrib_on_trip`, bool, default false)

- The estimator capture recorder (`est.attribArm`) is armed EVERY epoch (~10 vectors × n_shared
  doubles, cleared each epoch — negligible), so a trip can attribute the CURRENT epoch.
- Trigger sites (first trip per era, one-shot latch until the era clears): SH-gate ENGAGE, A4 σ̂₀
  withhold, clock-runaway trip, a00-band WARN.  All four already have log/warn sites; the trigger
  call is one line each.
- On trigger: run `networkRhsAttribution` for this epoch (bypassing the `rhs_attrib_tow` window;
  same dumps, same closure check) and — when `network.rhs_attrib_on_trip_prev` (bool, default true)
  — also dump the RETAINED previous epoch (deep copy of asm_res + captures + the kernel's
  per-station blocks needed by the reduction; ~15–30 MB retained at 63 stations, ~150 MB at 305 —
  document, and auto-disable prev-retention above `rhs_attrib_prev_max_stations`, int, default 128).

The pre-trip frame is the valuable one (era-2's 329130 "before" frame is what exposed the flip).
Armed, the NEXT discharge of any class names its own injector in the run where it first happens.

## 8. Wiring sites (verified this tree, Aug-1; re-verify at implementation)

- Census/fuse row walk + classifier: **its own seam, `filter/NetworkPrefitCensus` →
  `networkPrefitCensus` (SB_FRAMEDIAG)**, not a rider on the postfit sweep (§3, §16 deviation 1);
  classifier from the INC-0 hoist.
- Fuse weight site: produce-phase R.w finalization (ApplicationNetworkAssembly produce / SolveGlue
  row build — locate the single last-touch site).
- Epoch-status columns: the EpochStatusRow writer (`NetworkStashStatus` /
  `logging/NetworkRunTraces`), pattern of the P1-4 columns.  Events: the `emitEvent` path used by
  PHASE_DATUM/CB_DATUM tiers.
- Trigger sites: SH gate engage (the "Network (SH gate): ENGAGED" warn), A4 withhold,
  clock-runaway, a00-band WARN — all in the T1a-health / gate blocks of the driver loop.
- Counters: `NetworkServiceCounters` (aiq_*/hyg pattern); run-end lines in
  `filter/NetworkTeardown.cpp`.
- Config: `NetworkConfig` fields near `rhs_attrib_tow` (include/config/GnssConfigTypes.hpp:1022
  area); loader lines near the rhs_attrib block in `GnssConfigLoadNetwork.cpp` (~:1275) with arming
  info lines; INERT-guards where increments depend on each other (INC-5 requires the attribution
  machinery — always present; INC-3 shares INC-1's walk, so arming it implies the walk runs even if
  `trace_prefit_census` is off — say so in the loader line).
- CMake: manual entries for any new .cpp (NetworkRowClassify if not header-only; tests file).

## 9. Knobs (all default-OFF; none fingerprinted)

| knob | type | default | effect |
|---|---|---|---|
| `network.trace_prefit_census` | bool | false | INC-1 columns + PREFIT events |
| `network.prefit_event_sigma` | double | 1000 | INC-1 event threshold |
| `network.prefit_event_holdoff_ep` | int | 20 | INC-1 per-cell event rate limit |
| `network.prefit_fuse_sigma` | double | 0 (off) | INC-2 weight cap (SOLVE-affecting) |
| `network.seed_birth_check_sigma` | double | 0 (off) | INC-3 birth tripwire (measure-only) |
| `network.trace_topology` | bool | false | INC-4 TOPOLOGY events |
| `network.rhs_attrib_on_trip` | bool | false | INC-5 flight recorder |
| `network.rhs_attrib_on_trip_prev` | bool | true | INC-5 pre-trip frame retention |
| `network.rhs_attrib_prev_max_stations` | int | 128 | INC-5 retention auto-disable |

**Promotion outcome (decided; this section used to state the candidacy).**  The Aug-1 delta was
taken on the FT63 leg-19 gates: CLAUDE.md's standard armed set carries an **"Observability, prefit
tier (promoted Aug-1)"** containing `trace_prefit_census`, `trace_topology` and
`rhs_attrib_on_trip`, and `docs/CONFIG_promotion_ledger.md:124-126` records `trace_prefit_census` +
`trace_topology` → **ON**, `rhs_attrib_on_trip` → **SCOPED** (memory, not accuracy),
`prefit_fuse_sigma` → **KEEP_OFF** pending total_IF grading and a second event.  CLAUDE.md states
explicitly that the FUSE is **NOT** in that set.  No loader-default flips (no-promotion directive)
— arming goes into the config template (`scripts/p2_make_guard_configs.py`, which cites this §9).
The birth check joins only after its own ON validation.

## 10. Telemetry (invariant #4)

Every INC: a run-end line with totals, plus an explicit dead-wiring WARN when armed and zero
opportunities were counted (census: zero rows walked; recorder: armed but no trigger site ever
consulted the latch — count consultations, not just fires).

## 11. Unit tests

New file `tests/test_prefit_census.cpp` (ONE suite, unique name): (1) census math s=|y|√w per class,
pins/coupling excluded; (2) fuse cap w_eff formula + exclusion rules + OFF byte-identity
(structural: no allocation when off); (3) birth detection via presence map
(present→absent→present = birth); (4) event rate limiting; (5) topology tie-presence delta events;
(6) recorder latch: one fire per era, re-arms after era clears.  The attribution closure already
self-tests INC-5's dump path.

## 12. Field validation — KNOWN-ANSWER gates (standing harness, snap_20 leg)

Controls on disk: `runs/ft63_leg19_rhsat` (attribution baseline: break at 5738.6, BSHM obs_code C00
0.002→+6493, closure 5e-11), `runs/ft63_leg19_base`.  Config family: clone `_ft63_leg19_rhsat.json`
per arm, distinct run dirs (`ft63_leg19_census`, `_fuse`, `_topo`, `_trip`, `_birth18`).

**Interaction discipline (pre-declared)**: the deweight-release-hygiene card (concurrent) suppresses
the same break.  Every gate below runs with the hygiene knobs OFF, and hygiene's gates run with this
card's knobs OFF (census and topology, being measure-only, MAY be armed in hygiene legs after their
own OFF-identity passes).  Combined arms are a separate later leg.  Never grade either card on a leg
with the other armed first.

As-run verdicts and evidence are in §17, not repeated here.

| # | gate | KNOWN ANSWER it is graded against |
|---|---|---|
| 1 | Smoke (10-ep) | knobs parse, arming lines print, OFF twin byte-identical |
| 2 | **INC-1 census** | MUST name (BSHM00ISR, G07, obs_code) at **s ≈ 2.6e5** from the first processed epochs (~328020) through 329130, and (BSHM, G16, obs_code) at **s ≈ 7.3e4**; PREFIT events carry the identities; solve byte-identical to control |
| 2-neg | **clock-step immunity** — the false-positive test for the common-mode removal, on data already in the leg window | at TOW 329340 URUM00CHN takes a clean **1 ms** receiver clock step (raw \|y\| ≈ **3e5 m** on every row; verified in the raw RINEX and the rows dump).  The centred census MUST stay quiet — URUM max_s at the storm-sea level, **no PREFIT event** |
| 3 | **INC-4 topology** | TOPOLOGY event "BSHM tie-row count +1, gate weight 0→0.25" **at exactly 329160**; no event spam during quiet epochs (count bounded) |
| 4 | **INC-5 recorder** | `rhs_attrib_tow` DISARMED, `rhs_attrib_on_trip` armed → fires at 329160 (SH-gate engage), dumps 329160 **+ the retained 329130** frame; class totals match the manual baseline (BSHM obs_code **+6493 / 0.002**) within numerical noise; **closure ≤ 1e-6 rel** |
| 5 | **INC-2 fuse** (solve-affecting — own arm) | fuse at **1e3** caps the BSHM cells; the 19:26:00 discharge suppressed or reduced below the SH-gate band (**a00 < 150 through 19:36**), `prefit_fused` ≈ parked cells × epochs; QC otherwise flat vs control (D5 ±few %, rx-CB withholds ~14-15 %, σ̂₀ 1.00).  ⚠ **If the break survives at full size the fuse missed the fold site — a FAIL of PLACEMENT, not of the concept; find the second site.** |
| 6 | **INC-3 birth**, from `snap_archive/snap_18_*.bin`, offset 18.1125, duration ≥ 2400 s (covers G07@BSHM's ~18:34 mask crossing) | either `seed_birth_exceedance` fires at the birth epoch naming the seed values, or stays silent (mid-era poisoning) — **either outcome is a pass; record which**.  Not firing on ANY birth all leg long while births are counted nonzero is the **dead-wiring FAIL**.  ⚠ Harness correction in §17: the leg actually runs from `snap_19` |
| 7 | product claims | **total_IF only** (promotion-ledger convention) |

## 13. Governance — proposed standing invariant #7 (needs user sign-off, then CLAUDE.md)

⚠ **The heading is superseded design intent: the invariant WAS adopted — see §18.**  It is kept
verbatim because 401 inbound citations pin `§13`; read the "needs user sign-off" clause as history.

> **7. Any mechanism that changes which information flows into which states —
> gates, deweights, admissions, coverage/rank drops, datum handovers, holds,
> withhold eras — is an EVENT at BOTH edges, with counters and identities.
> The DECOUPLING edge must bound what the decoupled states can accumulate;
> the RE-COUPLING edge must re-validate or re-seed what it reconnects.
> "The weight ramps smoothly" is not an exemption — era-2's valve ramped at
> the configured rate limit and discharged anyway.**

Valve inventory to audit against it (status as of Aug-1 — **re-audit before quoting a cell**):

| valve | decouple edge | re-couple edge | status |
|---|---|---|---|
| iono deweight gate | counters only | **nothing** | fix: SPEC_deweight_release_hygiene |
| SH health gate (emission) | event ✓ | era-exit: **nothing** (181 m scar) | OPEN — vanish-return catalog |
| eq34 anchor handover | events ✓ | hygiene card ✓ | closed (latent-hazard guard) |
| CB admission / co-gate | events ✓ | partial (KP prior) | audit |
| D5 innov withhold → refold | counters | refold un-validated | audit |
| coverage gates (phase datum) | drop path ✓ | first-sighting exempt | audit (hygiene deviation-4 notes) |
| station vanish-return | **nothing** | **nothing** (phantom pin) | OPEN — S-system catalog |
| warm-resume seam | RTB-7d event ✓ | resume_warmup_vet UNBUILT | open deviation (flicker §8) |
| RAIM withhold / re-entry | events ✓ | re-entry un-validated | audit |
| eclipse exclusion (`exclude_eclipsing_satellites`) | **nothing** (no per-event log; windows only inferable wire-vs-visibility) | **wrong thing validated** — cta_recouple checks the post-re-solve value; clock-disc jump test GAP-BLIND across the absence | **OPEN — measured 2026-09-01**: ~55-min exclusions re-realize the clock↔C5Q split by 6.3–6.7 m at re-admission, held stiff for hours, URA silent (SPEC_synthetic_minimum_network §18.1) |

The audit is cheap (read each site against the invariant, one short card per NO cell, cheapest
first).  Keep this table here until the user wants it promoted to its own doc.

## 14. Deferral triage (pre-declared)

- Full MDB/testability statistics: `docs/SPEC_redundancy_census.md` (companion card, spec'd same
  day) — INC-1's census is the cheap tripwire; redundancy answers "where CAN'T the census see".
- Consumer/RT paths: producer-only for now (port the census pattern after producer validation).
- Per-row census persistence (a rows CSV every epoch): rejected — events + scalars suffice; the
  flight recorder produces full rows on demand.

---

# IMPLEMENTATION RECORD (built 2026-08-01, Opus session)

## 15. What landed, and where

| INC | code | knob(s) |
|---|---|---|
| 0 | `include/filter/NetworkRowClassify.hpp` + `.cpp` — `NetColFam`, `NetRowClass`, `SharedColumnMap`, `buildSharedColumnMap`, `classifyRow`, `rowClassName` | — |
| 1 | `filter/NetworkPrefitCensus` → `networkPrefitCensus` (SB_FRAMEDIAG) | `trace_prefit_census`, `prefit_event_sigma`, `prefit_event_holdoff_ep` |
| 2 | `networkPrefitFuseStation`, called from the SERIAL pre-`addStation` loop in `ApplicationNetworkAssembly.cpp` | `prefit_fuse_sigma`, **`prefit_fuse_classes`** (new — §16), `prefit_fuse_require_uncheckable` |
| 3 | same walk as INC-1 (presence map in `ctx.carry.prefit_present_prev`) | `seed_birth_check_sigma` |
| 4 | same walk as INC-1 (`topo_prev_*` baselines in `ctx.carry`) | `trace_topology` |
| 5 | `networkRhsTripEdge` + `networkRhsAttribTrip` + `NetworkAttribFrame` in `filter/NetworkRhsAttrib` | `rhs_attrib_on_trip`, `rhs_attrib_on_trip_prev`, `rhs_attrib_prev_max_stations` |

Trigger sites, all four wired and consulted UNCONDITIONALLY every epoch they evaluate (passing
`false` is what clears the era — a site that only called on its true branch would be a latch that
never re-arms, and `rec_consults` is the invariant-#4 proof it is wired): SH-gate engage
(`NetworkSolveGlue`), A4 σ̂₀ ceiling breach (`NetworkSolveGlue`), clock runaway (`NetworkSolPrep`),
a₀₀-band exit (`NetworkShHealth`).

Run-end telemetry: `filter/NetworkTeardown.cpp`, one block per increment, every zero-finding line
naming WHICH kind of zero it is.  Unit tests: `tests/test_prefit_census.cpp` (11 cases, one suite).
Harness: `scripts/pc_make_arms.py`, `scripts/pc_run_arms.ps1`, `scripts/pc_gate_verdict.py`.

## 16. Deviations from the spec, recorded

1. **The census is its OWN seam, not a rider on `networkPostfitSigma0`** (§3, §8 said otherwise).
   Reasons: it must run when `trace_postfit_sigma0` is OFF (it is the ALWAYS-ON tripwire, P1-4 is
   not); INC-3 and INC-4 need the same walk and do not belong inside a σ̂₀ statistic; modifying a
   validated diagnostic to host three new ones risks its output.  Cost: one extra O(rows)
   classification pass per station, which the postfit walk's `reconstructStation` dominates by
   orders of magnitude.

2. **`network.prefit_fuse_classes` (new knob, default `"code,sh"`)** narrows the FUSE — not the
   census — from §4's "all DATA classes".  This is the one place the spec applied literally would
   have done harm, and the evidence is the leg-19 smoke:

   - a PHASE row's prefit innovation contains its arc's FREE ambiguity linearization point.  In
     STEADY state that is small (BOGT00COL at TOW 329130: phase median 1.12 m, range −2.6…4.2 m,
     against a code median of 20.8 m) and nothing approaches the fuse;
   - on a WARM-RESUME or cold-start epoch every arc is re-seeded at once and the pool's centre is
     whatever the seeding produced (MRO100AUS two epochs after resume: phase median −4248 m, giving
     s ≈ 3.8e5 on a row whose innovation is exactly 0.0).  At `S_fuse = 1e3` the cap would be
     ×7e-6 on every freshly-risen arc's phase rows — a systematic convergence penalty on healthy
     data, and the FT63 harness is entirely warm-resume replay legs.

   `obs_code` and `iono_sh_row` have no free per-row absorber (receiver clock and receiver code bias
   are shared ACROSS satellites, so a single-satellite excursion survives the centring), and the
   era-2 injector is an `obs_code` cell — the designed target is fully covered by the default.  The
   cost of the restriction is COUNTED, not assumed: `fuse_skip_class` is the exposure and
   `fuse_would_class` is what the cap gave up, both on the run-end line.  `"all"` restores §4's
   literal domain for a deliberate arm, and arming it logs the hazard.

2b. **INC-1 yields born cells to INC-3** (§3 did not anticipate it).  The centring works exactly as
   designed — CAS100ATA's sixteen phase rows at TOW 329130 all sit at 177037.1…177037.6 m (a
   0.59 ms receiver-clock offset the phase model has not absorbed) and every one centres to ~0; the
   median |station phase level| across the network is 0.46 m.  What does NOT centre is a cell BORN
   this epoch inside such a station: the initializer seeds a fresh arc so its OWN innovation is ~0,
   which against an established level of 115 km (INEG00MEX) reads as **s ≈ 1e7**.  First measured
   cut: **10077 exceedances / 666 events in one hour**, essentially all healthy births, the run
   maximum (CAS100ATA|G32|obs_phase, **3.35e7**) among them.  A tripwire that cries wolf 666 times
   an hour is not a tripwire, and the era-2 signal would have been buried in it.

   So INC-1's statistic AND its events cover ESTABLISHED cells only, and born cells go to INC-3.
   Counted at both ends (`rows_born_deferred`, `cells_born_deferred`) and reported at run end
   **whether or not INC-3 is armed**, so "the census went quiet" and "the census deferred N births
   to a disarmed birth check" cannot read the same.  Births are detected whenever the walk runs, not
   only under `seed_birth_check_sigma`.  The known answers survive: G07@BSHM's arc is born ~18:34,
   before the leg-19 window, so it is an established cell throughout and still names itself.

2c. **`network.prefit_event_classes` (new knob, default `"code,sh,other"`)** narrows INC-1's EVENT
   channel — not its columns, not its statistic.  **Deviation 2b was the right idea aimed at the
   wrong mechanism**: excluding births moved 10077 exceedances to **10050** and 666 events to
   **665** (births were 27 cells / 832 rows).  The settled distribution, measured at threshold 1e3
   over 63 stations × 121 epochs:

   | class | events | stations | verdict |
   |---|---|---|---|
   | `obs_code` | **6** | **1** | every one is (BSHM00ISR, G07/G16), s growing 1.90e5 → 2.69e5 as the park intensifies.  Zero false positives. |
   | `iono_sh_row` | 51 | 44 | floor at s ≈ 1.2–2.4e3, with ONE 40× outlier — (BSHM00ISR, G03) at s = 9.47e4 at TOW 329190, the epoch after the discharge.  Usable. |
   | `obs_other` | 0 | 0 | — |
   | `obs_phase` | **608** | 55 | unusable, and not because of births. |

   The phase mechanism, measured rather than assumed: a phase row's prefit innovation is
   **arc-specific**, so a station's phase rows are not a common-mode population and their median is
   not a centre.  CAS100ATA's phase median walks 177037 → 150500 → 84184 → 17974 m within one leg
   while individual rows sit 133 km away from it.  Centring cannot fix that.

   So phase leaves the EVENT domain (`events_class_skipped` counts the suppression, the run-end line
   names the domain, arming phase logs the hazard), while the per-class COLUMNS keep reporting all
   four — free, measure-only, genuinely informative about ambiguity state.  Event volume at the
   default: 57/hour, with the two true findings the top hits by 40×.  Deviation 2b stands on its own
   merits (a birth's innovation is arbitrary by construction and belongs to INC-3) but is a small
   effect, not the fix.

3. **`prefit_fuse_require_uncheckable` is parsed and force-disabled with a WARN.**  §4 makes it
   conditional on the redundancy census, which HAS since landed
   (`docs/SPEC_redundancy_census.md`) — but that census is POST-solve and the fuse acts
   PRE-`addStation`, so the cross-flag would have to reach the fuse through a persistent
   uncheckable-cell set that neither card builds.  Failing loudly beats silently fusing on magnitude
   alone under a knob whose name promises otherwise.  The composition also wants care in the other
   direction: a freshly-seeded arc's row is uncheckable BY CONSTRUCTION (`r ≈ 0` is what "my own
   free state absorbs me" means), so a naive `require_uncheckable` would select exactly the benign
   births of deviation 2.

4. **INC-0's classifier returns an enum**; `rowClassName` maps it back to the original string
   literals, so every attribution dump byte is unchanged.  The attribution body was re-based onto a
   `NetworkAttribFrame` so the SAME code path serves the live epoch and INC-5's retained one; the
   live path stores POINTERS to the caller's assemblies (no copy), the retention path deep copies.
   Regression gate: gate 0 below.

5. **The A4 trigger consumes its own trip inline** (`NetworkSolveGlue`, right before the withhold's
   `SkipEpoch`).  That epoch never reaches SB_FRAMEDIAG, so a latch-only design would have made the
   A4 trigger permanently attribute the WRONG (next) epoch.  The same call retains the withheld
   epoch as the next trip's "before" frame.

6. **`rec_consults` counts consultations, not fires** (§10 asked for exactly this).  Measured
   4/epoch on the smoke leg = all four sites live.

7. **Measured retention cost**: 26.4 MB per retained frame at 63 stations — inside §7's "15–30 MB"
   estimate, so `rhs_attrib_prev_max_stations = 128` stands.

## 17. Field validation — as run

Build: same-build Release, renamed exe (`gnss_processor_pc.exe`) per the concurrent-session lock
rule.  Arms from `scripts/pc_make_arms.py` over `_ft63_leg19_rhsat.json`, each with its OWN run
directory and its OWN copy of the seed snapshot (the producer overwrites `state_snapshot_path` at
its first hour boundary — sharing one file between arms would re-seed later arms from the earlier
arm's output).  §12's interaction discipline holds: no arm carries a deweight-release-hygiene knob.
Verdicts: `python scripts/pc_gate_verdict.py`, which refuses to compare a run still writing (it
checks for the producer's exit marker) — grading mid-sweep produced four false byte-difference FAILs
before that guard existed.

**Harness correction, recorded**: §12.6 pairs `snap_18` with `start_offset_hours = 18.1125`.  The
archive's `snap_N` is written ~30 s before offset `(N−1).1125` — which is why §12's main leg pairs
`snap_20` with 19.1125 — so `snap_18` is 3690 s stale there and, with `state_snapshot_require` on,
the leg hard-aborts (0xC0000409).  The birth leg uses `snap_19`.  The abort is the RTB-7d guard
working as designed: a stale resume fails loudly instead of silently cold-starting.

### Results (63 stations, 121 epochs, TOW 328020–331650 unless noted)

| gate | verdict | evidence |
|---|---|---|
| 0 INC-0 hoist | **PASS** | all six attribution dumps (`attrib_`/`rows_`/`vec_` at 329130 + 329160) byte-identical vs `runs/ft63_leg19_rhsat` — the classifier hoist AND the `NetworkAttribFrame` re-basing change nothing |
| 1 smoke | **PASS** | knobs parse, arming lines print, solve byte-identical, all 4 recorder sites consulted (44 consultations / 11 epochs) |
| 2 INC-1 census | **PASS** | names (BSHM00ISR, G07, obs_code) s ∈ [1.90e5, 2.69e5] (spec: ≈2.6e5) and G16 ∈ [6.76e4, 7.35e4] (spec: ≈7.3e4); solve byte-identical to the same-build control |
| 2-neg clock step | **PASS, with verified opportunity** | URUM00CHN's G16 pseudorange jumps **+274 166 m** at 19:29:00 (≈1 ms after removing the −25.5 km/epoch trend, confirmed in the raw RINEX); **0** URUM PREFIT events there, while BSHM reads 2.77e5 in the same epoch |
| 3 INC-4 topology | **PASS** | BSHM tie-rows **+1 at exactly TOW 329160** (w 1→0.75); 355 events / 102 epochs, mean 3.5, worst 18 |
| 4 INC-5 recorder | **PASS** | with `rhs_attrib_tow` DISARMED the recorder self-fired at the SH-gate engage and dumped 329160 + the RETAINED 329130; BSHM `obs_code` C00 **+0.001894 → +6493** against the manual baseline **0.002 → +6493**; closure **4.5e-11** (≤1e-6 required); 401 consultations / 2 era trips; 25.4 MB per retained frame |
| 5 INC-2 fuse | **PASS** | below |
| 6 INC-3 birth | **PASS — branch closed** | below |

**Gate 3 found more than the spec predicted.**  §12.3 expected one event, "BSHM tie-row count +1,
gate weight 0→0.25".  The ledger shows *two* events one epoch apart: BSHM +1 tie row at 329160 with
w **1→0.75** (deweighting beginning), and the w-0.25→0.5 **release** of SAVO00BRA / OAFA00ARG /
POVE00BRA at 329190 — the same three stations the control leg carries as `n_dw_zero = 3` through
329130, collapsing to 0 at 329160.  The re-coupling edge fired 13 times over the leg and **every one
would have been silent** without the zero-baseline fix (a station with no tie rows used to drop out
of the ledger, so its return read as a first sighting).  That is proposed invariant #7's "both
edges" requirement, instantiated on the event it was written for.

**Gate 5 — the fuse suppressed the era-2 discharge outright**, at `prefit_fuse_sigma = 1e3`, domain
`code+sh`:

| | control | fuse |
|---|---|---|
| SH gate at 329160 | ENGAGED, a₀₀ = **5738.6 TECU** | *no engage*; every epoch through 329400 `emitted` |
| σ̂₀_pf at 329160 | 1.7473 | 0.0656 (= its pre-break level) |
| `n_dw_zero` at 329160 | 3 → **0** | stays 3 |
| withheld epochs, run | 83 | 72 |

171 rows capped out of 383 968 screened (0.045 %), all on the two known parked cells plus SH ties;
deepest cut w_eff/w = 1.16e-5.  **The domain restriction was material, not cosmetic**: 17 323
out-of-domain rows *would* have been capped — 143/epoch of healthy phase rows.  Caveats: ONE
solve-affecting arm against ONE known event; the gate still engages later at 329490 (a₀₀ =
−5708.6), so the fuse removed one discharge, it did not make the run clean; and **no
product-accuracy claim is made** — §12.7 wants total_IF and that has not been graded.

**Gate 6 closed the open branch — but NOT the way §5 predicted.**  Exactly TWO birth exceedances
land in `obs_code` — the only class where the statistic means anything — and both are BSHM:

```
327030  G07  BSHM00ISR  s=1.16e5  obs_code  y=198182  m=1.36  w=0.343  slant=0
327210  G16  BSHM00ISR  s=5.70e4  obs_code  y= 97548  m=2.13  w=0.342  slant=0
```

At the epoch its arc is BORN (TOW 327030 = 18:50:30 — §12 gate 6 guessed ~18:34) the cell is already
wrong.  §5 expected a SEED-PATH defect.  **It is not one.**  Traced against the products directly,
with no producer code in the loop:

| check | result |
|---|---|
| BSHM between-satellite residual (G07 − G03), C1C, vs SP3+CLK | **+198 195 m** |
| DJIG00DJI, same epoch, same two satellites, same signal | **+8.0 m** |
| does BSHM's reported G07 range match ANY other GPS satellite's geometry? | no — nearest is +198 km off |
| BSHM residual, all 13 GPS sats, receiver clock removed via G03 | −12.5…+19.1 m for eleven of them; **G07 +198 195**, **G16 +99 872** |

So orbit product, clock product, producer model and state initializer are all fine: the initializer
faithfully seeded a **corrupt C1C pseudorange**.  BSHM's receiver reports G07 at +198.2 km and G16 at
+99.9 km (0.661 ms and 0.333 ms of light time, ~2:1, drifting ~−4.8 m/s); the exact receiver failure
mode is not determinable from the RINEX alone.  What INC-3 bought still closes a branch: it dates the
onset to the arc's FIRST epoch, **ruling out mid-arc poisoning**, so neither the seed card nor the
hygiene card's INC-2 park bound owns this.

**This wanted a new owner: measurement admission — and it got one,
`docs/SPEC_measurement_admission.md`.**  The producer accepted a pseudorange 198 km from its own
orbit+clock products with no mechanism that could notice, because the only cell able to absorb it was
unconstrained.  `docs/SPEC_rinex_zero_sentinel.md §7` is RETRACTED (do-not-build; its motivating
mass-dropout observation was a parsing artifact) and its retraction note explicitly REOPENED the
era-1/era-2 root cause; the admission card owns that reopened question.

**Single-band is not the cause — it is the amplifier.**  G21 is ALSO C1C-only at BSHM in the same
epoch and reads −10.1 m: perfectly healthy.  Single-band removes the redundancy — 1 code + 1 phase row
against 2 free per-LOS states (slant iono + ambiguity) is r ≈ 0, so the 198 km is absorbed EXACTLY
into the slant-iono state and no residual test can reach it (Baarda: MDB → ∞ as r → 0).  The deweight
gate then cuts BSHM's SH ties, and the re-coupling at 329160 ties ~1.9e5 m of parked "ionosphere" to
the global field → a₀₀ = 5738 TECU.  That G21 row is the cleanest argument for the prefer-fallback
directive and for the redundancy card's insistence on BOTH halves of the cross-flag: excluding
single-frequency cells as a class would discard a good satellite to catch two bad ones.  The station
stays too — seven of BSHM's satellites are clean at 6–20 m, and the fuse capped 171 rows over the whole
leg while leaving ~30 healthy rows per epoch in the solve.

### Cost

SB_FRAMEDIAG net, 121 epochs at 63 stations: control 679 ms → census-only 1598 ms = **7.6 ms/epoch,
0.4 % of the epoch**.  The recorder (retaining a 25.4 MB frame every epoch) is the expensive half of
the combined 39 ms/epoch measured on the smoke leg.  Signal-to-noise of the code channel at TOW
329220: p99 = 38.3 against max = 269 386 — a **7000× separation** between the parked cell and the
99th percentile of the whole code population.

### Known gaps in this validation

- The fuse's `prefit_fused` events on `iono_sh_row` rows carry sat `?`: the slant-column→satellite
  fallback is implemented in the census walk but not in the fuse's own classification pass.  Station,
  row index and y are present, so the cell is still locatable.  Follow-up.
- The run-end "Run maximum s" headline reports the global max over all DATA classes, which on this
  data is an out-of-domain `obs_phase` cell (3.35e7).  It names its class and the same line states
  the event domain, so it is accurate — but an in-domain maximum would be more useful.  Follow-up.
- §12.5's "QC otherwise flat vs control" was not graded quantitatively (D5 rates, rx-CB withhold %,
  σ̂₀): the run-end counters for those tiers are not emitted in a form this harness parses.  The
  epoch-status timeline was compared instead.
- **⚠ INC-1's PHASE statistic is degenerate on truth-clean data, and this section did not know it.**
  Measured on a **fault-free** fixture (full table: `docs/SPEC_t3_null_derivation.md` §11.1) —
  `pc_phase_max_s` median **1.024e5 σ** against `pc_code_max_s` 75.1 σ, four to five orders out with
  no faults present: the same arc-specific mechanism deviation 2c measured.  `trace_prefit_census` is
  in the standard armed set, so this column is published network-wide; it is **operationally inert
  only by accident**, because `prefit_event_classes` defaults to `code,sh,other`
  (`GnssConfigLoadNetwork.cpp:1410-1412`) and phase is outside the event mask.  **Anyone who adds
  `phase` to that mask, or arms `prefit_fuse_classes` with phase, gets ~112 fuse events per epoch on
  clean data.**  Own card owed.

## 18. §13 governance — ACTIONED

**Standing invariant #7 was adopted.**  CLAUDE.md's "Standing invariants" section carries §13's text
verbatim, with the FT63 leg-19 topology-ledger evidence as its field demonstration (13 recovered
re-coupling edges; SAVO/OAFA/POVE's SH-tie weight 0.25 → 0.5 at TOW 329190, one epoch after the
era-2 discharge), and CLAUDE.md:253 points back at §13 for the open valve inventory.  The valve
inventory table stays here and its status column is dated Aug-1 — **re-audit against the current
tree before quoting a cell**.  INC-4 is the symptom-side half: it makes the DECOUPLING and
RE-COUPLING edges of the SH-tie valve into events with identities, which is what invariant #7
requires of every valve in that table.
