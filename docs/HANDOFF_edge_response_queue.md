# HANDOFF — the edge-response queue (bare datum/coupling edges, ranked by measured damage)

2026-08-05. Every failure class here is one template — **an unmediated frame/coupling step
meeting a long-memory (small-q) carry** — and the codebase already contains working
responses (S-transform / erase-refloat / re-tap) on the channels where field failures
forced them. These five edges are the ones still bare. Standing invariant #7 (CLAUDE.md)
governs; `docs/SPEC_level_information_budget.md` §24–§27 is the freshest evidence.

**Relation to the SPEC §27 ranked list** (do not reconcile by guessing): §27 ranks NEXT
ACTIONS (its item 1 = the S-consistent-harvest BUILD = this list's edge 3; its item 2 = the
two-sided disc channel = this list's edge 1). This list ranks BARE EDGES by measured
damage.

> ⚠ **AMENDED 2026-08-05 — the two documents NO LONGER agree on edge 1.** Edge 1's premise
> is retracted: the replay consumer's disc reaction path is LIVE (83 resets / 12 h on the
> arm §25 graded), so §25.5 / §27 item 2's "the closed-loop/replay consumer has no reaction
> path" is false, and "build the consumer reaction seam" is NOT the work. **This document
> is the current statement; SPEC §25.5 and §27 item 2 are stale on that point.** §25's
> other findings stand, with one caveat: its replay arms are at the now-void 10/25 posture
> (§1.5).

---

## Session constraints (violating any of these voids the work)

⚠ **`runs/` was archived off `C:` on 2026-08-12 to
`D:\GnssProcessor_archive\2026-08-12\runs\`** — every `runs/<name>` below resolves there.

- **⏱ RUN BUDGET (user directive, 2026-08-05): ~10–12 min wall MAX per run.** Measured
  tiers: 61-sta EXT at ~0.7–1.4 s/epoch ⇒ up to ~4–6 h of 30 s data (a 3 h arm =
  **248–332 s**, the workhorse); 303-sta at ~17–24 s/epoch ⇒ **rank tier only** (20–40 ep ≈
  6–16 min incl. ~105 s ingest); consumer replays nearly free (**~11–33 s/arm**). **The
  staged 12 h pair does NOT fit** (~17–30 min each) and stays parked.
- **The run-length law** (memory `project_level_information_budget`): ~20 ep RANKS two arms
  correctly; ~40 min of data quotes a residual penalty; **absolute LEVEL claims need ≥ 2 h
  at 61 sta**. Produce RANK and RESIDUAL verdicts freely; defer absolute-level verdicts.
- **Release-only for every data run**; RENAMED exe copy (concurrent rebuilds ⇒ LNK1104).
  ⚠ Same-build pairing (invariant #6): the §21–§27 comparison family is the **Aug-5 00:09
  binary** (`gnss_processor_lv4stage1.exe` = `f17` = `lvsession` = `edge1`, all md5
  `383ad4a7f339e06751b91a08de1f2956`); `gnss_processor.exe` was rebuilt 11:51 Aug-5 and
  differs. **Never compare across that boundary; hash the binary, don't trust the filename.**
- ⚠ **Pair on the resolved CONFIG CHAIN too, not just the build** (paid for 2026-08-05,
  §1.5). A concurrent session editing a shared BASE config silently re-postures every
  descendant: `_ft63_24h_conv.json` moved `elevation_mask_deg` **10→15** and
  `elevation_cutoff_deg` **25→15** at 09:01 Aug-5, so arms either side of that edit are
  **not comparable even on a byte-identical binary** (measured: same product, same exe,
  last-30 3D RMS **6.70 vs 2.78 m**, diverging from epoch 1). `gnss_config.log`'s
  resolved-config summary does NOT print enough to catch it (it showed the iono cutoff, not
  the obs mask). `ls -la` the whole chain against the arm's run time, or restate the posture
  explicitly in the new config.
- **Runs cannot overlap on this machine** (shared `C:\GnssProcessor\logs\` spdlog ⇒ second
  instance fast-fails 0xC0000409). Harvest traces (mtime ≥ run start) into
  `runs/<name>/logs/` BEFORE the next run.
- **Extends-chain depth ceiling is 8**: every new EXT arm extends
  `config/test_suite/_ext63_gps_dual_1h.json` directly and restates overrides (SPEC §23.7).
- **Denominators**: arm 32 (`runs/ext63_dual_3h_m15i25`, 61 sta) and arm 38
  (`runs/ext305_dual_1h_m15i25`, 303 sta), both obs 15 / iono 25. **Everything measured at
  10/25 (SPEC §9–§20) is VOID as a control.** Root config still pins 15/15; the validated
  15→25 cutoff edit is RECOMMENDED pending user sign-off — do not flip it silently.
- **New work is default-OFF with structural byte-identity** on shared paths. (The LV
  initiative's byte-identity waiver was scoped to LV-1..5 and does NOT extend to these cards.)
- **Level grading** needs BOTH the time-matched GIM series
  (`scripts/gim_global_mean_series.py`) AND the footprint
  (`scripts/field_vs_gim_footprint.py --config <the run's own network> --radius 5
  --every 1` — ⚠ a bare invocation silently reads the 61-sta footprint at epoch 1 only,
  SPEC §26.4). ≥ 2 h for any 61-sta level claim; 1 h is rank-only.
- **Product-affecting changes**: run `scripts/product_step_census.py` on the emitted product
  — a change that moves the mid-arc census has changed the consumer's world regardless of
  solve-side grades (SPEC §25.2).
- Commits: no Claude co-author trailer; `git config core.hooksPath .githooks` once per
  clone; compound-subject convention for concurrent workstreams.

---

## Edge 1 — the wire/consumer edge — ★ REWRITTEN 2026-08-05 after measurement

> **The original premise was FALSE and is retracted.** SPEC §25.5 / §27 item 2 asserted the
> closed-loop/replay consumer has no reaction path to any per-satellite disc. **It has one,
> it is wired, and it fired 83 times over 12 h in the very arm §25 graded.** What is bare is
> (a) the CONTRACT that makes it faithful and (b) the EVENT CLASS that drives the excursions.

### 1.0 How the false premise survived: a default-OFF logger

`Application::runConsumerReplay` drives a `RealTimeEngine`, and
`RealTimeEngine::processEpoch` calls **all five** `RealTimeEngineSsrBreaks` handlers
unconditionally (`src/core/realtime/RealTimeEngine.cpp:479-483`). The reactions log to the
`rt_engine` logger, and `logging.enable_rt_engine` **defaults to false**
(`GnssConfigLoad.cpp:3075`) — so `runs/replay_ext63_12h_ref/logs/rt_engine.log` is **0
bytes** and the whole reaction layer read as absent. Corollary to invariant #4: **an empty
log is not evidence of an inert path — check whether its logger is armed.**

### 1.1 The proof (byte-identical twin, logger ON)

`config/test_suite/_ext63_replay_12h_ref_twin.json` restates the reference arm's posture
(§1.5) and adds only `logging.enable_rt_engine: true`:

| | reference `replay_ext63_12h_ref` | twin |
|---|---|---|
| AR ran / fixed | 1424 / 5 | 1424 / 5 |
| first fix | tow 264630.0 | tow 264630.0 |
| last-30 3D RMS | 6.7027 m | 6.7027 m |
| `closed_loop_trace.csv` md5 | `3af4ff1a0d4dfee1faf0a5327eb68938` | **same** |
| `consumer_replay_hourly.csv` md5 | `7fba959eff71086e31d311792981bb51` | **same** |
| phase-bias disc resets | *(invisible)* | **83** |

Byte-identical traces ⇒ the logger toggle is solve-neutral ⇒ those 83 resets were in the
reference run too. A logger-OFF control (`_ext63_replay_12h_ref_ctl.json`) reproduces the
same solve.

### 1.2 What is actually bare, per channel (measured on the graded arm)

| channel | wire | consumer reaction | verdict |
|---|---|---|---|
| **phase-bias disc** | P records; disc field NOT serialized ⇒ replay RECOMPUTES | `resetOnPhaseBiasDisc` → `resetAmbiguity` | **LIVE — 83 resets / 12 h, at the WRONG threshold (§1.3)** |
| **code-bias disc** | **zero B records on the wire** | `reinflateOnCodeBiasDisc`, additionally gated on `estimate_ifb` (**off**) | dead at source, twice over |
| **clock disc** | C records present, `emit_clock_disc` OFF ⇒ counter ≡ 0 | `reinflateOnClockDisc` | dead at source |
| **generation id** | no `I` records | `reactToGenerationChange` | inert (single producer run) |
| **`datum_changed`** | E field ≡ 0 | forced bump | inert |
| **H heartbeat** | **1440 H records ARE on the wire** | `logProducerStatus` | **structurally invisible: `NetworkProductReader::replay` never feeds H** — only `NetworkProductTail::lastStatus` surfaces it, and batch replay does not use the tail |

§25.1's "the wire is mark-free" is right about the *marks* but understates one case (1440 H
records written then ignored by the batch reader) and overstates another (the phase channel
needs no serialized mark — replay recomputes it deterministically, which is why the
reaction works at all).

### 1.3 ★ The real producer-side defect: a v1 wire mis-thresholds the one live channel

The product is `# gnss_network_product v1` with **no S contract line**, because
`network.serialize_sbasis_contract` defaults to **false**. So
`NetworkProductReader::makeReplayEmitter` falls back to the DEFAULT emitter — flat
`kDefaultDiscThresholdM = 0.05 m` — while the producer ran
`network.disc_threshold_frac_cycles = 0.25` (per-λ: **0.0476 m** L1, **0.0611 m** L2).
Producer and consumer count **different events**. Over the 12 h wire (the scan reproduces
§25.2's census number 387 exactly):

| | count |
|---|---|
| mid-arc steps > 0.1 m (the §25.2 number) | **387** |
| would fire at PRODUCER threshold (0.25·λ) | 437 |
| would fire at CONSUMER fallback (flat 0.05 m) | 477 |
| → producer flags, consumer **MISSES** | **5** (all band 1; 0.25λ = 0.0476 < 0.05) |
| → consumer flags, producer **did NOT** | **45** (all band 2; 0.25λ = 0.0611 > 0.05) |

Band-asymmetric and mostly spurious: 45 false resets against 5 missed; observed resets are
`L2W 52 / L1W 31`, so the spurious population sits in the majority channel. **Fix is one
default-OFF knob, not new code:** arm `network.serialize_sbasis_contract` on the producer;
the contract already carries `discfrac=` (`ApplicationNetworkSetup.cpp:1062`) and
`makeReplayEmitter` already consumes it. *(An earlier draft called
`pending_contract.disc_threshold_m = kDefaultDiscThresholdM` at
`ApplicationNetworkSetup.cpp:1051` a latent bug — it is NOT. There is no config knob for the
flat threshold: the emitter itself is constructed with the same constant at
`ApplicationNetworkSetup.cpp:950`, so the contract stamps exactly what the emitter used —
the "R9: single source of truth" comment working as designed. Retracted.)*

### 1.4 ★★ The load-bearing NEGATIVE result: the live reaction does not touch the excursions

83 resets against the consumer's own excursion windows (err3d > 20 m), base reset rate
0.0576/epoch over 1440 epochs:

| window (h) | ep | peak | resets in onset (−120…+60 s) | enrichment |
|---|---|---|---|---|
| 1.51–1.68 | 22 | 307.6 m | **0** | **0.00×** |
| 3.30–4.70 | 169 | 545.6 m | **0** | **0.00×** |

Inside the windows: **10 of 83 resets against 11.0 expected at base rate — enrichment
0.91×, i.e. exactly nothing.** Zero at both onsets. This extends §25.3 one decisive step:
§25.3 showed the *steps* are at base rate inside excursions; this shows **the consumer's
reactions to those steps are too**, and are absent at the onsets. Therefore:

> **Arming marks and reactions on the phase-bias disc channel will NOT recover the excursion
> mass.** The original success criterion ("excursion mass drops materially in both arms with
> the reaction built") is not achievable through this channel and is retracted.

The excursion driver remains §25.3's event class — `refsat_reset`, `col_removed`,
`sbc_engage`, `eq34_reattach` — which has **no wire representation of any kind**. That is
Edge 1's bare edge: the producer knows it moved its datum and the wire has no record type
saying so. Nearest existing carriers: the per-sat forced bump (`markSatDiscontinuity` → `R`
record) and the per-constellation `datum_changed` / `datum_consts` E-fields — both present
in the format, both never written on this lineage.

### 1.5 ⚠ The §25 replay arms are at the now-VOID 10/25 posture

Discovered while building the twin; it affects every number §25 quotes. The chain root
`config/test_suite/_ft63_24h_conv.json` was edited at **09:01 Aug-5** (landed in `2862318`),
moving `processing.elevation_mask_deg` **10 → 15** and
`estimation.ionosphere_weighting.elevation_cutoff_deg` **25 → 15**. The §25 replays ran at
**07:52**, under the OLD **10/25** posture — which the Denominators rule declares void as a
control. Same product stream, same binary, current 15/25 posture ⇒ last-30 3D RMS **2.78 m
vs 6.70 m** and a completely different excursion profile. Consequences:

- reproducing any §25 replay number requires restating `elevation_mask_deg: 10.0` **and**
  `elevation_cutoff_deg: 25.0` (the twin config does this);
- the iono cutoff is a red herring **for the consumer** — 15 vs 25 gives bit-identical
  consumer output; the obs mask is what moves it;
- **same-build pairing (invariant #6) was not sufficient here** — the binaries were
  byte-identical (`md5 383ad4a7…`) and the arms still diverged from epoch 1. Hence the
  config-chain pairing rule above.

### 1.6 Revised card

1. **Make the live channel faithful** (cheap, default-OFF, no new code): arm
   `network.serialize_sbasis_contract`; fix the hardcoded `disc_threshold_m` at
   `ApplicationNetworkSetup.cpp:1051`. Grade: replay recomputes 437 events instead of 477,
   the 45 spurious band-2 resets vanish, the 5 band-1 misses appear. Product bytes change
   (new S line) ⇒ consumer-compat check + `product_step_census.py`.
2. **Give the excursion event class a wire record** (the real work): the format already has
   `R` records and `datum_changed`/`datum_consts`; the producer already detects
   `refsat_reset` / `col_removed` / `sbc_engage` / `eq34_reattach` for `trace_events`. Route
   those to `markSatDiscontinuity` / `markDatumChange` behind a default-OFF knob, then
   re-measure §1.4's onset enrichment — **that** table is the success criterion, not
   excursion mass in the abstract.
3. **The H rider, re-scoped**: two defects. (a) σ₀ ≡ 0 because the feed is the
   carry_warm-gated within-epoch RAIM (candidate: the always-on prefit census σ̂₀);
   (b) **the batch replay path ignores H entirely** — 1440 records written and dropped.
   (b) is the cheaper and more surprising half.

**Grade — fits the run budget.** Piece 1 and the §1.4 re-measure are replay-only (~16 s per
arm) against the existing 12 h products. Piece 2 needs fresh producer runs: **3 h pairs**
(~4–6 min each; the ref arm's h1.5 and h3.3 excursions both sit inside h0–3). Reproduction
recipe: `_ext63_replay_12h_ref_twin.json` (10/25 posture + `enable_rt_engine`), binary
`gnss_processor_lv4stage1.exe` (= `f17`, md5 `383ad4a7…`), harvested to
`runs/replay_ext63_12h_ref_twin/`.

### 1.7 ✅ PIECE 1 EXECUTED AND PASSED (2026-08-05) — all four predictions confirmed

Arms: producer `runs/ext63_dual_3h_contract` (`_ext63_gps_dual_3h_contract.json` = arm 32 +
the single knob `network.serialize_sbasis_contract: true`, 260 s, exit 0); control = **arm
32 itself** (`runs/ext63_dual_3h_m15i25`); replays `runs/replay_3h_{v1ctl,v2}`. Same binary
throughout (`gnss_processor_edge1.exe`, md5 `383ad4a7…`). **No code change was needed.**

| prediction | result |
|---|---|
| **P1** v2 header carrying `discfrac=0.25` | ✅ `# gnss_network_product v2` + `S,MBAR00UGA,1,nominal,0,2,0,0,0.05,1,COD0MGX…,discfrac=0.25`; replay logs "S-contract present (v2)" |
| **P2** record stream byte-identical (knob is header-only) | ✅ **0 diff lines** across all E/C/P records vs arm 32; the 167-byte size delta is the S line + legend |
| **P3** fewer resets, band-2/L2W dominated | ✅ **17 → 13 resets; all four removed are L2W. L1W unchanged 4 → 4** |
| **P4** producer runtime/solve unaffected | ✅ 260 s vs arm 32's 248 s; records byte-identical |

P3 is the sharp one: exactly the predicted spurious band-2 population, zero collateral on
band 1. Position domain, same window, identical observations: **last-30 3D RMS 11.6777 →
10.1977 m (−12.7 %)**, first fix 30 s earlier — the cost of four spurious ambiguity resets
over 3 h.

**Recommendation: arm `network.serialize_sbasis_contract` on every producer run that will be
replayed or tail-consumed.** Header-only, solve-byte-identical, free, and without it the
consumer is provably reacting to the wrong event set.

**⚠ Two corrections to §1.2/§1.3, both paid for by this run:**

1. **RETRACTED: "the clock channel is killed in transit."** The v1 fallback
   (`emit_clock_disc = false`) was NOT what suppressed the clock reactions: the v2 contract
   carried `emit_clock_disc=1`, `makeReplayEmitter` built the emitter with it, and clock
   reactions are **still 0**. The real reason is structural — the clock counter has **no
   magnitude jump test** (deliberate; `NetworkProductEmitter.cpp:271-281`), so its only
   triggers are a forced datum bump (`datum_changed` / `R` records) or
   `clock_disc_on_set_change`. On this lineage `datum_changed ≡ 0`, there are no `R`
   records, and the S line carries **no `clkset=` token**. The counter is enabled and
   nothing drives it. **This reinforces §1.4: the missing thing is the EVENT, not the
   channel.**
2. **Scope of the v1 defect, stated correctly.** A v1 wire silently defaults **all five**
   replay-emitter parameters, not just the threshold — `disc_threshold_m`,
   `emit_clock_disc`, `staleness_horizon_s`, `code_bias_jump_test`,
   `clock_disc_on_set_change`, plus `disc_threshold_frac_cycles`
   (`NetworkProductIO.cpp:892-901`). That is the latent blast radius. On THIS lineage only
   `discfrac` is materially active, which is why the measured effect is 4 resets and not 40
   — do not over-generalize the −12.7 % to other postures.

**Incidental:** this 3 h window at 15/25 has **zero epochs > 20 m** in both arms, so §1.4's
onset-enrichment grade could not be re-run here (the §25 excursions live on the 10/25 12 h
arms). Re-running §1.4 after piece 2 needs either a 12 h arm or a producer window known to
excurse at 15/25 — **budget that before starting piece 2.**

### 1.8 PIECE 2 BUILT — and its premise does not survive contact, for a reason worth keeping

Knob `network.mark_refsat_reset_disc` (default OFF) routes an IF-TD **reference-path** slip
to `markSatDiscontinuity`, draining through the B1 retro-bump's existing mark-and-serialize
path so it lands as an `R` record. Touchpoints: `NetworkSlipQc.{hpp,cpp}` (out-param),
`NetworkRunContext.{hpp,cpp}` (carry set + knob), `ApplicationNetworkAssembly.cpp` (call
site), `NetworkSolPrep.cpp` (drain), `GnssConfig.hpp` + `GnssConfigLoadNetwork.cpp`.

| gate | result |
|---|---|
| **OFF path byte-identical across the rebuild** (invariant #6) | ✅ pre-change `ext63_1h_evt_c0` vs post-change `ext63_1h_evt_off`: products md5 **`fa48369f56729fcc3628d47ccbb71ad0`** both |
| **ON path drives the wire** (invariant #4) | ❌ **R = 0. Feature INERT.** |

**Why it is inert, and why that is CORRECT.** The mark is gated on `carry_warm`, matching the
event's own `refloat`/`diagnostic` action label, and that gate is right: `refloat_pair`
(`NetworkSlipQc.cpp:366-378`) only calls `markAmbiguityPendingReseed` **`if (carry_warm)`**, so cold
carry ⇒ no producer reseed ⇒ the emitted phase bias never re-references ⇒ **a mark would be a
FABRICATED event injecting spurious consumer resets.** And `carry_warm` is **never** true here:
`carry_warm = pl_on && …` (`ApplicationNetwork.cpp:1416`) requires `phase_leveling`, which the
EXT-CB lineage does not run (fingerprinted; fresh-convergence only):

| arm | refsat_reset events | `refloat` | `diagnostic` |
|---|---|---|---|
| `ext63_1h_evt_*` (1 h) | 4 | **0** | 4 |
| `ext63_dual_3h_contract` (3 h) | 8 | **0** | 8 |
| `ext63_dual_12h_ref` (12 h) | 82 | **0** | 82 |

**★ The finding that outlives the card: ONE precondition, THREE consumer-visible
casualties.** `phase_leveling` OFF ⇒ `carry_warm` false ⇒ (1) the within-epoch RAIM tier is
inert (CLAUDE.md standard armed set); (2) the H heartbeat's σ₀ ≡ 0.000 on the wire (§1.2's
"structurally invisible" row); (3) **IF-TD ref-path slips never reseed, so the excursion
event class SPEC §25.3 fingered does not move the producer's datum at all.** These had been
treated as three separate defects; **they are one.**

**Status: built, correctly gated, default-OFF, byte-identical when OFF, structurally inert
on the EXT-CB lineage.** Do not arm it here — it would do nothing, and if the gate were
removed it would do harm.

> ⚠ **§1.8's retirement of piece 2's premise is SCOPED to `carry_warm == false` lineages**
> (§1.9). On an unleveled lineage a cold-carry refsat_reset performs no reseed, so the 5.0×
> onset enrichment is correlational, not transmitted (a SYMPTOM of the stressor that also
> causes the excursion), consistent with §25.3's own caveat (2–6 events per window,
> "suggestive, not mechanism-proof"). **On a LEVELED lineage the reseed genuinely happens
> and the event IS transmitted**, so the premise is live wherever leveling is armed. The
> "one precondition, three casualties" finding survives unchanged.

### 1.9 ✅ THE GATING EXPERIMENT RAN — `phase_leveling` makes the whole producer chain live

Arm `runs/ext63_1h_evt_pl` (`_ext63_1h_evt_pl.json` = the ON arm + `phase_leveling: true`,
fresh-convergence lineage, own snapshot, 1 h, binary `gnss_processor_edge2` md5
`5e749970…`). §1.8's "needs ≥ 2 h" worry did **not** bind: `carry_warm` is an
emitted-epoch-count latch (`phase_leveling_warmup_epochs`, default **6**,
`GnssConfigLoadNetwork.cpp:2507`), not a level claim, so 1 h answers it decisively.

| Q | prediction | result |
|---|---|---|
| **Q1** `carry_warm` goes true | true from ~epoch 7 | ✅ split lands exactly on the warm-up boundary |
| **Q2** refsat_reset flips `diagnostic` → `refloat` | yes | ✅ **2 diagnostic (pre-warmup) / 2 refloat (post-warmup)** |
| **Q3** `R` records appear iff Q2 | yes | ✅ **`R,G16` and `R,G11`**, at exactly the two refloat epochs, with matching `RETRO/retro_bump` events |

| TOW | sat | station | action | on the wire |
|---|---|---|---|---|
| 259230 | G31 | MAC100AUS | diagnostic | — |
| 259320 | G10 | KOKV00USA | diagnostic | — |
| 259470 | G16 | KOKV00USA | **refloat** | **`R,G16`** |
| 262020 | G11 | STHL00GBR | **refloat** | **`R,G11`** |

Producer chain proven end-to-end: IF-TD `refsat_reset` (carry_warm) →
`ctx.carry.refsat_mark_sats` → `NetworkSolPrep` drain → `markSatDiscontinuity` → `R` record
→ `RETRO` event. The RT0-5 "mark == land" semantics held (both marks landed at their own
epoch, nothing left pending). Same window, unleveled: **R = 0**.

**Q4 (the consumer link) — the FIRST attempt was UNDERPOWERED, not negative.** A replay pair
against the same product with and without its 2 R records (byte-exact control
`products_noR.txt`, 0 diff lines outside the stripped rows) gave **11 resets / last-30 3D
RMS 3.7804 m / `closed_loop_trace.csv` md5 `e12ae34433bc…` in BOTH arms** — because the
held-out consumer **never observes G16 or G11** (it reset `G05 G13 G15 G20 G30`). With 2
marks/hour against one station tracking ~8 usable satellites the overlap probability is
small. **Read it as "this experiment had no power", not "the marks do nothing".** Fixes in
cost order: a longer producer window, several held-out consumers, or selecting the consumer
from the marked satellites' visibility. → **RESOLVED POSITIVE, §1.10.**

**Status of the knob:** built, default-OFF, OFF-path byte-identical (§1.8 gate 1), producer
chain validated on a leveled lineage. Legitimately arm-able on any `phase_leveling` producer
run, with the honest caveat that its downstream value is unquantified. **Next:** re-run
§1.4's onset-enrichment table on a LEVELED producer arm long enough to accumulate marks.

### 1.10 ✅ Q4 RESOLVED + the H rider EXECUTED (2026-08-07 overnight; full record in `CONFIG_promotion_ledger.md` §A2, Q4 = F6)

**Q4 — the consumer link is PROVEN end-to-end.** The §1.9 power analysis was right:
WTZZ00DEU observes NEITHER marked satellite, and no station in the network observes both
(G16 over the Pacific, G11 over the South Atlantic — observer-set intersection EMPTY). Power
fix: two single-event replay pairs with the highest-elevation NON-ORIGIN observers as
consumers, against the byte-exact marks/no-marks product twins, binary
`gnss_processor_edge2.exe`:

| pair | mark | marks arm | nomarks arm |
|---|---|---|---|
| MAJU00MHL (G16 el 62°) | `R,G16` @ 259470 | **reset G16 L1C+L2W @ exactly 259470** | none |
| WIND00NAM (G11 el 48°) | `R,G11` @ 262020 | **reset G11 L1C+L2W @ exactly 262020** | none |

Chain validated END-TO-END: IF-TD refsat_reset (carry_warm) → mark → `R` record →
`markSatDiscontinuity` → both-band consumer ambiguity reset at the event epoch. Caveats: consumers
are in-network (mechanism proof, not an accuracy grade), and the VALUE of marking (net benefit vs
the reset transient — WIND marks hour-0 3D max 38.9 m vs 12.8 without) still needs the §1.4
onset-enrichment grade on a leveled long arm. Arms: `runs/replay_1h_pl_{maju,wind}_{marks,nomarks}`.

**The H rider (§1.6 item 3) is EXECUTED, both halves, and validated:**

- **(a) the batch replay reader now surfaces H.** `NetworkProductReader::read` gained an optional
  statuses out-param (standalone WITHHELD-epoch H's included — the case invisible to both records
  and `epoch_status`); `ApplicationConsumerReplay` forwards them to `setProducerStatus` interleaved
  by content epoch (the batch analogue of the RTA-6 tail wiring), with an invariant-#4 run-end
  count. Gates: `closed_loop_trace.csv` byte-identical across the change (`E12AE34433BC…`,
  `runs/replay_1h_pl_marks_s2hbin`); 121/121 forwarded; first `producer status: (none) -> ok` line
  in batch-replay history.
- **(b) the σ₀ feed: `network.h_sigma0_prefit_census`** (default OFF; requires
  `emit_epoch_status_record` + `trace_prefit_census`) — H σ̂₀ = the census pool's 1.4826·median over
  `h_sigma0_classes` (default "code,sh,other"; phase excluded per census deviation 2c; born cells
  deferred via the census's lag-1 presence map), computed PRE-solve so ALL FOUR H sites carry it
  (incl. the gate withhold, whose σ₀ was structurally 0). UNIFORM statistic per wire; the S contract
  advertises `hsig=pc` (new optional token, legend updated); the A4 ceiling still reads
  `sigma0_adapt`, untouched. Gates: record stream 0-diff vs control over 2700 lines (solve-neutral
  BY MEASUREMENT); H σ̂₀ varying 2.08→3.32 on the smoke vs the control's dead 0.0000.
- §1.9's casualty check on the leveled arm: the within-epoch tier came ALIVE with leveling (T2: 115
  opportunities / 0 flags, quiet hour) and H σ₀ became a **degenerate constant 1.0000** (vacuous
  pool, `n_adapt = 0`) — leveling un-zeroes the field but never made it a statistic; the census feed
  is the fix on BOTH lineages.

**Edge 3 is BUILT and field-validated through 3 h — card S2H,
`network.rx_cb_harvest_demean`** (the §17-item-3 S-consistent harvest; oracle
`GPS3710_test_PMINOLESS` passes in-suite). Every fold-side gate won: rms_phase +2.1–2.5 %
(noise; LV-4 paid +32 %), prires rxcb σ̂₀ 0.89–0.90× (QUIETER, not growing with era; LV-4
doubled), time-matched fold fraction +19 %, wire step census statistically identical, μ →
+0.0000 m settled (the transform's fixed point). Level (diagnostic): sits lower
(−4.35 ± 1.75 at h2-3 vs control's +0.44) — the zero-mean convention as a declared,
per-epoch-drained frame; the 12 h tracking verdict stays DEFERRED, and item 0b /
§13.4-option-1 remains the designed answer if the anchor's authority does not suffice.
Default-OFF, fingerprinted, no promotion.

## Edge 2 — the clock datum (worst re-realizer on the leveled lineage; candidates never validated)

**What.** CLAUDE.md amendment 5: 13 product-wide clock jumps post-convergence per 12 h on
the **leveled (golden/FT63) lineage**, vs 2 for code bias, 0 for phase bias; silent to the
consumer without `network.emit_clock_disc`. Candidates `network.clock_datum_sum_to_zero` /
`_broadcast_anchored` (both declared_dimension 0, "no pivot, no handover seam") had never
had a first validation run — do not arm blind.

**⚠ Lineage nuance (SPEC §25.2, do not conflate):** on the EXT-CB lineage the emitted clock
measured SMOOTH (zero common-mode steps > 0.1 m, zero per-sat outliers > 0.5 m over 12 h) —
under EXT-CB the churn lives in the free per-(sat,band) phase biases instead. The 13-jump
number belongs to the leveled lineage; validate the clock-datum candidates THERE (or with
`phase_leveling` armed), and use `product_step_census.py`'s clock channel as the instrument
on both sides of the A/B.

**Grade.** Same-build A/B, one knob; clock-channel census (common-mode step count),
`emit_clock_disc` counter population, consumer replay unchanged-or-better; expect solve
changes (the datum realization is not outputs-only), so grade datum-invariant quantities.

> ✅ **VALIDATED AND SUPERSEDED — the answer was not datum choice.** *(Wiring smoke
> 2026-08-07 F10, 12 h A/B 2026-08-08 F13/F15, card built 2026-08-09; full record
> `CONFIG_promotion_ledger.md` §A2 (F10/F13/F15) and `docs/SPEC_clock_emit_rebase.md`.)*
>
> - **1 h wiring smoke** (`runs/evt_pl_clkpin` pivot-pin baseline vs `runs/evt_pl_clks2z` +
>   `clock_datum_sum_to_zero`, session binary, both 121/121 emitted): every predicted loader
>   interlock fired (s2z banner, sticky-INERT warn, datum-graph d=0 warn); clock-channel
>   census CLEAN on both (0 steps, 0 outliers); **0 pivot/handover/set-change events in the
>   quiet hour — exactly the power prediction.** s2z upgraded from "never validated" to
>   **wiring-validated on the leveled lineage**. `emit_clock_disc` counters were armed in
>   both arms; nothing drove them.
> - **12 h A/B:** on the EXT-derived leveled lineage **BOTH** datums' emitted clocks are
>   census-CLEAN (0 steps / 0 outliers / 12 h; pin took 1 handover, s2z 0) and the consumer
>   trade is **TWO-SIDED** (s2z: −15 % resets and no 60 m tails, but ~9 m vs ~3–4 m steady
>   median h1–h7). **s2z does NOT graduate; the pin stays default.**
> - **⭑ The 13-jumps-class churn belongs to the INTERNAL lineage and is far larger than the
>   counter suggested:** census common-mode steps on **613 of 1440 epochs** (273 with per-sat
>   outliers), riding on **ONE** wire mark — the clock channel has no magnitude jump test by
>   design, so **the re-realization class is wire-invisible.** Edge 2's real work is
>   mediating (or at minimum marking) that class, with FC-1's 2.3 % churn ceiling as the
>   design input.
> - ✅ **That card is BUILT and 12 h-validated — Card CLK-R,
>   `docs/SPEC_clock_emit_rebase.md`.** `network.emit_clock_frame_hold` (emission re-base of
>   the clock ensemble onto the previous emission frame — the clock analogue of the PB
>   channel's `NetworkEmitRebase`) takes the internal census **613 → 1, and the surviving
>   step IS the marked pivot handover**: wire discontinuities and wire marks now coincide.
>   Outputs-only proven byte-level; consumer neutral-to-better (`runs/clkr_12h_hold`).
>   ⚠ `network.clock_disc_jump_test` (per-sat 1.5 m common-removed step →
>   `markSatDiscontinuity`/`R`) is mechanism-proven end-to-end but on a quiet day its **68
>   resets COST the live consumer (1 → 106 epochs > 20 m)** — default-OFF, arm only for
>   event-day / onset-enrichment studies (the §1.4 criterion). Per-key clock value smoothing
>   was **FALSIFIED at design time** (the per-sat channel is a walk) — per-sat stiffness
>   remains Q3 estimator work, and AR stays 0/1440 as pre-registered. Both knobs default-OFF
>   (no-promotion rule); stage 1 is the promotion candidate for the internal golden lineage.

## Edge 3 — the zero-mean rx datum's re-realizations (subsumed by the item-3 BUILD)

**What.** `rx_code_bias_datum`'s zero-mean row is over the PRESENT station set; set changes
step every rx-CB against the q = 1e-9 carry with no edge event. **The designed fix is SPEC
§17 item 3 / §27 item 1 — the "S-consistent harvest"**: post-solve Λ-weighted de-mean of the
HARVESTED rx-CB realization each epoch (solve → transform → harvest), making the carried set
zero-mean-by-construction; O(n), no cross-station prior rows, carry stays diagonal. LASER
source shows the transform without forming S (`C:\LASER` `CoreLsa.cpp:1678`,
`Net3DLsa.cpp:5573`); independent oracle `C:\LASER\test\GPS3710_test_PMINOLESS`.

**Falsified — do NOT retry** (SPEC §13/§15/§18.1/§19, memory
`project_extcb_defect2_rootcause`): LV-2's fold-innovation lag-1 de-mean (frozen pin — ramps
+15..+21); q-tuning in EITHER direction (1e-6 drifts +25; tighter than 1e-9 meaningless at
30 s); carry OFF bare (collapse −9 TECU/ep — keep the difference rigidity, only the common
mode is re-derived); a₀₀-anchoring alone (tension re-routes to the next-loosest level-like
direction — close ALL of them or re-derive the frame).

**New motivation from 2026-08-05** (SPEC §24/§25.6): the tracking authority exists in
admitted per-epoch code GF information (LV-4-alone tracked GIM within ±0.7 TECU from h1,
sd 1.81) but the fold-side vehicle costs +32 % settled phase rms; and the physical rx biases
are quasi-static (fleet mean −1.4 TECU drift/10 h) — the frame is the ONLY moving part,
which is exactly what a per-epoch post-solve transform carries.

**Grade — staged by run budget.** Within the 10–12 min cap: build + oracle test
(`GPS3710_test_PMINOLESS`), then a 20-ep rank pair and a 3 h stability/residual pair vs arm
32 (zero fold-side cost: rms_phase within noise of control; prires rxcb σ̂₀ NOT inflating;
level read as diagnostic only). The FINAL verdict — |a₀₀ − GIM(t)| ≲ 4 at hour 0 AND hour 11
with footprint read — needs the 12 h pair (`_ext63_gps_dual_12h_lv4_m15i25.json` +
`_ext63_gps_dual_12h_m15i25.json`, ~17–30 min each) and is DEFERRED to a session with
budget; do not quote a level verdict off the short windows in its place.

## Edge 4 — the SH health gate's re-coupling edge (no release/degrade policy)

**What.** An out-of-band era withholds emission for its FULL duration (95+ min measured on
DOY-129); the decoupling edge has no re-entry validation and no graded release. Doctrine:
adapt-and-EMIT within the detected epoch; a withhold is graded as an adaptation FAILURE
unless the epoch is provably unrecoverable (memory `feedback_raim_always_on`).

**⚠ Prior art — a measured deadlock, do not rediscover** (memory
`project_day129_gate_policy`): degrade-DURING-withhold deadlocks — the degradation
suppresses exactly the signal the gate needs to release. Any release policy must re-validate
on a channel the withhold does not distort (candidates: the prefit census σ̂₀ — pre-solve,
always-on; or the band statistic itself with hysteresis), and the re-coupling edge must
re-seed or re-validate what it reconnects (invariant #7's second clause), with counters and
identities at BOTH edges (`trace_topology` pattern).

**Grade.** Replay a DOY-129 sick-field era: time-to-release, consumer continuity across the
era, no oscillation (engage/release flapping), and the era visible in `network_events.csv` at
both edges.

## Edge 5 — the open valve inventory (observability landed; response pending)

**What.** `docs/SPEC_prefit_census_observability.md` §13 holds the inventory of valves
(gates/deweights/admissions/holds) that still cross their coupling edges with no
bounded-accumulation or re-validation mechanics. `trace_topology` (standard armed set) made
the edges VISIBLE — FT63 leg-19 recovered 13 silent re-coupling edges, including the
SAVO/OAFA/POVE tie-weight 0.25→0.5 discharge one epoch after era-2 — but observing is not
responding. Era-2's valve "ramped at the configured rate limit and discharged anyway" —
smooth ramps are NOT mediation.

**First move.** Read that §13 inventory and pick the valve with the largest measured
discharge; give it invariant #7's two-edge treatment (bound what decoupled states accumulate;
re-validate/re-seed at re-entry), one valve per card, counters at both edges. Grade on the
discharge signature (`rhs_attrib_on_trip` is armed and will attribute the next one).

---

## Pointers

Evidence: `docs/SPEC_level_information_budget.md` §24–§27 and §13–§23 and §29;
`docs/SPEC_prefit_census_observability.md` §13; CLAUDE.md standing invariants #1/#6/#7 +
AGU-26 amendments. Memories: `project_level_information_budget` (the campaign index),
`project_extcb_defect2_rootcause`, `project_day129_gate_policy`, `feedback_raim_always_on`,
`feedback_reset_over_repair_biasless`. Instruments: `scripts/product_step_census.py`,
`scripts/gim_global_mean_series.py`, `scripts/field_vs_gim_footprint.py` (flags!),
`scripts/fleet_bias_diurnal.py`, `scripts/config_posture_check.py`.
