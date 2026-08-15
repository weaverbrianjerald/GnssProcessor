# RTF-1 + RTF-2 implementation plan — Opus card queue

Status: **READY FOR EXECUTION** (Fable, 2026-07-02). Normative conventions:
`docs/rt_service_conventions.md` (FROZEN — deviations require editing that file
first). This plan decomposes the RTF-1+RTF-2 card ("producer a-priori re-base to
broadcast(+RTS orbit)" + "clock ephemeris-anchor contract") into Opus-sized
sessions with the Fable-level design decisions already made. Execute **one card
per session**, in spine order.

Conventions (identical to the RT0/RTA/RTB tier): every card default-OFF and
**byte-identical when `network.apriori_source = sp3`** (the default) unless the
card explicitly says otherwise; Release-pattern full build (all targets) + full
`gnss_tests` suite green before closing a card; new `.cpp` files added to
`CMakeLists.txt` explicitly; heavy Doxygen on public headers; UNCOMMITTED batch
convention. **All line numbers below were tree-verified 2026-07-02 and WILL
drift — re-grep the quoted symbol before editing.**

---

## Tree-verified facts (2026-07-02, this session — trust these over the Jul-1 audit)

- **PppModel broadcast phase gate**: `src/core/model/PppModel.cpp:699-732`.
  `ppp_obs.broadcast_only = geom.broadcast_only` (:710); the erase block (:717-725)
  removes every key starting with `'L'` from `observed/modeled/variances/
  zenith_variances`; one-shot log via `sats_phase_gated_` (:726). PppModel holds
  `config_` (GnssConfig) and `geometry_`.
- **`broadcast_only` origin**: `Geometry::computeSatelliteGeometry` out-param
  (`src/core/model/ModelGeometry.cpp:587-591`, set at :882 from `broadcast_grade`),
  which comes from `corrections_.getSatelliteClockCorrection(sat, emission_time,
  &broadcast_grade)` inside `computeEmissionState` (:552, function at :475).
- **Post-processing has NO broadcast-nav ephemeris path.** `RinexParser::parse`
  detects `BRDC`/`_MN.rnx` files and delegates to `ProductParser::parseKlobuchar`
  ONLY (`src/core/rinex/RinexParser.cpp:213-233`) — Klobuchar coefficients, no
  orbit/clock records. `Geometry::getSatellitePV` interpolates SP3 `OrbitData`
  nodes; its `liveFallback()` uses `LiveEphemerisStore`, which is empty in
  post-processing (:1015-1047). The SSR-orbit overlay gate comment (:1251-1259)
  explicitly anticipates "once a broadcast-nav parser populates iode on
  OrbitData".
- **LiveEphemerisStore (`include/products/LiveEphemerisStore.hpp`) already has
  almost everything RTF-2 needs**: per-sat `SatRecord{current, previous}`
  (:151-158 — an IODE change demotes current→previous); `computeSatellitePV(sat,
  time, ssr_iode)` and `computeSatelliteClock(sat, time, ssr_iode)` evaluate
  **pinned to a requested IODE** via `pickForSsr` (:168) and report the IODE used
  (`SatelliteState.iode`, `SatelliteClock{bias, iode}` in the IGS-SSR match
  convention); `insert(eph, from_igs)` doc already names "the network path" as an
  intended single-source caller. Record type: `BroadcastEphemeris`
  (`include/products/BroadcastEphemerisTypes.hpp`).
- **runNetwork product loading**: `src/core/app/ApplicationNetwork.cpp` —
  `resolveData()` (:433), product cache load + orbit hard error (:527-539:
  `product_cache.getOrbitCount() == 0` → error return), GIM-vs-SH mutual
  exclusion Phase 3b (:541-556, `config_.estimateNetworkIonoSh()`), product
  writer + `pending_contract` + `apriori_product_id` construction (:760-804,
  currently `<sp3-basename>+<clk-basename>`).
- **Clock anchor**: per-sat a-priori clock anchor information =
  `1/initial_std.sat_clock²` (anchor-info trace doc, ApplicationNetwork.cpp:968-973);
  `EstimationConfig.initial_std.sat_clock` default 1000.0 m is the LOOSE init
  (GnssConfigTypes.hpp:1392) — the actual anchor pseudo-obs σ and its <100 m
  binding gate live in the driver (grep `anchor` in ApplicationNetwork.cpp; the
  Jul-1 audit put the binding gate near :2854).
- **Wire structs** (`include/rtcm/RtcmTypes.hpp`): `SsrOrbitCorrection` :75
  (`iode{0}` :77), `SsrClockCorrection` :97 (NO iode; `disc_counter` :115),
  `SsrCombinedCorrection` :123 (`iode{0}` :125). Store merge type
  `OrbitClockCorrection` already `iode{-1}` (RT0-1,
  `include/products/SsrCorrectionStore.hpp:124`); `iodeMatches` (:529) is
  permissive when either side < 0.
- **C record format**: `C,<sat>,<clock_c0_m>` + RTA-4 optional disc-counter
  VALUES (banner `include/products/NetworkProductIO.hpp:34` — re-read the banner
  for the current full grammar including I/G/H/S records before touching it).
- **Emitter clock channel has NO jump test** — clock disc bumps are forced
  (datum handover) only (`src/core/products/NetworkProductEmitter.cpp` ~:213
  comment). An IODE re-level therefore does NOT risk a spurious emitter bump;
  the disc interaction lives entirely in the driver (Card 6).
- `NetworkSatSolution.has_clock/clock_c0_m/clock_ura_m`
  (`include/products/NetworkProductEmitter.hpp:66-70`); `NetworkSolutionEpoch` :76.

## Fable design decisions (do NOT re-litigate in cards; deviations → edit rt_service_conventions.md first)

- **D1 — Gate suppression is an explicit PppModel switch, not config inference.**
  `PppModel::setBroadcastPhaseGateSuppressed(bool)` (default false). ApplicationNetwork
  calls it on PRODUCER station models only, when `apriori_source == brdc_rts`.
  The closed-loop consumer's PppModel must NOT suppress (its broadcast-grade sats
  genuinely can't use phase; sats with network clocks aren't broadcast_only anyway).
  Rationale: the same GnssConfig serves producer and consumer in closed loop —
  config inference cannot distinguish them.
- **D2 — `OrbitClockCorrection` gains a separate `clock_iode{-1}`.** The single
  `iode` stays the ORBIT anchor. `getOrbitClock`: clock-only merge sets
  `clock_iode = SsrClockCorrection.iode` (and leaves orbit `iode` -1 as today);
  combined sets `clock_iode = iode`; orbit+clock merge keeps orbit `iode` and the
  clock record's own `clock_iode`. GnssCorrections gates ORBIT application on
  `iodeMatches(brdc_iode, iode)` (unchanged) and CLOCK application on
  `iodeMatches(brdc_iode, clock_iode)` (new; permissive while clock_iode = -1 →
  all existing producers/replays byte-identical).
- **D3 — Ephemeris cutover re-level = a deterministic KF state-mean shift in the
  producer driver, not an emitter feature.** The producer's estimable per-sat
  clock state is Δδtˢ vs the a-priori (broadcast) clock. When the eph version the
  geometry uses rolls (old IODE → new IODE), the a-priori jumps by
  Δ_eph(t) = brdc_new(t) − brdc_old(t); without action the filter would absorb it
  as a transient. Instead the driver shifts the state mean:
  `x[sat_clock] -= Δ_eph(t)` (covariance untouched; c1 of the 2-state clock model
  untouched; D2 carry for that sat shifted identically if a carried prior exists
  this epoch). Both eph versions are evaluable via
  `LiveEphemerisStore::computeSatelliteClock(sat, t, iode)` (previous retained).
  Emitted C0 is then continuous-in-reconstruction: consumer computes
  brdc(new_iode) + C0. NO disc bump on a successful re-level. Fallback (previous
  version no longer held → Δ_eph incomputable): per-sat forced disc bump through
  the existing markSatDiscontinuity/forced-bump machinery. Detection point: the
  per-sat IODE reported by the emission-state path, compared against a driver-held
  `last emitted IODE` map (Card 6).
- **D4 — Replay parity = RINEX-nav → LiveEphemerisStore.** The nav file is parsed
  into `BroadcastEphemeris` records and `insert()`ed into the SAME store the RT
  path fills from RTCM — `liveFallback()`/`computeSatelliteClock` then serve
  replay and live identically ("frame consistency from the shared code path",
  conventions §1). Do NOT build a parallel post-processing eph evaluator or
  populate SP3 `OrbitData` from nav.
- **D5 — Wire extension follows the RTA-4 optional-trailing-field convention.**
  New C-record fields (clock `iode`, orbit echo `orbit_iod_ssr` + orbit content
  epoch) are appended ONLY when set (iode ≥ 0); reader tolerates their absence.
  `sp3` mode never sets them → product files byte-identical. Grep how RTA-4
  serialized disc-counter values and copy that pattern exactly (writer, reader,
  emitToStore, replay self-check).
- **D6 — Anchor σ knob**: `network.apriori_clock_anchor_sigma_m`, default 0.0 =
  auto (sp3 → today's value unchanged; brdc_rts → 3.0 m per conventions §1).
  Explicit positive value wins in both modes. The <100 m binding gate is
  unchanged.
- **D7 — Consumer applies the clock against the STAMPED eph version.** On the
  live path, clock application evaluates the broadcast baseline via
  `computeSatelliteClock(sat, t, clock_iode)` (pinned eval — the store's
  current/previous retention absorbs the producer/consumer cutover skew);
  `iodeMatches(brdc_iode, clock_iode)` remains the drop gate when the pinned
  version is no longer held. This mirrors what `pickForSsr` already does for the
  orbit overlay.
- **D8 — GLONASS is OUT OF SCOPE.** Producer stamps clock_iode = -1 for R sats
  (permissive gate, today's behavior); GLONASS eph-anchor semantics belong to
  `docs/glonass_enablement_plan.md` (note there: `iodeMatches` permissive-on-(-1)
  masking is a known caveat).
- **D9 — SSR-orbit ingest into runNetwork replay = injection seam only.**
  A `setSsrStore`-style seam on the network driver with an orbit-only accept
  filter (reuse the RTA-8 `ssrPushAction` machinery), so recorded/live RTS orbits
  can be attached later; live transport is RTF-3. brdc-only replay is the
  accepted validation mode: broadcast radial orbit error absorbs 1:1 into the
  estimated clocks — the product is still consistent (consumer applies the same
  brdc orbit), just with the orbit error riding in the clock. Validation criteria
  in Card 8 account for this.
- **D10 — `apriori_product_id`**: `brdc:<nav-file-basename>` in brdc_rts replay;
  `brdc+rts:<provider-or-mount>` once an RTS orbit source is attached (seam
  carries an identity string); sp3 mode unchanged.

## Dependency spine

```
Card 1 (config)  ──► Card 2 (nav loader)      ──► Card 5 (producer core) ──► Card 6 (re-level) ──► Card 8 (armed runs)
        └────────► Card 3 (wire structs+gates) ──► Card 4 (C-line serialization) ──► Card 7 (consumer) ─┘
```
Cards 2 and 3 are independent after Card 1; Cards 4 and 5 are independent of
each other; Card 7 needs 3+4; Card 8 needs everything.

---

## Card 1 [OPUS, ~half session] — `network.apriori_source` config plumbing

New enum `NetworkAprioriSource { Sp3, BrdcRts }` in `GnssConfigTypes.hpp` (next
to the other network enums), member + Doxygen getter `networkAprioriSource()` in
`GnssConfig.hpp`, parse in the network section of
`src/core/config/GnssConfigLoad.cpp` (copy the `troposphere_apriori_source`
to_lower/string-match pattern at :1881-1891: `"sp3"` default, `"brdc_rts"`,
unknown → warn + sp3). Plus `network.apriori_clock_anchor_sigma_m` (double,
default 0.0 = auto per D6 — just parse+store here; consumption is Card 5).

Validation rules (all at load time, after the network section parses):
- `brdc_rts` + `estimate_sat_code_bias` + NOT `estimate_iono_sh` → **hard error**
  with message naming the fix (`network.estimate_iono_sh = true`); the GIM path
  is post-processing-only (conventions §1). (Grep Phase 3b in ApplicationNetwork
  to confirm wording matches its GIM/SH exclusivity comment.)
- `brdc_rts` + a realtime SSR source configured with a legacy (non-4076) dialect
  → **hard error** (conventions §3: producer corrections must be 4076/CoM). Find
  the RTB-5 dialect/mount config knob (grep `honor_ssr_reference_point` /
  legacy-dialect config in GnssConfigLoad) — if no per-mount dialect declaration
  exists yet, downgrade to a warn and note it for RTF-3.
- `service_mode` does NOT arm `apriori_source` (input-source choice is
  orthogonal to the toggle bundle) — add one line to the RTB-8 service_mode
  comment saying so.
Summary toggle-log line + `isLoggerEnabled` if user-visible per project
convention. Test: config round-trip (default sp3, brdc_rts parse, validation
error cases) in the existing config-test file (grep `test_codebias_network_config`
or the nearest network-config test).
Byte-identity: parse-only; default sp3 → no behavior change anywhere.

## Card 2 [OPUS, ~1 session] — RINEX-nav ephemeris loader → LiveEphemerisStore

The one genuinely missing subsystem. Build a RINEX 3/4 navigation-message parser
producing `BroadcastEphemeris` (study `include/products/BroadcastEphemerisTypes.hpp`
field-by-field, and mirror how the RTCM 1019/1042/1044/1046 decoders populate it —
grep `BroadcastEphemeris` construction in `src/core/rtcm/` for the authoritative
field conventions, INCLUDING the IGS-SSR IODE match convention (`ssrMatchIode`)
and the GAL 8-bit IOD mask (RTB-5)).

- Scope: GPS LNAV, GAL I/NAV (+F/NAV if trivially close), BDS D1/D2, QZSS.
  GLONASS deferred (D8) — skip R records cleanly.
- Placement: `include/products/RinexNavParser.hpp` + `src/core/products/
  RinexNavParser.cpp` (products/, not rinex/ — it produces product-side records;
  add to CMakeLists explicitly). Entry: `parse(path) → std::vector<BroadcastEphemeris>`;
  a thin `loadIntoLiveStore(path)` helper calls `LiveEphemerisStore::insert(eph,
  /*from_igs=*/true)` per record — insert() already handles freshest-toe +
  current/previous demotion.
- RINEX 4 nav (`> EPH` record framing) and RINEX 3 both — the bundled fixtures
  are `BRDC00IGS_R_*_MN.rnx` (RINEX 3, data/2024-129/products/); write the unit
  tests against a small in-repo fixture string, plus one integration test that
  parses the bundled file when present (skip-if-missing, like the Sp3 fixture
  test).
- Wire-in: NOTHING calls it yet (Card 5 does). This card is pure build + tests.
- Validation test (the important one): for a handful of sats/epochs, evaluate
  `computeSatellitePV`/`computeSatelliteClock` from the parsed nav and compare
  against the SP3/CLK products for the same epoch: |Δorbit| < 10 m, |Δclock| <
  10 m (broadcast-grade tolerances; catches unit/scale/week bugs decisively).
Byte-identity: new code, uncalled — trivially byte-identical.

## Card 3 [OPUS, ~1 session] — per-record clock IODE + orbit echo through the store and gates

- `SsrClockCorrection` (include/rtcm/RtcmTypes.hpp:97): add `int iode{-1}`
  (Doxygen: broadcast eph version the producer's C0 baseline used; -1 unknown),
  `int orbit_iod_ssr{-1}` + `GnssTime orbit_epoch{0,0.0}` (orbit coherence echo,
  conventions §2 — only meaningful in brdc_rts).
- Default change `iode{0}` → `iode{-1}` on `SsrOrbitCorrection` (:77) and
  `SsrCombinedCorrection` (:125). **Sweep EVERY construction site first**
  (decoders always set iode explicitly; tests may rely on 0 — fix tests to be
  explicit rather than keeping the unsafe default). This closes the RT0-1
  sibling nit. `iodeMatches(x, -1)` is permissive, so a site that formerly
  defaulted to 0 and was gated `iodeMatches(brdc, 0)` becomes permissive — enumerate
  affected sites and show each is a decoder (sets iode) or a test.
- `OrbitClockCorrection` (include/products/SsrCorrectionStore.hpp:124): add
  `clock_iode{-1}`; `getOrbitClock` merge per D2 (clock-only branch REPLACES the
  RT0-1 inherit-default comment with real propagation; combined branch copies).
- `GnssCorrections`: split the gate per D2 — clock application checks
  `iodeMatches(brdc_iode, clock_iode)`; keep the "SSR clock present but NOT
  applied" diagnostic on the new condition (grep that string).
- Tests: extend the RT0-1 suite in `tests/test_realtime_seams.cpp`
  (`RealtimeSsrIodeGateTest`): clock-only record with iode=N propagates
  clock_iode=N; gate drops clock when brdc=M≠N; permissive at -1 both sides;
  combined unchanged. Plus an iodeMatches truth-table touch-up in test_rtcm.cpp
  if defaults change any existing expectations.
Byte-identity: all new fields default -1 → every existing path (RTCM decodes,
closed loop, replay) produces identical gate outcomes; prove by the reduction
argument in the card notes + green suite.

## Card 4 [OPUS, ~half-1 session] — C-line serialization of the new fields

Follow the RTA-4 optional-trailing-value pattern exactly (grep how disc-counter
VALUES were added to C records in `src/core/products/NetworkProductIO.cpp` —
writer, reader, `emitToStore`, `makeReplayEmitter` self-check).
- Writer: append `iode` (and, when set, `orbit_iod_ssr` + orbit epoch) only when
  `iode >= 0`; never in sp3 mode.
- Reader: parse-if-present → `SsrClockCorrection.iode/orbit_iod_ssr/orbit_epoch`
  → store via the Card-3 plumbing. Replay must reproduce (fields are VALUES on
  the wire, not recomputed).
- Update the NetworkProductIO.hpp banner grammar comment.
- Tests in `tests/test_network_product_io.cpp`: round-trip with iode set;
  **twin-file byte-identity test** proving an sp3-mode emission (iode -1) writes
  byte-identical output to pre-card code (the established twin-file pattern).

## Card 5 [OPUS, 1-1.5 sessions] — producer brdc_rts core path

All gated on `networkAprioriSource() == BrdcRts`:
- **Nav load**: in runNetwork Phase 3 (after `product_parser_->loadProducts()`,
  ApplicationNetwork.cpp:527 area), call the Card-2 loader on
  `config_.getNavigationPath()`; hard error if the file is missing/empty or the
  store stays empty. LOG the loaded sat count per constellation.
- **Lift the SP3 hard error** (:535-539): in brdc_rts the requirement becomes
  "LiveEphemerisStore non-empty"; keep the SP3 error verbatim in sp3 mode. When
  SP3 IS also present in brdc_rts, WARN that precise orbits are loaded but the
  a-priori is broadcast (SP3 still shadows geometry via getSatellitePV node
  interpolation — decide NOTHING here; simply refuse: hard error "brdc_rts with
  SP3 products loaded is not a supported combination; remove product paths or
  use sp3 mode". Revisit when the RTS-orbit seam (D9) lands).
- **Phase gate suppression** (D1): `PppModel::setBroadcastPhaseGateSuppressed(true)`
  on producer station models at construction (grep where per-station PppModels
  are built); NOT on the closed-loop consumer's. In PppModel.cpp:717 the erase
  block becomes `if (geom.broadcast_only && !broadcast_phase_gate_suppressed_)`;
  the one-shot log gains a suppressed-variant so the operator sees the mode.
- **Anchor σ re-base** (D6): locate the per-sat a-priori clock anchor pseudo-obs
  σ (grep `anchor` in ApplicationNetwork.cpp; anchor_info doc :968-973 names it)
  and apply `apriori_clock_anchor_sigma_m` resolution (0 → 3.0 m in brdc_rts /
  unchanged in sp3; explicit wins). Do NOT touch initial_std.sat_clock (1000 m
  loose init is mode-independent).
- **IODE stamping**: thread the per-sat IODE from the emission path into
  `NetworkSatSolution` (new `int clock_iode{-1}`): `computeEmissionState`'s clock
  evaluation reports it in brdc_rts (LiveEphemerisStore::SatelliteClock.iode —
  check what GnssCorrections exposes; extend its out-params if needed), driver
  copies into the solution, emitter passes through to the C record (Card 4
  fields). In sp3 mode stays -1 everywhere.
- **`apriori_product_id`** (D10): at the :782-793 site, brdc_rts →
  `"brdc:" + navigation_path.filename()`.
- **W6/GIM**: Phase 3b already never loads a GIM when `estimate_iono_sh` is on;
  Card 1's validation makes the illegal combination unreachable — add a
  belt-and-braces assert/log in Phase 3b.
- Tests: producer-station gate suppression unit test (PppModel-level: broadcast_only
  obs keeps L keys when suppressed, drops when not); config-resolution test for
  the anchor σ; emission-state IODE stamp test if reachable at unit level (else
  document as Card-8 validation).
Byte-identity: every change behind the brdc_rts check; sp3 path textually
unchanged except the gate-suppression flag read (default false) — prove via the
reduction argument + suite.

## Card 6 [OPUS, ~1 session] — ephemeris cutover re-level (D3)

- Driver holds `std::map<std::string,int> last_emitted_iode_` (brdc_rts only).
  Each epoch, after the solve and before the emit block, compare each emitted
  sat's stamped IODE vs the map:
  - changed AND `computeSatelliteClock(sat, t, old_iode)` still evaluable →
    `Δ_eph = c·(brdc_new.bias − brdc_old.bias)` at this epoch; shift the KF
    sat-clock state mean by `−Δ_eph` (find the state index via the StateManager
    sat-clock key — grep StateKeySchema for the network sat-clock key maker);
    shift the D2 carried prior for that sat the same way IF the carry map holds
    it; c1 untouched; covariance untouched; log at info with Δ_eph in metres.
    NO disc bump.
  - changed and old version gone → per-sat forced disc bump via the existing
    machinery (grep `markSatDiscontinuity` / the RT0-5/RTA-2 forced-bump flags
    for the per-sat variant), log warn.
- **Ordering**: the shift must run BEFORE the epoch's C0 is read out for
  emission and BEFORE the clock-runaway/C1 passes (same region the RT0-5 card
  fought over — re-read its DONE note; put the re-level immediately after the
  solve state is final, before the C1 drift-stamp pass).
- Extract the delta computation as a pure helper
  (`NetworkEstimator::computeEphCutoverShift` or a free function) and unit-test
  it (two synthetic BroadcastEphemeris versions in a LiveEphemerisStore, known
  af0 difference → exact shift; missing previous → nullopt).
- **Snapshot (RTB-7)**: add `last_emitted_iode_` to NetworkStateSnapshot
  (serializeState/restoreState + version note) — a warm resume that lost the map
  would re-level against a stale baseline. Follow the m6 comment conventions at
  the snapshot write site.
- Clock-model interaction: with `network.clock_model` on, the 2-state [c0,c1]
  prior carries — shift c0 mean only (the osc map baselines: grep `clock_osc`;
  shift its level term identically or the jump test in the clock monitor will
  flag the re-level as a fault — check the monitor's jump test input and make
  the re-level invisible to it).
- Tests: pure-helper tests + a driver-level test if the closed-loop harness can
  inject an eph roll (if not feasible, document; Card 8 validates on data).
Byte-identity: map only populated in brdc_rts; sp3 untouched.

## Card 7 [OPUS, ~1 session] — consumer: pinned clock eval + echo verification + age skew

- **Pinned eval** (D7): at the consumer clock-application site in GnssCorrections
  (the RT0-1 gate region — grep "SSR clock present but NOT applied"), when
  `clock_iode >= 0` and the live store holds that version, evaluate the broadcast
  baseline `computeSatelliteClock(sat, t, clock_iode)`; when the pinned version
  is gone, the Card-3 gate drops the clock (existing stale-hold ladder rungs then
  govern). Confirm the post-processing path (live store empty, brdc_iode -1)
  is untouched.
- **Echo verification**: when a clock record carries `orbit_iod_ssr`/`orbit_epoch`
  and the consumer's applied SSR orbit for that sat has a different iod_ssr or
  content epoch older/newer than the echo by > a config budget
  (`network_consumer.orbit_clock_max_skew_s`, default 0 = check off), count
  per-sat mismatches (new counter alongside the RTA-13 attribute-mismatch
  counters) and (only when the budget is set) withhold the clock for that sat.
  Default OFF → byte-identical.
- **Age-skew rung**: extend the RTA-9 ladder doc + summary counters with the skew
  counter; no new behavior beyond the above.
- Tests: seams-level (store + gate) tests for pinned eval, echo mismatch counter,
  and skew-budget withhold; closed-loop regression must stay green (closed loop
  never sets the fields).

## Card 8 [OPUS+user, validation session] — armed brdc_rts runs

- Config: clone the network producer config used for the armed V-runs; set
  `network.apriori_source = brdc_rts`, no SP3/CLK product paths, navigation_path
  auto-resolved (BRDC fixture), `service_mode` on, closed-loop consumer on.
- Cadence per [[feedback_test_cadence_smoke_first]]: 10-min smoke → 1 h.
- Pass criteria (recorded signatures, NOT byte-identity):
  1. Producer emits (phase NOT erased): per-station phase-obs counts > 0; epochs
     solve; σ̂0 EWMA stabilizes near sp3-mode levels.
  2. C records carry iode for G/E/C sats; reconstructed clock
     (brdc(iode) + C0) at IODE-roll epochs is continuous to < 5 cm
     (the Card-6 signature; find rolls via the re-level info logs).
  3. Disc counters: zero clock-disc bumps attributable to eph rolls (re-level
     working); any bump maps to a genuine handover/fault event.
  4. Closed-loop consumer converges; position vs the sp3-mode baseline degrades
     gracefully (expect dm-level worse — broadcast orbit error rides in the
     clocks per D9; record the actual number as the RTF-3-era benchmark).
  5. Restart mid-run (RTB-7 snapshot) resumes without spurious re-levels
     (Card 6 snapshot field).
- Deliverable: numbers + anomalies into the memory queue file; open a follow-up
  card list (expected: RTS-orbit seam activation, conditioning/q retune per
  conventions §1 "cold-start conditioning and the D2 carry q get re-tuned in the
  RTF-1 card's validation").

---

## Explicitly out of scope (do not build in these cards)

- Live RTS-orbit transport into the producer (RTF-3 territory; D9 seam only).
- RTCM-4076 encoder / Tier-2 wire (conventions §4 — no card until an external
  rover exists).
- GLONASS eph anchoring (D8 → glonass_enablement_plan.md).
- Producer-side datum re-leveling across pivot handover (RTF-5 — different
  mechanism; do not conflate with the Card-6 EPH re-level).
- Consumer AR changes (RTF-7).
