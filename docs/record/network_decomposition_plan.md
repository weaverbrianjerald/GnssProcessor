# ApplicationNetwork.cpp decomposition — consolidated plan

*Jul-4 2026 design (16-agent scoping workflow: 9 mapping agents producing a section-by-section anatomy
of the then ~7,300-line file plus its external coupling and config growth, 3 competing architects, 1
judge, 3 adversarial verifiers). Winning design: **domain-first along existing network seams** — scored
8.8 vs 7.7 incremental-TU and 6.5 class-decomposition, improved with the losers' best mechanics, amended
with every blocker/major from verification; all three verifier lenses returned **sound-with-fixes**.
Absorbed 2026-08-14: the Jul-16 addendum (freeze SHA `56da23a`) — now §7-§11 + Appendix A.*

**STATUS: the series is COMPLETE (INC-0 … INC-12).** It cut `ApplicationNetwork.cpp` to ~1.6k lines of
thin epoch-loop orchestration (`subBegin; networkX_(...); subEnd;`) — the figure CLAUDE.md still quotes.
⚠ Measured 2026-08-15 the file is **2,359 lines**: post-series growth, which is exactly what INC-12's
anti-regrowth placement decision-tree (delivered into CLAUDE.md, not here) exists to bound. This doc is
the **design + landed record**, not a work queue.

> ⚠ **Provenance.** Every `runs/<name>` path below resolves under `D:\GnssProcessor_archive\2026-08-12\runs\`
> (the 619-dir / 40.6 GB tree was archived off C: on 2026-08-12), not in the working tree. Golden run
> dirs and the 3h crash-evidence dir are in that archive; the COMMITTED capture manifests at
> `goldens/decomp/*.manifest.json` are not.
> ⚠ **Section numbering is APPEND-ONLY.** §0-§6 are cited externally as "§2-6"
> (`docs/SPEC_inc8b_election_split.md` preamble, `MEMORY/project_network_decomposition_plan.md`).
> Never renumber or retitle them. Absorbed/new material appends at §7+.

---

## Implementation status (living section — update as increments land)

### Landed record, per increment

Gate results, file-size deltas and DEVIATIONS-TO-ABSORB are the durable content here; §5 carries the
design intent, not a second copy. Baseline = freeze SHA `56da23a` (§7). The per-increment suite integers
(780/779/1 … 818/817/1) are **gate evidence at that increment**, not the suite size — current suite,
re-counted 2026-08-12: **1360 cases / 196 suites / 89 files**, one fixture-gated skip.

- **INC-0 ✅** — 8-arm fast matrix captured: twice-run selfcheck CLEAN ×8, ON-path counters verified per arm, manifests at `goldens/decomp/*.manifest.json`, maskspec FROZEN. Additions: D0/T2b/timing runlog drops + snapshot M-record UTC mask.
- **INC-1 ✅** — config split: `GnssConfigLoadNetwork.cpp` 3,110 lines cut verbatim from `GnssConfigLoad.cpp` 6,000→2,926; suite 780/779/1; matrix 8/8 byte-identical.
- **INC-2 ✅** — anon-ns hoist → `filter/NetworkRunTypes.hpp` + `filter/NetworkRobustStats.hpp` + `slip/NetworkSlipQc.hpp` seed; +12 unit tests for the pure helpers; suite 792/791/1; matrix 8/8. NOTE: `RunState.hpp` deferred to INC-4 where the context is born, `FlagRate` to INC-3 with its header.
- **INC-3 ✅** — `logging/NetworkRunTraces` owns the 19 trace streams + paths + rotation-header consts + `FlagRate`; startup open scope → `openAll()`, roster → `writeStationRoster()`; alias block keeps writer use-sites unchanged; 14,176→13,822; suite 792/791/1; matrix 8/8. DEVIATIONS: L15 manifest writer + teardown flag-rate/run-summary CSVs deferred to INC-4 — they read the ~40-knob/counter state that becomes `NetworkKnobs`/`ServiceCounters` there, per the rescope principle.
- **INC-4 ✅ (context MILESTONE)** — `app/NetworkRunContext.hpp` = `NetworkKnobs` [81 const snapshots, factory preserves read order] + `NetworkCarryState` [Tier-1 stretch verbatim incl. the WL/UWL run-latched sets + D5 tag sets] + `NetworkServiceCounters`; `EpochWorkspace` [cap replay channel + `clk_pred`/`clk_prior_info`] in `NetworkRunTypes.hpp`; ten free lifecycle fns in `app/NetworkRunContext.cpp` [fingerprint/restore/snapshot + 4 injects + harvest + retro/URA + stash — free fns over `(ctx, config_)` instead of `Application` members: recorded deviation, none needs other `Application` state]; `NetworkRunTraces` absorbs manifest/heartbeat/status/event/rotation/teardown CSVs/`closeAll`; suite 792/791/1; 13,822→12,004.
  - **10-arm gate byte-identical** = the 8 fast arms + NEW `snap_resume` resume-pair leg (leg B warm-resumes the golden snap-arm snapshot via `start_offset_hours=1/12`, baselined pre-INC-4, WARM RESUME non-vacuous: 31 D2 carries) + NEW `obs` observability arm (the 8 arms leave every moved writer OFF-path; `obs` arms manifest/heartbeat/flag-rate/status/events, baselined pre-INC-4 via stash round-trip; P1 rotation covered on the guard path only).
  - DEVIATIONS TO ABSORB LATER: WL/UWL machinery + local types (INC-9b), Card-6 relevel block (INC-10), emit-site URA/retro landing + `cl_*` metrics + WithheldGate H-line (INC-11), `brdc_nav_staged`/cursor (INC-5), SB_* sub-timers (INC-12).
- **INC-5 ✅** — Setup TU: `networkValidateInputs_` [Phase 0] + `networkSetup_(NetworkSetupResult&)` [Phases 2-4 + parallelism + owned sink/emitter + product serialization/v2 contract + closed-loop consumer, 172-1108 verbatim] into `ApplicationNetworkSetup.cpp`; `app/NetworkSetupResult.hpp` = non-movable fill-in-place handoff (address-pinned by sync→roster, engine→`consumer_config` `GnssConfig&` and `&network_store`); `navAvailabilityGpsSeconds` → shared inline fn; all 13 early return-1 exits verbatim with rc forwarding. DEVIATION: est-hoist block stays in the run TU (log-ordering + `carry_driver` entanglement; absorb at INC-12). Gate: suite 796/795/1 (the +4 are the concurrent MW `bridge_sample_debias` WIP tests, all passing) + 10-arm matrix byte-identical + 2 failure smokes (`_dcmp_fail_products` exit-1 no-orbit text, `_dcmp_fail_norinex` exit-1 Phase-0 text). 12,004→11,110.
- **INC-6 ✅** — Slip QC: `PassABuffers` seam replaces the SB_SLIP brace-scope buffers (aliases keep the PASS-A loop verbatim until INC-7); T0 repair drain + MW-detect/LLI refloat (`refloat_signal`) + IF-TD driver (`refloat_pair` + `PairSample`) relocated VERBATIM into `src/core/slip/NetworkSlipQc.cpp` as three free functions over explicit refs (config gates + knob snapshots move INSIDE; `emitEvent` thin-forwarder copies; `carry_slipped_ambs` by NON-CONST ref — ONE instance, C3c funnel intact; `if_pair_scale` EWMA stays in `NetStation`; SB_SLIP subBegin/subEnd pair intact in the run TU). DEVIATIONS: in-loop per-station detection stays with the OMP loop (moves at INC-7 with the loop); detector CONSTRUCTION stays in the Setup TU (zone-1 routing overtaken by the INC-5 verbatim cut). Gate: suite 796/795/1 + 10-arm matrix byte-identical. 11,110→10,797. ⚠ **The INC-6 gate was weaker than it reads** — §9's slip-QC coverage warning; §5's original "compare with slip traces armed + 1h QC run (c)" was SUPERSEDED by §8 before it ran.
- **INC-7 ✅ (Assembly TU MILESTONE)** — nine private member fns in `ApplicationNetworkAssembly.cpp` (+`/bigobj`): H1-H4 pre-loop helpers (`isGaugePinned` / `splitStation` [throw kept] / `appendGimIonoRows` [`gim_` via member] / `appendShIonoRows` [`sh_model`/`iono_deweight`/`shc`/`sh_dev` explicit params]) + P1 PASS-A head (2 `sync_skip`s) + P2 PASS-A parallel tail + slip scans (OMP pragma verbatim; `carry_slipped_ambs` by non-const ref) + P3 `slip_raw` writer + prefilter (writer-before-prune structural) + P4 column registration (const counts → `int&` out-params; `est.beginEpoch` inside) + P5 PASS-B produce/consume (coupling cluster rides along for 7b; D0 assembly wall moves whole with `double&` sinks). `EpochAction {Proceed,SkipEpoch,StopRun}` (`NetworkRunTypes.hpp`) replaces the 4 moved epoch-`continue`s — skip bookkeeping verbatim IN-BODY, call-site `continue` preserves the `subEnd(SB_PREASM)` bypass. Alias-preamble mechanic: every preamble line is a byte-copy of its run-TU original (4-lens adversarial verify: zero defects, all diffs classified). STAYS: SB brackets, anchor+eq34 elections (INC-8/8b), `buildEnsembleGauges`/`buildCarryDdPriors` (9b), W6 datum + heal (8), ws cap block. DEVIATIONS: `asm_res`/`total_pins`/`est_index_of` hoisted outside the (masked) D0 timed wall; `i_lo` inside the SB_SLIP window; prewarm re-audit = construction-time-guarantee comment only. Gate: suite 796/795/1 at each of 4 sub-steps + per-step 2-arm byte checks + 10-arm matrix byte-identical. 10,797→9,160; Assembly TU 1,952.
- **INC-8 ✅** — existing-seam moves: W6 code-bias datum rows → static `NetworkSbasis::buildCodeBiasDatumRows` (+`kCodeBiasGaugeWeight` class const, `CodeBiasDatumRows` result; freq-oracle passed BY REFERENCE as a driver-owned `std::function`; the fold + `cap_*` RAIM-replay capture + first-N summary stay driver-side per the writer-site contract; scratch clears stay unconditional); estimability spectrum dump → `NetworkEstimator::dumpEstimabilitySpectrum` + `kNullSpaceTol` as the class const (all 6 use sites re-pointed; the `spectrum_diag_budget` latch stays driver-side — THREE-reader coupling with the W6-cb one-shot log and the Assembly `col_desc` gate; `col_desc` stays in the Assembly registration — plan-sheet deviation, its `"CLK\|sat"` format is golden-gated and ≠ `sharedColumnLabel`); `healNearNullPhaseBias` → NEW seam `filter/NetworkPhaseDatum` (free fn over explicit refs; anchor election + incumbent carry deferred to INC-8b — the election is interleaved with the eq34 election via `anchor_incumbent`, splitting the 4-way chain is an 8b-time ruling). +9 ADDITIVE unit tests (5 Sbasis builder incl. Card-2 time-link + hold/latch/relatch, spectrum smoke, 3 phase-datum heal incl. gate-throws-before/passes-after). NEW `estim` gate arm (`_dcmp_50sta_10ep_estim`, `trace_estimability` armed, baseline captured from the `4ca878a` binary, selfcheck CLEAN) — the spectrum text is now byte-gated. Gate: suite 805/804/1 (+9) + 10-arm matrix + `estim` arm ALL byte-identical; per-sub-step 2-arm checks (`consumer` for W6, `estim` for spectrum, `allon` for heal). 9,160→8,780.
- **INC-7b ✅** — `NetworkCoupling` seam: the Card-1/1b/1d conditional coupling-row cluster relocated VERBATIM out of `Application::networkPassB_` into NEW `filter/NetworkCoupling.{hpp,cpp}` — the six former `[&]` lambdas (`resolveSharedCol`/`couplingId`/`couplingQ`/`tuGaugeRestore`/`tuReinitSince`/`buildCplRow`) + the joint/scalar append blocks become `CouplingRowBuilder` member fns over explicit const-ref/value members (aggregate, constructed FRESH per station in the PASS-B OMP produce loop — the former loop-body scratch `crows`/`craw`/`white_pw`/`white_map` are members, so the bodies relocate byte-for-byte; NO `config_`/`gim_`/`log_manager_` reachability ruled out the INC-7 member-fn shape); the serial counter fold + Card-1d debug dump become the free fn `foldAndDumpCouplingRows`. STAYS: the type vocabulary (`CoupledShared`/`CplRowRaw`/`kCouplingSparsifyRelThresh`/`whitenCouplingRows` — the last kept header-INLINE so its two callers keep identical codegen) in `NetworkRunTypes.hpp` (moving them = mutual include cycle via `LocalCarry::coupling`/`StationAssembly::cpl_raw` by-value); the harvest (`regcol`/`harvestCoupling` in `NetworkRunContext.cpp` — INC-4 locality + zero collision with the concurrent LF-1 lane-fold session); the run-TU RAIM re-whiten + Card-6 relevel (consumers, unedited); the legacy diagonal `inj` path + `tu_reinit_hist` serial pre-loop update. DEVIATION: the S/T/U three-way append chain re-forms to `if(carry_cpl_cond){cb.appendCouplingRows();}else{U}` (boundary structural adaptation, verbatim bodies). +11 ADDITIVE unit tests (`test_network_coupling.cpp`: real `whitenCouplingRows` incl. the refloat skip-set + non-SPD fallback + empty-success — all zero prior coverage; `CouplingRowBuilder` scalar/joint/departed/κ-shrink/SH-marginalize; append scatter scalar+joint). Assembly TU 1,952→1,577; `ApplicationNetwork.cpp` unchanged at 8,780.
  - **CRITICAL GATE FINDING:** the entire 10-arm matrix injects 0 coupling rows (every coupling-armed arm also sets `carry_invariant=true`, which supersedes phase-leveling), so the byte gate would have validated dead arithmetic — added TWO NEW coupling-LIVE arms baselined from the pre-cut binary: `cpllive` (scalar) + `cpljw` (joint), each 173,775 present + 172,245 κ-shrunk terms + 12 debug-dump lines, twice-run CLEAN, gate asserts nonzero counters (invariant #4). Gate: suite 818/817/1 (+11) + full matrix byte-identical (`alloff`/`allon`/`jw`/`wlinj`/`consumer`/`snap`/`snap_resume`/`obs`/`estim`/`cpllive`/`cpljw` ALL PASS; `lane`/`uwl` trip ONLY on the concurrent LF-1 session's `fold_blocked=false` config-banner append, unrelated to this cut).

### INC-8b … INC-12 — landed, NOT recorded here

The table above stops at INC-8/7b because the record moved. **INC-8b** (the anchor/eq34 election split)
has its own spec, `docs/SPEC_inc8b_election_split.md`. **INC-9 … INC-12** landed; their outcome is
CLAUDE.md's "Network producer — where new code goes" decision-tree, which names seams absent above
(`NetworkSolPrep`, `NetworkEmitStore`, `NetworkTeardown`, `NetworkIonoDiag`, `NetworkStashStatus`,
`NetworkMwWlGlue`, `NetworkSolveGlue`, `NetworkInstallCarry`, `NetworkDdPriorBuild`, `NetworkRelevel`,
`NetworkChainDatum`, `NetworkDdFrac`, `NetworkShDevVtec`, `NetworkShHealth`, `NetworkLeveling`). No
per-increment gate record exists in doc form for 9-12 — read the commits.

⚠ **Stale by construction:** every line number in §7-§11 and Appendix A is AS-OF-`56da23a`; the symbols
they name have since been relocated into seam TUs. Historical inventory, never a cut anchor. The same
applies to §1-§6: **all line numbers below are ADVISORY** — the original map was surveyed while S1/C1
edits were landing (verified drift +13 to +190 lines), and INC-0 regenerated the whole cut map anchored
by symbols/banner comments against the frozen baseline SHA. A cut boundary that cannot be resolved to a
unique symbol is a plan defect, not a judgment call.

---

## Philosophy

Every extracted unit either (a) extends an existing factored network class (`NetworkEstimator`,
`NetworkSbasis`), (b) becomes a new domain class in the established category dir following the
`NetworkPivotHandover` extract-for-testability exemplar, or (c) becomes an additional `Application` TU per
the `ApplicationBaseline`/`ApplicationRealtime` same-class-multi-TU precedent. `runNetwork()` survives as
a ~550-line phase orchestrator. Every increment is verbatim code motion, compiles standalone, passes
`gnss_tests`, and is proven byte-identical on a golden-run matrix before commit. No behavior changes; all
cleanups (dead `clockOscUpdate`, `cap_*` replay duplication, the EIGHT epoch-skip idioms, refloat idiom
duplication, `sysOfChar` duplication, `NetworkConfig` struct consolidation) are deferred to follow-up cards.

## 0. Sequencing gates (BEFORE the series starts)

All four CLOSED before INC-0 (§7 confirms the freeze state).

1. **C1 full-cov carry lands and validates.** The state inventory below must be re-derived AFTER it lands — C1 adds `prev_carry_cov`, `cap_dense_keys`/`cap_dense_cov` and a dense-stash lifecycle the original map missed.
2. **S1 phase-datum working tree committed.**
3. **pdp coverage-threshold union-gating card lands and validates BEFORE INC 0** (verifier requirement, not optional): small, surgical, touches anchor election — which moves at INC 7 — and fixes a known non-PD defect in the just-validated flagship feature. Deferring it means golden baselines bake in defective anchor-election behavior and the fix later lands into freshly relocated code. Card 7/8 consumer work had to be triaged EXPLICITLY (before series, or queued after INC 12 — no "or" left unresolved); triaged to **after INC-12**. v3 robust-M + per-LOS iono **stay holstered** for the series.
4. **Freeze `ApplicationNetwork.cpp` to this series** (Gate 5, TU freeze). Any urgent fix lands at an increment boundary (each increment commits independently green). Expect `LNK1104` build-lock retries; pre-kill WER zombie `gnss_processor` processes before harness runs.

## 1. Target file tree (est. lines; sizes advisory)

| Unit (dir) | est. | Contents |
|---|---:|---|
| `scripts/compare_network_run.py` | ~200 | field-aware golden-run compare (INC 0, tooling) |
| `config/GnssConfigLoadNetwork.cpp` | ~1780 | verbatim cut of loadNetworkSection_ + consumer couplings/profile helpers (INC 1) |
| `filter/NetworkRunState.hpp` | ~330 | CarryPrior, LocalCarry, PlAmb, PlZwd, StationAssembly, ClockOsc/Pred + predict/update |
| `filter/NetworkRunTypes.hpp` | ~280 | NetStation, EpochWorkspace, PassABuffers, NetworkSetupResult, EpochStatusRow, EpochAction — `filter/`, NOT `app/`, which would invert the filter/slip → app include direction |
| `filter/NetworkRobustStats.hpp` | ~190 | medianCopy, robustProminence, medianPolish2Way (header-only, unit-testable) |
| `filter/NetworkRaim.{hpp,cpp}` | ~1300 | within-epoch adaptation + post-fit RAIM + attribution + consensus refloat |
| `filter/NetworkClockQc.{hpp,cpp}` | ~650 | Card-6 re-level, oscillator fold, NIS, monitor |
| `filter/NetworkPhaseDatum.{hpp,cpp}` | ~400 | anchor election + incumbent carry + healNearNullPhaseBias — home for the pdp coverage card and the future RT brdc-anchored datum mode (verifier-mandated seam) |
| `filter/NetworkEstimator` (ADD) | ~150 | column descriptors + estimability spectrum, kNullSpaceTol as class const |
| `filter/NetworkSbasis` (ADD) | ~170 | buildCodeBiasDatumRows (W6 gauge rows; freq-oracle passed BY REFERENCE) |
| `slip/NetworkSlipQc.{hpp,cpp}` | ~620 | ifElevMap, parseIfPairResetKey, T0 repair drain, MW-detect refloat, IF-TD driver, PassABuffers |
| `logging/NetworkRunTraces.{hpp,cpp}` | ~1650 | all 16+ streams, single-definition headers, open/rotate/close, manifest, status/event/health writers, diagnostic blocks, flag-rate summary, FlagRate struct (lives HERE, not `filter/`) |
| `app/NetworkRunContext.hpp` | ~320 | NetworkCarryState / NetworkServiceCounters / NetworkKnobs + non-owning refs (lifetime-ordered) |
| `app/Application.hpp` (ADD) | ~90 | ~14 private member-fn decls; FORWARD-DECLARE context/workspace types (refs only) — full headers included only in the network TUs |
| `app/NetworkRunContext.cpp` | ~680 | fingerprint + warm-resume restore + snapshot write + carry lifecycle — the "three-place rule" in ONE TU |
| `app/ApplicationNetworkSetup.cpp` | ~850 | Phases 0-3b as TWO fns split around `resolveData()` (stays in the orchestrator), buildStation, sinks, consumer, est hoist |
| `app/ApplicationNetworkAssembly.cpp` | ~1200 | splitStation, iono row appenders, PASS A/B, consume scatter (`/bigobj`) |
| `app/ApplicationNetworkEmit.cpp` | ~800 | emit sequence, closed-loop eval, contract check |
| `app/ApplicationNetwork.cpp` (SHRUNK) | ~580 | runNetwork orchestrator only |

**AS BUILT the tree went further:** `ApplicationNetworkEmit.cpp` never appeared under that name — emit
split into `NetworkSolPrep`/`NetworkEmitStore`, and INC-9..12 added the rest of the seam list in the
Landed-record section. CLAUDE.md's decision-tree is the current map; this is the design intent it grew
out of. New public headers: genuine seams (`NetworkRaim`, `NetworkClockQc`, `NetworkPhaseDatum`,
`NetworkSlipQc`, `NetworkRunTraces`) get full house-style Doxygen; run-loop support headers
(`NetworkRunState`, `NetworkRunTypes`, `NetworkRunContext`) get `@internal` banners with brief-level
docs, per the `EpochUpdateSupport.hpp` precedent. CMake: +10 sources, explicit list; `/bigobj` moves to
Assembly/RAIM, dropped from the shrunk orchestrator at INC 12 if a clean build proves it.

## 2. Shared-state strategy

Three tiers by lifetime; `Application` keeps ZERO network data members (current deliberate design).

**Tier 1 — cross-epoch: `NetworkRunContext`** (a `runNetwork()` local passed by reference):
- `NetworkCarryState`: re-derived at INC 0 from a fresh scan of ALL cross-epoch mutables in the post-C1 tree (`prev_carry`, `prev_carry_cov` + `fc_cols` dense lifecycle, `local_carry` ORDERED map, `zwd_carry`, `clock_osc`, `prev_clk`, `prev_pivot`/`last_pivot`, anchor incumbency, ura/retro/ramp state, `relevel_iode`, `eph_cutover_disc_pending`, `generation_id`, `iod`, `prev_week`/`tow`, `start_anchor_s`, …).
- The snapshot correspondence is an annotated SUBSET rule, not a definition: "fields marked `[snapshot]` must be added to fingerprint + restore + snapshot write — all three live in `NetworkRunContext.cpp`." `NetStation`'s deliberately non-snapshot fields (`if_pair_scale` EWMAs, slip-detector state) are enumerated in a header comment with the exemption rationale; any NEW cross-epoch field defaults to `NetworkCarryState` under the three-place rule unless explicitly exempted (named expected consumer: future per-station carries).
- `NetworkServiceCounters`: `n_emitted` family, health-window anchors, `FlagRate`s, timers.
- `NetworkKnobs`: the ~45 const config snapshots initialized in the SAME read order (const from birth; 81 as built). `clk_anchor_sigma` joins ONLY if `resolveNetworkAprioriClockAnchorSigmaM()` is verified a pure config read (no logging, no run state); otherwise it stays at its current position in the Setup TU under the alias treatment.
- Non-owning refs in lifetime-encoding declaration order (sync outlives stations; `network_store` before consumer; `est` outlives the loop — the 146 GB memory-pool lesson).

**Tier 2 — per-epoch: `EpochWorkspace`**, introduced at INC 4 (was unscheduled — verifier fix). Freshly constructed EVERY loop iteration — NEVER hoisted for reuse (`sigma0_adapt==0` and "cleared and refilled on every `assess()`" are load-bearing semantics). Members whose current initializers are mid-loop expressions keep their assignment at the original site with the member default-initialized to the same value; per-member write-before-read audit at INC 4/7/9. Includes the C1 dense-capture channel (`cap_dense_keys`/`cap_dense_cov`) alongside `cap_H`/`z`/`priors`, with a doc block naming every writer site of the RAIM replay channel.

**Tier 3 — QC classes own NO state** beyond references (carry maps, refloat sets, oscillators stay in the context because the snapshot serializes them) — StateManager-discipline conformant.

**Migration mechanic:** every increment that re-homes declarations replaces only the declaration block with `auto& name = ctx.name;` aliases under the original names — downstream use-sites stay textually unchanged, so each diff reviews as pure relocation. Aliases peel only when a block moves out of the TU (new signature takes ctx/ws explicitly).

**Control flow:** phases return `EpochAction {Proceed, SkipEpoch, StopRun}`. The EIGHT skip/withhold sites (4× `sync_skip`, `datum_failed`, `gate_withheld`, `solve_failed`, `sigma0_withheld` — count re-verified against tree) keep their exact site-specific bookkeeping verbatim; payloads differ per site and are deliberately NOT unified. Emit-only baseline freeze becomes structural (emit code unreachable from Skip paths). Two documented non-normalizable exceptions: Card-6 re-level mutates carry pre-solve; `resolve_dropping` replays via `cap_*`.

**Lambda policy + peel discipline (verifier-hardened):** single-consumer lambdas stay internal to their hoisted function; cross-phase lambdas become functions/methods with explicit args. For EVERY peel produce a capture-list diff — each captured entity, whether read at call time (by-ref) or definition time (by-value), and proof the new parameter/member reproduces the same read point. The W6 freq-oracle and every `cap_*`-touching callable take REFERENCES; by-value capture of any container mutated between construction and invocation is banned. `emitEpochStatus`/`emitEvent` take `GpsTime` explicitly; their `es_*` config echoes become members initialized bit-identically. Eigen: new signatures take concrete const-ref types (never auto/templates); no `auto` bindings of Eigen expressions in moved code; fixed-size vectorizable Eigen members in new structs get alignment treatment or verified non-vectorizable. `splitStation` keeps its throw.

**OpenMP:** both parallel regions move verbatim inside their functions; ambient `[&]` captures become explicit (ctx, ws) references — identical sharing, and NO capture ever converts to `this` or by-value. Both use `schedule(dynamic)`, so parallel byte-identity rests entirely on per-index-slot-write discipline: INC 0 audits no `omp atomic`/`critical`/`ordered`, no stream writes inside regions, all shared mutation via index-addressed slots drained serially. PASS A private buffers become the `PassABuffers` handoff struct preserving produce-then-serial-drain.

## 3. Byte-identity harness (INC 0 deliverable)

- **`compare_network_run.py`** — field-aware, and NOT vacuously passable (blocker fix): at golden capture it records the exact artifact set (relative paths, expected-non-empty flags, expected QC-row-count minimums) and FAILS on any missing or extra file, plus asserts nonzero data-row counts for the QC traces the run is supposed to trip. CSV-column / JSON-key granular masks only.
- **Mask list built EMPIRICALLY, then frozen:** run the pre-refactor binary TWICE per matrix config and self-compare — every diff is a mask candidate. Known wall-clock writers to mask: manifest `written_utc` + `__DATE__`/`__TIME__`, health windowed ms-per-epoch column, run-summary `loop_ms`, and the state snapshot M-record `wall_utc` (field-level, format-aware — the snapshot is the strongest full-precision artifact and must never be file-level excluded). After freeze, a "needed" new mask mid-series is a red flag that behavior changed.
- **Snapshots armed in EVERY matrix config** with a forced final-epoch write (the snapshot's full-precision `{}` doubles are the only gate below CSV print precision of `{:.4f}`/`{:.6f}`). Optional: debug-only hex-float dump of `x_s` at the final epoch.
- **Golden matrix as DESIGNED — ⚠ SUPERSEDED AS BUILT by the fast matrix of §8/§9** (heavy arms and the 3h work moved to a post-INC-12 phase). Each serial AND parallel; `parallel_max_threads` PINNED in configs; capture machine recorded; each parallel golden run TWICE and self-compared — a config that is not run-to-run identical is fixed or dropped before any code moves. Arms: (a) `_net305_3sys_smoke` legacy pivot; (b) prioritized-datum + SH-iono + AR smoke; (c) 1h QC-armed run that demonstrably TRIPS RAIM/attribution/clock-monitor; (d) closed-loop; (e) `brdc_rts` (Card-6 + IODE); (f) warm-resume pair — DETERMINISTIC protocol: fixed epoch-count first leg so the final snapshot is always the same epoch, then resume the NEW binary from the OLD binary's snapshot (cross-version compatibility is the stated goal). ALL variant goldens (all-traces-armed, `trace_estimability`, W6 sat-code-bias, clock-monitor-only) are captured at INC 0 from the pre-refactor binary; every increment compares against INC-0 goldens, never against the previous increment.
- **3h crash signature** (moved to the post-series phase by §8): run the known-crashing QC-armed 305 3h config once at INC 0; record crash epoch, stations, message sequence. Re-run at INC 12 and require the identical signature — a changed signature halts and bisects like any byte diff. Annotate the crash evidence (`runs/net305_qc_3h_crash/`) and memory notes with the old→new file map at INC 12.
- **parallel_reduce probe:** twice-run self-compare at INC 0; if deterministic, add one reduce-on smoke A/B to the matrix; else document the residual risk explicitly.
- **Cadence — ⚠ SUPERSEDED by §8** (fast matrix only, heavy battery deferred post-INC-12): smokes after every increment; 1h QC run (c) at increments 4, **6**, 7, 9, **10**, 12 (INC 6/10 added — the increments moving the most branch-heavy QC code must be verified on taken paths, or proven from INC-0 golden row counts that smokes trip them); resume pair (f) at 4, 7, 9, 12; one clean-config 3h A/B at INC 12 only. Still in force: `gnss_tests` green at every increment; any test diff is a red flag, not a fixture update (no test reaches `ApplicationNetwork.cpp` internals — verified); one deliberate failure smoke (bad product path) compared on log tail + exit code covers the setup failure paths the SHA battery never exercises.
- `/fp:precise` confirmed (no `/fp:fast` anywhere).

## 4. Config split (INC 1 — independent, lowest risk)

Cut `loadNetworkSection_` + anon-ns `parseCoordEpochYear` + `applyNetworkConsumerCouplings` +
`deriveClosedLoopConsumerConfig` + `applyNetworkConsumerProfile` verbatim into
`src/core/config/GnssConfigLoadNetwork.cpp` (boundaries re-derived at INC 0 — the original survey's end
line sliced a closing `log->info`). The cross-section forcing writes — **13 sites / 10 distinct fields**
(the Jul-16 re-survey CORRECTED the original "eight"; all inside `loadNetworkSection_`) — move for free.
The estimation section's raw-JSON peek at `"network.estimate_glonass"` stays behind untouched. Drops
`GnssConfigLoad.cpp` ~4,648 → ~2,870 and gives monthly knob growth its own TU. Flat-member →
`NetworkConfig` struct consolidation deferred (separate card; ~77 forwarding getters). Verified
boundaries and the new-TU include list are in §11.

## 5. Ordered increments (each: build -> gnss_tests -> matrix compare vs INC-0 goldens -> commit)

Intent + the boundary constraints that still bind. Landed detail, gate results and deviations are in the
Landed-record section above; the heavy-run cadence named here is superseded by §8.

- **INC 0 — Preconditions + harness.** Gates §0 satisfied. Regenerate symbol-anchored cut map. Build compare script + capture all goldens (incl. variants) + empirical mask freeze + OpenMP audit + parallel twice-runs + 3h crash signature + resume-pair baseline. No production changes.
- **INC 1 — Config split** (§4). Full build, config-focused tests, smoke compare.
- **INC 2 — Type hoist.** Anon-ns structs/helpers verbatim → `NetworkRunState.hpp`, `NetworkRobustStats.hpp`, `NetworkRunTypes.hpp` (all `filter/`); `ifElevMap`/`parseIfPairResetKey` → `NetworkSlipQc` skeleton; `FlagRate` → `NetworkRunTraces.hpp`. Linkage-only (anon-ns → named inline). ADD unit tests for `medianPolish2Way` / `robustProminence` / `parseIfPairResetKey`. Enlarged hoist set + order dependency: §11.
- **INC 3 — Logging unit (RESCOPED).** `NetworkRunTraces` owns ONLY what needs streams/paths/config: stream decls + single-definition headers, open scope, roster CSV, manifest (jesc/FNV), rotation, close, teardown flag-rate summary + `gaussAlpha`. The state-reading writers (health heartbeat, raw-slip trace, `emitEpochStatus`/`emitEvent` bodies) move LATER with the increments that introduce the state they read. Compare with all trace toggles armed (golden from INC 0).
- **INC 4 — Context + workspace + RTB-7 trio (milestone).** Introduce `NetworkRunContext` AND `EpochWorkspace` via reference aliases (declaration blocks only; use-sites untouched). Move to `NetworkRunContext.cpp`: fingerprint (concatenation order byte-for-byte), warm-resume restore (emitter-first all-or-nothing), snapshot write, carry lifecycle (D2 inject incl. C1 dense fold, harvest, stash, retro/URA bookkeeping). Health heartbeat + `emitEpochStatus`/`emitEvent` join `NetworkRunTraces` now (their state exists).
- **INC 5 — Setup TU.** Two functions split around `resolveData()` (stays in orchestrator): input validation; ingest/products/store/emitter/contract/consumer setup + est hoist. Early return-1 exits become status codes with identical log lines; failure smoke covers them.
- **INC 6 — Slip QC.** T0 drain, `refloat_signal`/`refloat_pair` verbatim (NOT merged), MW-detect refloat, IF-TD driver + `PairSample`. `PassABuffers` replaces the brace scope; cross-epoch EWMA stays in `NetStation`.
- **INC 7 — Assembly TU (milestone).** `isGaugePinned`, `splitStation` (still throws), `appendGim`/`ShIonoRows` (need `gim_` → Application TU), PASS A head + skip exits, PASS A parallel tail (pragma verbatim), observability prefilter, column registration + counts, PASS B produce (`foldAmb`/GLONASS/carry-build) + serial consume. **Anchor election + incumbent carry go to `filter/NetworkPhaseDatum`, NOT Assembly** (verifier fix — the highest-churn workstream gets its own seam; Assembly shrinks to pure row/column assembly). `/bigobj`. Re-audit logger-prewarm list against the moved call tree. Checks: legacy AND prioritized datum configs.
- **INC 8 — Existing-seam moves.** `healNearNullPhaseBias` → **NetworkPhaseDatum** (with anchor election, completing the seam); column descriptors + estimability spectrum + `kNullSpaceTol` → `NetworkEstimator`; W6 code-bias datum gauge rows → `NetworkSbasis::buildCodeBiasDatumRows` (freq-oracle by reference). Unit tests per the `test_network_estimator.cpp` pattern. Compare incl. estimability-armed + W6 configs (INC-0 goldens).
- **INC 9 — RAIM unit (milestone).** `filter/NetworkRaim`: within-epoch adaptation (`assess`, `resolve_dropping` with `cap_*`/`cap_dense_*` channel intact, culprit loop), reduce-once check, per-const withhold (delegating to `NetworkEstimator::decidePerConstWithhold` — exists, verified), post-fit passes, sigma0+EWMA, consensus, attribution + `refloat_sat_at`, contamination log, consensus refloat. `x_s`/`est_index_of` mutated by reference exactly as today. Header banner flags the primary-path/replay duplication (`applySharedSystem` unification deferred). Likely `/bigobj`. Checks: 1h QC-tripping (c) — the load-bearing gate — + resume pair. (**INC-9b** was promoted from glue to a MILESTONE — §11.)
- **INC 10 — Clock QC unit.** `filter/NetworkClockQc`: Card-6 re-level, oscillator prior fold + stale-drop, post-solve NIS, clock monitor; operates on `ctx.carry` by reference; `handover_affects` passed as predicate; C1 stamp stays at emit site. Compare three ways at 1h: clock_model on; clock-monitor-only (INC-0 golden); `brdc_rts` (e).
- **INC 11 — Emit TU.** Solve + `solve_failed` skip, sol build → stamps, quarantine, carve-out prune, URA + ramp, datum-bump + PIVOT events, retro/eph-cutover landing, pivot-baseline advance (emit-only position documented), anchor handover record, deferred contract-writer open, store emit + rebroadcast, product write, counters + T2 commit, closed-loop eval; teardown contract self-check + closed-loop summary. Read-only diagnostic writers move to `NetworkRunTraces` taking an `EpochSolutionView`. Ordering preserved verbatim (`emitToStore` before consumer and soln trace; prev-time stamp before snapshot write; H heartbeat position). Compare product + closed-loop + full CSV battery + contract-check PASS.
- **INC 12 — Final trim + validation (milestone).** Orchestrator banner rewrite; `runNetwork` < ~600 lines; `wc` every TU. Try dropping `/bigobj` from the orchestrator. Doxygen pass (seams full, support headers `@internal`). Full matrix serial/parallel × sp3/brdc_rts × legacy/prioritized/sum-to-zero × closed-loop + resume pair + clean 3h A/B + **3h crash-signature recheck**. **CLAUDE.md gets the placement decision tree verbatim** (the anti-regrowth deliverable): (a) extend `NetworkEstimator`/`NetworkSbasis`/`NetworkPhaseDatum`; (b) new domain class in `filter/`//`slip/`//`logging/` per the `NetworkPivotHandover` exemplar; (c) Application TU only for phase orchestration and `gim_`-dependent glue — plus the `NetworkCarryState` three-place rule. Annotate crash-evidence/memory line references with the old→new file map. Commit.

## 6. Risks & guardrails (verifier-hardened)

- MSVC codegen drift across TU boundaries under `/fp:precise`: possible in principle; the per-increment byte gate catches it. Any diff halts and bisects that increment — never patch forward.
- Lambda→function conversion is the highest-frequency error source: capture-list diff per peel, knobs const-from-birth, mutable state through ctx/ws references only, Eigen rules (§2).
- Fingerprint + knob read order are behavior-visible (pre-refactor snapshot resume); resume pair gates at 4, 7, 9, 12 with the deterministic cross-version protocol.
- The eight skip payloads differ by design; a reviewer tempted to unify them is making a behavior change — reject during this series.
- `cap_*`/`cap_dense_*` replay drift across TUs: `EpochWorkspace` doc block names every writer site; `NetworkRaim` header cross-references.
- OpenMP: `schedule(dynamic)` means identity rests on slot-write discipline — audited at INC 0, twice-run goldens prove run-to-run determinism before any motion.
- Under-verification of QC paths: 1h QC-tripping gate at 4, 6, 7, 9, 10, 12 (superseded by §8's row-count assertions on the fast arms); row-count assertions make untripped tiers loud. The pre-existing ~1.5h 305 crash and BeiDou divergence stay out of scope, but the crash signature is pinned at INC 0 and rechecked at INC 12.
- Scope creep: the only permitted new code is Doxygen, declarations, unit tests for already-pure helpers, and the harness script.
- Environment: `LNK1104` → retry; WER zombies → pre-kill; TU frozen for the series; concurrent sessions re-grep markers.

## Executor split (agreed pattern, Jul 4 2026)

- **INC 0 = harness + masks + cut map + post-C1 state inventory.** Self-referentially unverifiable — a quiet scope-narrowing that masks a real diff would pass an unexercised gate. Build with maximal skepticism; goldens + masks are READ-ONLY once frozen.
- **INC 1, 2, 3, 5, 6, 8, 10, 11 = mechanical, byte-gate-protected.**
- **INC 4, 7, 9 milestones + INC 12 final** get per-increment verification pre-commit (capture-semantics / `cap_*` replay / OpenMP / fingerprint-order hazards can pass an unexercised gate).
- **Hard rules:** goldens + masks READ-ONLY (any diff = stop, never regenerate/patch forward); one increment per session; every lambda peel ships a capture-list diff artifact; re-grep symbol anchors at session start.

## Deferred (explicitly NOT this series)

Dead `clockOscUpdate`; `cap_*` replay dup (`applySharedSystem`); refloat idiom dup; `sysOfChar` dup; skip
unification; `NetworkConfig` struct consolidation (~77 forwarding getters); `GnssConfig.hpp` /
`GnssConfigTypes.hpp` split beyond the loader TU.

## Effort estimate

Estimated 13 increments / 1.5-2 focused weeks, wall-clock dominated by validation runs rather than
editing, pausable at any increment boundary since each commits independently green. Weighting as scoped:
INC 0-3 are light (harness + mechanical cuts); 4, 7, 9 are the heavy milestones (context introduction,
assembly, RAIM). **Outcome:** the series ran to INC-12 as scoped; §8's fast-matrix directive is what kept
the validation cost inside that estimate.

---

# Absorbed: decomposition addendum 2026-07-16 (freeze SHA `56da23a`)

*§7-§11 + Appendix A were `docs/network_decomposition_addendum_2026-07-16.md` (now under `docs/attic/`),
produced from a 10-agent survey workflow at HEAD `56da23a`. They amended §0-§6 above; where they
disagree, THEY win. The addendum's sibling reference `network_decomposition_resurvey_2026-07-13.md` was
deleted in commit `a8f044f` ("docs: prune 47 finished card records") — recover it with
`git show a8f044f^:docs/network_decomposition_resurvey_2026-07-13.md` if a cross-reference below needs
its context.*

## 7. Freeze state at SHA `56da23a` (AS-OF-FREEZE, not current)

| Item | Value AT FREEZE |
|---|---|
| **Freeze SHA** | **`56da23a`** ("Missed a config for WL test", atop `89fe588` "Network enhancements, WLs, parallelizations.") — the MW-WL/UWL workstream (WL-1/WL-2/WL-3/UWL-1), all lineage configs, handoffs and specs are IN the baseline. |
| `ApplicationNetwork.cpp` | **14,657 lines** (`wc -l`; was 11,739 at the Jul-13 resurvey — +2,918, all WL/UWL + H1-H3 hardening). `runNetwork()` spans 610→~14,655. |
| `GnssConfigLoad.cpp` | **6,000** (was 4,648 Jul-4). `GnssConfig.hpp` 4,705 lines; `GnssConfigTypes.hpp` 2,340 lines. |
| `gnss_tests` | **780/779 pass/1 skip**. Tree clean; Release build clean. |
| Gates 1–4 (§0) | All CLOSED: C1/S1/pdp committed+validated; Card 7/8 triage = **after INC-12** (recorded); v3 robust-M + per-LOS iono **stay holstered**. |
| Gate 5 (TU freeze) | In force from this commit forward. Urgent fixes land only at increment boundaries. |
| `carrycpljoint` fingerprint fix | CONFIRMED in baseline at 3773-3774. |

**Current, for contrast:** `ApplicationNetwork.cpp` 2,359 lines (2026-08-15; ~1.6k at INC-12);
`GnssConfigLoad.cpp` split at INC-1 (`src/core/config/GnssConfigLoadNetwork.cpp` now exists); suite
1360/196/89 with one fixture-gated skip (re-counted 2026-08-12).

## 8. Series-wide test policy — ⚠️ USER DIRECTIVE, Jul-16

**Every in-series gate runs the FAST matrix only: 50 stations, ≤10 epochs, GPS-only where the toggle
allows, `network.parallel_carry=true` / `parallel_reduce=false`.** The heavy battery (1h QC-tripping run,
305-sta arms, 3h crash-signature recheck, heavy resume pairs) is **deferred to a single post-INC-12
validation phase**. **This SUPERSEDES the §3/§5 heavy-run cadence.** Consequences:

- The Jul-4 cadence table ("1h QC at INC 4/6/7/9/10/12") is replaced by: fast matrix at EVERY increment + fast resume-pair (ARM 8 snapshot leg) at milestones 4, 7, 9b, 12.
- The 3h crash-signature re-pin moves from INC-0 to the post-series phase (signature compared pre/post series at that point; the crash stays out of scope).
- QC-path coverage in-series comes from row-count assertions on the QC traces the fast arms DO trip (harness fails on golden-had-rows/candidate-has-none), not from 1h runs.
- Rationale: token/wall-clock budget; the byte gate catches code-motion defects at any epoch count — long runs only add taken-branch coverage, which the post-series phase restores.

## 9. Fast golden matrix (INC-0 capture, this SHA)

Harness: `scripts/compare_network_run.py` (capture / compare / selfcheck / dump-maskspec; exit 0=pass
1=diff 2=usage; fails on missing/extra files and vacuous captures). Each arm: run TWICE → `selfcheck`
(mask discovery / determinism proof) → `capture` → goldens+masks READ-ONLY. Parallel arms never use
`--include-logs` (shared-logger line reorder); `run.log` content-compare + `wall.txt` verbatim still
apply. Per arm, grep the run log once for the loader's pcarry/preduce-mutex WARN (must be absent).
Configs are harness-owned copies `config/test_suite/_dcmp_*.json` (the `_wlver_*` lineage stays
`pcarry=false` per the WL-card directive — do NOT mutate it).

| Arm | Config (all: 50 sta, ≤10 ep, pcarry=true/preduce=false) | Covers |
|---|---|---|
| 1 `alloff` | NEW — from `_wlver_50sta_10ep_tripleoff.json` with the full carry stack forced OFF (carry_invariant/innov_gate/calibrate/honesty/carry_vce/full_cov/coupling/coupling_TU/eq34 all false; mw_wl+lane_carry blocks removed; **carry_invariant_emit_rebase=false explicitly** — loader defaults it TRUE) | The default-OFF byte-identity contract (no 50-sta all-OFF config existed) |
| 2 `allon` | copy of `_neqexp_50sta_10ep.json` | carry_invariant + D5 gate/calibrate + eq34 + carry_vce + emit_rebase(default-true) + honesty + full_cov + coupling + coupling_TU + neq_export + emit_pb_filter + iono-deweight, GPS-only |
| 3 `jw` | copy of arm 2 + `carry_coupling_joint_whiten=true` | the lone unarmed Card-1 knob (only other jw config is 50 ep) |
| 4 `wlinj` | copy of `_wlver_50sta_10ep_wlinj.json` | WL-2 EWL cascade + WL inject, GPS-only |
| 5 `lane` | copy of `_wlver_50sta_10ep_lane.json` | WL-3 lane-coordinate carry, GPS-only |
| 6 `uwl` | copy of `_wlver_50sta_10ep_uwl_dualfam_mid.json` | UWL-1 all five sub-toggles (uwl+mid+dual_top+family_pairs+cold_fix)+ewl+lane, GPS+Gal |
| 7 `consumer` | copy of `_net50_allon_n4_em25_smoke.json` | closed-loop consumer + AR + product file (G-line mask path) + W6 sat-code-bias + decimation + serial-seam variant (~5 net epochs) |
| 8 `snap` | copy of arm 2 + `network.state_snapshot_path` armed | snapshot writer (full-precision artifact) + **fast resume-pair protocol**: leg A fixed 10 ep writes final snapshot → leg B resumes from it; both legs captured |
| 9 `snap_resume` | NEW at INC-4 — copy of arm 8 + `network.start_offset_hours = 0.08333333333333333` (exactly 300 s in IEEE754) | resume-pair leg B: seeds the GOLDEN arm-8 snapshot (`--seed` in `run_decomp_arm.py`) at `logs/network_state_snapshot.json`, fast-forwards past leg A's span, WARM-resumes (age 30 s vs 900 s horizon, 31 D2 carries + emitter state restored) and processes epochs 11-20. Baseline captured from the pre-INC-4 binary (byte-identity chain to INC-0); the warm gate's `first_live_s` honours start_offset, and neither start_offset_hours nor max_duration is fingerprinted, so the changed config still matches. |
| 10 `obs` | NEW at INC-4 — copy of arm 2 + `write_run_manifest` + `health_interval_s=60` + `trace_flag_rates` + `trace_epoch_status` + `trace_events` + `trace_rotate_mb=0.005` | the INC-4-moved observability writers ON-path (the original 8 arms leave ALL of them OFF): L15 manifest startup+complete records, RTB-6 heartbeat, L1 status rows, L2 event rows, B8 flag-rate + L3 run-summary CSVs. P1 rotation reaches only the guard path (10-epoch files never hit the roll threshold). Baseline captured from a pre-INC-4 build via git-stash round-trip. |
| 11 `estim` | ADDED artifact `logs/network_estimability.csv`, card BD-DATUM **DG5** (2026-07-22) | The only enumerated artifact delta. The spectrum dump's WARN text is unchanged — the CSV is FILLED FROM the same locals, in the same loops, in the same order (`NetworkEstimator::dumpEstimabilitySpectrum` returns an `EstimabilityDump` the driver writes). The file appears only under `network.trace_estimability`; every OFF arm is untouched. The DG6 run-wide gate telemetry rides in this file's trailing `RUN` record rather than a new run-end log line, precisely so the arm's `run.log` stays comparable. |

A golden arm may gain a file ONLY when it is listed in that last row's form — with the card that added it
and the reason the delta is confined to that file. Golden run dirs: `runs/decomp_goldens/<arm>[_b]/`
(gitignored, machine-pinned — now under the archive); capture manifests COMMITTED at
`goldens/decomp/<arm>.manifest.json` so every increment can gate via `compare --manifest` even if run dirs
are lost. Runs use a renamed exe copy (`gnss_processor_dcmp.exe`) per the build-lock directive; pre-kill
WER zombies.

### ⚠ What this matrix does NOT cover — slip QC

`docs/SPEC_inc8b_election_split.md` §9 measured it: **no `_dcmp_*` arm arms `if_phase_td` or `mw_detect`**
(all 15 configs checked; every one has `cycle_slip: {if_phase_td: false}` and none mentions `mw_detect`).
**INC-6's slip-QC seam was therefore gated by a matrix that never armed IF-TD**, and the QC-row assertion
described for it does not exist in `run_inc4_gate.ps1`. Plausible is not proven — `NetworkSlipQc.cpp`
+141 lines is on the PASS-A drain path all arms execute — but INC-6's byte gate must not be read as
slip-path coverage.

## 10. WL/UWL delta map (what `89fe588` adds to the seams)

Six zones; destination TUs per the existing seam map:

1. **Station setup 1132-1169** → SlipQc (INC 6): MW-detector build-gate relaxation (built whenever any WL lane armed), `setBridgeEnabled()`, `setLaneTiers()`. Plus mw_detect carve-out 4299-4318 (mw_wl-only ⇒ Welford-only, NO carry refloat) and the T0_mw_detect emit 4448-4457.
2. **Run-state block 2677-3017** → RunState/RunTypes (INC 2/4): flags (mw_wl_on 2697, ewl_on 2744, uwl_* 2749-2762, wl_inject_on 2815, lane_* 2818-2820), `mw_wl_layer` 2735, `EwlLane`+`ewl_lanes` 2779-2807 (**preferred rung BEFORE fallback — order load-bearing**), `WlInjectPlan`/`wl_inject_epoch` 2826-2835, `LaneTraceEntry`/`lane_trace_epoch` 2849-2857, `WlCounters`+`wlAccumulate` 2862-2935 (in-source bit-rot warning: keep together), F6 types+`mw_wl_f6` 2952-2975, d5 tag sets 2858-2861, run-latched sets 3004-3017.
3. **Inside buildCarryDdPriors 6812-8199** → 9b: carry_arc_amb_key2 6812 (+joint clear 6827), hoisted machinery 6829-6929 (revmap/lin_of/iono_of/band_top/top2/mid/lane_pref_pair/lam cache), census 6930-7043, family admission 7044-7095, pair routing 7198-7292 (`carry_driver.buildDdPriors` core call **7296**), WL-1 lane 7309-7428, WL-2/UWL EWL lanes 7430-7551, WL-3 MW increments 7562-7640, bridge→WL walk 7642-8137 (mid hop 7934, sibling closure 7987, inject stash 8128), lane-trace build 8139-8192, **block fold order wl_out → ewl_out (gated ewl_fold_rows) → wl_hold_out 8193-8196 (installPosterior index contract)**.
4. **Post-install 9935-10269** → 9b + Traces: counter folds 9938-9949/10060-10079, F6 CSV 9950-10058, inject apply 10087-10137, lane-trace CSV 10139-10176, D5 walk band-2 funnel 10213-10222, d5 dump 10225-10265, **tagset rotation prev.swap(now) 10266-10269 (cross-epoch)**.
5. **Run-end telemetry 14107-14245** → Traces (INC 3): five summary blocks reading zone-2 state.
6. **Manifest fields 2518-2521** → Traces.

### New run-state entities (cold-start carries rule applies to ALL)

| Entity (decl) | Tier | Hazard |
|---|---|---|
| **`lane_admit_pairs` (3016)** | 1, run-latched, **BEHAVIOR-BEARING** | Highest-risk entity in the delta: losing it mid-run de-admits family pairs → pair arcs silently fall back to per-band groups → refloat/regroup churn. The latch exists to prevent membership flicker. |
| `d5_inject_chords_prev`/`_now` (2860) | 1/2, **cross-epoch** via swap 10266 | Loss zeroes the `injected_prev` d5-dump column — grading corruption, no crash. |
| `mw_wl_f6` (2975) | 1, one-shot latch | `.written` loss re-fires the F6 dump; `.pending` loss between build/install drops it silently. |
| `lane_trace_header_written` (2857), `d5_dump_header_written` (2861) | 1, run-latched | Loss ⇒ duplicate header mid-CSV (breaks `grade_lane_trace.py`). |
| `uwl_warned_cons` (3008), `lane_census_logged` (3010), `lane_family_logged` (3017) | 1, run-latched | Log-only (dup lines). |
| `wl_epoch`/`wl_cons_epoch` (2898/2902), `wl_inject_epoch` (2835), `mw_wl_epoch` (2944), `lane_trace_epoch` (2856), `d5_hold_chords` (2859), `d5_ewl_stasat` (2858), `carry_arc_amb_key2` (6812) | 2 | Reset inside the build lambda, consumed post-installPosterior — **must survive the build→install seam** (last-build-wins is RAIM-re-solve safe). |
| `ewl_lanes[].run` (2785), `wl_run`/`wl_cons_run`, `mw_wl_run`, `lane_pair_arcs`/`lane_resid_arcs`/`lane_pair_tree_mismatch` (2937-2939) | 1 | Folded at ACCEPTED install only; loss silently zeroes run-end telemetry (invariant #4). |
| `walk_decided`/`lane_incr_done` (7660), `ewl_fixed`/`_top` (7320/7323), `wl_out`/`ewl_out`/`wl_hold_out` (7319), hoisted maps (6849-6929) | build-local | Correctly constructed-fresh — do NOT hoist (carry_arc_amb_key precedent). |

### ⚠️ OPEN / OWED — no WL/UWL/lane fingerprint terms exist

Fingerprint block 3649-3776 read in full: **there are none.** WL-2/WL-3 carry solve-determining
cross-epoch state that is neither fingerprinted nor snapshotted, so **a warm resume across a WL/lane
toggle flip is UNGATED** — the exact gap class the `carrycpljoint` Jul-13 fix closed. All WL toggles are
default-OFF research cards, and fixing it would change bytes for the armed arms 4-6, so it was
deliberately deferred; **still open, queued with the WL card follow-ups.**

## 11. Increment-plan amendments, seam constraints, and coupling asymmetries

- **INC-1 (config split) — VERIFIED READY.** One uninterrupted span `GnssConfigLoad.cpp` **2825-5897** (3,073 lines: anon-ns parseCoordEpochYear 2825-2862 [sole caller 3018, inside the move], banner 2864-2866, loadNetworkSection_ 2867-5745, applyNetworkConsumerCouplings 5747-5819, deriveClosedLoopConsumerConfig 5821-5854, applyNetworkConsumerProfile 5856-5897) between loadBaselineSection_ (ends 2823) and loadLoggingSection_ (starts 5902). Decls at GnssConfig.hpp 126/158/188/4389; only cross-unit call is the header-inline `networkConsumerArIgnoreMwWl()`. Cross-section forcing writes: **13 sites / 10 distinct fields** (⚠ CORRECTION — supersedes §4's original "eight") — all inside loadNetworkSection_, move for free. estimation-section peek at `network.estimate_glonass` is line 1026, stays. New-TU includes: GnssConfig.hpp, GnssConfigJsonHelpers.hpp (using jval/jsection only — NOT toLower/joinStrings), Constants.hpp, NetworkProductEmitter.hpp, StationInfoFile.hpp, nlohmann/json, fmt, spdlog, `<optional> <cstdio> <algorithm> <cctype>`. Old-TU include cleanup deferred to INC-12 (keep INC-1 pure-motion).
- **INC-2 (type hoist)** — hoist set enlarged: + CarryPrior/sharedCarryKey/SharedCarryCov/CoupledShared/kCouplingSparsifyRelThresh/CplRowRaw/LocalCarry/PlAmb/PlZwd/ClockOsc(+Pred+predict)/MedianPolish/medianCopy/ifElevMap/parseIfPairResetKey (anon-ns, 126-608) and the WL PODs from zone 2 where separable. Order dependency: CoupledShared/CplRowRaw before LocalCarry/StationAssembly/whitenCouplingRows. `static` helpers become `inline` in headers (linkage change only — ODR-safe, but keep them header-inline, not TU-exported). robustProminence is a template ⇒ header-only. NOT plain PODs (hoist with care or defer to INC-4): EwlLane (owns NetworkMwWlLayer), F6Capture (declared with instance), WlCounters (+wlAccumulate pair), CarryEigenScope/SubBlockTimer (RAII).
- **INC-3 (traces)** — the L15 manifest writer (2367-2552) sits mid-knob-region and echoes ~40 knob locals; dd_frac/vtec_sh_health decl+open sites are 1656-1888 with write sites at 11839/12152. Adds from the delta: 3 CSV writers (F6/lane-trace/d5-dump), 5 run-end WL summary blocks, manifest fields. carry_vce has TWO readout sites (12314-12315 per-epoch + 14350-14374 run-end) — both to whichever unit owns NetworkCarryVce.
- **INC-9b is now a MILESTONE, not glue.** Was ~200 lines; now ~1,700 (zone 3 + zone 4 counters/inject). It owns the three most dangerous channels (carry_innov_pending, carry_slipped_ambs, lane_admit_pairs). Still fable-only, still before INC-9 (RAIM re-solve re-invokes buildEnsembleGauges 9651 + buildCarryDdPriors 9655 — capture semantics change at BOTH call sites). Sub-split option (decide at 9b time): 9b-i eq34 group build + driver core; 9b-ii WL/UWL lane machinery as `filter/NetworkMwWlGlue` (zone 3 is cleanly `if (mw_mach_on)`-gated).
- **SB_* sub-timer brackets are seam constraints** (new): SB_SLIP 4244-4672 brackets geometry+slip; SB_NETAR 10272-10544 brackets AR diag+NEQ export; SB_EMITSTORE 11285-11704 brackets rebase+PB-filter+closed-loop consume; SB_STASH_STATUS opens 13465 before the D2 stash and the VCE shared-prior snapshot 13472-13488 must stay stash-side. A cut that splits a subBegin/subEnd pair changes D0 run totals → byte diff.
- **Cut-map anchor regen** is per-increment (re-grep at increment start — already a hard rule), not a monolithic INC-0 deliverable; Appendix A covers INC 1-4 planning.
- Card-6 relevel coupling: decls 2154-2176, restore 3882-3886, block 8964-9054, snapshot fields 13911-13912 — four far-flung sites, INC-4/10 coordinate.
- Card-2 `code_bias_prev_`/ifpin refresh 13618-13643 is deliberately NOT snapshotted (in-code note 13622) — preserve the asymmetry at INC-4.

### Order of operations (as executed from the freeze)

1. ✅ Housekeeping commit = the addendum + plan-doc status update + `_dcmp_*` configs (freeze SHA for goldens = that HEAD; production code unchanged from `56da23a`).
2. **INC-0 (fable-only):** capture the §9 matrix (twice-run selfcheck → capture → commit manifests). Goldens/masks READ-ONLY after.
3. **INC-1** config split → each increment thereafter: build → `gnss_tests` → fast-matrix compare vs INC-0 → commit. One increment per session where possible; pausable at every boundary.

## Appendix A — anchor map at freeze SHA `56da23a` (HISTORICAL)

> ⚠️ **HISTORICAL — never use a row here as a cut anchor.** Every symbol below has since been relocated
> into a seam TU by INC-6..12, and the table was already measured stale before that:
> `docs/SPEC_inc8b_election_split.md` §9 records the 8b row (`4919-5556`) as "stale by ~−3,400". The
> lesson the table exists to teach is the drift rule. **Drift is non-uniform (+5 early → +3,619 late);
> never reuse resurvey line numbers.** Definition sites in `ApplicationNetwork.cpp` at the freeze SHA
> ("Was" = the deleted Jul-13 resurvey's numbers):

| Unit (dest TU) | Now | Was |
|---|---|---|
| NetStation / StationAssembly / whitenCouplingRows / robustProminence / medianPolish2Way (anon-ns, INC-2) | 132-183 / 316-365 / 378-425 / 513-526 / 543-590 | +5..+6 |
| runNetwork() start | 610 | 605 |
| Persistent cross-epoch decls | 2021-2272 (prev_carry 2021, clock_osc 2148, local_carry 2229, zwd_carry 2232, rpb_carry 2243) — continues interleaved to 3956 (prev_tow/iod/gen 2553-2559, **WL/UWL state 2677-3017**, sh_dev_carry 3280) | 1870-2213 |
| Knob const-snapshots | 2273-2365 AND 2633-2758 (two regions, L15 manifest writer 2367-2552 between; counters 2553-2615; FlagRate 2605) | 2362-2525 |
| splitStation / appendGim / appendSh | 3055-3175 / 3190-3248 / 3313-3451 | 2536 / 2671 / 2890 |
| Fingerprint build / restore / epoch-loop head | 3649-3776 (cpljoint 3773) / 3777-3947 / **3957** | 2982 / 3091 / 3264 |
| D5 pending seed / refloat_signal / refloat_pair / PASS-A head / if_phase_td driver | 4186-4189 / 4421-4439 / 4524-4536 / 4280 / 4515-4670 (SB_SLIP 4244-4672) | 3491 / 3720 / 3823 / 3584 |
| eq34 election (8b) | **4919-5556** (4-way chain: legacy 4797 / ensemble 4904 / eq34 4919 / default 5557-5606; +D3c RPB alloc sibling 5619-5636; shared_phase_keys decl 4231) | 4216-4903 |
| do_fold pins (INC-7, foldAmb 5908 / foldRpb 5924) | 5900-6019 | 5195-5314 |
| PendCplRow / TuEnt / buildCplRow (7b) | 6109 / 6170 / 6217-6352 (+tuReinitSince 6212) | 5404-5647 |
| buildEnsembleGauges / **buildCarryDdPriors** / beginEpoch+invoke | 6738-6793 / **6823-8199 (65→1,377 lines — §10)** / 8201-8212 | 6032 / 6113-6177 / 6178 |
| W6 code-bias datum (Sbasis) | 8215-8520 (~306 L, size unchanged) | 6189-6494 |
| healNearNullPhaseBias / Card-6 relevel | 8752 / 8964-9054 | 6726 / 6935 |
| solve / assess / resolve_dropping / **installPosterior span** | 9360 / 9453 / 9533 / **9914-10271 (52→358 lines — §10)** | 7306 / 7417 / 7497 / 7877-7929 |
| AR Phase-2 + NEQ export (SB_NETAR 10272-10544) | 10274-10452 + 10454-10542 | 7931-8081 |
| Emit head (SB_SOLPREP) / rebase+PB-filter (SB_EMITSTORE) / consumer iono | 10547-11284 / 11286-11433 / 11527-11630 | 8083 / 8801 / 8995 |
| dd_frac / vtec_sh_health(+H1 gate+VCE readout) / carry_vce beginEpoch / regcol+harvestCoupling / VCE addRow-1 | 11839 / 12152 (readout 12314-12315) / 12342 / 12363-12443 / 12499 | 9299 / 9602 / 9771 / 9790 / 9926 |
| post-fit RAIM (SB_RAIM) | 12668-13105 | 10067-10529 |
| osc block / emit_clock_rate C1 / clock monitor | 13108-13266 / 13268-13305 / 13307 | 10531 / — / 10691 |
| D2 stash (SB_STASH_STATUS 13465) / VCE addRow-2 / snapshot write | 13490-13708 / 13704 / 13826-13924 (wall_utc 13851) | 10918 / 11086 / 11192 |
| epoch loop closes / teardown / VCE estimate | 13925 / 13927-14657 (WL/UWL telemetry 14107-14245) / 14350-14374 | — / 11286 / 11442 |
