# ApplicationNetwork.cpp decomposition — consolidated plan

*Produced by a 16-agent scoping workflow (Jul 4 2026): 9 mapping agents (section-by-section anatomy
of the ~7,300-line file, external coupling, config growth), 3 competing architects, 1 judge, 3
adversarial verifiers. Winning design: **domain-first along existing network seams** (scored 8.8 vs
7.7 incremental-TU and 6.5 class-decomposition), improved with the losers' best mechanics and
amended with every blocker/major from verification. All three verifier lenses returned
**sound-with-fixes**.*

---

## Implementation status (living section — update as increments land)

**➡ ACTIVE AMENDMENT: `docs/network_decomposition_addendum_2026-07-16.md`** — freeze SHA
**`56da23a`**, refreshed anchors (this doc's and the Jul-13 resurvey's line numbers are STALE),
WL/UWL delta map + new run-state entities, verified INC-1 boundaries, and the **fast golden matrix**
(user directive Jul-16: every in-series gate is 50 sta / ≤10 ep / GPS-preferred /
`parallel_carry=true`; the heavy battery — 1h QC, 3h crash signature, 305-sta — moves to a single
post-INC-12 validation phase, superseding this doc's §3/§5 heavy-run cadence).

| Item | State (Jul-16 2026) |
|------|----------------------------------|
| Baseline SHA | **FROZEN at `56da23a`** (WL/UWL workstream + H1-H3 hardening + lineage configs all committed). Tree clean; Release build clean; `gnss_tests` 780/779/1. |
| Gates 1-4 | CLOSED (C1/S1/pdp committed+validated; Card 7/8 triage = after INC-12; v3 robust-M + per-LOS iono stay holstered). |
| Gate 5 — TU freeze | **In force.** Urgent fixes land at increment boundaries only. |
| Current file sizes | `ApplicationNetwork.cpp` **14,657**; `GnssConfigLoad.cpp` **6,000**; `GnssConfig.hpp` **4,705**; `GnssConfigTypes.hpp` **2,340**. |
| INC 0 tooling | `scripts/compare_network_run.py` **built + validated** (capture/compare/selfcheck/dump-maskspec). Cut-map anchors regenerated **per-increment** (addendum §4 covers INC 1-4). |
| Increments landed | **INC-0 ✅** (8-arm fast matrix captured: twice-run selfcheck CLEAN ×8, ON-path counters verified per arm, manifests at `goldens/decomp/*.manifest.json`, maskspec FROZEN — additions: D0/T2b/timing runlog drops + snapshot M-record UTC mask) · INC-1: pending |

---

⚠️ **All line numbers below are ADVISORY.** The original map was surveyed while S1/C1 edits were
landing; verified drift is +13 to +190 lines. INC 0 regenerates the entire cut map anchored by
symbols/banner comments against the frozen baseline SHA. A cut boundary that cannot be resolved to a
unique symbol is a plan defect, not a judgment call.

## Philosophy

Every extracted unit either (a) extends an existing factored network class (`NetworkEstimator`,
`NetworkSbasis`), (b) becomes a new domain class in the established category dir following the
`NetworkPivotHandover` extract-for-testability exemplar, or (c) becomes an additional `Application`
TU per the `ApplicationBaseline`/`ApplicationRealtime` same-class-multi-TU precedent. `runNetwork()`
survives as a ~550-line phase orchestrator. Every increment is verbatim code motion, compiles
standalone, passes `gnss_tests`, and is proven byte-identical on a golden-run matrix before commit.
No behavior changes; all cleanups (dead `clockOscUpdate`, `cap_*` replay duplication, the EIGHT
epoch-skip idioms, refloat idiom duplication, `sysOfChar` duplication, `NetworkConfig` struct
consolidation) are explicitly deferred to follow-up cards.

## 0. Sequencing gates (BEFORE the series starts)

1. **C1 full-cov carry lands and validates.** The state inventory below must be re-derived AFTER it
   lands — C1 adds `prev_carry_cov`, `cap_dense_keys`/`cap_dense_cov` and a dense-stash lifecycle
   that the original map missed.
2. **S1 phase-datum working tree committed.**
3. **pdp coverage-threshold union-gating card lands and validates BEFORE INC 0** (verifier
   requirement, not optional): it is small, surgical, touches anchor election — which moves at
   INC 7 — and fixes a known non-PD defect in the just-validated flagship feature. Deferring it means
   golden baselines bake in defective anchor-election behavior and the fix later lands into freshly
   relocated code. Card 7/8 consumer work: triage explicitly (before series, or queued after INC 12 —
   no "or" left unresolved).
4. **Freeze `ApplicationNetwork.cpp` to this series.** Any urgent fix lands at an increment boundary
   (each increment commits independently green). Expect `LNK1104` build-lock retries; pre-kill WER
   zombie `gnss_processor` processes before harness runs.

## 1. Target file tree (est. lines; sizes advisory)

```
scripts/
  compare_network_run.py                    ~200  field-aware golden-run compare (INC 0, tooling)

src/core/config/
  GnssConfigLoadNetwork.cpp                 ~1780 verbatim cut of loadNetworkSection_ + consumer
                                                  couplings/profile helpers (INC 1)

include/filter/  (+ src/core/filter/ pairs)
  NetworkRunState.hpp                       ~330  CarryPrior, LocalCarry, PlAmb, PlZwd,
                                                  StationAssembly, ClockOsc/Pred + predict/update
  NetworkRunTypes.hpp                       ~280  NetStation, EpochWorkspace, PassABuffers,
                                                  NetworkSetupResult, EpochStatusRow, EpochAction
                                                  (filter/, NOT app/ — avoids filter/slip -> app
                                                  include inversion)
  NetworkRobustStats.hpp                    ~190  medianCopy, robustProminence, medianPolish2Way
                                                  (header-only, unit-testable)
  NetworkRaim.hpp/.cpp                      ~1300 within-epoch adaptation + post-fit RAIM +
                                                  attribution + consensus refloat
  NetworkClockQc.hpp/.cpp                   ~650  Card-6 re-level, oscillator fold, NIS, monitor
  NetworkPhaseDatum.hpp/.cpp                ~400  anchor election + incumbent carry +
                                                  healNearNullPhaseBias — home for the pdp coverage
                                                  card and the future RT brdc-anchored datum mode
                                                  (verifier-mandated seam)
  NetworkEstimator (ADDITIONS)              ~150  column descriptors + estimability spectrum,
                                                  kNullSpaceTol as class const
  NetworkSbasis (ADDITIONS)                 ~170  buildCodeBiasDatumRows (W6 gauge rows;
                                                  freq-oracle passed BY REFERENCE)

include/slip/ (+ src/core/slip/)
  NetworkSlipQc.hpp/.cpp                    ~620  ifElevMap, parseIfPairResetKey, T0 repair drain,
                                                  MW-detect refloat, IF-TD driver, PassABuffers

include/logging/ (+ src/core/logging/)
  NetworkRunTraces.hpp/.cpp                 ~1650 all 16+ streams, single-definition headers,
                                                  open/rotate/close, manifest, status/event/health
                                                  writers, diagnostic blocks, flag-rate summary,
                                                  FlagRate struct (lives HERE, not filter/)

include/app/
  NetworkRunContext.hpp                     ~320  NetworkCarryState / NetworkServiceCounters /
                                                  NetworkKnobs + non-owning refs (lifetime-ordered)
  Application.hpp (ADDITIONS)               ~90   ~14 private member-fn decls; FORWARD-DECLARE
                                                  context/workspace types (refs only) — full headers
                                                  included only in the network TUs

src/core/app/
  NetworkRunContext.cpp                     ~680  fingerprint + warm-resume restore + snapshot write
                                                  + carry lifecycle (inject/harvest/stash/retro) —
                                                  the "three-place rule" lives in ONE TU
  ApplicationNetworkSetup.cpp               ~850  Phases 0-3b as TWO fns split around resolveData()
                                                  (stays in the orchestrator), buildStation, sinks,
                                                  consumer, est hoist
  ApplicationNetworkAssembly.cpp            ~1200 splitStation, iono row appenders, PASS A/B,
                                                  consume scatter (/bigobj)
  ApplicationNetworkEmit.cpp                ~800  emit sequence, closed-loop eval, contract check
  ApplicationNetwork.cpp (SHRUNK)           ~580  runNetwork orchestrator only
```

New public headers: genuine seams (`NetworkRaim`, `NetworkClockQc`, `NetworkPhaseDatum`,
`NetworkSlipQc`, `NetworkRunTraces`) get full house-style Doxygen; run-loop support headers
(`NetworkRunState`, `NetworkRunTypes`, `NetworkRunContext`) get `@internal` banners with brief-level
docs, per the `EpochUpdateSupport.hpp` precedent. CMake: +10 sources, explicit list; `/bigobj` moves
to Assembly/RAIM, dropped from the shrunk orchestrator at INC 12 if a clean build proves it.

## 2. Shared-state strategy

Three tiers by lifetime; `Application` keeps ZERO network data members (current deliberate design).

**Tier 1 — cross-epoch: `NetworkRunContext`** (a `runNetwork()` local passed by reference):
- `NetworkCarryState`: re-derived at INC 0 from a fresh scan of ALL cross-epoch mutables in the
  post-C1 tree (`prev_carry`, `prev_carry_cov` + `fc_cols` dense lifecycle, `local_carry` ORDERED map,
  `zwd_carry`, `clock_osc`, `prev_clk`, `prev_pivot`/`last_pivot`, anchor incumbency, ura/retro/ramp
  state, `relevel_iode`, `eph_cutover_disc_pending`, `generation_id`, `iod`, `prev_week`/`tow`,
  `start_anchor_s`, ...). The snapshot correspondence is an annotated SUBSET rule, not a definition:
  "fields marked `[snapshot]` must be added to fingerprint + restore + snapshot write — all three
  live in `NetworkRunContext.cpp`." `NetStation`'s deliberately non-snapshot fields (`if_pair_scale`
  EWMAs, slip-detector state) are enumerated in a header comment with the exemption rationale; any
  NEW cross-epoch field defaults to `NetworkCarryState` under the three-place rule unless explicitly
  exempted (named expected consumer: future per-station carries).
- `NetworkServiceCounters`: `n_emitted` family, health-window anchors, `FlagRate`s, timers.
- `NetworkKnobs`: the ~45 const config snapshots initialized in the SAME read order (const from
  birth). `clk_anchor_sigma` joins ONLY if `resolveNetworkAprioriClockAnchorSigmaM()` is verified a
  pure config read (no logging, no run state); otherwise it stays at its current position in the
  Setup TU under the alias treatment.
- Non-owning refs in lifetime-encoding declaration order (sync outlives stations; `network_store`
  before consumer; `est` outlives the loop — the 146 GB memory-pool lesson).

**Tier 2 — per-epoch: `EpochWorkspace`**, introduced at INC 4 (was unscheduled — verifier fix).
Freshly constructed EVERY loop iteration — NEVER hoisted for reuse (`sigma0_adapt==0` and "cleared
and refilled on every `assess()`" are load-bearing semantics). Members whose current initializers are
computed mid-loop expressions keep their assignment at the original site with the member
default-initialized to the same value; per-member write-before-read audit at INC 4/7/9. Includes the
C1 dense-capture channel (`cap_dense_keys`/`cap_dense_cov`) alongside `cap_H`/`z`/`priors`, with a doc
block naming every writer site of the RAIM replay channel.

**Tier 3 — QC classes own NO state** beyond references (carry maps, refloat sets, oscillators stay in
the context because the snapshot serializes them) — StateManager-discipline conformant.

**Migration mechanic:** every increment that re-homes declarations replaces only the declaration
block with `auto& name = ctx.name;` aliases under the original names — downstream use-sites stay
textually unchanged; each diff reviews as pure relocation. Aliases peel only when a block moves out of
the TU (new signature takes ctx/ws explicitly).

**Control flow:** phases return `EpochAction {Proceed, SkipEpoch, StopRun}`. The EIGHT skip/withhold
sites (4× `sync_skip`, `datum_failed`, `gate_withheld`, `solve_failed`, `sigma0_withheld` — count
re-verified against tree) keep their exact site-specific bookkeeping verbatim; payloads differ per
site and are deliberately NOT unified. Emit-only baseline freeze becomes structural (emit code
unreachable from Skip paths). Two documented non-normalizable exceptions: Card-6 re-level mutates
carry pre-solve; `resolve_dropping` replays via `cap_*`.

**Lambda policy + peel discipline (verifier-hardened):** single-consumer lambdas stay internal to
their hoisted function. Cross-phase lambdas become functions/methods with explicit args. For EVERY
peel, produce a capture-list diff: each captured entity, whether read at call time (by-ref) or
definition time (by-value), and verify the new parameter/member reproduces the same read point. The
W6 freq-oracle and every `cap_*`-touching callable take REFERENCES; by-value capture of any container
mutated between construction and invocation is banned. `emitEpochStatus`/`emitEvent` take `GpsTime`
explicitly; their `es_*` config echoes become members initialized bit-identically. Eigen: new
signatures take concrete const-ref types (never auto/templates); no `auto` bindings of Eigen
expressions in moved code; fixed-size vectorizable Eigen members in new structs get alignment
treatment or verified non-vectorizable. `splitStation` keeps its throw.

**OpenMP:** both parallel regions move verbatim inside their functions; ambient `[&]` captures become
explicit (ctx, ws) references — identical sharing, and NO capture ever converts to `this` or
by-value. Both regions use `schedule(dynamic)` — parallel byte-identity rests entirely on
per-index-slot-write discipline, so INC 0 audits: no `omp atomic`/`critical`/`ordered`, no stream
writes inside regions, all shared mutation via index-addressed slots drained serially. PASS A private
buffers become the `PassABuffers` handoff struct preserving produce-then-serial-drain.

## 3. Byte-identity harness (INC 0 deliverable)

- **`compare_network_run.py`** — field-aware, and NOT vacuously passable (blocker fix): at golden
  capture it records the exact artifact set (relative paths, expected-non-empty flags, expected
  QC-row-count minimums) and FAILS on any missing or extra file, plus asserts nonzero data-row counts
  for the QC traces the run is supposed to trip. CSV-column / JSON-key granular masks only.
- **Mask list built EMPIRICALLY, then frozen:** run the pre-refactor binary TWICE per matrix config
  and self-compare — every diff is a mask candidate. Known wall-clock writers to mask: manifest
  `written_utc` + `__DATE__`/`__TIME__`, health windowed ms-per-epoch column, run-summary `loop_ms`,
  and the state snapshot M-record `wall_utc` (field-level, format-aware — the snapshot is the
  strongest full-precision artifact and must never be file-level excluded). After freeze, a "needed"
  new mask mid-series is a red flag that behavior changed.
- **Snapshots armed in EVERY matrix config** with a forced final-epoch write (the snapshot's
  full-precision `{}` doubles are the only gate below CSV print precision of `{:.4f}`/`{:.6f}`).
  Optional: debug-only hex-float dump of `x_s` at the final epoch.
- **Golden matrix** (each serial AND parallel; `parallel_max_threads` PINNED in configs; capture
  machine recorded; each parallel golden run TWICE and self-compared — a config that is not
  run-to-run identical is fixed or dropped before any code moves):
  (a) `_net305_3sys_smoke` legacy pivot; (b) prioritized-datum + SH-iono + AR smoke; (c) 1h QC-armed
  run that demonstrably TRIPS RAIM/attribution/clock-monitor; (d) closed-loop; (e) `brdc_rts` (Card-6
  + IODE); (f) warm-resume pair — DETERMINISTIC protocol: fixed epoch-count first leg so the final
  snapshot is always the same epoch, then resume the NEW binary from the OLD binary's snapshot
  (cross-version compatibility is the stated goal). ALL variant goldens (all-traces-armed,
  `trace_estimability`, W6 sat-code-bias, clock-monitor-only) are captured at INC 0 from the
  pre-refactor binary; every increment compares against INC-0 goldens, never against the previous
  increment.
- **3h crash signature:** run the known-crashing QC-armed 305 3h config once at INC 0; record crash
  epoch, stations, message sequence. Re-run at INC 12 and require the identical signature — a changed
  signature halts and bisects like any byte diff. Annotate the crash evidence
  (`runs/net305_qc_3h_crash/`) and memory notes with the old->new file map at INC 12.
- **parallel_reduce probe:** twice-run self-compare at INC 0; if deterministic, add one reduce-on
  smoke A/B to the matrix; else document the residual risk explicitly.
- **Cadence:** smokes after every increment; 1h QC run (c) at increments 4, **6**, 7, 9, **10**, 12
  (INC 6/10 added — the increments moving the most branch-heavy QC code must be verified on taken
  paths, or proven from INC-0 golden row counts that smokes trip them); resume pair (f) at 4, 7, 9,
  12; one clean-config 3h A/B at INC 12 only. `gnss_tests` green at every increment; any test diff is
  a red flag, not a fixture update (no test reaches `ApplicationNetwork.cpp` internals — verified).
  One deliberate failure smoke (bad product path) compared on log tail + exit code covers the setup
  failure paths the SHA battery never exercises.
- `/fp:precise` confirmed (no `/fp:fast` anywhere).

## 4. Config split (INC 1 — independent, lowest risk)

Cut `loadNetworkSection_` + anon-ns `parseCoordEpochYear` + `applyNetworkConsumerCouplings` +
`deriveClosedLoopConsumerConfig` + `applyNetworkConsumerProfile` verbatim into
`src/core/config/GnssConfigLoadNetwork.cpp` (boundaries re-derived at INC 0 — the original survey's
end line sliced a closing `log->info`). The eight cross-section forcing writes move unchanged; the
estimation section's raw-JSON peek at `"network.estimate_glonass"` stays behind untouched. Drops
`GnssConfigLoad.cpp` ~4,648 -> ~2,870 and gives monthly knob growth its own TU. Flat-member ->
`NetworkConfig` struct consolidation deferred (separate card; ~77 forwarding getters).

## 5. Ordered increments (each: build -> gnss_tests -> matrix compare vs INC-0 goldens -> commit)

- **INC 0 — Preconditions + harness.** Gates §0 satisfied. Regenerate symbol-anchored cut map. Build
  compare script + capture all goldens (incl. variants) + empirical mask freeze + OpenMP audit +
  parallel twice-runs + 3h crash signature + resume-pair baseline. No production changes.
- **INC 1 — Config split** (§4). Full build, config-focused tests, smoke compare.
- **INC 2 — Type hoist.** Anon-ns structs/helpers verbatim -> `NetworkRunState.hpp`,
  `NetworkRobustStats.hpp`, `NetworkRunTypes.hpp` (all `filter/`); `ifElevMap`/`parseIfPairResetKey`
  -> `NetworkSlipQc` skeleton; `FlagRate` -> `NetworkRunTraces.hpp`. Linkage-only (anon-ns -> named
  inline). ADD unit tests for `medianPolish2Way` / `robustProminence` / `parseIfPairResetKey` (pure
  helpers, new coverage, zero behavior change). Smoke compare.
- **INC 3 — Logging unit (RESCOPED).** `NetworkRunTraces` owns ONLY what needs streams/paths/config:
  stream decls + single-definition headers, open scope, roster CSV, manifest (jesc/FNV), rotation,
  close, teardown flag-rate summary + `gaussAlpha`. The state-reading writers (health heartbeat,
  raw-slip trace, `emitEpochStatus`/`emitEvent` bodies) move LATER with the increments that introduce
  the state they read. Compare with all trace toggles armed (golden from INC 0).
- **INC 4 — Context + workspace + RTB-7 trio (milestone).** Introduce `NetworkRunContext` AND
  `EpochWorkspace` via reference aliases (declaration blocks only; use-sites untouched). Move to
  `NetworkRunContext.cpp`: fingerprint (concatenation order byte-for-byte), warm-resume restore
  (emitter-first all-or-nothing), snapshot write, carry lifecycle (D2 inject incl. C1 dense fold,
  harvest, stash, retro/URA bookkeeping). Health heartbeat + `emitEpochStatus`/`emitEvent` join
  `NetworkRunTraces` now (their state exists). Checks: full matrix + resume pair + 1h (c).
- **INC 5 — Setup TU.** Two functions split around `resolveData()` (stays in orchestrator): input
  validation; ingest/products/store/emitter/contract/consumer setup + est hoist. Early return-1 exits
  become status codes with identical log lines; failure smoke covers them. Smoke compare.
- **INC 6 — Slip QC.** T0 drain, `refloat_signal`/`refloat_pair` verbatim (NOT merged), MW-detect
  refloat, IF-TD driver + `PairSample`. `PassABuffers` replaces the brace scope; cross-epoch EWMA
  stays in `NetStation`. Compare with slip traces armed + **1h QC run (c)**.
- **INC 7 — Assembly TU (milestone).** `isGaugePinned`, `splitStation` (still throws),
  `appendGim`/`ShIonoRows` (need `gim_` -> Application TU), PASS A head + skip exits, PASS A parallel
  tail (pragma verbatim), observability prefilter, column registration + counts, PASS B produce
  (`foldAmb`/GLONASS/carry-build) + serial consume. **Anchor election + incumbent carry go to
  `filter/NetworkPhaseDatum`, NOT Assembly** (verifier fix — the highest-churn workstream gets its own
  seam; Assembly shrinks to pure row/column assembly). `/bigobj`. Re-audit logger-prewarm list against
  the moved call tree. Checks: smoke serial+parallel + 1h (c) + legacy AND prioritized datum configs.
- **INC 8 — Existing-seam moves.** `healNearNullPhaseBias` -> **NetworkPhaseDatum** (with anchor
  election, completing the seam); column descriptors + estimability spectrum + `kNullSpaceTol` ->
  `NetworkEstimator`; W6 code-bias datum gauge rows -> `NetworkSbasis::buildCodeBiasDatumRows`
  (freq-oracle by reference). Unit tests per the `test_network_estimator.cpp` pattern. Compare incl.
  estimability-armed + W6 configs (INC-0 goldens).
- **INC 9 — RAIM unit (milestone).** `filter/NetworkRaim`: within-epoch adaptation (`assess`,
  `resolve_dropping` with `cap_*`/`cap_dense_*` channel intact, culprit loop), reduce-once check,
  per-const withhold (delegating to `NetworkEstimator::decidePerConstWithhold` — exists, verified),
  post-fit passes, sigma0+EWMA, consensus, attribution + `refloat_sat_at`, contamination log,
  consensus refloat. `x_s`/`est_index_of` mutated by reference exactly as today. Header banner flags
  the primary-path/replay duplication (`applySharedSystem` unification deferred). Likely `/bigobj`.
  Checks: 1h QC-tripping (c) — the load-bearing gate — + resume pair.
- **INC 10 — Clock QC unit.** `filter/NetworkClockQc`: Card-6 re-level, oscillator prior fold +
  stale-drop, post-solve NIS, clock monitor; operates on `ctx.carry` by reference; `handover_affects`
  passed as predicate; C1 stamp stays at emit site. Compare three ways at 1h: clock_model on;
  clock-monitor-only (INC-0 golden); `brdc_rts` (e).
- **INC 11 — Emit TU.** Solve + `solve_failed` skip, sol build -> stamps, quarantine, carve-out prune,
  URA + ramp, datum-bump + PIVOT events, retro/eph-cutover landing, pivot-baseline advance (emit-only
  position documented), anchor handover record, deferred contract-writer open, store emit +
  rebroadcast, product write, counters + T2 commit, closed-loop eval; teardown contract self-check +
  closed-loop summary. Read-only diagnostic writers move to `NetworkRunTraces` taking an
  `EpochSolutionView`. Ordering preserved verbatim (`emitToStore` before consumer and soln trace;
  prev-time stamp before snapshot write; H heartbeat position). Compare product + closed-loop + full
  CSV battery + contract-check PASS.
- **INC 12 — Final trim + validation (milestone).** Orchestrator banner rewrite; `runNetwork` < ~600
  lines; `wc` every TU. Try dropping `/bigobj` from the orchestrator. Doxygen pass (seams full,
  support headers `@internal`). Full matrix serial/parallel × sp3/brdc_rts × legacy/prioritized/
  sum-to-zero × closed-loop + resume pair + clean 3h A/B + **3h crash-signature recheck**. **CLAUDE.md
  gets the placement decision tree verbatim** (the anti-regrowth deliverable): (a) extend
  `NetworkEstimator`/`NetworkSbasis`/`NetworkPhaseDatum`; (b) new domain class in
  `filter/`//`slip/`//`logging/` per the `NetworkPivotHandover` exemplar; (c) Application TU only for
  phase orchestration and `gim_`-dependent glue — plus the `NetworkCarryState` three-place rule.
  Annotate crash-evidence/memory line references with the old->new file map. Commit.

## 6. Risks & guardrails (verifier-hardened)

- MSVC codegen drift across TU boundaries under `/fp:precise`: possible in principle; the
  per-increment byte gate catches it. Any diff halts and bisects that increment — never patch forward.
- Lambda->function conversion is the highest-frequency error source: capture-list diff per peel, knobs
  const-from-birth, mutable state through ctx/ws references only, Eigen rules (§2).
- Fingerprint + knob read order are behavior-visible (pre-refactor snapshot resume); resume pair gates
  at 4, 7, 9, 12 with the deterministic cross-version protocol.
- The eight skip payloads differ by design; a reviewer tempted to unify them is making a behavior
  change — reject during this series.
- `cap_*`/`cap_dense_*` replay drift across TUs: `EpochWorkspace` doc block names every writer site;
  `NetworkRaim` header cross-references.
- OpenMP: `schedule(dynamic)` means identity rests on slot-write discipline — audited at INC 0,
  twice-run goldens prove run-to-run determinism before any motion.
- Under-verification of QC paths: 1h QC-tripping gate at 4, 6, 7, 9, 10, 12; row-count assertions make
  untripped tiers loud. The pre-existing ~1.5h 305 crash and BeiDou divergence stay out of scope, but
  the crash signature is pinned at INC 0 and rechecked at INC 12.
- Scope creep: the only permitted new code is Doxygen, declarations, unit tests for already-pure
  helpers, and the harness script.
- Environment: `LNK1104` -> retry; WER zombies -> pre-kill; TU frozen for the series; concurrent
  sessions re-grep markers.

## Executor split (agreed pattern, Jul 4 2026)

- **INC 0 = harness + masks + cut map + post-C1 state inventory.** Self-referentially unverifiable —
  a quiet scope-narrowing that masks a real diff would pass an unexercised gate. Build with maximal
  skepticism; goldens + masks are READ-ONLY once frozen.
- **INC 1, 2, 3, 5, 6, 8, 10, 11 = mechanical, byte-gate-protected.**
- **INC 4, 7, 9 milestones + INC 12 final** get per-increment verification pre-commit
  (capture-semantics / `cap_*` replay / OpenMP / fingerprint-order hazards can pass an unexercised
  gate).
- **Hard rules:** goldens + masks READ-ONLY (any diff = stop, never regenerate/patch forward); one
  increment per session; every lambda peel ships a capture-list diff artifact; re-grep symbol anchors
  at session start.

## Deferred (explicitly NOT this series)

Dead `clockOscUpdate`; `cap_*` replay dup (`applySharedSystem`); refloat idiom dup; `sysOfChar` dup;
skip unification; `NetworkConfig` struct consolidation (~77 forwarding getters); `GnssConfig.hpp` /
`GnssConfigTypes.hpp` split beyond the loader TU.

## Effort estimate

13 increments. INC 0-3 are light (harness + mechanical cuts); 4, 7, 9 are the heavy milestones
(context introduction, assembly, RAIM). With 1h gates at six increments plus milestone resume pairs,
wall-clock is dominated by validation runs, not editing. Realistic: 1.5-2 focused weeks, pausable at
any increment boundary since each commits independently green.
