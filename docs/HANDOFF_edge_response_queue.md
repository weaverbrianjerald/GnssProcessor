# HANDOFF — the edge-response queue (bare datum/coupling edges, ranked by measured damage)

2026-08-05, for the continuing session.  Context in one line: every failure class this
list names is an instance of one template — **an unmediated frame/coupling step meeting
a long-memory (small-q) carry** — and the codebase already contains working responses
(S-transform / erase-refloat / re-tap) on the channels where field failures forced
them.  These five edges are the ones still bare.  Standing invariant #7 (CLAUDE.md) is
the governing rule; `docs/SPEC_level_information_budget.md` §24–§27 (2026-08-05) is the
freshest evidence.

**Relationship to the SPEC §27 ranked list** (do not reconcile them by guessing): §27
ranks NEXT ACTIONS (its item 1 = the S-consistent-harvest BUILD = this list's edge 3;
its item 2 = the two-sided disc channel = this list's edge 1).  This list ranks BARE
EDGES by measured damage.  Working either order is defensible; the two documents agree
on content.

---

## Session constraints (violating any of these voids the work)

- **⏱ RUN BUDGET (user directive, 2026-08-05): ~10–12 minutes wall MAX per run —
  ultra-fast turnaround phase.**  What fits, from this week's measured timings:
  - 61-station EXT arms at ~0.7–1.4 s/epoch: up to ~4–6 h of 30 s data (a 3 h arm =
    248–332 s measured — the workhorse tier).  **The staged 12 h pair does NOT fit**
    (~17–30 min each); it stays parked until a session with budget.
  - 303-station arms at ~17–24 s/epoch: **rank tier only** (20–40 epochs ≈ 6–16 min
    incl. ~105 s ingest).  No 305 1 h arms this phase.
  - Consumer replays are nearly free (~11–33 s/arm) — use them liberally.
  - The run-length law still holds (memory `project_level_information_budget`):
    ~20 ep RANKS two arms correctly; ~40 min of data quotes a residual penalty;
    absolute LEVEL claims need ≥ 2 h at 61 sta — so within this budget, produce RANK
    and RESIDUAL verdicts freely, and defer absolute-level verdicts rather than
    quoting them off short windows (§14's trap).
- **Release-only for every data run** (`build/x64-Release`); run a RENAMED copy of the
  exe (concurrent sessions rebuild; LNK1104).  ⚠ Same-build pairing (invariant #6):
  the §21–§27 comparison family is the **Aug-5 00:09 binary** (`gnss_processor_lv4stage1.exe`,
  copy `gnss_processor_f17.exe`); `gnss_processor.exe` was rebuilt 11:51 Aug-5 — never
  compare a pair across that boundary; build fresh pairs from ONE binary.
- **Runs cannot overlap on this machine** (shared `C:\GnssProcessor\logs\` spdlog →
  second instance fast-fails 0xC0000409).  Harvest traces (mtime ≥ run start) into
  `runs/<name>/logs/` BEFORE the next run.
- **Extends-chain depth ceiling is 8**: every new EXT arm extends
  `config/test_suite/_ext63_gps_dual_1h.json` directly and restates overrides
  (SPEC §23.7).
- **Denominators**: arm 32 (`runs/ext63_dual_3h_m15i25`, 61 sta) and arm 38
  (`runs/ext305_dual_1h_m15i25`, 303 sta), both obs 15 / iono 25.  Everything measured
  at 10/25 (SPEC §9–§20) is void as a control.  Root config still pins 15/15; the
  validated cutoff 15→25 edit is RECOMMENDED, pending user sign-off — do not flip it
  silently.
- **New work is default-OFF with structural byte-identity** on shared paths.  (The LV
  initiative's byte-identity waiver was scoped to LV-1..5 — it does NOT extend to
  these cards.)
- Level grading: time-matched GIM series (`scripts/gim_global_mean_series.py`) AND
  footprint (`scripts/field_vs_gim_footprint.py --config <the run's own network>
  --radius 5 --every 1` — bare invocation silently reads the 61-sta footprint at epoch
  1 only, SPEC §26.4).  ≥ 2 h for any 61-sta level claim; 1 h is rank-only.
- Product-affecting changes: run `scripts/product_step_census.py` on the emitted
  product — a change that moves the mid-arc census has changed the consumer's world
  regardless of solve-side grades (SPEC §25.2).
- Commits: no Claude co-author trailer; `git config core.hooksPath .githooks` once per
  clone; compound-subject convention for concurrent workstreams.

---

## Edge 1 — the wire/consumer edge (fully bare; blocks the consumer grade for EVERY posture)

**What.**  Producer datum re-realizations ship as unmarked value steps; the consumer
has no reaction path.  Measured (SPEC §25, runs `ext63_dual_12h_{ref,lv125}` +
`replay_ext63_12h_{ref,lv125}`): the wire carries only C/E/H/P records — zero R
records, zero `phase_disc`/`clock_disc` fields, `datum_changed ≡ 0`, H-heartbeat
σ₀ ≡ 0.000.  Mid-arc PB steps > 0.1 m on ~17 % of epochs (median ~3 m, max ~290 m);
the producer's own counter says **394 ref-path-slip steps in both arms, and the wire
census recovers 387/385 — counted, shipped, zero marked**, under `sigma=` ≤ 0.56 m
advertised.  Consumer excursions (to 979 m) are float-side and onset-enriched 5–13×
in re-referencing/topology events, not in value steps.

**The card is two-sided by construction** (SPEC §25.5 / §27 item 2):
1. *Producer:* arm the existing mark machinery — `NetworkProductEmitter.cpp:75-136`
   has per-sat disc counters, `disc_threshold_m`, `emit_clock_disc`,
   set-change tracking; the emit re-base already counts each preserved ref-path step.
   Emitting marks changes product bytes → default-OFF knob + consumer-compat check.
2. *Consumer:* the reader already parses every mark (`NetworkProductIO.cpp:120,278,
   603-620`) but only the RT engine reacts (`RealTimeEngineSsrBreaks.cpp` — order is
   load-bearing there: `reactToGenerationChange` global union BEFORE per-signal
   handlers).  The closed-loop/replay consumer (`ApplicationConsumerReplay.cpp` logs
   the contract at :171 and nothing else) needs the equivalent reaction seam:
   per-sat disc ⇒ reset that sat's carried SD ambiguity (reset-over-repair,
   biasless — NOT a repair attempt).
3. *Rider:* the H heartbeat's σ₀ is fed by the carry_warm-gated within-epoch RAIM —
   structurally dead on non-leveled lineages.  Candidate fix: feed it from the
   always-on prefit census σ̂₀ (`trace_prefit_census` tier).

**Grade — fits the run budget.**  Marks need fresh producer runs (the existing 12 h
products are mark-free); use **3 h producer pairs** (~4–6 min each — both §25 arms
already show excursions inside h0–3: ref h1.5 peak 308 m, lv125 h2.4 peak 979 m) +
replays (~11 s).  Success = excursion mass (epochs > 20 m, peak) drops materially in
both arms with the reaction built; `product_step_census.py` shows the same steps now
MARKED (census counts unchanged — the steps are legitimate re-datums; only their
visibility changes).  Ground truth for tests: the emitter's per-run ref-path-slip
count vs the census tie-out (394/394 at 12 h; expect ~100 at 3 h).  The h9.5–11.5
storm-era grade rides the parked 12 h tier.

## Edge 2 — the clock datum (worst re-realizer on the leveled lineage; candidates never validated)

**What.**  CLAUDE.md amendment 5: 13 product-wide clock jumps post-convergence per
12 h on the **leveled (golden/FT63) lineage**, vs 2 for code bias, 0 for phase bias;
silent to the consumer without `network.emit_clock_disc`.  Candidates
`network.clock_datum_sum_to_zero` / `_broadcast_anchored` (both declared_dimension 0,
"no pivot, no handover seam") have never had a first validation run — do not arm blind.

**⚠ Lineage nuance (SPEC §25.2, do not conflate):** on the EXT-CB lineage the emitted
clock measured SMOOTH (zero common-mode steps > 0.1 m, zero per-sat outliers > 0.5 m
over 12 h) — under EXT-CB the churn lives in the free per-(sat,band) phase biases
instead.  The 13-jump number belongs to the leveled lineage; validate the clock-datum
candidates THERE (or with phase_leveling armed), and use `product_step_census.py`'s
clock channel as the instrument on both sides of the A/B.

**Grade.**  Same-build A/B, one knob; clock-channel census (common-mode step count),
`emit_clock_disc` counter population, consumer replay unchanged-or-better; solve-side
byte-identity if the datum realization claims to be outputs-only (it does not — expect
solve changes, so grade on datum-invariant quantities).

## Edge 3 — the zero-mean rx datum's re-realizations (subsumed by the item-3 BUILD)

**What.**  `rx_code_bias_datum`'s zero-mean row is over the PRESENT station set; set
changes step every rx-CB against the q = 1e-9 carry with no edge event.  This is the
level campaign's core subject.  **The designed fix is SPEC §17 item 3 / §27 item 1 —
the "S-consistent harvest"**: post-solve Λ-weighted de-mean of the HARVESTED rx-CB
realization each epoch (solve → transform → harvest), making the carried set
zero-mean-by-construction; O(n), no cross-station prior rows, carry stays diagonal.
LASER source shows the transform without forming S (`C:\LASER` `CoreLsa.cpp:1678`,
`Net3DLsa.cpp:5573`); independent oracle `C:\LASER\test\GPS3710_test_PMINOLESS`.

**Falsified — do NOT retry** (SPEC §13/§15/§18.1/§19, memory
`project_extcb_defect2_rootcause`): LV-2's fold-innovation lag-1 de-mean (frozen pin —
ramps +15..+21); q-tuning in EITHER direction (1e-6 drifts +25; tighter than 1e-9
meaningless at 30 s); carry OFF bare (collapse −9 TECU/ep — keep the difference
rigidity, only the common mode is re-derived); a₀₀-anchoring alone (tension re-routes
to the next-loosest level-like direction — close ALL of them or re-derive the frame).

**New motivation from 2026-08-05** (SPEC §24/§25.6): the tracking authority exists in
admitted per-epoch code GF information (LV-4-alone tracked GIM within ±0.7 TECU from
h1, sd 1.81) but the fold-side vehicle costs +32 % settled phase rms; and the physical
rx biases are quasi-static (fleet mean −1.4 TECU drift/10 h) — the frame is the ONLY
moving part, which is exactly what a per-epoch post-solve transform carries.

**Grade — staged by run budget.**  Within the 10–12 min cap: build + oracle test
(`GPS3710_test_PMINOLESS`), then 20-ep rank pair and a 3 h stability/residual pair vs
arm 32 (zero fold-side cost: rms_phase within noise of control; prires rxcb σ̂₀ NOT
inflating; level read as diagnostic only).  The FINAL verdict — |a₀₀ − GIM(t)| ≲ 4 at
hour 0 AND hour 11 with footprint read — needs the 12 h pair
(`_ext63_gps_dual_12h_lv4_m15i25.json` + `_ext63_gps_dual_12h_m15i25.json`, staged
with predictions, ~17–30 min each) and is DEFERRED to a session with budget; do not
quote a level verdict off the short windows in its place.

## Edge 4 — the SH health gate's re-coupling edge (no release/degrade policy)

**What.**  An out-of-band era withholds emission for its FULL duration (95+ min
measured on DOY-129); the decoupling edge has no re-entry validation and no graded
release.  Doctrine: adapt-and-EMIT within the detected epoch; a withhold is graded as
an adaptation FAILURE unless the epoch is provably unrecoverable
(memory `feedback_raim_always_on`).

**⚠ Prior art — a measured deadlock, do not rediscover** (memory
`project_day129_gate_policy`): degrade-DURING-withhold deadlocks — the degradation
suppresses exactly the signal the gate needs to release.  Any release policy must
re-validate on a channel the withhold does not distort (candidates: the prefit census
σ̂₀ — pre-solve, always-on; or the band statistic itself with hysteresis), and the
re-coupling edge must re-seed or re-validate what it reconnects (invariant #7's second
clause), with counters and identities at BOTH edges (`trace_topology` pattern).

**Grade.**  Replay a DOY-129 sick-field era: time-to-release, consumer continuity
across the era, no oscillation (engage/release flapping), and the era visible in
`network_events.csv` at both edges.

## Edge 5 — the open valve inventory (observability landed; response pending)

**What.**  `docs/SPEC_prefit_census_observability.md` §13 holds the inventory of
valves (gates/deweights/admissions/holds) that still cross their coupling edges with
no bounded-accumulation or re-validation mechanics.  `trace_topology` (standard armed
set) made the edges VISIBLE — FT63 leg-19 recovered 13 silent re-coupling edges,
including the SAVO/OAFA/POVE tie-weight 0.25→0.5 discharge one epoch after era-2 —
but observing is not responding.  Era-2's valve "ramped at the configured rate limit
and discharged anyway" — smooth ramps are NOT mediation.

**First move.**  Read that §13 inventory and pick the valve with the largest measured
discharge; give it invariant #7's two-edge treatment (bound what decoupled states
accumulate; re-validate/re-seed at re-entry), one valve per card, counters at both
edges.  Grade on the discharge signature (`rhs_attrib_on_trip` is armed and will
attribute the next one).

---

## Pointers

Evidence: `docs/SPEC_level_information_budget.md` §24–§27 (this session) and §13–§23;
`docs/HANDOFF_ext_cb_iono_level.md`; `docs/SPEC_prefit_census_observability.md` §13;
CLAUDE.md standing invariants #1/#6/#7 + AGU-26 amendments.  Memories:
`project_level_information_budget` (the campaign index),
`project_extcb_defect2_rootcause`, `project_day129_gate_policy`,
`feedback_raim_always_on`, `feedback_reset_over_repair_biasless`.
Instruments: `scripts/product_step_census.py`, `scripts/gim_global_mean_series.py`,
`scripts/field_vs_gim_footprint.py` (flags!), `scripts/fleet_bias_diurnal.py`,
`scripts/config_posture_check.py`.
