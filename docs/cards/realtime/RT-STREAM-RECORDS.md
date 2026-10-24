# RT-STREAM: real-time engine records: the AsteRx-U SBF rover path, the IGS RTCM stream and their failure modes

**STATUS**

- **id:** RT-STREAM
- **topic:** realtime
- **status:** reference
- **summary:** Records of the real-time PPP-RTK bring-up (2026-06 to 2026-08): the Septentrio AsteRx-U SBF rover path and its hard constraints, the IGS RTCM stream recipe, the BSSD epoch blow-up chain, the real-time model degradation list and the Galileo C5Q/C7Q code bias.
- **dates in the text:** 2026-06-15 to 2026-10-07
- **source:** agent memory notes `project_realtime_asterx_ppprtk`, `project_realtime_igs_rtcm_stream`, `project_realtime_bssd_epoch_blowup`, `project_realtime_model_degradation` and `project_galileo_c5c7_code_bias`, copied 2026-10-24

**Outcome.** The real-time engine and its two live feeds (the rooftop AsteRx-U over SBF, and the IGS NTRIP RTCM stream) were brought up between June and August 2026. These notes keep what the theory layer (`docs/report/realtime_engine.tex`, `ssr_corrections.tex`) and the code comments do not: the receiver's hard constraints, the working stream recipe, the causal chain of the BSSD epoch blow-up, which models degrade in real time, and the Galileo code-bias findings. The status notes mark statements overtaken since, among them the E23 I/NAV odd-page decode defect (fixed 2026-10-23, card INAV-ODD) and the rename of the satellite-bias sign constant.

> Results are appended TO THIS FILE, below the existing text.

---

## 2026-10-24: AsteRx-U rooftop receiver to real-time PPP-RTK: hard constraints, conventions and traps (checked 2026-08-15)

These results were recorded only in the agent memory note `project_realtime_asterx_ppprtk` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: `kSsrPhaseBiasSign` and `kSsrCodeBiasSign` no longer exist: one `kSatBiasSign = +1.0` (`include/measurement/ObservationEquation.hpp`) serves producer and consumer (card SBASIS-CONFORMANCE-F still cites the old name). The live GPS + Galileo + SSR sessions of 2026-10-22 and 23 (cards INAV-ODD and EPH-JUMP) ran with SSR applied, which exercises the 4076 IOD-width fix and convergence past 76 s; the BSSD re-gauge and the cross-system ATX fallback are not separately recorded. The mountpoint SSRA00IGS0 cannot be checked offline (`config/realtime_ppp_rtk_AR.json` names it; the live configs use SSRC01CAS1).

Rooftop Septentrio AsteRx-U (Oregon State, Corvallis; LAN-only 10.214.154.211, web UI) to real-time PPP-RTK.
Milestones M0-M6, the SBF / Meas3 / raw-subframe decoders and every fix below are committed. Theory layer:
`docs/report/realtime_engine.tex` + `ssr_corrections.tex`; the conventions and traps below are memory-only
(checked 2026-08-15).

Hard constraints (do not re-attempt):
- The unit CANNOT emit RTCM3/CMR: permission file (S/N 3013924) has `rtkbase=0 dgpsbase=0 rtkrover=0`; the
  internal caster stays empty ("Output of RTCMv3 not supported", 404). Unlocking needs a purchased RTK Base
  permission file. SBF is the only rover feed.
- Ports: `28785` = IPS1 DATA (MeasEpoch 4027, Meas3Ranges 4109, PVTGeodetic 4007), the one to use; `28784`
  = CLI (no MeasEpoch); `2101` = caster (moot). Config `realtime.input_type=tcp`, `input_format=sbf`
  (`config/realtime_ppp_rtk.json`; replays `config/sbf_replay.json`, `sbf_replay_covl145.json`).
- Decoded nav is OnChange only (about 1 block / 90 s): bootstrap from RawNavBits (GPSRawCA 4017, GALRawINAV
  4023, BDSRaw 4047, GLORawCA 4026, via `SubframeAssembler`) or `setSBFOutput ... sec30` + `exeCopyConfigFile`.
- `smrf, OnlyRef` makes every Meas3 epoch self-decodable (delta epochs before the first reference are skipped).
- Every constellation needs a `signal_priorities` entry or it is dropped silently
  (memory note `project_multignss_signal_priorities_drop`).

Reference authority:
- Meas3 (4109) is deliberately UNDOCUMENTED (SBF Ref Guide p.272; pysbf2 lacks it). Only public reference:
  rtklibexplorer RTKLIB `demo5`, `src/rcv/septentrio.c::decode_meas3ranges()`. `Meas3Decoder` is a clean-room
  port and a process-wide singleton (delta epochs need reference state; `feedSbfFrame` builds a fresh parser
  per frame). `gnss_meas3_xcheck` on dual-logged 4027/4109: PR < 1 mm, phase < 0.002 cy, 166 epochs.
- BeiDou D1 raw subframes: 32-bit stride, NO shift (RTKLIB `decode_cmpraw`), not 30-bit/`>>2` like GPS LNAV.
- IGS-SSR 4076 (sources: RTKLIB rtcm3.c, IGS-SSR v1.00, cssrlib): IM numbering GPS 21-27, GLO 41-47, GAL 61-67,
  QZS 81-87, BDS 101-107, SBAS 121-127; wrong values cross-wire every constellation. The satellite block is
  UNIFORM: 8-bit IOD, NO IODCRC (legacy 1258/SBAS only). Signature of the legacy-descriptor bug: GPS works,
  GAL/BDS never reach cm grade. "GNSS IOD": GPS = LNAV IODE;
  Galileo = low 8 bits of IODnav; BeiDou = mod(toe_sow/720, 240), not AODE. Single mapping site
  `LiveEphemerisStore::ssrMatchIode()`. Mountpoint SSRC03IGS1 (SSRA00IGS0 does not exist). Legacy dialect:
  orbits are APC-referenced, so an unconditional sat-PCO double-counts 0.6-2.6 m.

Open (as of 2026-08-15 unless dated):
1. `kSsrPhaseBiasSign = +1.0` (`MeasurementModel.cpp`) is UNVALIDATED: WL/NL fractions near 0 = correct, near
   0.5 = flip. No measurement record found 2026-10-07 (card SBASIS-CONFORMANCE-F: "empirically pinned to one
   provider", a second provider is a live hazard). The in-house closed loop uses the same sign at producer and
   consumer, so the exposure is third-party SSR. Trust nothing downstream of AR on it until measured.
2. The IGS combination stream cannot support AR (Galileo pbias 0/19, no E5a code bias):
   memory note `project_galileo_c5c7_code_bias`. Probe SSRC00CAS0 with `SSR-CBIAS-KEYS`; CNES is on its own caster.
3. Live validation owed: the 4076 IOD-width fix (E/C sats log "(broadcast+SSR)"), the BSSD re-gauge
   (`sat_bias_trace.csv` -94,715 m to about ±10 m, position unchanged), the receiver cross-system ATX fallback,
   float convergence past 76 s.
4. BDS SSR epoch tagged {GPS week, BDT tow} (about 14 s early); revisit if BDS SSR looks off.

Estimation mechanisms and traps:
- ⚠ [up, ZWD] null-space runaway under BSSD (no clock/ISB state; height and ZWD partials both
  elevation-monotone): ZWD to -1000 m, height to +1000 m, self-consistent, 5-7 min recovery; trigger = a
  single-constellation start. Post-fit standardization is floored at R not S, so broadcast-grade code (1-2 m)
  never trips 6σ; peeling phase-stripped Galileo code removes the geometry that separates ZWD from height.
  Fixes: per-constellation quorum over `getActiveSystems()` (CONFIGURED, not present, so a late constellation
  HOLDS the gate) with `min_satellites_per_system=4`, `need = min(per_system, max(total,2))`;
  `estimation.zwd_constraint` pseudo-obs 0.10 ± 0.30 m via
  `applyPseudoObservation` (covariance-consistent; a value clamp is re-derived next epoch); `drop_broadcast_grade`;
  pivot via `pickExclusionAwareBest`.
- Startup gate `estimation.defer_until_corrections_ready.{enabled,min_satellites,timeout_s=120}`: QUORUM
  semantics (`require_all_above_mask=false`) by the author's reversal after two lagging sats stalled everything.
  The harm prevented is the global code-only cold start, not late joiners. The progress log names the
  still-broadcast-only sats so a long wait is never silent.
- Phase is gated to code-only on broadcast-grade sats (keyed on CLOCK provenance, `SatGeometry.broadcast_only`;
  flips back when the SSR clock applies). Removed the ~148 km per-ambiguity IF seeds.
- BSSD code-bias re-gauge: subtract one per-epoch MEDIAN code innovation (gated `enableBetweenSatelliteSd() &&
  !hasClock()`) so sat-bias states do not seed with the unsteered clock; exactly SD-invariant (gauge only):
  conditioning hardening, NOT a cure for anything. Median, not WLS: `getClockIndex()` is `SIZE_MAX` under BSSD.
- ⚠ PP/RT asymmetry: RT discarded `kalman_filter_->update()`'s result, so a `NeedsMeasurementRebuild` was
  dropped and `takeRequestedPivotHandovers()` never drained (E05 stayed pivot after an IODE roll: ~105 m
  pre-fit, position frozen 7 epochs). Shared seam `include/app/EpochUpdateSupport.{hpp,cpp}`
  (`computeRebuildPassBudget`, `forwardKfPivotHandovers`, `reinflateResetSatStates`), called by both
  `Application.cpp` and the RT seams (`RealTimeEngineEstimate.cpp`, `RealTimeEngineSlipQc.cpp` since the
  2026-08-01 seam cut). Rule: anything added to the PP epoch loop goes through this seam or RT silently
  diverges.
- dt=0: 4027 and 4109 co-logged at one TOW ran `processEpoch` twice; guards in `processEpoch` and
  `KalmanFilter::predict`.
- Expected, not bugs: seeding-epoch code innovation exactly 0 and identical large phase innovations (the `Meas`
  trace is undifferenced); "no GPS code observations" ISB note under BSSD; "Clock interp ... no data" in live.
- Read the per-epoch `[RT] SSR coverage: N sats | clk-applied X/N | in-store orb/clk/cbias/pbias` line:
  the gap to in-store separates IODE reject from stream gap; low pbias means no AR.
- Galileo saw-tooth (1.5-3 min) was orbit/clock IODE mismatch at I/NAV cutovers: fixed by
  `LiveEphemerisStore::pickForSsr` on both paths and an always-live clock gate. A smaller ZOH code drift
  on matched sats (flat C0-only SSR clock) remains, untouched.

ATX traps (the parser bug bites post-processing too; see memory note `project_atx_radome_fallback_card`):
- `std::from_chars` (floating) rejects a leading `+`; Geo++ files (`LEIAR25 NONE` / `LEIT` type means) prefix
  every PCV with `+`, giving an EMPTY PCV grid (coverage drop, rx PCV silently 0). Fixed with a `+`-tolerant
  wrapper. Ground truth: `gnss_atx_diag <igs20.atx> [type-substring]` (the coverage drop is the reliable tell).
- `LEIAR25_NONE` keys are only {G01,G02,R01,R02}: with `enable_receiver_cross_system_atx_fallback`, E5a maps
  to G02, not G05 (the `.R3/.R4` variants are distinct types; E08 has no chain entry and is dropped); the
  satellite antenna never borrows another constellation's entry.
- `model_corrections.receiver_antenna_type_override` seeds header-less live SBF (post-processing unchanged).
- `gpt3_grid_path` selected but missing degrades to NMF with one WARN only: grep the startup log.

Build: `--target gnss_core` does not relink `gnss_processor.exe` (a stale exe makes a fix look inert). Ninja
once missed a header cascade (2 jobs instead of ~18): check the job count, touch the header if needed.

Tools and captures: `scripts/sbf_probe.py`, `scripts/sbf_rawnav_dump.py`, `gnss_sbf_diag`, `gnss_meas3_xcheck`,
`gnss_atx_diag`; `sbf_capture*.sbf` (dual-logged oracle), `data/sbf/covl1452.26_` (76 epochs, WLS rms 1.24 m,
within about 3.5 m of PVTGeodetic from broadcast alone).

memory note `project_realtime_glonass`, memory note `project_realtime_model_degradation`, memory note `project_rt_report_surveyed_reference`.

## 2026-10-24: IGS RTCM stream as the analogue of the SBF rover path: recipe, traps and open fallbacks

These results were recorded only in the agent memory note `project_realtime_igs_rtcm_stream` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: the RTCM parser keeps a stream-time tracker (nearest continuation) and uses the host clock only at cold start (`include/rtcm/RtcmParser.hpp`), so the statement below that MSM observations take the GPS week from the wall clock is outdated (a historical replay still gets the host's week). The TLS keys are `realtime.ntrip_use_tls` and `realtime.ntrip_tls_insecure` (`NtripClientConfig.use_tls` does not exist). Open items with no register row: the stall-fallback decision, the BDS SSR age policy, the unsteered receiver-clock gauge fix and the outlier amplifier fixes.

`config/realtime_igs_ntrip.json` is the RTCM analogue of the SBF rover path (memory note `project_realtime_asterx_ppprtk`).
Everything below is committed (as of 2026-08-15).

Working recipe:
- Decode: MSM4 (1074-1134) + MSM7 (1077-1137); eph 1019/1020/1042/1044/1046 (the engine uses 1046 I/NAV and
  ignores 1045 F/NAV); station coords 1005/1006. ⚠ MSM obs take the GPS week from wall clock: stale replays get
  the wrong week, the stream must be genuinely live.
- A-priori seed: `feedRtcmFrame` seeds position from the 1005/1006 `arp_ecef` on the first epoch when no
  reference is set; `receiver.reference_position_ecef` always wins (a fast-converge convenience).
- Third input `realtime.ephemeris` (default `enabled=false`, `openAuxRtcmStream` in ApplicationRealtime): a
  broadcast-nav mount (e.g. BCEP00BKG0 on products.igs-ip.net) for obs-only stations.
- Verified station BRUX00BEL0 on `igs-ip.net:2101` (MSM7 all systems + eph + 1006; ECEF 4027881.628 /
  306998.537 / 4919498.984; antenna `JAVRINGANT_DM   SCIS`). Alternatives WTZR00DEU0, ONSA00SWE0, GANP00SVK0.
  One free BKG account (register.rtcm-ntrip.org) covers igs-ip.net and products.igs-ip.net.
- TLS works: `NtripClientConfig.use_tls` (https + `CURLUSESSL_ALL`, `tls_insecure` for tests), for port-443
  casters like CDDIS.
- `realtimeUsesLiveOrbitClockSource()` true: SP3/CLK skipped, broadcast + SSR overlay; igs20.atx still required.

⚠ Traps that each cost a whole run (0 epochs, near-silent):
1. `receiver_antenna_type_override` must be an ATX ANTENNA name, not the receiver model (P389 had
   `"SEPT POLARX5"`; correct `TRM59800.80 SCIT`). With `require_atx_coverage:true` every signal fails coverage
   and the skip logs at DEBUG on `ppp_model`. Matching is whitespace-insensitive. Fallback:
   `require_atx_coverage:false` (keeps data, few-cm height bias).
2. `input_format` MUST be `"rtcm"`: the routing test is `== "sbf"`, so a leftover value decodes nothing.
3. An obs-only station (no 1019) leaves LiveEphemerisStore empty, so SSR deltas have no baseline
   (`eph_iode=-2` / `brdc_iode=-1` in corrections.log). Arm `realtime.ephemeris` or use an obs+nav station.

Root cause that shaped ingest: obs lag accepted future-stamped SSR. EPH-PICK showed all sats at `age = -5424 s`
(one uniform offset): engine backlog, not the feed; `freshEnough` accepted `age < 0` and the clock polynomial
extrapolated backward. Landed: (A) bounded DRAIN loop in `pumpFrames`; (B) `processing.enable_obs_skip_to_latest`
(drops obs > N s behind wall-clock GPS, before any state mutation, so the gap path trips on resume); (C1)
`freshEnough` rejects `age < -max_age` (per-sat REJECTED warn); (C2) always-on fit-interval guard (GPS/QZS 4 h,
GAL/BDS 2 h, GLO 0.5 h) applied AFTER `pickForSsr` (EPH-STALE warn). Knobs (`processing`):
`enable_obs_skip_to_latest`, `obs_skip_max_lag_s` 5, `rt_backlog_warn_s` 5, `ssr_max_age_s` 30. Single wall-clock
source: `TimeUtils::nowContinuousGpsSeconds()`. ⚠ Skip-to-latest needs an NTP-synced host. C1 alone gives a
broadcast-grade solution at stale obs time; A+B restore real time and cm grade.

⚠ ONE causal chain with memory note `project_realtime_bssd_epoch_blowup` Mode 2: fix B pins obs to wall clock and pushes
them AHEAD of the lagging SSR, which forces the exclusion cascade and is why `enable_obs_ssr_sync` exists.
Reason any change at one end against the other.

P389 forensics:
- Geometry-free IF plausibility gate (`ModelGeometry`, `kMaxGeomFreeMeters = 30.0`): a pair whose code GF exceeds
  30 m falls through to the next pair / single frequency. Motive: C1S decoded as a byte-duplicate of C5Q and
  G05 C2W 124 m below C1W; IF amplification flew the WLS ~600 km off. Decoder tables are statically correct; the
  corruption was never localized.
- Diagnostic recipe: closure from a known-correct a-priori, `obs_IF + sat_clk - geom_range` must be one common
  rx clock; a ~500 km spread puts the defect upstream of the filter. Traces: CODE-DECOMP (all systems), RAWOBS,
  EPH-PICK.
- ⚠ Unsteered receiver-clock gauge trap (diagnosed, FIX NOT IMPLEMENTED as of 2026-10-07; the IGS config still
  has `clock_bias_q` 1e3): P389's clock drifts ~2.4 km/s (`wls=416685 m`); `filter.clock_bias_q=1e3` allows
  ~30 m/epoch, and the non-BSSD WLS re-seed shifts only the clock, not the ambiguities (clock vs ambiguity
  is a gauge freedom), so phase drags the clock back
  (62 km tug-of-war) and 100 % of code is rejected. Probe `clock_bias_q` about 1e7. Proper fix: white-noise
  clock AND co-shift ambiguities + phase clock by any re-seed delta; BSSD needs the same. Loosening the 6σ /
  `code_std_m` gate is diagnosis only, never the fix.

OPEN: (1) obs/SSR ~50 s desync: hold buffer and amplifiers in memory note `project_realtime_bssd_epoch_blowup`. (2) BeiDou
SSR is genuinely ~50 s old on the IGS combination and trips `ssr_max_age_s=30` (broadcast fallback): needs a
per-constellation age policy, not a global bump.

## 2026-10-24: BSSD epoch blow-up in real time: the obs-ahead-of-SSR chain and the Galileo ephemeris overwrite

These results were recorded only in the agent memory note `project_realtime_bssd_epoch_blowup` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: OPEN item 1 below (the receiver's Galileo ephemeris differs from the IGS one at the same IODnav, decode audited correct) was wrong: `SubframeAssembler::consumeGal` read the I/NAV odd-page data at page bits 122 to 137 where Septentrio NAVBits places it at 116 to 131. Fixed on 2026-10-23 (commit 0378e6d, merged f45070e, card INAV-ODD); `processing.ephemeris_jump_reseed` (commit 691a8e7, default off, card EPH-JUMP) was added. The stall fallback itself is unchanged.

Both modes share the obs-ahead-of-SSR forcing; seen on `realtime_ppp_rtk.json` (SBF rooftop) and
`realtime_igs_ntrip.json` (IGS NTRIP). Fixes committed (as of 2026-07-29). Read with
memory note `project_realtime_igs_rtcm_stream` (one causal chain).

Mode 1, BSSD "every measurement an outlier": after ~8 clean epochs one epoch goes bad (TOW 336454: max|sres|
40708, 68/70 over threshold), with "Reinflated iono state" around it.
- Chain (log-proven): the receiver's SBF Galileo eph overwrote the IGS-BCEP eph at EQUAL toe/IODE; Galileo ZD
  code innovations stepped -26..-172 m (GPS uniform, cancels in SD); the near-singular gain (cond(S) about
  1.1e10, phase σ 1 mm) made dx.norm about 1e5 m; the 68/70 table is POST-update; the pivot-slip detector then
  stormed (~80 handovers) and iono reinflated. Those last two were downstream, not causes.
- Why IGS eph MUST win: the IGS-RTS SSR delta is referenced to IGS broadcast; the SD residual literally is the
  IGS-vs-SBF broadcast difference.
- Fixes: source priority in `LiveEphemerisStore::insert(eph, bool from_igs)` (a receiver record never overwrites a
  current IGS record unless its toe is at least `kIgsStallFallbackSec` = 1800 s newer; strict `<` because 1800 s
  is the GLONASS toe cadence). Live: max dx.norm 53.6 (was 1.18e5), 0/90 over threshold. Freshest-toe rule
  (necessary, not sufficient). Toe-aware pivot: `kPivotToeSpreadMaxS = 1200 s` bars a stale-toe sat from PIVOT
  election only (inert in post).
- Author decisions: an absolute innovation / gross-error MAGNITUDE gate is REJECTED (unsteered-clock ZD
  innovations are always huge; the >100 m check stays warn-only). A numerical dx.norm / cond(S) update-reject +
  pre-fit per-sat ZD gate is DEFERRED (threshold entangled with the variable-clock problem).

Mode 2, obs/SSR exclusion cascade: progressive "Excluding previously rejected measurement" until all code is
dropped, starting near `SSR for E07 REJECTED stale: |age|=40.9s > 30.0s`.
- Root cause: `enable_obs_skip_to_latest` pins obs to wall clock, AHEAD of the lagging IGS-RTS SSR.
  ⚠ Two time bases: the freshness gate keys on `received_time`, the `|age|` warning on the CONTENT epoch
  (`getSsrEpoch`). The stale sat is DROPPED (`drop_broadcast_grade` is a per-epoch filter, not a startup gate);
  the peeled rows are the cm-grade SURVIVORS. Red herrings: the BSSD
  re-gauge (SD-invariant, runs after the drop).
- Amplifiers: tight code R (0.3 m, 6σ); `handleCodeOutlier` never feeds `last_reset_satellites_`
  (memory note `project_codeonly_pivot_iono_scrub_gap`) and stuck_code_bias was disabled, so it recurs every epoch; the
  mixed code+phase reset denominator (`bad_meas_percent=0.5`) never trips on all-code-dropped (silent
  phase-only commit, no "Resetting filter").
- Fixes: `processing.enable_obs_ssr_sync` (default false; ON in the IGS config) holds obs newer than
  `newestSsrEpoch() + ssr_max_age` in a keep-latest buffer drained by `drainHeldObs()` after each SSR push;
  `obs_ssr_sync_max_hold_s` (30 s) latches `sync_outage_` so a dead feed cannot starve the filter; `flush()`
  bypasses via `draining_final_`; a held-obs drain bypasses skip-to-latest. Content-epoch dt: SSR polynomial dt
  now runs from the record's content epoch
  (`OrbitClockCorrection.epoch_time` for clock, `orbit_epoch_time` for orbit); the freshness gate deliberately
  stays on `received_time`.

STILL OPEN (as of 2026-07-29; stall fallback re-checked in code 2026-10-07):
1. SBF Galileo eph genuinely differs from IGS at the SAME IODnav, yet a field-by-field audit of
   `SubframeAssembler::consumeGal` vs `RtcmNavDecoder::decodeGalileo1046` found the decode correct. The
   EPH-SAME-IODE-DIFF diagnostic logs the differing element on collision: do NOT guess-fix the decoder before that
   data lands.
2. Stall fallback is unsafe: on a real IGS outage it reintroduces the divergent SBF orbit under IGS-referenced
   SSR (still the code's behaviour 2026-10-07). Choose: force broadcast-grade / SSR-off for receiver-source sats,
   or drop on stall (author decision pending).
3. Amplifier fixes not done: streak-guarded reinflate in `handleCodeOutlier`; class-aware (code vs phase) reset
   denominator; age-proportional code-R inflation near staleness. Live check: `Outlier flagged sat=...` should
   name SURVIVORS, and `SSR coverage clk-applied X/N` collapses at onset.

Method lesson: adversarial verification against the RUN LOG was decisive; code-only agents got the mechanism wrong
three times (the re-gauge was exonerated three separate times).

memory note `project_pivot_handover_flipflop_fix`.

## 2026-10-24: which models degrade in real time (audit of 2026-06-27)

These results were recorded only in the agent memory note `project_realtime_model_degradation` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: the satellite-PCO versus SSR-clock APC datum question is settled in the code: legacy-dialect orbits are APC-referenced and `realtime.honor_ssr_reference_point` (default off) skips the satellite PCO for them; the default keeps the double count (documented).

Audit of 2026-06-27, verified against the tree. RT shares the FULL model layer
(`RealTimeEngine::processEpoch` calls `ppp_model_->computeModels`, the same `GnssCorrections` / `Antenna` /
`ModelGeometry`); nothing is simplified, only the input products change.

Degrades in RT:
- ERP: no stream carrier, only a `.erp` file. With `erps_` empty, pole tide and ocean pole tide are omitted
  (zero + warn), up to ~25 mm radial; the Sun/Moon ECI to ECEF frame silently uses `xp=yp=ut1=0` (sub-mm).
  `interpolateErp` flat-extrapolates, so a recent ERP is sub-mm. Correction 2026-10-07 ("all RT configs set
  `enable_pole_tide=false`" is wrong): the loader default is true; `realtime_ppp_rtk.json`,
  `realtime_igs_ntrip.json` and `sbf_replay_covl145.json` set false, `realtime_ppp_rtk_AR.json` sets true, and
  `sbf_replay.json` omits it (on), so those two need an ERP or log the omission warning.
- GIM: absent (the SSR decoder rejects VTEC IGS-IM-201). `enable_higher_order_ionosphere` hard-skipped + warned;
  `ionosphere_weighting` and `adaptive_iono_process_noise` are NO-OPS in RT (wired only in `Application.cpp`),
  each WARNs at RT init. Per-sat slant iono still estimated (float only).
- Orbit/clock/bias: broadcast + SSR; SP3 interpolation (incl. `trig_sp3`) is dead code in RT. SSR clock is often
  C0-only ZOH (the Galileo saw-tooth). ⚠ sat-PCO vs SSR-clock APC datum is unreconciled (igs20.atx PCO applied
  without a provider-datum check): a few-cm radial/clock sleeper. SSR bias coverage has gaps vs a daily `.BIA`.

Post-only (loader forces false in RT): `run_smoother`, `enable_noncausal_slip_bridge`, `enable_neq_export`;
`gnss_rate_s` decimation; `max_duration_s` is re-implemented in RT.
RT-only (inert in post): `zwd_constraint`, `defer_until_corrections_ready`, `enable_obs_skip_to_latest`,
`enable_obs_ssr_sync`.
Identical: the site-displacement / propagation stack (solid tide, OTL, APL, Shapiro, Sagnac, relativity, rx/sat
PCO+PCV, wind-up), `refine_emission_time`, and GPT3+VMF3 troposphere (static bundled climatology grid, NOT
operational VMF3 NWM).

Predicted-ERP fetch (landed): `ProductResolver::resolveErp(year, doy)`: in live RT
(`download_orbits_clocks==false`) with `product_type==FIN`, switch ERP to RAPID and walk back
`kErpMaxAgeDays=7` days (first hit wins). Honors explicit RAP/ULT. Plumbing: `type_override` through
`buildProductFilename` / `buildProductUrl` / `resolveProduct`, `typeSeg()` (FIN to MGXFIN, RAP to OPSRAP, ULT to
OPSULT), `shiftDate()`; one-time WARN in `ProductParser::loadProducts` when `erps_` is empty. Helps
production-mode RT; internal-mode RT still needs a bundled `.erp` fixture (warns if missing). ULT-named ERP is
routed to RAP (lesson 9 in memory note `project_rt_service_work_queue`).

## 2026-10-24: Galileo C5Q/C7Q code bias on the IGS stream and the user-side +2.8 cm Up residual (status of 2026-10-07)

These results were recorded only in the agent memory note `project_galileo_c5c7_code_bias` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: `code_bias_term` is gone and `ssrCoversCodeSignal` moved: the SSR bias is applied by `addSsrBias` in `src/core/measurement/ObservationEquation.cpp`.

Status as of 2026-10-07: both halves OPEN; no newer user-side measurement found. Card GAL
(`docs/refound/GAL_gps_galileo_dual_2026-09-22.md` §2, §5) cites the JPLM +0.028 m result and notes that the
receiver antenna model was NOT ruled out; its G4 row shows JPLM00USA (`AOAD/M_T NONE`) takes the GPS-frequency
fallback for its Galileo receiver calibration (igs20.atx lacks Galileo frequencies for that antenna).

Bias-path architecture:
- `enable_code_phase_bias` gates ONLY the external `.BIA` path (`getBiasCorrection` to `code_bias_term`), not SSR.
- SSR biases apply through a separate, ungated path: `kSsrCodeBiasSign * SsrCorrectionStore::getCodeBias(sat,
  ref_type)`, matched by EXACT RINEX code, so it silently returns 0 when the provider lacks the tracked
  attribute (provider C5Q vs tracked C5X). RTA-13's band fallback is `processing.ssr_bias_band_fallback`
  (default false). The network producer disables the SSR bias import entirely.
- BSSD groups by `constellation|signal_type` (full signal); a row with no same-group pivot is REJECTED, not
  zero-differenced (rx-clock-leak purity rule).

RT verdict (2026-06-15): `SSR-CBIAS-KEYS` on SSRC03IGS1 shows E16 keys C1C / C1Z / C6A / C7X, no E5a, and C7X
differs from tracked C7Q. So `ssr_cbias` is +1.040 on C1C and 0 on C5Q/C7Q: the provider, not a decode bug.
Keep `estimate_sat_biases` ON (the author proposed disabling it; declined, since sat_bias is the only correction
those signals get). The ~-90 m Galileo `sat_bias` is BSSD-unobservable common mode (seeded from the unsteered
start, held by `sat_bias_q=1e-10`) and cancels in SD. The IGS combination advertises phase biases but delivers
Galileo pbias 0/19, and splits code and phase across separate-datum streams: no single-stream PPP-AR source
there. Candidate: SSRC00CAS0 (CAS, CoM, C1C/C5Q/C7Q); verify keys before trusting it. CNES is on its own caster.

POST verdict (2026-06-28, JPLM 2024-129, COD0MGXFIN, IF, vs ITRF2020 SINEX; memory note `project_realdata_test_suite`):
- GPS-only E -0.4 / N -0.1 / U +2.9 cm, 3D-RMS 4.4; GPS+GAL E +1.4 / N +0.1 / U +5.7 cm, 3D-RMS 6.4.
- Adding Galileo: +2.8 cm Up, +1.9 cm 3D-RMS. Same sign, smaller in uncombined PPP-RTK/BSSD (dU -1.29).
- Ruled out (dU): `enable_code_phase_bias=false` +0.01, `estimate_sat_biases=false` +0.00,
  `frequency_mode=dual` +0.24, `enable_atx_frequency_fallback=false` +0.36.
- Remaining suspects: GAL orbit/clock product datum (COD MGX), GAL sat-PCO/PCV, E1/E5a IF datum, receiver antenna
  model. ⚠ Read against the 2026-07-29 ARP ruling (memory note `project_vertical_instability_solidtide`): JPLM DELTA H
  +0.061 m predicts the GPS+GAL +5.7 cm absolute DC to 4 mm, so the absolute Up alone does not convict Galileo;
  the open item is the 2.8 cm GPS-only vs GPS+GAL DIFFERENCE. Owed:
  residual_trace + a per-system residual viewer (grading is DC position only), an alternate GAL product, a GAL
  sat-PCO check.

⚠ Cascade toggles (verified in code; do not re-propose): `enable_huber_r_inflation` is PHASE-ONLY and sub-cycle
(skips `sig[0] != 'L'`, `getPhaseHuberSresThreshold`), the memory note `feedback_subcycle_suppression_hurts_position`
path: leave OFF. The code path is `handleCodeOutlier` hard-exclude + `recordRejection` streak, and the stuck
code-bias scan resets `sat_bias` when streak >= `min_streak` (5) or EWMA |mean| > `bias_floor_m` (3 m). The real
toggles: `residual_history.enabled` (feeds `evaluateStuckBias`) + `stuck_code_bias.enabled`. The scan skips
BSSD pivots by design.

Diagnostics (need `measurement_model:trace` + `bssd_transform:trace`): `CODE-DECOMP` (rx_clk | iono |
ssr_cbias | sat_bias | bia_cbias split), `BSSD-PIVOT`, `BSSD-PAIR` (sd-paired / reject-no-pivot /
reject-singleton / pivot), `SSR-CBIAS-KEYS`.

Folded in: RT sat_bias allocation was SSR-blind (only `.BIA` consulted); fixed with
`SsrCorrectionStore::hasCodeBias` + `MeasurementModel::ssrCoversCodeSignal` (uncombined path only). Live
AsteRx-U validation owed.
