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

> ⚠ **AMENDED 2026-08-05 — the two documents NO LONGER agree on edge 1.**  Edge 1 has
> been rewritten against measurement and its premise retracted: the replay consumer's
> disc reaction path is LIVE (83 resets / 12 h on the arm §25 graded), so §25.5 /
> §27 item 2's "the closed-loop/replay consumer has no reaction path" is false, and
> "build the consumer reaction seam" is NOT the work.  **This document is the current
> statement; SPEC §25.5 and §27 item 2 are stale on that point** and should be
> corrected there when that doc is next touched.  §25's *other* findings stand, with
> one caveat: its replay arms are at the now-void 10/25 posture (edge 1 §1.5).

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
  copies `gnss_processor_f17.exe` / `gnss_processor_lvsession.exe` / `gnss_processor_edge1.exe`,
  all md5 `383ad4a7f339e06751b91a08de1f2956`); `gnss_processor.exe` was rebuilt 11:51
  Aug-5 and differs — never compare a pair across that boundary; build fresh pairs
  from ONE binary, and **hash it** rather than trusting the filename.
- ⚠ **Pair on the resolved CONFIG CHAIN too, not just the build** (new, paid for
  2026-08-05 — see edge 1 §1.5).  A concurrent session editing a shared BASE config
  silently re-postures every descendant arm: `_ft63_24h_conv.json` moved
  `elevation_mask_deg` 10→15 and `elevation_cutoff_deg` 25→15 at 09:01 Aug-5, so arms
  run before and after that edit are not comparable even on a byte-identical binary
  (measured: same product, same exe, last-30 3D RMS 6.70 vs 2.78 m, diverging from
  epoch 1).  The resolved-config summary in `gnss_config.log` does NOT print enough to
  catch this — it showed the iono cutoff but not the obs mask.  Before pairing against
  a stored arm, `ls -la` the whole chain and compare mtimes against the arm's run time,
  or restate the posture explicitly in the new config (the twin config does this).
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

## Edge 1 — the wire/consumer edge — ★ REWRITTEN 2026-08-05 after measurement

> **The premise of the original card was FALSE and is retracted.**  SPEC §25.5 /
> §27 item 2 (and this card's first draft) asserted that "the closed-loop/replay
> consumer has no reaction path to any per-satellite disc."  **It has one, it is
> wired, and it was firing 83 times over 12 h in the very arm §25 graded.**  Do not
> spend a session building a consumer reaction seam — it exists.  What is bare is
> (a) the CONTRACT that makes it faithful and (b) the EVENT CLASS that actually
> drives the excursions.  Everything below is measured, not inferred.

### 1.0 How the false premise survived: a default-OFF logger

`Application::runConsumerReplay` drives a `RealTimeEngine`, and
`RealTimeEngine::processEpoch` calls **all five** `RealTimeEngineSsrBreaks` handlers
unconditionally (`src/core/realtime/RealTimeEngine.cpp:479-483`).  The reactions
log to the `rt_engine` logger, and `logging.enable_rt_engine` **defaults to false**
(`GnssConfigLoad.cpp:3075`) — so `runs/replay_ext63_12h_ref/logs/rt_engine.log` is
**0 bytes** and the whole reaction layer read as absent.  Invariant #4 ("run-end
counters are the ON-path telemetry") has a corollary this cost a card:
**an empty log is not evidence of an inert path — check whether its logger is armed.**

### 1.1 The proof (byte-identical twin, logger ON)

`config/test_suite/_ext63_replay_12h_ref_twin.json` restates the reference arm's
posture (see §1.5) and adds only `logging.enable_rt_engine: true`:

| | reference `replay_ext63_12h_ref` | twin |
|---|---|---|
| AR ran / fixed | 1424 / 5 | 1424 / 5 |
| first fix | tow 264630.0 | tow 264630.0 |
| last-30 3D RMS | 6.7027 m | 6.7027 m |
| `closed_loop_trace.csv` md5 | `3af4ff1a0d4dfee1faf0a5327eb68938` | **same** |
| `consumer_replay_hourly.csv` md5 | `7fba959eff71086e31d311792981bb51` | **same** |
| phase-bias disc resets | *(invisible)* | **83** |

Byte-identical traces ⇒ the logger toggle is solve-neutral ⇒ those 83 resets were
in the reference run too.  A logger-OFF control (`_ext63_replay_12h_ref_ctl.json`)
reproduces the same solve, confirming the toggle changes nothing but the log.

### 1.2 What is actually bare, per channel (measured on the graded arm)

| channel | wire | consumer reaction | verdict |
|---|---|---|---|
| **phase-bias disc** | P records; disc field NOT serialized ⇒ replay RECOMPUTES | `resetOnPhaseBiasDisc` → `resetAmbiguity` | **LIVE — 83 resets / 12 h, but at the WRONG threshold (§1.3)** |
| **code-bias disc** | **zero B records on the wire** — no satellite code biases emitted at all | `reinflateOnCodeBiasDisc`, additionally gated on `estimate_ifb` (**off**) | dead at source, twice over |
| **clock disc** | C records present, `emit_clock_disc` OFF ⇒ counter ≡ 0 | `reinflateOnClockDisc` | dead at source |
| **generation id** | no `I` records | `reactToGenerationChange` | inert (single producer run) |
| **`datum_changed`** | E field ≡ 0 | forced bump | inert |
| **H heartbeat** | **1440 H records ARE on the wire** | `logProducerStatus` | **structurally invisible: `NetworkProductReader::replay` never feeds H** — only `NetworkProductTail::lastStatus` surfaces it, and the batch replay path does not use the tail |

So §25.1's "the wire is mark-free" is right about the *marks* but understates one
case (1440 H records are written and then ignored by the batch reader) and
overstates another (the phase channel needs no serialized mark — replay recomputes
it deterministically, which is why the reaction works at all).

### 1.3 ★ The real producer-side defect: a v1 wire mis-thresholds the one live channel

The product is `# gnss_network_product v1` with **no S contract line**, because
`network.serialize_sbasis_contract` defaults to **false**.  So
`NetworkProductReader::makeReplayEmitter` falls back to the DEFAULT emitter —
flat `kDefaultDiscThresholdM = 0.05 m` — while the producer ran
`network.disc_threshold_frac_cycles = 0.25` (per-λ: **0.0476 m** L1, **0.0611 m** L2).
Producer and consumer are counting **different events**.  Quantified over the 12 h
wire (scan validated: it reproduces §25.2's census number 387 exactly):

| | count |
|---|---|
| mid-arc steps > 0.1 m (the §25.2 number) | **387** |
| would fire at PRODUCER threshold (0.25·λ) | 437 |
| would fire at CONSUMER fallback (flat 0.05 m) | 477 |
| → producer flags, consumer **MISSES** | **5** (all band 1; 0.25λ = 0.0476 < 0.05) |
| → consumer flags, producer **did NOT** | **45** (all band 2; 0.25λ = 0.0611 > 0.05) |

The error is **band-asymmetric and mostly spurious**: 45 false resets against 5
missed.  The observed resets are `L2W 52 / L1W 31` — i.e. the band carrying the
spurious population is the majority channel.  **Fix is one default-OFF knob, not
new code:** arm `network.serialize_sbasis_contract` on the producer; the contract
already carries `discfrac=` (`ApplicationNetworkSetup.cpp:1062`) and
`makeReplayEmitter` already consumes it.  ⚠ One latent bug to fix with it:
`pending_contract.disc_threshold_m` is hardcoded to
`NetworkProductEmitter::kDefaultDiscThresholdM` (`ApplicationNetworkSetup.cpp:1051`)
rather than the configured value — harmless while `discfrac` > 0 dominates, wrong the moment a run
sets a non-default flat threshold.

### 1.4 ★★ The load-bearing NEGATIVE result: the live reaction does not touch the excursions

Correlating the 83 resets against the consumer's own excursion windows
(err3d > 20 m), base reset rate 0.0576/epoch over 1440 epochs:

| window (h) | ep | peak | resets in onset (−120…+60 s) | enrichment |
|---|---|---|---|---|
| 1.51–1.68 | 22 | 307.6 m | **0** | **0.00×** |
| 3.30–4.70 | 169 | 545.6 m | **0** | **0.00×** |

Inside the windows: **10 resets of 83, against 11.0 expected at base rate —
enrichment 0.91×, i.e. exactly nothing.**  Zero at both onsets.

This extends §25.3 one decisive step.  §25.3 showed the *steps* are at base rate
inside excursions; this shows **the consumer's reactions to those steps are too**,
and are entirely absent at the onsets.  Therefore:

> **Arming marks and reactions on the phase-bias disc channel will NOT recover the
> excursion mass.**  The original card's success criterion ("excursion mass drops
> materially in both arms with the reaction built") is not achievable through this
> channel and is retracted.

The excursion driver remains §25.3's event class — `refsat_reset`, `col_removed`,
`sbc_engage`, `eq34_reattach` — which has **no wire representation of any kind**.
That, not the reaction seam, is Edge 1's bare edge: the producer knows it moved its
datum and the wire has no record type that says so.  The nearest existing carrier is
the per-sat forced bump (`markSatDiscontinuity` → `R` record) and the
per-constellation `datum_changed` / `datum_consts` E-fields — both present in the
format, both never written on this lineage.

### 1.5 ⚠ The §25 replay arms are at the now-VOID 10/25 posture

Discovered while building the twin, and it affects every number §25 quotes.  The
chain root `config/test_suite/_ft63_24h_conv.json` was edited at **09:01 Aug-5**
(landed in `2862318`), moving `processing.elevation_mask_deg` **10 → 15** and
`estimation.ionosphere_weighting.elevation_cutoff_deg` **25 → 15**.  The §25 replays
ran at **07:52**, i.e. under the OLD **10/25** posture — which this document's own
Denominators rule declares "void as a control".  Same product stream, same binary,
current 15/25 posture ⇒ last-30 3D RMS **2.78 m vs 6.70 m** and a completely
different excursion profile.  Consequences:

- reproducing any §25 replay number requires restating `elevation_mask_deg: 10.0`
  **and** `elevation_cutoff_deg: 25.0` (the twin config does exactly this);
- the iono cutoff is a red herring **for the consumer** — measured: 15 vs 25 gives
  bit-identical consumer output.  The obs mask is what moves it;
- **same-build pairing (invariant #6) was not sufficient here.**  The binaries were
  byte-identical (`md5 383ad4a7…`) and the arms still diverged from epoch 1.  New
  rule for this document: *pair on the resolved CONFIG CHAIN as well as the build* —
  a shared base config edited by a concurrent session silently re-postures every
  descendant arm, and the resolved-config summary does not print enough to catch it
  (it showed the iono cutoff but not the obs mask).

### 1.6 Revised card

Three separable pieces, in dependency order:

1. **Make the live channel faithful** (cheap, default-OFF, no new code): arm
   `network.serialize_sbasis_contract`; fix the hardcoded `disc_threshold_m` at
   `ApplicationNetworkSetup.cpp:1051`.  Grade: replay recomputes 437 events instead
   of 477, the 45 spurious band-2 resets vanish, the 5 band-1 misses appear.
   Product bytes change (new S line) ⇒ consumer-compat check + `product_step_census.py`.
2. **Give the excursion event class a wire record** (the real work): the format
   already has `R` records and `datum_changed`/`datum_consts`; the producer already
   detects `refsat_reset` / `col_removed` / `sbc_engage` / `eq34_reattach` for
   `trace_events`.  Route those to `markSatDiscontinuity` / `markDatumChange` behind
   a default-OFF knob, then re-measure §1.4's onset enrichment — **that** table is
   the success criterion, not excursion mass in the abstract.
3. **The H rider, re-scoped**: two defects, not one.  (a) σ₀ ≡ 0 because the feed is
   the carry_warm-gated within-epoch RAIM (candidate: the always-on prefit census
   σ̂₀); (b) **the batch replay path ignores H entirely** — 1440 records written and
   dropped.  (b) is the cheaper and more surprising half.

**Grade — fits the run budget.**  Piece 1 and the §1.4 re-measure are replay-only
(~16 s per arm) against the existing 12 h products.  Piece 2 needs fresh producer
runs: **3 h pairs** (~4–6 min each; the ref arm's h1.5 and h3.3 excursions both sit
inside h0–3).  Reproduction recipe for everything above:
`_ext63_replay_12h_ref_twin.json` (10/25 posture + `enable_rt_engine`), binary
`gnss_processor_lv4stage1.exe` (= `f17`, md5 `383ad4a7…`), harvested to
`runs/replay_ext63_12h_ref_twin/`.

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
