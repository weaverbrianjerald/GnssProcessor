# SPEC — eq34 reattach hygiene (phase-datum cold-anchor guard + time-linked bridge)

**Status**: SPEC'd 2026-07-31 (late); **BUILT 2026-07-31** (see §8) — field validation
(§6) still OPEN, to be run in the FT63 session on the standing replay harness.  This card
was root-caused and spec'd in the FT63 session, BUILT in a fresh session.
**Memory context**: `project_ft63_fast_tier` (24h study + era-2 root cause + replay
mechanics), `project_adaptive_iono_q_network` (the falsified alternative + the
not-in-fingerprint precedent this card reuses).
**Scope discipline**: default-OFF knobs, OFF structurally byte-identical, no loader-default
flips (no-promotion directive). Non-goals (separate cards): RAIM T2 carry_warm decouple,
single-system clock-runaway degeneracy, SH-divergence RESET action, gate-release relevel.

---

## 1. Problem — field evidence (do not re-derive; all artifacts on disk)

DOY-129 (2024-05-08, storm-onset day), FT63 24h all-on `runs/ft63_24h_allon`
(v3k+rxcb stack, exe `gnss_processor_fable_qclk.exe`, config `_ft63_24h_allon.json`):

- 19:25:30 (TOW 329130) ingests three ~17.5 h arc re-entries (G05@STJO00CAN,
  G22@WIND00NAM, G29@INEG00MEX, dGF to −5.2 m) and the phase-datum event
  **`PHASE_DATUM/eq34_reattach G29 GAMB00PYF->INEG00MEX`** — the anchor re-hosted onto
  INEG's **one-epoch-old** arc (G29 rising after a 2138-epoch gap). Epoch still solves
  cleanly (postfit phase σ0 2.95).
- 19:26:00 (TOW 329160): one-epoch whole-field break — a00 SUM 38.7 → **5739 TECU**
  (layer split C00 +3342 / +2352), postfit phase σ0 → **56.7**, every satellite's code
  residuals blow (worst cell (BSHM00ISR, G07) 39.4 km — **raw RINEX verified clean**;
  G07 is L1-only at BSHM). SH health gate engages (emission-only backstop); era decays
  76 min; **exit scar**: across-gap between-satellite total_IF re-shuffle **181 m span
  (G07 −116 m)** that NEVER heals (3.3 h, τ ≈ the 1e-10 sat-CB carry class).
- **NEQ-SVD attribution** (export pair at 329130/329160, n_shared 416; recipe §6):
  σ_min UNCHANGED across the break (2.97e-2 → 2.95e-2, cond ~5e7) ⇒ **not an
  observability collapse**. Same weak PB subspaces both epochs ({G05,G20}, {G11},
  {G26,G27,G08} on L2W/L1C/L5Q). **RHS loading along weak-3 swings +33 → −2733.**
  Δx is PB-led (PB:G24:L5Q −5470, PB:G07 all bands −4.6/−3.8/−3.7 km, PB:G06:L5Q
  −4163, CLK:G07 −2057 m), **G29's own PB ~quiet — it is the pin**: the datum row
  fixed G29 at the cold value and the bias constellation re-expressed around it.
- Deterministic: the event reproduces EXACTLY (same engagement epoch, same peak class)
  from warm snapshot `snap_20` — see §6. Adaptive iono q at the 100× clamp does not
  move it (`runs/ft63_leg19_aiq`) — the discharge is a datum-value shock, not
  deviation lag.
- Screens that did NOT catch it (by construction): RAIM T2 zero-opportunity all day
  (carry_warm-gated, leveling OFF); T3/T4 zero-opp (carry_invariant); clock-runaway
  magnitude-degenerate single-system; innovation gates never see a DATUM row.

**The broken assumption** (in-code): [NetworkSolPrep.cpp ~:788] — "a tree-edge REATTACH —
the emitted δ̃_s steps by an EXACT integer number of cycles … usually 0 under
pin-to-round." True only when the NEW anchor's ambiguity is CONVERGED. A cold
(1-epoch) arc's float is code-seeded (±meters); realizing the datum on it re-levels the
gauge by an arbitrary amount that discharges along the soft PB directions into the
storm-deweighted SH field (W6 coupling), and the stiff CB carries then HOLD the wrong
level (the scar).

## 2. Design — three increments, independently knobbed

### INC-1 (core): anchor-host eligibility in the election
An ambiguity key is an ELIGIBLE anchor host only if its (station, amb-key) arc age
≥ `network.eq34_anchor_min_arc_ep` (int, epochs; **0 = legacy, default**). Election
order among eligible: incumbent first (stickiness — matches the clock-pivot priority
convention in NetworkSbasis.cpp:153-158), then the existing candidate ranking. If NO
eligible host exists for a key: do NOT reattach cold — hold the key out of the anchored
set this epoch (the existing coverage-gate drop path in NetworkPhaseDatum.cpp:145-153
is the precedent for "a key can sit out") and count it (§4). INC-2, when armed,
bridges instead of dropping.

Arc-age source: a ctx-side persistent map (sta_idx, amb_key) → consecutive-present
epochs, incremented in the serial drain after PASS A (station-local presence is known
there), erased on absence or slip-funnel hit (`carry_slipped_ambs` — union semantics,
same projection the T5v2 erase uses, ApplicationNetworkAssembly ~:1786-1829). Do NOT
reuse MW-detector Welford counts (detector is null on the datum station and under
non-leveling configs the count semantics differ).
**Warm-resume rule** (required, or a resumed leg blocks every reattach): on warm
resume, seed ages for all carried keys at `eq34_anchor_min_arc_ep` (arcs restored from
a snapshot are by definition mature). Do NOT serialize the age map (no snapshot format
change in this card).

### INC-2: time-linked phase datum (the `code_datum_time_linked` analog)
`network.phase_datum_time_linked` (bool, default false). On a handover where INC-1
found no eligible host (or, when armed without INC-1, on every handover), fold the OLD
anchor's realized pin value as a SOFT pseudo-obs row (σ = `phase_datum_bridge_sigma_m`,
default 0.05) for that key, for at most `phase_datum_bridge_max_ep` (default 20)
epochs or until an eligible host appears — whichever first.
**Standing invariant #1** (non-negotiable): the bridge is a stiff time-linked quantity
⇒ wired event response: DROP the bridge immediately on (a) slip-funnel hit of the
bridged key, (b) datum_changed epoch, (c) staleness > max_ep, (d) the key leaving the
shared set. Each drop reason counted separately (§4).

### INC-3 (cheap softener, optional): fresh-pin deweight ramp
`network.eq34_fresh_pin_sigma_scale` (double, default 1.0 = off): when a reattach IS
performed onto a host with age < min_arc_ep + `eq34_fresh_pin_ramp_ep`, inflate the
datum-row σ by this scale, decaying linearly over the ramp. Only meaningful if INC-1
is disabled or as belt-and-suspenders during the ramp; implement last, gate
independently.

## 3. Wiring sites (verified anchors, this tree @ Jul-31 night)

- Election: `ApplicationNetwork.cpp:1271/:1284` → `networkElectEnsembleDatum` /
  chain-elect in `filter/NetworkChainDatum.cpp` (alias preamble reads
  `ctx.carry.this_epoch_anchor` at :42). INC-1's eligibility filter goes in the
  election loop; keep the driver thin (seam contract).
- Coverage-gate precedent + in-code description of the thin-anchor pathology:
  `filter/NetworkPhaseDatum.cpp:128-153`.
- Datum-row realization (INC-2 pseudo-obs + INC-3 σ scale): `filter/NetworkSbasis.cpp`
  (shared datum builder) — mirror how the broadcast-anchored mode folds soft
  per-satellite rows at 1/σ² BEFORE the rank gate (NetworkSbasis.cpp:95-125).
- Reattach observability (existing trace, extend with block/bridge events):
  `filter/NetworkSolPrep.cpp:783-807`.
- Counters: `NetworkServiceCounters` (include/app/NetworkRunContext.hpp:623 — follow
  the aiq_* style added Jul-31); run-end line in `filter/NetworkTeardown.cpp`
  (before the SD-EWL block ~:402; copy the invariant-#4 phrasing of the adaptive-iono-q
  block directly above it).
- Config: `NetworkConfig` struct (include/config/GnssConfigTypes.hpp, near
  adaptive_iono_q added Jul-31); loader section in GnssConfigLoadNetwork.cpp near the
  sh_health_gate / adaptive_iono_q blocks (~:2027-2100) with arming info lines and the
  INERT-guard pattern (requires phase_datum_eq34 || phase_datum_prioritized — else warn
  + disable).
- **Fingerprint**: deliberately NOT appended (forward-only election/row policy; stored
  state meaning unchanged — the adaptive_iono_q precedent). Confirm against the
  conditional-append builder in `app/NetworkRunContext.cpp:134-260` and say so in the
  knob's doxygen. This is what keeps the §6 shared-snapshot A/B legal.
- CMake: no new .cpp expected (all edits in existing TUs); if a new seam file is
  created it MUST be added to CMakeLists.txt manually.

## 4. Telemetry (invariant #4 — counters or it didn't happen)

Events (network_events.csv, tier PHASE_DATUM): `reattach_blocked_cold` (key, station,
age), `pin_bridge_open` / `pin_bridge_drop` (reason ∈ slip|datum|stale|left_set|healed).
Counters: reattaches_total, reattaches_blocked_cold, bridges_opened, bridge_folds,
bridge_drops[4], fresh_pin_ramps. Run-end line gated on any knob armed; explicit
dead-wiring warn if an armed run counts zero election passes.

## 5. Unit tests

New file `tests/test_eq34_reattach_hygiene.cpp` (one suite, unique name — suite/file
ownership rule). Cases: (1) mature-host preferred over cold host regardless of rank
order; (2) cold-only candidates ⇒ blocked, key held out, counter bumped; (3) blocked +
INC-2 armed ⇒ bridge row folded with old value/σ, capped at max_ep; (4) bridge drops on
slip-funnel hit (invariant #1); (5) warm-resume age seeding (resume ⇒ mature); (6) OFF
⇒ election byte-identical (structural: no age map allocated, no new rows).

## 6. Field validation (run in the FT63 session, standing harness)

Controls on disk: `runs/ft63_24h_allon` (+ `snap_archive/`), `runs/ft63_leg19_off`
(OFF leg, gate ON), `runs/ft63_leg19_aiq`, NEQ pair `runs/ft63_leg19_neq51/52`.
Replay recipe (validated): copy `snap_archive/snap_20_*.bin` → leg dir
`state_snapshot.bin`; same config family `_ft63_leg19_*.json`; `state_snapshot_require:
true`; `start_offset_hours: 19.1125`; durations 3600. Decode snapshot generation from
the text `M,<week>,<tow>` header — archive numbering drifts ~30 s/write. NEQ export:
`neq_export_epoch` is EMITTED-count-based and snapshot-continued (pre-break = 2236);
the break epoch is reachable only via a `sh_health_gate: false` probe leg (gate is
emission-only ⇒ solve identical; 2237).

Gates, in order (10-ep smoke first per cadence, then legs, then 24h):
1. Smoke: knobs parse + arming lines + OFF twin byte-identical.
2. OFF leg from snap_20 (new build): a00 series identical to `runs/ft63_leg19_off`
   at engagement (the Jul-31 OFF-bit-preservation check passed this way).
3. **INC-1 leg**: expect `reattach_blocked_cold` (or a mature-host reattach) at
   19:25:30 in place of GAMB→INEG; **no SH-gate engagement at 19:26:00; a00 SUM stays
   < 150 through 19:36** (baseline: engage at 19:26:00, peak 5739/5864). Genuine
   storm behavior later in the era window is NOT a failure — grade the 19:26 step
   specifically.
4. NEQ probe pair (2236 / 2237-gate-off): Δx PB steps collapse (baseline table in
   `project_ft63_fast_tier`; PB:G24:L5Q −5470 → O(σ)), weak-3 RHS loading back to
   O(pre-break ±33).
5. QC flat vs OFF leg: D5 flags ±few %, rx-CB withholds ~14-15%, σ̂₀ 1.00, no new
   refloat storm.
6. 24h rerun (~80 min): era-2 19:26 step absent; grade era-1 (07:32) observationally —
   its trigger is NOT established as reattach-class, do not claim it; **exit-scar
   grade**: across-gap total_IF span at any surviving era ≪ 181 m; day-end quiescent
   trend back to NO-DRIFT class.
7. Product claims on total_IF only (promotion-ledger convention).

## 7. Deferral triage (pre-declared)

- Era-1 (07:32) root cause: OPEN, not claimed by this card (no reattach event in its
  onset window has been established) — field-inert for this card's verdict, gate 6
  grades it observationally only.
- Bridge value across a CLOCK-datum handover epoch: bridge drops (reason=datum) —
  re-bridging across datum changes is out of scope (vanish-return catalog item stays
  open in `project_network_ssystem_dia_assessment`).
- Age-map serialization into the snapshot: deferred (warm-resume seeding rule covers
  it); revisit only if a future card needs sub-min_arc_ep fidelity across resumes.

---

## ADDENDUM (Jul-31, while implementation in progress) — INC-1 must be WARM-CONDITIONED

User-raised design gap: absolute eligibility mishandles network-wide outages/restarts.
A cold pin is dangerous ONLY in proportion to the warm memory that contradicts it; a
full cold start has no old realization to fight, and today's cold-start behavior
(pin cold arcs, converge) is correct and must be preserved.

**Refined INC-1 rule**: the cold-host block applies to a key ONLY when a FRESH prior
realization exists for it — concretely: the key has an `emitted_anchor` baseline (the
handover-detection map at NetworkSolPrep.cpp:761-806) whose last emit is within a
validity horizon `network.eq34_prior_realization_max_gap_s` (default: reuse the
carry-validity horizon class; a bare new knob defaulting to ~1800 s is acceptable).
Consequences by scenario:
- single-key vanish-return, warm net: BLOCK (+bridge under INC-2) — unchanged;
- regional outage: election routes through surviving mature hosts — unchanged;
- network restart / cold start: no fresh baselines ⇒ NO blocking ⇒ legacy behavior
  (a restart must not extend the PB outage by min_arc_ep);
- outage shorter than horizon: bridge across (INC-2), reattach when mature;
- outage longer than horizon: baselines stale ⇒ cold pins allowed (the carried state
  has expired via its own 5τ/staleness machinery — nothing left to shock).
Counter split accordingly: `reattach_blocked_cold` vs `reattach_cold_allowed_no_prior`
(the second is the restart path and must be visible, not silent).
Unit-test additions: (7) cold host + stale/absent baseline ⇒ NOT blocked;
(8) cold host + fresh baseline ⇒ blocked (the original case).

---

## 8. Implementation record (BUILT 2026-07-31, a separate session)

Debug + Release both build clean; unit suite 1007 passed / 1 skipped (the standing
fixture-gated skip).  A 10-epoch cold-start smoke on the FT63 station set (scratchpad
config derived from `_ft63_leg19_off.json`, snapshot channel disabled so no `runs/`
artifact was touched) shows the wiring LIVE, not inert: 11 election passes, 15 890
arc-epochs aged, 3 reattaches, 2 blocked-cold holds, 4 bridges opened / 11 row-folds,
2 bridge drops on slip + 1 on left_set, bridge information 6.3 decades above the
rank-gate cutoff.  Field gates (§6) are UNRUN and belong to the FT63 session.

### Where it lives

- **New seam** `filter/NetworkAnchorHygiene.{hpp,cpp}` (+ CMake): arc ageing,
  eligibility verdicts, the bridge lifecycle, the hold-out realization.  Called from
  `ApplicationNetwork.cpp` between `networkBuildPhaseDatumCandidates` and the election
  dispatch — one line in the driver, per the seam contract.
- **Elections**: `networkElectChainDatum` (eq34) and `networkElectPhaseAnchors` (plain
  prioritized) both take `const AnchorHygiene&`; a default-constructed one reproduces
  the pre-card election exactly.
- **Bridge row**: `networkInjectPinBridgePrior` in `NetworkRunContext.cpp`, folded from
  `NetworkSolveGlue` right after the Fix-A phase hold (PRE-gate, same reason).
- **INC-3**: `NetStation::pinned_amb_sigma_scale` → the pin weight in
  `ApplicationNetworkAssembly`'s `foldAmb` (empty map ⇒ the exact legacy 1e10).
- **Telemetry**: `NetworkAnchorTallies` in `ctx.counters.hyg`; run-end block in
  `NetworkTeardown.cpp`; PHASE_DATUM events `reattach_blocked_cold`,
  `reattach_unblocked`, `pin_bridge_open`, `pin_bridge_drop`.
- **Fingerprint**: confirmed NOT appended, with the exemption recorded in-place at the
  end of `networkSnapshotFingerprint` — this is what keeps the §6 shared-snapshot A/B
  legal.

### Deviations and extensions the spec did not state (all deliberate, all tested)

1. **Age update runs BEFORE the election, not in the PASS-B serial drain** (§2 said
   "the serial drain after PASS A").  That drain runs AFTER the election, so an arc
   that slipped THIS epoch would still present a mature age to the election that
   re-hosts on it — a standing-invariant-#1 hole in the guard itself.  The seam sits
   between the census and the election, where both presence and this epoch's slip
   funnel are known.
2. **The slip feed is a UNION** of `carry_slipped_ambs` (carry_invariant-gated) and
   `StateManager::isAmbiguityPendingReseed` (carry_warm/leveling-gated).  Neither alone
   covers both stack shapes; the loader WARNS when NEITHER is live, because the age
   then only resets on absence.
3. **Warm-resume seeding is keyed on PRESENCE, not on "carried keys"** (§2 said carried
   keys).  The FT63-class stacks run `phase_leveling` OFF, so `local_carry` is empty
   there and a carry-keyed seed would silently seed nothing — the exact failure the
   rule exists to prevent.  One-shot latch set in `networkTryWarmResume`.
4. **The hold-out PERSISTS** (`ctx.carry.anchor_blocked`) instead of lasting one epoch.
   A held-out key is absent from `this_epoch_anchor`, so it also drops out of
   `anchor_incumbent`; without the set it would read as a first sighting the very next
   epoch, sail past the gate (first sightings are never gated) and be pinned on the
   same still-cold host one epoch later.  It releases when a host converges (counted)
   and, via the addendum's conditioning, also expires with the warm memory.
5. **The addendum's warm conditioning is keyed on a new
   `ctx.carry.emitted_anchor_s`** (stamped at the same emit site as `emitted_anchor`),
   not on `emitted_anchor` alone — that map has no timestamp.  It also carries a case
   `anchor_incumbent` structurally cannot: a satellite returning after a gap has
   already been dropped from the wholesale-replaced incumbent map, so the realization
   baseline is the ONLY thing that can tell a re-host from a first sighting.  Knob
   `network.eq34_prior_realization_max_gap_s` (default 1800 s; ≤ 0 disables the
   conditioning).  Counter split as specified.
6. **Under eq34 a bridged satellite becomes a spanning-FOREST ROOT**, not a tree child.
   Its gauge is the shared bridge row, so the rank arithmetic closes as 1 RPB pin + B
   bridge rows + (n_c + m_c − 1 − B) tree edges = n_c + m_c, and the walk may traverse
   THROUGH it (a receiver reached that way still pins its own z^s_r, subject to the
   same eligibility test).  Its shift-walk seed is a KNOWN zero — which is the point:
   the subtree keeps its carries instead of being re-referenced.
7. **The eligible-first walk is two-pass with a counted cold fallback.**  Restricting
   tree edges cannot be allowed to disconnect a component (that would trip the pin/gauge
   cross-check and the W5 gate), so pass 1 admits eligible edges only and pass 2 admits
   whatever is still needed, counting each unavoidable cold pin.  The INCUMBENT queue is
   never filtered — keeping the incumbent is not a reattach.
8. **A bridge dropped for slip / datum / stale does NOT re-open** on the same level in
   the same epoch (it would make the staleness bound and the invariant-#1 response
   cosmetic); it falls through to the INC-1 hold-out instead.
9. **K2-style margin check on the bridge row** (mirroring Card KP), logged once: a datum
   row whose information sits at the rank-gate cutoff is counted as a NULL direction and
   would withhold the very epochs the bridge exists to save.  Measured 6.3 decades at
   the default σ = 0.05 m on the FT63 set.

### Known residuals (not blocking; declare in the field report)

- **Bridge value across the datum-change epoch ITSELF** is one epoch stale (the election
  commits before `buildSharedDatum` picks the pivots); the drop lands at the next
  hygiene pass with `reason=datum`.  Bounded and benign because the row is SOFT: it
  owns only the gauge direction, and along every other direction it is 2–3 decades
  below the data.  Pre-declared in §7.
- **An incumbent whose OWN arc refloats keeps the datum** — the exemption is age-based
  and does not test the incumbent's slip state.  That is a different mechanism (the pin
  TARGET reverting to 0 on a live host) with its own existing carry-erase / reref
  response, and widening the guard to drop it would deliberately CREATE handovers.  Out
  of scope, stated here so it is not mistaken for coverage.
- `reattach_cold_allowed_no_prior` is expected to be LARGE on any cold-start leg (every
  key qualifies for the first `min_arc_ep` epochs).  Read it against `epochs`, not
  absolutely.

---

## §6 VALIDATION RECORD (FT63 session, Aug-1 early)

Gates 1-3 run on `gnss_processor_fable_eq34.exe` (fresh Release, 426/426 net+hygiene
unit filter):
- Smoke: PASS (both increments arm; full telemetry; cold start takes 327 COUNTED
  unavoidable fresh pins — addendum restart semantics verified live).
- OFF-identity: PASS (a00 at era-2 engagement 5738.6, bit-matching the pre-card build).
- ON leg (INC-1+INC-2, min_arc_ep 10): **mechanism PASS, era-2 hypothesis FALSIFIED** —
  holds/bridges/heals fired (G30@MBAR bridge, G24@KOKV block), the cold G29 re-host never
  happened, and the 19:26:00 break fired anyway (a00 5741.1 vs 5738.6; same 83 oob).
- Gates 4-6 (NEQ Δx collapse / QC / 24h era-2 absence): MOOT for era-2 — the card does
  not own that defect.  Card VERDICT: **valid latent-hazard guard (cold datum re-hosts),
  keep default-OFF per no-promotion; NOT the era-2 fix.**  Era-2 investigation continues
  under `project_ft63_fast_tier` (internal W6-null amplification; ablation + gauge-churn
  probes).
