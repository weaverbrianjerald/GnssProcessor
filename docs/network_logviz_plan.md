# Network logging & visualization enhancement plan (L-queue)

**Written 2026-07-01.** One card per OPUS session, executed in L-number order unless noted.
Produced by a 26-agent analysis (6 inventory readers over the C++ trace/product/consumer
surface and pyviz, 3 design lenses, a merge judge, and one adversarial verifier per card).
**Every anchor below was tree-verified on 2026-07-01** — line numbers WILL drift; always
re-grep the quoted symbol before editing.

**Goal:** make the armed validation plan V0–V7 (see memory `project_network_armed_validation_plan`)
mechanically judgeable — the PASS/FAIL signature renders as a figure instead of being
hand-grepped from CSVs and stdout — plus operator debugging (per-sat QC event timelines,
station drill-down) and product/consumer inspection (URA, disc counters, C1, S-contract,
correction age).

## V-card → L-card map

| Validation card | Served by |
|---|---|
| V1 (B4 floors vs ep~42 lock-in) | L1+L5 (plateau view), L6 (carry_frac), L9 (A/B) |
| V2 (clock-model NIS on G15@h4) | L4 (NIS viewer + config gap), L2+L5 (timeline), L12 (C1 honesty) |
| V3 (A4 withhold on clean data) | L1+L2+L3+L5 (disposition + flag rates) |
| V4 (URA closed-loop A/B) | L8 (product file), L10+L11 (consumer), L9 (A/B), L12, L14 (contract) |
| V5 (DD-fractional + anchor info) | L6 |
| V6 (B2 measurement) | L3+L9 (+ L1 sigma0 series) |
| V7 (ZWD carry A/B) | L7+L9 |

## Conventions for EVERY card (read first)

- Whole network workstream is UNCOMMITTED on master. Do not commit. Re-grep your edits
  after any build-lock retry (tree is shared across concurrent sessions).
- Build: vcvars64 pattern from CLAUDE.md; network RUNS use `build/x64-Release`;
  library iteration `--target gnss_core`. Tests: `gnss_tests` (419+ pass currently).
  New C++ test files require an explicit `CMakeLists.txt` entry (no globbing).
- Every new C++ trace goes behind a config toggle **default-OFF** and must be
  **byte-identical when OFF** (hash `network_trace.csv` / `network_solution.csv`).
  Toggle plumbing (verified pattern — network trace toggles are NetworkConfig POD
  fields, NOT the `enable_X_` private-member pattern):
  - member in `struct NetworkConfig`, include/config/GnssConfigTypes.hpp:194-201
    (trace block `trace_stations`…`trace_flag_rates` at :195-200, `serialize_sbasis_contract` :201),
    `///<` inline Doxygen;
  - parse in `loadNetworkSection_` (src/core/config/GnssConfigLoad.cpp:2583; trace group
    :2653-2667) — assignment target is `network_config_.`, e.g.
    `network_config_.trace_X = jval<bool>(json, "network.trace_X", "trace_X", false);`;
  - siblings parse silently; the one ON-info-line precedent is
    `serialize_sbasis_contract` at :2668-2671 — follow it when a card asks for an ON line;
  - consume in ApplicationNetwork.cpp via `nc.trace_X`
    (`const auto& nc = config_.getNetworkConfig();` at :398).
- ApplicationNetwork.cpp shared geography (all verified): trace-file paths declared
  ~:881-935; unconditional trace-open scope `{ ... }` :937-1063
  (`create_directories(log_dir_)` :939); epoch loop `for (const auto& se : synced)` :1514;
  explicit stream-close block :4455-4462; run summary
  `"Network producer complete: {} epochs emitted, {} skipped ({} … gate, {} … σ̂0 ceiling) — of {} synced."`
  :4464-4467. `log_dir_` is the hardcoded `"logs"` member (include/app/Application.hpp:492).
  Time fields in every CSV: `se.time.week` + `fmt::format("{:.3f}", se.time.tow)`.
- pyviz viewer rules: pure declarative `figures.Figure` specs (NO matplotlib/plotly import
  in viewers — test asserts source text, cf. pyviz/tests/test_colormaps.py:39-43);
  `Viewer(name, title, required_csvs, build, default_enabled=True)`
  (pyviz/gnssviz/viewers/base.py:38-53); register by import + append in
  pyviz/gnssviz/viewers/__init__.py `REGISTRY = [` (:45, network group ends
  `_network_closed_loop,` :53); the driver auto-skips when any `required_csvs` fails
  `ctx.has_data` (driver.py:58) — raise `ViewerSkip` only for present-but-empty data;
  helpers: io.load_trace(:21 — bare `pd.read_csv`, blanks→NaN)/require_cols(:38)/add_utc(:53),
  identifiers.system_of/prn_num, palettes (SYSTEM_ORDER, color_by_prn :59,
  distinguishable_colors :42), strip.build_sat_epoch_strip (strip.py:79 — long-form
  DataFrame `system,satellite,facet,utc,value`; kwargs `colormap=` ("viridis"/"vik"
  supported), `caxis=` (NOT clim), `diverging=`; NaN cells transparent).
  Colormaps: viridis sequential / vik diverging, never hand-rolled ramps.
  Figure primitives that EXIST: Line(:41, axis="right"), StackedArea(:54), HLine(:64),
  TextBox(:73, mono=), Raster(:95, NaN transparent, row_labels), Markers(:107, has
  `marker`, raster-addressed `rows`), Scatter(:119, edgecolor/linewidth/size), Band(:144,
  NaN breaks segments), VLine(:159), Stem(:184, vertical only), ColorBar(:239).
  Primitives that DO NOT exist: Bar, VSpan/axvspan, categorical xtick labels.
  `Axes.x_is_datetime` defaults True — set False on numeric axes.
  Tests: pyviz/tests/test_network_viewers.py pattern (`TraceContext(log_dir=str(tmp_path))`,
  write CSV text, call build fn directly, `pytest.raises(ViewerSkip)`).
  CLI: `python pyviz/run_report.py <log_dir> --only <names> -o out.html` (`--only` nargs='*',
  `--all` for default_enabled=False viewers).
- Metric convention: clock stats = **median + std + worst-sat(NAME)**, never rms alone.
- Test cadence: build green → 10-min smoke → 1h on the `_net50_*` configs before any 305 run.
  Isolated CWD for runs; pre-kill WER-zombie `gnss_processor` before timing anything.

## Current trace surface (so cards don't re-derive it)

CSV files written by `runNetwork` today (headers verified): `network_stations.csv`
(trace_stations, default TRUE), `network_trace.csv` (always;
`week,tow,n_present,n_clock_sats,n_phase_bias,declared_dim,gate_ok,solved,rms_clock_m,max_clock_m,n_pivot_pins`,
:942-943), `network_solution.csv` (always; `week,tow,sat,kind,signal,value_m,disc_counter`,
:948; kinds CLK/PB/CB, CLK signal field empty), `network_ionosphere.csv` (trace_ionosphere),
`network_vtec_sh.csv` (estimate_iono_sh + global_vtec_sh.trace), `closed_loop_trace.csv`
(closed_loop.enable; `week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio,pos_err_3d_m,east_m,north_m,up_m`,
:971), `network_if_slip.csv` + `network_raim.csv`
(`week,tow,sat,station,s_max_raw,s_max_norm,sigma0,raim_k,spiked,n_sta_spiked,min_sta,refloated`, :996)
+ `network_raim_attrib.csv` + `network_clock_monitor.csv` (all under trace_slip_events),
`network_anchor_info.csv` (trace_anchor_info,
`week,tow,sat,anchor_info,carry_info,data_info,total_info,anchor_frac_data,carry_frac,frac,is_pivot`, :1040),
`network_dd_fractional.csv` (trace_dd_fractional,
`week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,dd_m,lambda_m,dd_cyc,frac_cyc`, :1055).
pyviz has viewers for 5 of these (map/iono_map/health/products/closed_loop); the six QC
diagnostics and dd_fractional have none (dd_fractional has only the untracked standalone
scripts/view_network_dd_fractional.py). trace_flag_rates (B8) is log-lines-only. The
product file (U/R/S/E records, C1) has no Python reader at all.

**Incidental findings (not carded, note only):** no network trace stream is flushed
mid-run and three (clk_mon/anchor_info/dd_frac) are closed only by RAII — a mid-run crash
(WER-zombie mode) loses buffered tails; `log_dir_` has no config override and the
closed-loop consumer's TraceLogger hardcodes the same `"logs"` (L10 relies on that
coincidence); a solve-throw epoch writes NO `network_trace.csv` row at all (L1 fixes the
observability, not the row-hole in the legacy file).

---

## L1. [OPUS, cpp, ~1 day, P0] Per-epoch disposition + σ̂0 + clock-stats trace: `network_epoch_status.csv` (`network.trace_epoch_status`)

**Unblocks:** V1 (clock-stat series), V3 (disposition timeline), V6 (σ̂0 level-shift). Consumed by L5, L9.
**Deps:** none.

**Why:** network_trace.csv encodes disposition only implicitly ((gate_ok,solved)=(0,0)
conflates 5 skip causes; σ̂0-withhold is the undocumented (1,0) pattern at :3246-3248;
solve/buildSharedDatum throws write nothing), carries rms/max clock only (violates the
median+std+worst-sat convention), and never records σ̂0, n_adapt, refloat counts, retro
arming, or datum changes.

**Spec:**
- Toggle `bool trace_epoch_status{ false };` after GnssConfigTypes.hpp:200; parse
  `network_config_.trace_epoch_status = jval<bool>(json, "network.trace_epoch_status", "trace_epoch_status", false);`
  next to :2664-2665.
- Open `network_epoch_status.csv` in the trace-open scope (beside `dd_frac_trace_path`
  :935), header exactly:
  `week,tow,disposition,sigma0,n_adapt,n_present,n_clock_sats,clock_median_m,clock_std_m,worst_sat,worst_dev_m,n_refloat_sats,n_retro_applied,datum_changed,sigma0_trigger,withhold_ceiling`
- ONE row per processed epoch, written BEFORE each `continue` (the loop exits via
  continue — never write at loop end). Full disposition site inventory (all 8 `++n_skipped`
  sites, current lines):
  - `sync_skip`: :1563 (datum station no obs), :1571 (pivot computeModels fail) — only
    week/tow available, blanks elsewhere;
  - `sync_skip`: :2062 (numShared==0), :2410 (`!epoch_ok || est.numShared()==0`);
  - `datum_failed`: :2573 (buildSharedDatum throw; warn at :2572) — today a silent CSV hole;
  - `gate_withheld`: write in the `if (!gate_ok)` block :2737-2743 beside the existing
    trace row (gate_ok set false only in the catch :2723-2736);
  - `solve_failed`: :2906 (solve throw; warn :2904) — the other silent hole; x_s empty, no
    clock stats;
  - `sigma0_withheld`: at the `"WITHHOLDING epoch (too contaminated to publish)"` warn
    :3241-3243 (`++n_sigma0_withheld` :3245); sigma0=sigma0_adapt, n_adapt=n_adapt;
  - `emitted`: at the existing `// --- Trace ---` emit site :4410-4424.
- Field scope table (write BLANKS, not 0, where unavailable — L5 depends on it):
  registered_clock_sats :1958, n_clock :2031, n_pb :2032, n_present :2034, total_pins :2331,
  plan :2568, `const bool datum_changed = pivotHandoverSignalled(...)` :2586-2587,
  x_s :2900, clock_refloat_sats :2918, n_adapt :2936, sigma0_adapt :2938 (0.0 unless
  within-epoch RAIM ran, set :3211 — document, don't fake).
- Clock stats computed ONLY inside `if (epoch_status_trace.is_open())`, over the same
  registered_clock_sats / `x_s(est.clockColumn(sat))` loop as the rms computation
  (`double sumsq = 0.0, maxabs = 0.0;` :4411): clock_median_m = median, clock_std_m = std
  about the median, worst_sat = sat NAME maximizing |v−median|, worst_dev_m = that deviation.
- `n_retro_applied` = `retro_emit_sats.size()` captured at the consume site
  `retro_emit_sats = std::move(ura_retro_sats);` :3319-3322 (= bumps APPLIED this epoch;
  `ura_retro_sats` at the loop tail means pending-for-NEXT-emit — do not use it there).
- Config-echo constants: `config_.networkRaimSigma0Trigger()` (GnssConfig.hpp:1605),
  `config_.networkRaimWithholdSigma0()` (:1619) — so viewers draw honest threshold lines.
- Close in the :4455-4462 block. Pure ASCII (no σ̂ glyphs).

**Validation:** build Release, gnss_tests green. OFF byte-identity on
`config/test_suite/_net50_em25_pl_zeroq_d2floor_1h.json` (hash network_trace/network_solution,
no new file). ON (10-min smoke → 1h): exact header; **row count == n_emitted + n_skipped**
(NOT synced.size() — the start_offset fast-forward `continue` :1520-1523 and max_duration
`break` :1529-1536 are uncounted by design, and the test config sets max_duration_s 3600);
per-disposition sums reconcile the :4464 completion line including solve_failed rows.

**Risks:** miss one `continue` path and reconciliation fails — grep every `++n_skipped`.
Keep the median sort strictly inside `is_open()`.

---

## L2. [OPUS, cpp, 1-2 days, P0] Structured per-event QC log: `network.trace_events` → `network_events.csv`

**Unblocks:** V1/V2/V3 timeline judging (WHICH sat/station/arc was acted on, when). Consumed by L5.
**Deps:** none (complementary to L1).

**Why:** every QC action is stdout-only or fully silent today: pivot handover has ZERO log
line, retro-bump arming is silent, within-epoch culprit lists exist only in a truncated log
string. One long-format event CSV replaces spdlog regex-scraping.

**Spec:**
- Toggle `bool trace_events{ false };` (member after :200, parse after :2665, optional ON
  info line per the :2668-2671 precedent). Open `network_events.csv` in the trace-open
  scope; header exactly `week,tow,tier,event,sat,station,statistic,threshold,action,detail`.
  tier ∈ {T1_if_slip, T2_within_epoch, T3_raim, T4_attrib, T5_clock_model, T6_clock_monitor,
  GATE, SIGMA0, PIVOT, RETRO}. Define an `emitEvent(...)` lambda just inside the epoch loop
  (:1514); all writes `is_open()`-guarded; `detail` sanitized (','→';'), pure ASCII (the log
  strings contain σ̂0 — do not copy verbatim).
- Emission sites (verified lines):
  1. **T1** IF-slip flags at the flagged-sample site :1833-1839 (`refloat_pair(sp->sta,...)`
     :1835; log suffix `carry_warm ? " for refloat" : " (carry cold; diagnostic)"` :1892);
     statistic=abs_dev_m, threshold=thr_m, detail=sig1;sig2. ALSO the refsat common-mode
     reset loop :1875-1886 (`parseIfPairResetKey` :1879, `refloat_pair(i,...)` :1880) which
     today writes NO if_slip row — event=refsat_reset, statistic/threshold EMPTY (not in scope).
  2. **GATE** withhold: emit inside the catch :2723-2736 (ex.what() only there) or stash
     the message beside `gate_ok = false` and emit at `if (!gate_ok)` :2737.
  3. **T2** within-epoch settle at the `"Network[RAIM within-epoch]: TOW {:.1f} re-conditioned in {} re-solve(s)"`
     info :3195-3199: one row per sat in drop_sats (we_refloat_sat), per (station,sat) in
     drop_arcs (we_refloat_arc), per excluded station (we_exclude_station). The sets are
     declared at :2960-2962 in the within-epoch BLOCK (:2944-3212) and survive to the log
     site — emit from the settled final sets. statistic=sigma0_adapt, threshold=trig (:2945),
     detail=sigma0_first(:3104)→sigma0. Also the escape hatch at :4038-4041
     (`"STILL > trigger"` … `"(emitted anyway; inspect network_raim_attrib.csv)"`) as
     event=emitted_contaminated.
  4. **SIGMA0** withhold at :3241-3243, statistic=sigma0_adapt, threshold=raim_withhold
     (:1115), action=skip_epoch.
  5. **T3** post-fit consensus at `"Network[RAIM]: TOW {:.1f} refloated {} carried ambiguity(ies)"`
     :4065: one row per refloated sat. Statistic: `raim_details` is built ONLY when
     raim_trace.is_open() (:3808-3809) — either extend that gate to
     `raim_trace.is_open() || events_trace.is_open()`, or compute per-flagged-sat max
     s_norm from `amb_stats` (a.r_max / sigma0, in scope).
  6. **T4** attribution at :4023-4027: SAT rows via sat_prom.at(sat) (:3885). ARC rows:
     `bad_arcs` (:3939-3945) stores only (sta,sat) — capture `mp.interaction[k]` and
     prominence |interaction|/inter_mad (inter_mad :3901) during that build loop, or emit
     ARC rows there.
  7. **T5** clock-model: emit per-sat INSIDE the loop at :4146-4149 where `nis`,
     `clk_model_k`, `ext` are in scope (the :4194 warn has only a joined string); exclude
     ext-only sats (logged by their originating tier). Plus the currently-SILENT state
     actions: R6 vanish-return C1-ZOH suppression — the implicit else of
     `if (sat_dt >= 0.0 && sat_dt <= clk_model_max_gap)` :3271 (comment :3263), emit when
     false && oit->second.init, **edge-triggered** (it re-fires every emit while stale);
     R11 stale-prior drop :2823 (`// R11: stale → no prior (reseed)`; clk_model_max_gap
     declared :1135); datum-change osc wipe :4087 (`clock_osc.clear();` inside
     `if (clk_model_warm) { if (datum_changed) {` :4085-4087) — these are V2's blind spots.
  8. **T6** clock-monitor at :4296 warn; per-sat dev :4249 / jump :4261, flags :4250/:4262.
  9. **PIVOT** handover at the COMMIT site `if (datum_changed) {` :3309-3317, BEFORE
     `prev_pivot = plan.clock_pivot;` :3330 (that assignment destroys old pivots) — NOT the
     detection at :2586, so withheld epochs never log a committed handover. One row per
     changed constellation (changed_consts is set<char>; maps are keyed by constellation
     string), sat=new pivot, detail=old→new + arm. Arm is NOT returned by
     `pivotHandoverSignalled` (include/filter/NetworkPivotHandover.hpp:51) — derive:
     prev_pivot has the key → adjacent, else → vanish_return (old pivot from last_pivot).
  10. **RETRO** at `retro_emit_sats = std::move(ura_retro_sats);` :3321: one row per sat
      from the consumed set, event=retro_bump, action=disc_bump. Tier attribution is lost
      at the union :4311-4316 — carry a small map<sat,tier> fed at :3205/:3209 (T2),
      :3823/:3827 (T3), :3925/:3931 (T4), :4149 (T5), :4268 (T6); if provenance is
      genuinely lost write detail=union, don't fabricate.
- Reference header styles: network_raim.csv :996, network_raim_attrib.csv :1008,
  network_clock_monitor.csv :1021.

**Validation:** OFF byte-identity as usual. ON (copy of the d2floor config + trace_events
+ raim + raim_within_epoch + clock_model), 1h: every stdout
`Network[RAIM within-epoch]`/`[clock-model]`/`[clock-monitor]` line has matching rows at the
same TOW; ≥1 PIVOT row per realized handover (cross-check disc_counter steps in
network_solution.csv).

**Risks:** events on subsequently-withheld epochs are intentional (that IS the disposition
story) — document in a header comment.

---

## L3. [OPUS, both, ~1 day, P0] Flag-rate + run-summary CSVs under existing `network.trace_flag_rates` + `network_flag_rates` viewer

**Unblocks:** V3 (per-tier realized rate vs implied α), V6 (flag-rate deltas). L9 diffs both CSVs — keep columns frozen.
**Deps:** none.

**Spec (C++):**
- Inside the existing `if (trace_flag_rates) {` end-of-run block :4515 (banner :4527),
  refactor the `emit` lambda (:4517, signature `(const char* tier, const FlagRate& fr, double k, bool has_alpha)`)
  to write BOTH sinks. The two log format strings MUST survive byte-identical (recorded
  signatures string-match them):
  `"  {:<24}: {:>10} opp, {:>8} flag, rate {:.3e}  (k={:.1f} => implied Gaussian a {:.3e})"` :4521-4522 and
  `"  {:<24}: {:>10} opp, {:>8} flag, rate {:.3e}  (absolute bound; no Gaussian a)"` :4524-4525.
  Simplest: capture an extra `std::ofstream&` in the lambda, append the CSV row after the
  unchanged log->info.
- `network_flag_rates.csv` header `tier,label,opportunities,flags,rate,k,implied_alpha,has_alpha`.
  The seven emit calls :4528-4534 map to (tier, label, counter, k, has_alpha):
  T1 "IF-phase slip (T1)" fr_if_slip networkCycleSlipIfPhaseTdKMad() true;
  T2 "within-ep trigger (T2)" fr_we_trigger networkRaimSigma0Trigger() true;
  T3 "post-fit RAIM cell (T3)" fr_raim_cell networkRaimKSigma() true;
  **T3c** "RAIM consensus (T3)" fr_raim_sat networkRaimKSigma() true (both labels say "(T3)"
  — the disambiguation exists only in the CSV tier column, hard-code the mapping);
  T4 "attribution SAT (T4)" fr_attrib networkRaimKSigma() true;
  T5 "clock-model NIS (T5)" fr_clk_model networkClockModelKSigma() true;
  T6 "clock-monitor (T6)" fr_clk_mon 0.0 false (implied_alpha empty).
  implied_alpha = existing `gaussAlpha` `std::erfc(k/std::sqrt(2.0))` (:4516), %.6e.
  FlagRate fields are `opp`/`flag` (struct :1148; counters :1149-1155, function-scope).
- `network_run_summary.csv` header
  `n_synced,n_emitted,n_skipped,n_gate_withheld,n_sigma0_withheld,loop_ms,ms_per_epoch,n_stations,n_threads,parallel`.
  **Scope bug to fix:** `loop_ms` lives inside the braced timing block :4445-4453 — hoist
  `double loop_ms = 0.0;` above it (assign inside). ms_per_epoch = the existing expression
  `n_loop_epochs ? loop_ms / (double)n_loop_epochs : 0.0` (n_loop_epochs :1489/:1538 —
  denominator is loop epochs, NOT n_synced; document). parallel written as int 1/0
  (parallel_on :698, n_threads :702, stations.size() as at :4452).
- No new toggle — both files gated on the ONE existing trace_flag_rates; extend its
  Doxygen (:200, currently ends "…OFF (default) = byte-identical.") with the two filenames.

**Spec (pyviz):** viewer `network_flag_rates`, required_csvs=("network_flag_rates.csv",).
Axes 1: log-x per-tier rows — realized rate vs implied_alpha as paired points connected by
a 2-point `Line(x=[rate, alpha], y=[row,row])` (`Stem` is vertical-only, can't do this);
`x_is_datetime=False`; clamp rate 0.0 to a positive floor (1e-12) or omit (the matplotlib
backend only applies log-x when x data is positive, matplotlib_backend.py:322); hollow
'no exposure' markers via `Scatter(color=(1,1,1), edgecolor=<tier color>)`; VLine guides
at 1e-3/1e-6 (class is `VLine`, figures.py:159). Axes 2: TextBox(mono=True) table
tier|opp|flags|rate|alpha|ratio + run_summary numbers when network_run_summary.csv present
(optional via `ctx.has_data`, graceful without) incl. withheld_frac = n_sigma0_withheld/n_synced
(the V3 pass metric). Summary: advisory per-tier one-liners + "all tiers within [0.1x,10x]
of implied alpha" or offending list. Docstring: aggregates only — event TIMING lives in the
(planned L1/L2) traces; do NOT reference those files as existing.
Tests test_network_flag_rates.py: one-tier-at-10x named; zero-opportunity no div-by-zero;
skip on absent CSV; missing run-summary tolerated.

**Validation:** OFF: neither file, stdout unchanged. ON (25-min smoke): every CSV rate
numerically equals the stdout line's (compare numerically — CSV %.6e vs log {:.3e});
run_summary matches the completion line. pytest green; `run_report.py --only network_flag_rates`.

---

## L4. [OPUS, both, 1-2 days, P0] Clock-model NIS instrumentation: `nis,mode` columns + `network.trace_clock_full` + `network_clock_nis` viewer

**Unblocks:** V2's pass signature (NIS fires on G15@h4; prediction bands; drift sanity).
**Deps:** none. **V2 run-recipe note:** `_net305_clockmodelock_1h.json` — actually
`config/test_suite/_net305_clockmodel_1h.json` — sets clock_model:true (:109),
clock_monitor:true (:110), start_offset_hours **3.5** (:111) but does NOT arm
trace_slip_events; the V2 run must add it (the clock CSV hides under trace_slip_events).

**Why:** network_clock_monitor.csv is the V2 instrument but its columns are silently
REMAPPED under clock_model (comment :4177-4179: clk_m=solved, median_m=predicted,
dev_m=de-meaned innov, prev_clk_m=drift, jump_m=raw innov, abs_max_m=band √S, jump_max_m=k,
flag_abs=|nis|>k), flag_jump is hardcoded 0 (:4190 `<< ... << ",0,"`), rows are band-pass
filtered below 0.3k, and no viewer exists.

**Spec (C++, explicit trailing-column format break — the allowed exception; say so in the
toggle Doxygen):**
- Header :1021-1022 is TWO adjacent string literals — append inside the second:
  `"abs_max_m,jump_max_m,flag_abs,flag_jump,flag_ext,refloated,nis,mode\n"`.
- Model-branch rows :4180-4191 (filter `(flagged || std::abs(nis) > 0.3 * clk_model_k)`,
  nis at :4144 `const double nis = dm / std::sqrt(S);`) append nis + mode=model.
  Monitor-branch rows :4270-4287 (inside `if (clk_mon_on && !clk_model_on && ...)` :4223;
  blanks `",,"` at :4281) append empty nis + mode=monitor.
- New toggle `bool trace_clock_full{ false };` (member beside :197, parse
  `network_config_.trace_clock_full = jval<bool>(...)` in :2653-2667): when true replace
  BOTH write filters with `true` (keep is_open()). Monitor filter exact text :4270-4272:
  `(refloated || std::abs(dev) > 0.5 * clk_mon_abs || (have_jump && std::abs(jump) > 0.5 * clk_mon_jump))`
  — NOT a 0.3k band. File gate stays `nc.trace_slip_events && (clockMonitor()||clockModel())`
  (:1014) — note the hiding in the Doxygen.
- Row-count caveat even with trace_clock_full: seed epochs `continue` at :4140, block gated
  clk_model_warm :4085 and skipped on datum_changed :4086-4087, monitor gated
  n_emitted ≥ pl_warmup :4223 → count ≈ n_clock_sats × emitted epochs, keep the "≈".
- k source: clk_model_k = networkClockModelKSigma() (:1132; key network.clock_model_k_sigma,
  default 6.0). Monitor bounds clk_mon_abs 20.0 / clk_mon_jump 10.0. Both-on semantics:
  monitor branch is gated !clk_model_on → a V2 CSV contains ONLY model rows (loader logs
  the supersession, GnssConfigLoad.cpp:3089-3093).

**Spec (pyviz):** viewer `network_clock_nis`, required_csvs=("network_clock_monitor.csv",).
Mode resolution: `mode` column; legacy CSVs (no column) use the CORRECTED heuristic —
**mode=model iff df['abs_max_m'].nunique()>1** (model writes per-row √S {:.4f} :4188;
monitor writes the constant clk_mon_abs {:.2f} :4283; jump_max_m is constant in BOTH modes
and flag_jump==0 on any clean run — the naive rules misclassify). Secondary tells: blank
prev_clk_m/jump_m (`,,` :4281) occur only in monitor mode; model prev_clk_m has 6 decimals
(:4186) vs monitor 4. Default monitor when undecidable; test the degenerate single-row case.
Figures (model mode): (1) |NIS| per sat — Scatter (rows sparse below 0.3k unless
trace_clock_full), color_by_prn, HLine at k, red rings flag_abs==1, orange flag_ext==1;
(2) worst-6 by max|nis| small multiples: Line clk_m + dashed Line median_m (the PRE-update
prediction, clk_pred x0, :4184/:4100) + Band(predicted ± k·abs_max_m), NaN-gapped across
non-adjacent epochs; (3) |nis| strip via build_sat_epoch_strip — build the long-form
DataFrame (`system,satellite,facet,utc,value`=|nis|) and call
`build_sat_epoch_strip(D, colormap="viridis", caxis=(0.0, 1.5*k))` (no value=/clim= kwargs
exist); (4) drift prev_clk_m per sat, flag |drift| > 5× pool robust σ (R6 regression watch).
Monitor-mode fallback: dev_m vs abs_max_m and jump_m vs jump_max_m bounds, flags ringed,
blanks→NaN. Legend must label jump_max_m per mode (10 m bound vs k). Summary per metric
convention + nis>k fire count + TOWs. Register in REGISTRY; tests: synthetic model
(with mode col, one over-k fire), monitor, legacy no-mode CSVs.

**Validation:** file only opens under trace_slip_events && clock_* — default run byte-identical.
With trace_slip_events+clock_model, trace_clock_full=false: rows identical to pre-change
after cutting the 2 trailing columns. V2 dry-run (start_offset 3.5): NIS strip saturates on
G15 at the jump epoch; band panel shows the innovation leaving ±k√S.

---

## L5. [OPUS, pyviz, 1-2 days, P0] QC timeline viewers: `network_epoch_status` + `network_events`

**Unblocks:** the single picture V1/V2/V3 are judged on.
**Deps:** L1, L2 (develop against their header contracts with synthetic fixtures — no C++
run needed; only the network_trace.csv raster fallback is runnable today).

**Spec:**
- **(A) network_epoch_status.py**, required_csvs=("network_epoch_status.csv",) [L1 header].
  3 x-linked Axes: (1) disposition lanes — categorical Scatter over
  {emitted, gate_withheld, sigma0_withheld, solve_failed, datum_failed, sync_skip}, FIXED
  palette (emitted green, sigma0 red, gate grey, failures black), VLine at datum_changed==1;
  (2) sigma0 Line (NaN where blank — never plot zeros), HLine dashed at sigma0_trigger +
  solid at withhold_ceiling (from the CSV's constant columns), Scatter overlays where
  n_refloat_sats>0 / n_retro_applied>0; (3) clock median ± std — Line clock_median_m,
  Band(median±std), worst_dev_m Line; run-worst sat NAME in summary.
  Summary + plateau heuristic: median(clock_std_m) last-third / first-third of emitted
  epochs, ratio > 2 → "LOCK-IN SUSPECTED" — strictly advisory text.
- **(B) network_events.py**, required_csvs=("network_events.csv",) [L2 header].
  Figure 1 'QC events by satellite': per-constellation Axes (SYSTEM_ORDER), each an
  all-NaN Raster (n_sats rows, row_labels=sat names — the only y-tick-label facility;
  NaN renders transparent) + one Markers layer per (tier-color × action-shape)
  (Markers HAS `marker`, figures.py:107-115, but is raster-addressed via 0-based `rows`;
  first consumer of Markers in the suite). Fixed tier→color dict from
  distinguishable_colors, stable order; shapes: refloat='x', withhold='v', retro_bump='s',
  pivot_handover='D'.
  Figure 2 'Epoch disposition raster' from network_trace.csv (OPTIONAL — degrade when
  absent; PREFER network_epoch_status.csv's explicit column when present). Complete
  bit-pattern classification (verified writer sites): emitted=(1,1) :4419-4423;
  sigma0_withheld=(1,0) :3246-3248; gate_withheld=(0,0) with declared_dim>0 :2739-2741;
  solve/setup-fail=(0,0) declared_dim==0 with n_present>0 :2411-2413; early
  numShared==0 row :2063-2064 (n_clock_sats==0); all-zeros=sync_skip :1564/:1572;
  **buildSharedDatum failure writes NO row** — an absent epoch, unclassifiable here.
  Render as 1-row Raster with strip_colormap("viridis", 4) categorical levels
  (`Figure.categorical=True`) + rms_clock_m on a right-axis Line (`axis='right'`,
  `y2label`) masked where solved==0 (values are literal 0, not NaN — same mask as
  network_health.py:44-45).
  Summary: events per tier, top-5 sats, disposition counts.
- Tests: plateau fixture asserts LOCK-IN and flat doesn't; disposition classification
  covers ALL row shapes above; ViewerSkip paths; no-matplotlib-import assertion
  (test_colormaps.py:39-43 pattern).

**Validation:** pytest green from header contracts alone. After L1+L2 land: 1h instrumented
run, `run_report.py --only network_epoch_status network_events` — stdout withhold TOWs
appear in the lanes; PIVOT rows align with disc_counter steps in network_products.

---

## L6. [OPUS, pyviz, 1-2 days, P0 — **dependency-free, good first card**] Promote V5 instruments: `network_dd_fractional` + `network_anchor_info` viewers

**Unblocks:** V5 entirely. **Deps:** none — both CSVs already produced (toggles default-OFF).

**Spec (no C++):**
- **network_dd_fractional.py**, required_csvs=("network_dd_fractional.csv",). Port
  scripts/view_network_dd_fractional.py (untracked; anchors `def wrap_half(x):` :31,
  `if lam <= 0.0:` :46): skip lambda_m<=0 rows (writer zero-stubs unknown wavelengths,
  init :3569, forced 0 at :3588-3590), wrap frac to (−0.5,0.5] via x−round(x)
  (producer already wraps — idempotent), group by (cons,signal). **Robust σ must
  replicate the script's estimator exactly** (upper median without interpolation,
  script :53-58 — np.median differs for even n) to satisfy the 3-decimal cross-check.
  Figure A: tile per (cons,signal) (cap ~8 largest-n, disclose), 50-bin FIXED histogram
  rendered with Stem or step-Line (NO Bar primitive), HLine at uniform density n/50,
  VLines ±0.1 cyc, title '<GPS L1C> n=… σ=… cyc, …% within ±0.1'. Figure B (net-new —
  the script ignores tow entirely): per-group robust σ vs tow, Line per group.
  `cons` is a SINGLE LETTER ("G","E","C","J","S") — map via identifiers.system_of before
  palettes.system_color (keyed by full names). Producer already excludes pivot station
  (:3537) and GLONASS (:3549) — do NOT re-filter. Aggregate before figure construction
  (O(sta×sat×epoch) file). Summary: per-group pct_within_0.1cyc + advisory GO/NO-GO wording.
- **network_anchor_info.py**, required_csvs=("network_anchor_info.csv",). Figures:
  (a) carry_frac strip via build_sat_epoch_strip(colormap="viridis", caxis=(0,1)) — melt to
  the long-form contract (iono_strip.py:46 / satellite_strip.py:41 `_melt` pattern; utc via
  io.add_utc); high = carry-dominated lock-in; (b) anchor_frac_data strip caxis=(0,0.5) +
  HLine 0.1 advisory companion axes; (c) StackedArea data/carry/anchor info for the
  run-worst sat + cross-sat median Lines with p10-p90 Band. EXCLUDE is_pivot==1 rows from
  stats (pin inflates data_info; row write ends `<< (is_pivot ? 1 : 0)` :3501-3509), note
  the exclusion; list sats with carry_frac>0.9 sustained ≥10 epochs.
- Register both (Viewer needs `title` too). Afterwards gut the standalone script to a
  shim pointing at `run_report.py --only network_dd_fractional` (untracked; don't commit).
- Tests: clustered fixture ⇒ small σ + GO wording; uniform ⇒ large σ; lambda_m=0 ignored;
  carry-dominated sat named; pivot rows excluded; skip-when-absent.

**Validation:** pytest green. Re-run the d2floor config with trace_dd_fractional +
trace_anchor_info **added** (its network section has NEITHER today, :109); per-group n and
robust σ match the standalone script to 3 decimals; on the zero-q config the carry_frac
strip shows the known V1 lock-in sats hot.

---

## L7. [OPUS, both, 1-2 days, P0] Per-station ZWD trace: `network.trace_zwd` → `network_zwd.csv` + `network_zwd` viewer

**Unblocks:** V7 (border formal-σ sharpening under carry_zwd) — no data source exists today.
**Deps:** none; L9 overlays std_m.

**Spec (C++)** — five verifier corrections folded, follow exactly:
- Toggle `bool trace_zwd{ false };` + parse (loader is parse-only like siblings; ON line
  optional near the B9 validation block GnssConfigLoad.cpp:3196-3213). Declare
  `zwd_trace`/path near :902-903, open in the :937-1013 block mirroring :951-957, header
  `week,tow,station,zwd_m,std_m,carried`; close beside :4457-4461.
- Write per emitted epoch adjacent to the iono-trace block (:3433-3463 — proven-safe
  post-solve site; anywhere in :3433-3629, BEFORE the carry harvest at :3630/:3667), loop
  stations, skip est_index_of[i]<0, try/catch like :3437-3440.
- **Column source — do NOT use R.pl_zwd/pl_zwd_present to find the column** (only set
  under carry_warm && zwd_on :2227/:2278 → run A of the V7 A/B and warmup would emit zero
  rows). Locate independently: `const size_t tropo_idx = stations[i].sm->getTropoIndex();`
  (StateManager.hpp:237, sentinel size_t max, usage :2280) →
  `asm_res[i].h2local.find((int)tropo_idx)` (std::map<int,int>, StationAssembly :202) →
  zcol; bounds-check vs x_r.size() like :3450-3452.
- zwd_m = `stations[i].sm->getStateVector()(tropo_idx)` + `x_r(zcol)` (mirrors the B9
  harvest value_abs :3682; x_lin is ≡0.0 today — troposphere_zwd seeded 0.0 :631 — keep
  the +x_lin for robustness; header comment: value is the estimated correction about the
  modeled a-priori).
- **std_m: the API is `est.conditionalLocalVarianceDiag(est_index, {zcol}, plan.shared_datum)`**
  (NetworkEstimator.hpp:241 — "conditionalLocalCovarianceDiag" does NOT exist; full-matrix
  :220 is O(n_r³), don't use it). Copy the exact call pattern from the B9 harvest
  :3676-3683. Datum-pinned shared columns already zeroed inside (doc :223-240) — don't
  re-implement. Emit only if var finite and >0 (guard like :3684).
- carried flag: for truth add `bool pl_zwd_injected{false};` to StationAssembly (beside
  pl_zwd_present :209), set true at the `inj.emplace_back(zcol, zlambda, zeta)` site
  :2298-2299; write `carried = R.pl_zwd_injected ? 1 : 0`. (Cheaper fallback:
  pl_zwd_present = "carry-eligible" — rename semantics if used.) Pivot stations DO get
  ZWD carried when zwd_on (:2227 gate `!st.is_pivot || zwd_on`) — include the pivot; only
  its clock is gauge-pinned.

**Spec (pyviz):** viewer `network_zwd`. Axes: (1) per-station zwd_m Line + Band(±std_m),
legend capped ~12; (2) per-station std_m vs time — THE V7 axis — carried==1 spans shaded
(NO VSpan primitive: Band spanning the y-range with NaN outside carried windows, or a 0/1
Stem); (3) station-ranked last-30-min median σ — NO categorical xtick labels exist: numeric
x + TextBox labels, or extend Axes/backends with an optional xtick_labels field;
`x_is_datetime=False` there. Summary: 'per-station σ (last 30 min): median …; worst <STA> …;
carried …%'. Tests: two-station fixture with one σ-sharpening; ranking; skip.

**Validation:** OFF byte-identity. ON 10-min smoke: n_present rows per emitted epoch,
std_m finite positive, |zwd_m| < 1 m. Record the V7 recipe: A carry_zwd=false vs
B carry_zwd=true, both trace_zwd=true → σ(B) ≤ σ(A) at border stations (L9 renders).

---

## L8. [OPUS, pyviz, ~1 day, P0 — **dependency-free**] Product-file parser `gnssviz/product_file.py` + `network_product_file` viewer

**Unblocks:** V4 (URA timelines, R records, S-contract rendered). Feeds L12, L14.
**Deps:** none.

**Spec:**
- **Parser** implementing the verified grammar of src/core/products/NetworkProductIO.cpp
  (writer :62-145, reader :151-268; RE-GREP before coding):
  banner `# gnss_network_product v2` (:65) / v1 (:87); '#' skip BEFORE tag dispatch; CRLF
  tolerated (pop_back '\r' :190);
  `E,week,tow,iod_ssr,provider_id,solution_id,datum_changed[,datum_consts]` — field 7
  ALWAYS written (:112-120), field 8 only when non-empty and is a CONCATENATED constellation
  string ("GE"), not comma-separated;
  `R,sat[,sat...]` — written immediately after E, only when the retro set is non-empty
  (:124-128), applies to the current epoch;
  `C,sat,clock_c0_m[,clock_c1_m_s]` — field 4 only when ≠0 (:133, "// B3b");
  `B,sat,signal,code_bias_m`; `P,sat,signal,bias_m,signal_integer,wide_lane_integer`;
  `U,sat,clock_ura_m` (:142-143, only under network.emit_ura);
  `S,ref_rx,bias_sign,windup_id,amb_datum,isb_datum,emit_cb,cb_datum,disc_thresh_m,emit_clock_disc,apriori_product_id[,<const>:<pivot_sat>...]`
  (:73-85; reader guard f.size()≥11 :236; enums serialized as ints — parse as ints;
  pivots colon-split :249-252; v2-only header record → v1 yields contract=None).
  Numbers are fmt "{}" shortest-round-trip — Python float() handles all. Unknown tags:
  skip + count. Return dataclass: header_version, contract dict|None, long table
  (week,tow,sat,record,signal,value + per-epoch datum_changed/datum_consts/retro flags).
  Note: the v2 writer is DEFERRED — opens at FIRST emit (:752-758) so a zero-epoch run has
  no file.
- **Discovery:** `network.product_output_path` (GnssConfigLoad.cpp:2640-2641); relative
  paths land under log_dir_ (:763, comment :760-761). Glob
  `os.path.join(ctx.log_dir, 'network_products*.txt')`, newest mtime, surface chosen
  filename+mtime in the summary (stale-file hazard); ViewerSkip when none. Some ad-hoc
  configs use absolute scratch paths the glob won't find — document the miss.
- **Viewer** `network_product_file`, required_csvs=() with glob+ViewerSkip inside build
  (network_map.py:57 precedent). Figures: (a) per-sat URA timeline from U records, log-y,
  color_by_prn, per-constellation tiles (reuse the `_by_system` pattern — it's PRIVATE in
  network_products.py:76, import it or replicate); R-record epochs as black rings on the
  affected sat; datum_changed epochs as faint vertical marks labeled with datum_consts;
  (b) per-epoch counts of U lines, C lines, C-with-nonzero-c1, R-flagged sats.
  Summary: header version; decoded S contract (every field + realized pivots); URA
  per-constellation median+std+worst-sat NAME; total R records; c1-carrying count.
  If network_contract_check.csv (L14) exists, append its rows verbatim — tolerate absence
  (the only branch that executes today). **Datum-pivot sats get NO U line by design**
  (:3287 `// datum pivot — bogus pin variance, no honest URA`) — docstring note, not data loss.
- Tests test_product_file.py: synthetic v2 exercising E(+datum_consts)/C(+c1)/B/P/R/U/S +
  CRLF + unknown tag; v1 without optionals; viewer figures + skip. Cross-validate grammar
  against tests/test_network_product_io.cpp `UraAndClockDriftRoundTrip` (:422; asserts
  clock_c1_m_s −0.0123456 :447, clock_ura_m 0.037 :449).

---

## L9. [OPUS, pyviz, 1-2 days, P0] A/B run-comparison (`pyviz/run_compare.py` + `gnssviz/compare.py`) + all-diagnostics validation preset

**Unblocks:** V4/V6/V7 — all two-run judgements; today run_report.py takes exactly one dir.
**Deps:** comparators (a),(e) + (b)-fallback work today; full value after L1/L3/L7.

**Spec:**
- **gnssviz/compare.py:** `ComparePair` (two TraceContexts + labels 'A'/'B') + comparator
  functions **returning ViewerResult** (not bare Figures — write_html_report consumes
  (title, ViewerResult) pairs, report.py:47-58) — declarative only. Keep a separate ordered
  COMPARATORS list; do NOT register in the viewers REGISTRY (driver protocol is single-ctx).
  Comparators (skip via has_data on EITHER side, collect (name,reason)):
  (a) compare_closed_loop — closed_loop_trace.csv: log-y pos_err_3d_m A grey solid vs B
  accent dashed, accepted-fix markers (`ar_success > 0` convention, network_closed_loop.py:67),
  B−A delta axes; summary fix rate / first-fix / converged-window 3D RMS — the V4 headline.
  (b) compare_epoch_status — network_epoch_status.csv (L1) both sides: clock median/std
  overlays + sigma0 level-shift readout (median σ̂0 B−A over emitted epochs) — V6 primary;
  FALLBACK to network_trace.csv rms_clock_m overlay masking solved==0 rows (values are
  literal 0 — mask, and use those rows as withheld markers).
  (c) compare_flag_rates — network_flag_rates.csv (L3): TextBox table tier|rate_A|rate_B|ratio,
  >3x called out. **Skip-only until L3 — do NOT parse the log lines.**
  (d) compare_zwd — network_zwd.csv (L7): per-station std_m overlay + final-window delta
  table — the V7 signature. Skip-only until L7.
  (e) compare_network_clock — network_solution.csv kind==CLK: per-epoch median(|clk|)/std/
  worst-sat NAME per run.
  (f) compare_scorecard — text-panel diff of the comparators' own summary numbers in
  MetricDelta.one_line STYLE (gnssviz/eval/regression.py:101/:108/:124 — style only, the
  eval machinery operates on different inputs).
  Epoch alignment: join on (week, round(tow*1000)) — tow is {:.3f} so exact; inner-join for
  deltas, full series for overlays; **REPORT unmatched-epoch counts per side** (withheld
  sets differ by construction — silent bias otherwise). Colors: A dark neutral, B vik
  endpoint; deltas vik.
- **CLI pyviz/run_compare.py** modeled on run_report.py (argparse :23):
  `python run_compare.py <dirA> <dirB> [--label-a --label-b] [-o compare_report.html] [--only ...]`;
  render via the same get_backend('matplotlib') + write_html_report — do not fork rendering.
- **Preset config/test_suite/_net50_validation_diag_1h.json:** copy
  _net50_em25_pl_zeroq_d2floor_1h.json and arm every OBSERVATION toggle that exists —
  trace_slip_events, trace_anchor_info, trace_dd_fractional, trace_flag_rates,
  serialize_sbasis_contract (trace_stations already default-true; trace_ionosphere false —
  too heavy; product_output_path already set in the base config). Add the FUTURE names
  trace_epoch_status/trace_events/trace_zwd/trace_clock_full with a "_comment" that they
  arm as L1/L2/L7/L4 land (unknown keys are ignored by jval — harmless; do NOT expect
  their CSVs in the smoke test). Leave DECISION toggles unarmed with a _comment naming
  them (network.raim :2895, raim_within_epoch :2902, clock_model :2985, emit_ura :3019,
  carry_zwd :2857, ura_retro_bump :3028) + the gotcha that raim_withhold_sigma0>0
  auto-enables raim_within_epoch (:2940-2944).
- Tests test_compare.py: two synthetic run dirs; delta sign on a constructed level shift;
  one-side-missing skips; scorecard contains the worst-sat name; joined ≤ min run length.

**Validation:** pytest green. CLI smoke: duplicate a run into A/B, perturb one column in B
— deltas nonzero only where perturbed; self-vs-self all-zero. Preset 10-min smoke: armed
CSVs appear, unbuilt-card traces simply absent, run must not error.

---

## L10. [OPUS, cpp, ~1 day, P0] Consumer-side SSR application trace: `network.trace_ssr_consumer` → `ssr_consumer_trace.csv`

**Unblocks:** V4 consumer-side evidence (URA actually applied; correction age; disc/staleness reactions).
**Deps:** none. L11 consumes the header — keep it byte-exact.

**Spec** (verifier corrections folded — the write-site one is CRITICAL):
- Toggle `bool trace_ssr_consumer{ false };` + parse per conventions + ON info line.
- RealTimeEngine member `std::ofstream ssr_consumer_trace_;` opened in initialize() right
  after `trace_logger_ = std::make_unique<TraceLogger>(...)` (RealTimeEngine.cpp:300;
  log_dir is the hardcoded "logs" :298 — same dir as producer traces by the two-hardcodes
  coincidence). Header exactly:
  `week,tow,sat,ssr_age_s,clk_applied,c0_m,ura_m,r_add_var_m2,pb_age_s,n_pb,clk_disc,pb_disc_max,cb_disc_max,pb_fresh`
- **Write site: NOT inside the coverage census.** The census (:847-861,
  `ssrStore().coverageFor` :855) is nested in `if (config_.realtimeUsesLiveOrbitClockSource())`
  which is FALSE unless AppMode::RealTime (GnssConfig.hpp:2544-2548) — the closed-loop
  consumer config (deriveClosedLoopConsumerConfig, GnssConfigLoad.cpp:3348) keeps
  post-processing app mode, so rows would be zero in exactly the V4 case. Add a SIBLING
  block right after :861, BEFORE the drop-broadcast-grade erase loop :863-880 (or dropped
  sats vanish from the trace), gated only on toggle + is_open(), looping
  `for (const auto& obs : modeled_obs)`; `current_time` (week/tow) in scope. Note
  processEpoch can early-return via the corrections-ready gate (:737) — those epochs
  produce no rows, acceptable.
- Per-sat fields — query the ENGINE'S store via `ssrStore()` (repointed by setSsrStore in
  closed loop), never the singleton:
  - ssr_age_s from `getSsrEpoch(sat)` vs current_time (any-record-type timestamp,
    SsrCorrectionStore.hpp:213);
  - **clk_applied = `!obs.broadcast_only`** (the TRUE applied signal — a fresh-in-store
    probe can still be IODE-rejected; comment :841-845); c0_m from the
    `getOrbitClock(sat, current_time, config_.getSsrMaxAge())` probe, blank when nullopt;
  - ura_m = `getUra(sat, current_time, config_.getSsrMaxAge())` (hpp:189-190);
  - r_add_var_m2 recomputed with the IDENTICAL formula as the apply site
    MeasurementModel.cpp:1668-1684 (ura² :1675 + q2·dt³/3 :1681, q2=networkUraStalenessQ2(),
    dt from getSsrEpoch) and 0 when networkConsumeUra() off — the V4 A/B discriminator;
    cross-cite the formula in a comment at BOTH sites (consumer's processing_mode_ is
    PPP_RTK so the :1669 `!= PPP_RTK_Network` gate passes — recompute matches 1:1);
  - **pb_age_s: needs a tiny new accessor** — getPhaseBias returns SsrPhaseBiasEntry
    (bias_m, signal_integer, wide_lane_integer, discontinuity_counter; RtcmTypes.hpp:142-147)
    with NO timestamp; freshness gates on the record-level `phase_bias->received_time`
    (SsrCorrectionStore.cpp:243), one per sat. Add
    `std::optional<GnssTime> getPhaseBiasTime(const std::string& sat) const` to
    SsrCorrectionStore (hpp+cpp) mirroring getSsrEpoch; emitter fills received_time =
    sol.epoch_time (NetworkProductEmitter.cpp:122/149/158/196) so age is well-defined;
  - n_pb / pb_fresh by probing getPhaseBias per tracked signal — mirror the A5 loop filter
    exactly (:1286-1289: keys of obs.modeled, skip non-'L' and '_'-containing codes); the
    A5 effective limit is getSsrBiasMaxAge()×getSsrBiasStaleHoldFactor() (:1283-1284,
    whole block gated enableSsrBiasMaxAge() :1278);
  - clk_disc = getClockDiscontinuity(sat) — **blank on nullopt**, not value_or(0);
    pb_disc_max = max per-signal `discontinuity_counter`; cb_disc_max via
    getCodeBiasDiscontinuity (hpp:202-204 aged overload), max over signals.
  - Blanks (not 0) for all absent optionals. Allocation-light (live 1 Hz NTRIP shares this
    path). No CMakeLists change unless a new test .cpp is added (then it IS required).
- Tests: extend the tests/test_network_emit.cpp round-trip pattern
  (RoundTripsThroughStore :72-74) — push clock+bias+URA through emitter→store, assert the
  r_add_var formula matches MeasurementModel's; config-parse test per
  tests/test_codebias_network_config.cpp (:74).

**Validation:** OFF byte-identity on a short closed-loop run (hash every CSV). ON:
exact header; ura_m populated only when the producer ran emit_ura; r_add_var_m2 nonzero
only when consume_ura true; clk_applied=0 rows at producer-withheld epochs —
cross-check against **network_trace.csv** gate_ok/solved=0 rows (network_epoch_status.csv
doesn't exist until L1).

---

## L11. [OPUS, pyviz, ~1 day, P1] `ssr_consumer` viewer: correction-age strip, R-inflation timeline, product-gap shading over closed-loop error

**Deps:** L10 schema (embedded above — develop against a synthetic fixture in parallel;
re-verify byte-exactness when L10 lands).

**Spec:** viewer `ssr_consumer`, required_csvs=("ssr_consumer_trace.csv",).
(1) Correction age — per-sat ssr_age_s strip via build_sat_epoch_strip (viridis; NaN cells
are TRANSPARENT, not grey; **no decimation helper exists in pyviz — roll one**, ≤2000
columns) + per-epoch max/median age Line. The limits are NOT in the CSV and TraceContext
has no consumer-config field: config keys are `processing.ssr_max_age_s` (default 30.0)
and `processing.ssr_bias_max_age_s` × `processing.ssr_bias_stale_hold_factor` (default 2.0)
— 'limit unknown' annotation is the NORMAL path; label any HLine accordingly.
(2) URA R-inflation — per-sat sqrt(r_add_var_m2) Lines per constellation tile
(`from .network_products import _by_system, _SYS_OF, _SYS_ORDER` — private but permitted,
or replicate; melt into the `value_m` column contract with key_mode='key_sat'); summary
applied-URA median+std+worst-sat NAME.
(3) Events + gaps — step counts per epoch: clk_applied==0 (product gap), pb_fresh 1→0
flips (A5 stale-hold), clk_disc/pb_disc_max increments; when closed_loop_trace.csv also
present (has_data, NOT in required_csvs) overlay log-y pos_err_3d_m with gap epochs shaded
— NO VSpan primitive: Band with NaN outside gap windows (positive lo on log-y) or per-gap
VLine; adding a real VSpan means touching figures.py + both backends.
Summary: gap epochs, stale flips, disc resets, age stats, and — with closed loop — median
3D error inside vs outside gap-shaded epochs (the causality number).
closed_loop join: per-EPOCH rows (no sat column), join on week/tow via add_utc; tolerate
epoch-set mismatch. Tests: synthetic gap window + stale flip + disc bump; inside/outside
medians; header-only skip goes through run_viewers/has_data (driver handles it), not the
build fn.

---

## L12. [OPUS, pyviz, ~1 day, P1] C1 clock-rate honesty + cumulative disc-counter timelines: `network_discontinuities`

**Unblocks:** V2's R6 vanish-return C1 gate check; V4 disc-counter timelines with bump-cause classification.
**Deps:** L8 for the C1 half (product file). The disc-timeline half is standalone on
network_solution.csv — **implement it first** so the card degrades gracefully.

**Spec:** one viewer, required_csvs=() (driver would skip the whole viewer if
network_solution.csv were listed and absent — per-half presence checks inside build,
ViewerSkip only when BOTH sources absent; network_map.py:189 precedent).
- **(A) C1 honesty** (product file via L8's parser; if L8 unbuilt, a minimal inline parser
  per L8's grammar): per sat with ≥2 C records, fd = Δc0/Δt on ADJACENT epochs by per-sat
  TIME (withheld epochs create gaps — never diff row index); tiles per constellation:
  emitted c1 Line vs fd dots, residual (c1−fd) axes with ±2e-4 m/s guide band. **C1==0 is
  encoded by OMISSION of C-record field 4** (3-field C record) — the R6 pass check is
  "C record is 3-field on the first post-gap epoch". Mark epochs where the per-sat gap
  exceeds `network.clock_model_max_gap_s` — **fallback default 120 s, not 300**
  (GnssConfigLoad.cpp:2999; defaults to phase_leveling_max_gap_s = 120.0). Exclude FD
  across any E with datum_changed=1 (scoped to datum_consts constellations when field 8
  present) — FD across a handover steps constellation-wide and would false-alarm.
  When NO C record in the whole file carries field 4 (clock_model off — its default),
  degrade to an informational note. Physics note: x1 is the drift carried INTO the epoch
  from the previous posterior (:3255-3258) — c1 vs fd has an inherent one-epoch basis
  offset; the guide band, not zero-residual, is the criterion.
- **(B) Disc timelines** from network_solution.csv: cumulative step plots per sat for
  kinds CLK/PB/CB, one axes per kind. **CLK disc_counter is hard 0 unless
  network.emit_clock_disc** (default false) — render all-zero gracefully. Classify bump
  epochs: with the product file present use E datum_changed/datum_consts + R records as
  GROUND TRUTH; CSV-only fallback heuristic = 'datum' when ≥80% of a constellation bumps
  the same epoch, else 'per-sat'. Distinct markers. Cross-checks (both optional/dead
  branches until siblings land): every R-record epoch produced a disc increment on the
  flagged sat within 1 epoch → report "RETRO NOT REFLECTED: <sat> tow <t>"; network_events
  PIVOT rows vs datum-classified epochs.
- Product-file location: NOT necessarily in log_dir (path "used verbatim" per
  GnssConfigTypes.hpp:168) — reuse L8's glob/path handling; do NOT assume
  ctx.path("network_products.txt").
- Tests: synthetic product file with gap + stale-C1 case (flagged); synthetic solution CSV
  with one constellation-wide and one isolated bump (classification asserted); skip case.

---

## L13. [OPUS, both, 1-2 days, P1] Per-station residual field: `network.trace_station_residuals` → `network_station_residuals.csv` + `network_station_strip` viewer

**Unblocks:** "which station is poisoning the network" — currently only spiked-event rows +
truncated warns. **Deps:** requires network.raim (+ phase_leveling) at runtime.

**Spec (C++):**
- Toggle per conventions. **Loader warn goes in GnssConfigLoad.cpp** (NOT ApplicationNetwork):
  near the `if (network_raim_)` block :3233-3251, warn when
  `network_config_.trace_station_residuals && !network_raim_`, and mention BOTH
  network.raim AND network.phase_leveling as prerequisites (the RAIM block is gated
  `if (raim_on && carry_warm)` :3721 and carry_warm needs pl_on :1550).
- Open `network_station_residuals.csv` in the trace-open block, gated on the new toggle
  alone (independent of raim_trace's trace_slip_events gate :977). Header
  `week,tow,station,sat,s_raw,s_norm,sigma0`.
- Emit inside the Pass-2 cell loop :3803-3810 (anchor
  `if (trace_flag_rates) { ++fr_raim_cell.opp; if (spiked) ++fr_raim_cell.flag; }` :3806)
  — write EVERY cell, not just spiked: `se.time.week`, tow {:.3f},
  `stations[a.sta].name`, `a.sat`, s_raw=`a.r_max`, s_norm (computed :3804),
  sigma0 (:3790-3794). **B2 normalization needs nothing extra:** it is applied in Pass 1
  while building amb_stats (:3768-3773 `r /= std::sqrt(ri);`) so a.r_max already carries
  it — consistency with the spike decision is automatic. '\n' only, %.3f.
- Doxygen notes: file size (50 sta × ~40 sats × 3600 ep ≈ 7M rows — opt-in deep trace);
  rows exist only for NON-PIVOT stations with carried ambiguities (:3742 `is_pivot`
  continue, :3744 empty-pl_amb continue) — the pivot never appears; expected row count is
  Σ over RAIM epochs of (sats × non-pivot-stations-with-carries). s values are ABSOLUTE
  (|resid|·√w :3767) — non-negative.

**Spec (pyviz):** viewer `network_station_strip`, required_csvs=("network_station_residuals.csv",),
**default_enabled=False** (heavy; run via --only/--all). (1) Station health raster — raw
Raster/ColorBar/Axes primitives (build_sat_epoch_strip renders y=satellite, can't do
y=station): y=station sorted by RMS of s_norm (worst on top), x=epoch, value=per-epoch RMS
over that station's sats, viridis. (2) Worst-station drill-down via
`build_sat_epoch_strip(long, colormap="vik", diverging=True, caxis="auto")`
(**`strip_colormap(diverging=True)` does not exist** — the strip helper handles the
diverging path itself, strip.py:124-126, symmetric clim via _resolve_caxis) — but note the
data is non-negative, so sequential viridis is arguably right for both figures; executor's
choice, do not expect signed residuals. Long-form melt per residual_strip.py `_tag_system`
:194 / `_melt` :200. Summary: per-station median+std+worst-sat of s_norm, top-3 ranked.
**REGISTRY edit is mandatory** (import + append).

**Validation:** OFF byte-identity. ON + raim on the 50-sta 1h config: every
network_raim.csv spiked row has a matching s_norm>raim_k row at the same (week,tow,sat,station)
(note the two files' sat,station column order differs); no rows before warmup. pytest;
`run_report.py --only network_station_strip`.

---

## L14. [OPUS, cpp, hours, P2] Machine-readable S-contract check: `network_contract_check.csv`

**Unblocks:** V4's contract leg as data instead of spdlog strings. **Deps:** pairs with L8.

**Spec:**
- No new toggle — the live-check block only runs under nc.serialize_sbasis_contract
  (default-OFF): gate `if (nc.serialize_sbasis_contract && product_writer && !prod_path.empty())`
  :4489 (comment "This is a LIVE checkContract call" :4469-4474); OFF trivially byte-identical.
- After `NetworkProductReader::read(prod_path.string(), &parsed, &rok);` :4493 and
  checkContract, write log_dir_/"network_contract_check.csv" header `check,status,detail`:
  ('file_readable', rok), ('contract_present', parsed.present), one row per `mism` string
  ('field_mismatch','FAIL',sanitized), **plus ('pivot_map_roundtrip', …) — the third warn
  variant at :4502 (`parsed.decl.clock_pivot_sat != pending_contract.decl.clock_pivot_sat`)
  is NOT in mism** (checkContract only compares expected-side pivots,
  NetworkProductIO.cpp:314-325) — 'overall' must FAIL on it too; final ('overall', PASS/FAIL,
  "<n> realized pivot(s) serialized", parsed.decl.clock_pivot_sat.size(); exact PASS log
  string at :4505-4507). Write the file on PASS too — absence means "check never ran".
- Sanitization: **no current mismatch string contains a comma** (NetworkProductIO.cpp:296-339;
  the test fixture at test_network_product_io.cpp:473 is commented "comma-free") — keep
  ','→';' as DEFENSIVE hardening only, worded as such. To make it testable put it as
  `static std::string csvSanitize(const std::string&)` on NetworkProductReader
  (hpp:170 area), NOT a lambda in the 4600-line runNetwork.
- Semantics: serialize=true + zero-epochs-emitted → deferred writer never opened
  (contract_pending warn block :4475-4488, product_writer null at :4489) → no CSV; same as
  a crash. State this in the change comment; optionally write ('overall','FAIL','writer
  never opened') from the :4475 block to distinguish.
- pyviz hook: **viewers/network_product_file.py (L8) does not exist yet** — either defer
  the hook to L8, or add an OPTIONAL summary block to network_products.py's build
  (`if ctx.has_data("network_contract_check.csv"): ...`) — do NOT add to required_csvs.
  Always write at least the 'overall' row (a header-only file counts as absent for has_data).
  Extend test_network_products (pyviz/tests/test_network_viewers.py:54).

**Validation:** extend tests/test_network_product_io.cpp (SbasisContractRoundTripAndCheck
:464) to cover csvSanitize. Short run with serialize_sbasis_contract=true: overall=PASS;
hand-edit the S line's disc_thresh, re-run the pyviz check → FAIL surfaced.

---

## L15. [OPUS, both, ~1 day, P2 — schedule LAST] Run provenance manifest: `network.write_run_manifest` → `network_run_manifest.json` (JSONL)

**Why:** V0-V7 archives are judged against recorded signatures weeks later; with everything
uncommitted, git state says nothing about which toggle set produced a logs/ folder.
**Deps:** none hard; L9 labels A/B by hash when present. Its value grows as L1/L2/L4/L7/L10/L13
add toggles — enumerate whatever exists at implementation time.

**Spec (C++):**
- Toggle per conventions (+ON info line). Write site: **after :1136** (the local const
  snapshot pl_on/ura_on/raim_on/clk_model_*/start_offset_s at :1099-1136 is then in scope)
  and before `double prev_tow = -1.0;` :1137. `create_directories` already ran (:939).
- Hand-emit JSON with fmt (available, `#include <fmt/format.h>` :86; no new JSON lib);
  escape backslashes in Windows paths; deterministic field order. Fields: written_utc
  ISO8601; config_path — **use the existing `Application::config_path_` member**
  (include/app/Application.hpp:491, used at Application.cpp:90) — no new accessor;
  config_hash_fnv1a64 of the raw config bytes (re-read config_path_); build
  {compiled: `__DATE__ " " __TIME__`}; the network.* dump — MIX of `nc.*` POD fields
  (stations.size(), clock_datum_station/satellite, epoch_sync_tolerance_s,
  product_output_path, parallel_*, trace_*, serialize_sbasis_contract) and
  `config_.networkXxx()` getters for the QC knobs (they are private GnssConfig members:
  networkPhaseLeveling() :1268, networkClockModel() :1756, networkEmitUra() :1815,
  networkStartOffsetHours() :1851, …). Full verified key inventory in loadNetworkSection_
  :2635-3314 — enumerate at least: clock_datum_station/satellite, epoch_sync_tolerance_s,
  product_output_path, parallel_{stations,min_stations,max_threads,reduce}, all trace_*,
  serialize_sbasis_contract, estimate_{sat_code_bias,iono_sh,glonass},
  phase_leveling{,_q_amb,_warmup_epochs,_max_gap_s,_var_floor_m2},
  shared_carry_var_floor_{pb,cb}_m2, carry_zwd, zwd_carry_q, cycle_slip.{turbo_edit*,
  if_phase_td*}, raim{,_k_sigma,_min_stations,_attribution,_within_epoch,_sigma0_trigger,
  _withhold_sigma0,_surgical_station,_station_drop_bias,_redundancy_normalize},
  clock_monitor{,_abs_max_m,_jump_max_m,_couple_raim,_reinit_sigma_m},
  clock_model{,_q1,_q2,_k_sigma,_meas_std_m,_max_gap_s}, emit_ura, ura_ewma_alpha,
  consume_ura, ura_retro_bump, ura_refloat_inflate, **ura_staleness_q2 (:3062 — easy to
  miss)**, pivot_vanish_return_signal, emit_clock_disc, start_offset_hours,
  ambiguity_resolution.{enable,min_success_rate,ratio_threshold,ratio_upper_bound,
  nl_max_per_pair_frac,enable_partial_ar,wl_cascade,min_epochs}; process_noise
  {sat_clock 1e-2, sat_phase_bias 1e-12, sat_code_bias 1e-10} via getProcessNoise()
  (q_clock/q_pb/q_cb locals at :887-889).
- End of run: APPEND (reopen) a second JSON object on a new line (document as JSONL) —
  {n_synced, n_emitted, n_skipped, n_gate_withheld, n_sigma0_withheld, loop_ms} — after
  :4467, before the B6 block :4469. **loop_ms must be hoisted** out of the :4445-4453
  timing block (same fix as L3 — coordinate if both land).
- Do NOT read git state at runtime (tree shared across concurrent sessions — misleading;
  config hash + toggle dump is the honest provenance). Crash-resilience: JSONL-append,
  never rewrite — a crashed run keeps the startup record (itself diagnostic).

**Spec (pyviz):** viewer `network_manifest`, required_csvs=() — build must raise ViewerSkip
itself when the json is absent (required_csvs=() makes the driver check vacuous).
**Figure-less ViewerResult is already tolerated** (test_driver.py:13 builds one;
report.py:102-103 renders summary-only as `<pre>`) — return
ViewerResult(figures=[], summary=pretty key:value block). Report header: pyviz/gnssviz/report.py
build_html_report (meta line :88-89; log_dir already a kwarg) — read the manifest there,
or pass via run_report.py's existing extra_html kwarg (:57/:91-92). NOT eval/report.py.
Tests: present/absent/JSONL-two-records.

**Validation:** OFF byte-identity. ON 10-min smoke:
`python -c "import json;[json.loads(l) for l in open('logs/network_run_manifest.json')]"`
parses both records; every existing network.* key present with the run value; end-record
counts match the completion line. pytest; report header shows the hash.

---

## Execution notes

- **Canonical order = L1…L15** (dependency-then-priority, from the merge judge). L6 and L8
  are pure-Python and dependency-free — ideal first cards or fillers while a C++ card's
  validation run bakes. L5 can be developed from the L1/L2 header contracts before those
  land (fixtures are synthetic).
- L3 and L15 both touch the `loop_ms` scope hoist at :4445-4453 — whichever lands second
  re-greps and reuses.
- After each card: re-run `pytest pyviz/tests -q` and the gnss_tests suite; confirm
  OFF-path byte-identity before any armed run; record which V-card the new instrument
  serves in the session summary.
