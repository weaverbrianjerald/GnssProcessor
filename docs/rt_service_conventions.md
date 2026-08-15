# RT service root conventions (RTF-1 / RTF-2 freeze + RTF-4 protocol decision)

Status: **§1–§5 FROZEN** (Fable, 2026-07-02) — the convention decisions the RTF cards inherit; deviations require
editing this file first.  Scope: the self-sufficient real-time PPP-RTK service — the network producer estimates
satellite clocks / phase biases / code biases (later atmosphere); the ONLY external real-time product is orbits.
**§6 onward is the program record** (D1–D10, Cards 1–8 outcomes, the Card-8 armed validation, the RTF-3 streaming
increment, the open RTF-4…RTF-9 briefs): history and status, NOT frozen convention.  Section numbers are
**append-only** — code cites §1 by number and `D6` by label.  Every `runs/<name>` path below resolves under
`D:\GnssProcessor_archive\2026-08-12\runs\`, not the working tree.

## 1. Producer a-priori orbit/clock source (RTF-1)

Two modes, selected by `network.apriori_source`:

- **`brdc_rts`** (primary service mode): broadcast ephemeris (LiveEphemerisStore, fed from an RTCM nav stream or a
  RINEX-nav loader for replay parity) plus external IGS-SSR (4076) orbit corrections ingested with
  `corrections_accept = orbit_only` (RTA-8).  The per-satellite a-priori clock is the broadcast polynomial; the
  estimable clock state absorbs broadcast-clock error.
- **`sp3`** (current behavior, default): resolved SP3(+CLK) files.  With predicted ultra-rapids (RTB-4) still a
  legitimate low-infrastructure mode; the product is datum-locked to the file identity.

Consequences frozen here:

- The `broadcast_only` phase-drop gate in PppModel is **suppressed on producer station models in `brdc_rts` mode**.
  The metre-level broadcast clock error is exactly what the estimable clock state absorbs; erasing phase there is
  wrong-for-producer.  User-mode behavior unchanged.
  > ⚠ **Superseded wording, corrected in place because §1 is frozen.**  This clause originally read "suppressed when
  > the config estimates satellite clocks (network producer mode)" — design intent, not the live mechanism.  D1 made
  > it an explicit switch keyed on the MODE, not on clock estimation: `ApplicationNetworkSetup.cpp:588`
  > `st.model->setBroadcastPhaseGateSuppressed(brdc_rts)`.  An **sp3-mode network producer estimates satellite
  > clocks and does NOT suppress.**  §6 D1 governs.
- Clock anchor prior re-bases to broadcast grade in `brdc_rts`: σ ~3 m (config default; the <100 m binding gate
  still admits it).  Cold-start conditioning and the D2 carry q get re-tuned in the RTF-1 card's validation.
- The W6 code-bias iono constraint in `brdc_rts` is SH-VTEC co-estimation (`estimate_iono_sh`); the GIM file path is
  post-processing-only.
- Frame consistency comes from the shared code path: producer and user both resolve satellite state through
  Geometry/GnssCorrections against the same store types.

## 2. Clock product convention (RTF-2)

- **Emitted C0 is a delta against broadcast(IODE) + the applied RTS orbit** (IGS-SSR convention) in `brdc_rts`;
  against the SP3/CLK identity in `sp3` (unchanged).
- **Per-record ephemeris anchor:** `SsrClockCorrection` gains an `iode` field (int, -1 = unknown), stamped by the
  producer from the eph version its baseline used, serialized as a new optional C-line field, propagated through
  `getOrbitClock` (replacing the clock-only inherit-default), consumed by the existing `iodeMatches`/`pickForSsr`
  machinery.  The `iode{-1}` sentinel extends to `SsrOrbitCorrection` and `SsrCombinedCorrection` (closes the
  verified RT0-1 sibling nit).
- **Ephemeris cutover = re-level, not reset:** when the producer's broadcast eph rolls to a new IODE, the producer
  re-levels that satellite's C0 to the new baseline (it holds current/previous in LiveEphemerisStore) so the emitted
  stream is continuous, and stamps the new IODE.  The per-sat disc bump is the fallback ONLY when re-leveling is
  impossible (missing previous version).  Disc counters remain fault/datum signals — RTF-5 philosophy: continuity by
  construction, discs for faults.
- **Orbit coherence echo:** in `brdc_rts` each emitted clock record also carries the RTS orbit reference it was
  estimated against (IOD-SSR + orbit content epoch, optional C-line fields), so the consumer can verify it applies
  the same orbit update.  Age skew between orbit and clock gets its own consumer staleness budget (extends RTA-9).
- **`apriori_product_id` becomes a live identity string:** `"brdc+rts:<provider-or-mount>"` or
  `"sp3:<basename>[+clk:<basename>]"`.  Existing checkContract semantics (compare only when the consumer declares).

## 3. Reference point (RTF-1/2, interacts with RTB-5)

The service standardizes on **CoM end-to-end**: SP3+ATX (CoM) or IGS-SSR 4076 (CoM).  Legacy APC-referenced SSR
mountpoints are supported for the USER engine only via the RTB-5 tag + `honor_ssr_reference_point`; the PRODUCER's
corrections stream must be 4076/CoM.  The contract's `oref` field (RTA-12) becomes dynamic accordingly.

> ⚠ **Superseded design intent:** this section originally said config validation *rejects* legacy-dialect mounts for
> `apriori_source = brdc_rts`.  It does not — the dialect is decoded from the message type at runtime and no
> per-mount dialect declaration exists in config, so the landed check is a **WARN**
> (`GnssConfigLoadNetwork.cpp:2542-2554`).  The hard refusal is owed to RTF-3.  See §6.1 rule 2.

## 4. Wire protocol (RTF-4 decision)

- **Tier 1 (now): the existing line protocol IS the wire format.**  With RTA-3/4/5/10 landed, the file records
  already satisfy the streaming requirements: absolute values + disc-counter VALUES per record, periodic S-contract
  re-broadcast, generation ID, per-epoch H heartbeat, flush-per-epoch, a tail reader with mid-file join.  Transport
  = the same byte stream over a TCP socket or an NTRIP caster mountpoint; one text line = one message;
  connect/resync handshake = current S line + latest H + next epoch block (identical to a mid-file tail join).
  ~160 kbps/connection at 1 Hz x 60 sats: acceptable for the server-to-server tier.
- **Tier 2 (deferred until external/standard rovers need it): RTCM/IGS-SSR 4076 encoder** for fan-out bandwidth,
  plus one proprietary extension message carrying {generation ID, clock disc counters, code-bias disc counters};
  phase-bias disc maps onto the standard 2-bit field with a max-one-bump-per-update-interval invariant (retro-bumps
  are edge-triggered; handovers are rare — holds by construction).  No implementation card until a consumer that
  cannot speak the line protocol exists.

## 5. Sequencing note

Tier-1 transport means RTF-3 (multi-station ingestion), RTF-5/6 (datum continuity) and closed-loop live testing
proceed WITHOUT any encoder work.  Implementation order stays: RTF-1+RTF-2 (one card, both sides of this
convention) → RTF-3 → live end-to-end → RTF-5/6 hardening.

## 6. Design decisions D1–D10 (RTF-1 + RTF-2 card decomposition)

From the RTF-1+RTF-2 plan (Fable, 2026-07-02).  **Do NOT re-litigate; deviations → edit §1–§5 first.**  The `D<n>`
labels are load-bearing — `include/config/GnssConfig.hpp:1655` cites **D6** by label.

- **D1 — Gate suppression is an explicit PppModel switch, not config inference.**
  `PppModel::setBroadcastPhaseGateSuppressed(bool)` (default false), called on PRODUCER station models only, when
  `apriori_source == brdc_rts`.  The closed-loop consumer's PppModel must NOT suppress (its broadcast-grade sats
  genuinely can't use phase; sats with network clocks aren't broadcast_only anyway).  Rationale: the same GnssConfig
  serves producer and consumer in closed loop — config inference cannot distinguish them.
- **D2 — `OrbitClockCorrection` gains a separate `clock_iode{-1}`.**  The single `iode` stays the ORBIT anchor.
  `getOrbitClock`: clock-only merge sets `clock_iode = SsrClockCorrection.iode` (orbit `iode` stays -1); combined
  sets `clock_iode = iode`; orbit+clock merge keeps orbit `iode` and the clock record's own `clock_iode`.
  GnssCorrections gates ORBIT application on `iodeMatches(brdc_iode, iode)` (unchanged) and CLOCK application on
  `iodeMatches(brdc_iode, clock_iode)` (new; permissive while clock_iode = -1 → all existing producers/replays
  byte-identical).
- **D3 — Ephemeris cutover re-level.**  The estimable per-sat clock state is Δδtˢ vs the a-priori (broadcast) clock.
  When the eph version the geometry uses rolls, the a-priori jumps by Δ_eph(t) = brdc_new(t) − brdc_old(t);
  **without action the filter absorbs Δ_eph as a transient**, and it trips the two-state NIS / clock-monitor jump
  test as a spurious per-sat fault.  Both versions are evaluable via
  `LiveEphemerisStore::computeSatelliteClock(sat, t, iode)` (previous retained), so emitted C0 stays
  continuous-in-reconstruction: the consumer computes brdc(new_iode) + C0.  **NO disc bump on a successful
  re-level.**  Fallback (previous version no longer held → Δ_eph incomputable): per-sat forced disc bump through the
  existing machinery.
  > ⚠ **SUPERSEDED DESIGN INTENT — mechanism only; the rationale and fallback rule above stand.**  The plan
  > specified a **post-solve KF state-mean shift** (`x[sat_clock] -= Δ_eph`, covariance untouched, c1 untouched, the
  > D2 carry shifted identically if a carried prior exists this epoch) off a driver map named `last_emitted_iode_`.
  > **That is not what landed** — §7 Card 6 has the live pre-solve carried-PRIOR shift and why post-solve was wrong.
  > Do not restore the plan text as an instruction.
- **D4 — Replay parity = RINEX-nav → LiveEphemerisStore.**  The nav file is parsed into `BroadcastEphemeris` records
  and `insert()`ed into the SAME store the RT path fills from RTCM — `liveFallback()`/`computeSatelliteClock` then
  serve replay and live identically ("frame consistency from the shared code path", §1).  **Do NOT build a parallel
  post-processing eph evaluator or populate SP3 `OrbitData` from nav.**
- **D5 — Wire extension follows the RTA-4 optional-trailing-field convention.**  New C-record fields (clock `iode`,
  orbit echo `orbit_iod_ssr` + orbit content epoch) are appended ONLY when set (iode ≥ 0); the reader tolerates
  their absence.  `sp3` never sets them → product files byte-identical.  Copy the RTA-4 disc-counter-VALUE pattern
  exactly (writer, reader, emitToStore, replay self-check).
- **D6 — Anchor σ knob**: `network.apriori_clock_anchor_sigma_m`, default 0.0 = auto (sp3 →
  `initial_std.sat_clock`, unchanged; brdc_rts → 3.0 m per §1).  Explicit positive value wins in both modes; the
  <100 m binding gate is unchanged.  Landed as `GnssConfig::resolveNetworkAprioriClockAnchorSigmaM()`
  (`GnssConfig.hpp:1681-1687`; the plan's pre-decomposition coordinate was `GnssConfig.hpp:1452-1458`).
- **D7 — Consumer applies the clock against the STAMPED eph version.**  On the live path clock application evaluates
  the broadcast baseline via `computeSatelliteClock(sat, t, clock_iode)` (pinned eval — the store's
  current/previous retention absorbs producer/consumer cutover skew); `iodeMatches(brdc_iode, clock_iode)` remains
  the drop gate when the pinned version is no longer held.  Mirrors `pickForSsr` for the orbit overlay.
- **D8 — GLONASS is OUT OF SCOPE.**  Producer stamps `clock_iode = -1` for R sats (permissive gate, today's
  behavior); GLONASS eph-anchor semantics belong to `docs/glonass_enablement_plan.md` (note there: `iodeMatches`
  permissive-on-(-1) masking is a known caveat).
- **D9 — SSR-orbit ingest into runNetwork replay = injection seam only.**  A `setSsrStore`-style seam on the network
  driver with an orbit-only accept filter (reuse the RTA-8 `ssrPushAction` machinery), so recorded/live RTS orbits
  can be attached later; live transport is RTF-3.  **brdc-only replay is the accepted validation mode: broadcast
  radial orbit error absorbs 1:1 into the estimated clocks — the product is still consistent** (the consumer applies
  the same brdc orbit), **just with the orbit error riding in the clock.**  The §8 criteria account for this.
- **D10 — `apriori_product_id`**: `brdc:<nav-file-basename>` in brdc_rts replay;
  `brdc+rts:<provider-or-mount>` once an RTS orbit source is attached (the seam carries an identity string); sp3
  unchanged.

### 6.1 Knobs, defaults, and the refusal rules

| knob | default | behaviour |
|---|---|---|
| `network.apriori_source` | `sp3` | enum `NetworkAprioriSource{Sp3,BrdcRts}`; unknown string → **warn + sp3** |
| `network.apriori_clock_anchor_sigma_m` | `0.0` = auto | sp3 → `initial_std.sat_clock`; brdc_rts → **3.0 m**; explicit positive wins in BOTH modes; **<100 m binding gate unchanged** |
| `processing.network_consumer.orbit_clock_max_skew_s` | `0.0` = count-only | Card 7 echo/age-skew budget (`GnssConfigTypes.hpp:1932`); counting is unconditional, only the *withhold* is budget-gated |
| `EstimationConfig.initial_std.sat_clock` | `1000.0 m` (`GnssConfigTypes.hpp:2865`) | the LOOSE per-sat clock init, mode-independent — **do NOT repurpose it as the brdc_rts anchor knob** (the 1000 m loose init stays; use `apriori_clock_anchor_sigma_m`).  It **is** the sp3-mode anchor σ: the a-priori clock anchor pseudo-observation carries information `1/initial_std.sat_clock²` (anchor-info trace doc, pre-decomposition `ApplicationNetwork.cpp:968-973`; the plan's stale struct coordinate was `GnssConfigTypes.hpp:1392`) |

Refusals, all fail-loud and never masked — **four landed, not the three this heading claimed until 2026-08-15**
(rule 1b post-dates the plan):

1. **Load time (hard error)** — `brdc_rts` + `estimate_sat_code_bias` + NOT `estimate_iono_sh`, naming the fix
   (`network.estimate_iono_sh = true`): the GIM path is post-processing-only (§1).
1b. **Load time (hard error)** — `brdc_rts` + `network.external_sat_code_bias`
   (`GnssConfigLoadNetwork.cpp:2532-2540`): the `.BIA` is a daily post-processing bias product the self-sufficient
   service does not have.
2. **Load time (WARN, not a refusal)** — `brdc_rts` + a realtime SSR source: producer corrections must be 4076/CoM
   (§3), but the dialect is decoded from the message type at runtime and no per-mount dialect declaration exists in
   config, so this **cannot be a load-time hard error yet** and ships as a warn
   (`GnssConfigLoadNetwork.cpp:2542-2554`).  Hardening is owed to RTF-3.
3. **Startup (hard error)** — `brdc_rts` with ANY precise product loaded:
   `getOrbitCount() > 0 || getClockCount() > 0` (`ApplicationNetworkSetup.cpp:363`).  An orbit-only check **misses a
   CLK-without-SP3 set**, where `interpolateClock` wins while geometry uses broadcast orbits — a *silent orbit/clock
   frame split* with the IODE stamp naming the broadcast eph, leaving the consumer's brdc(iode)+C0 metre-level wrong.

Also: **`service_mode` does NOT arm `apriori_source`** — input-source choice is orthogonal to the toggle bundle.

### 6.2 Byte-identity posture and the RT0-1 sibling-nit sweep

Every card is default-OFF and **byte-identical when `network.apriori_source = sp3`** (the default).  The new wire
fields all default −1, so every existing RTCM decode, closed loop and replay produces identical gate outcomes; the
reduction argument is written per card, not asserted from a green suite (§12).  The one non-trivial piece: the
default change **`iode{0}` → `iode{-1}`** on `SsrOrbitCorrection` and `SsrCombinedCorrection`.  **Sweep EVERY
construction site first** — `iodeMatches(x, −1)` is *permissive*, so a site that formerly defaulted to 0 and was
gated `iodeMatches(brdc, 0)` becomes permissive.  Enumerate the affected sites and show each is a decoder (which
sets `iode` explicitly) or a test (fix the test to be explicit rather than keeping the unsafe default).

### 6.3 Pointer discipline

`include/products/NetworkProductIO.hpp`'s banner (`NetworkProductIO.hpp:34`) is the **AUTHORITATIVE** C-record
grammar — `C,<sat>,<clock_c0_m>` plus the RTA-4 optional disc-counter VALUES, and the I/G/H/S records.  Re-read it
before touching serialization rather than trusting any transcription.  Solution-side struct:
`NetworkSatSolution.has_clock/clock_c0_m/clock_ura_m` (`NetworkProductEmitter.hpp:79-85`; the plan's pre-drift
coordinate was `NetworkProductEmitter.hpp:66-70`, `NetworkSolutionEpoch` at `:76`).  Wire structs:
`SsrClockCorrection` (`RtcmTypes.hpp:97`), merged `OrbitClockCorrection` (`SsrCorrectionStore.hpp:148`, `iode` at
`:157`, `clock_iode` at `:166`; the plan cited the pre-drift `SsrCorrectionStore.hpp:124` and
`SsrCorrectionStore.hpp:138-139`).  **The emitter clock channel has NO jump test** — clock disc bumps are *forced*
(datum handover) only, so an IODE re-level cannot trigger a spurious emitter bump; the whole disc interaction lives
in the driver.

## 7. Cards 1–8 — what landed, the two intentional deviations, three field-forced corrections

The RTF-1/RTF-2 card queue is **CLOSED + COMMITTED** (landed at/before `dee2824`).  Outcome record, not a plan.
**Plan line numbers are not reproduced bare** — `ApplicationNetwork.cpp` was decomposed by INC-0..12 from ~14.7k
lines to ~2.4k with bodies relocated into `filter/Network*` seams, so old coordinates are unresolvable or point at
different code.  Plan coordinates quoted below are marked pre-decomposition; otherwise grep the symbol names.

**Card 1 — `network.apriori_source` config plumbing.**  Enum + getter + parse + the §6.1 validation rules +
`apriori_clock_anchor_sigma_m`.  Parse-only; default sp3 → no behaviour change anywhere.  (Pre-decomposition
brdc_rts flag site: `ApplicationNetwork.cpp:480-481`.)

**Card 2 — RINEX-nav ephemeris loader → LiveEphemerisStore.**  The one genuinely missing subsystem — and the trap
that made it necessary is still live: **post-processing has NO broadcast-nav ephemeris path.**
`RinexParser::parse` detects `BRDC`/`_MN.rnx` and delegates to `ProductParser::parseKlobuchar` ONLY
(`RinexParser.cpp:213-233`) — Klobuchar coefficients, no orbit/clock records; a reader seeing RinexParser "already
handle" `_MN.rnx` would otherwise conclude nav records are parsed there.  Placement
`include/products/RinexNavParser.hpp` + `src/core/products/RinexNavParser.cpp` — **products/, not rinex/, because it
produces product-side records.**  Scope: GPS LNAV, GAL I/NAV (+F/NAV), BDS D1/D2, QZSS; **GLONASS skipped cleanly**
(D8).  Entry `parse(path) → std::vector<BroadcastEphemeris>`; RINEX 3 and RINEX 4 (`> EPH` framing) both.  Field
conventions mirror the RTCM 1019/1042/1044/1046 decoders, including the IGS-SSR IODE match convention
(`ssrMatchIode`) and the GAL 8-bit IOD mask (RTB-5).  *Validation tolerances (the decisive test):* evaluate
`computeSatellitePV` / `computeSatelliteClock` from the parsed nav against SP3/CLK **for the same epoch** —
**|Δorbit| < 10 m, |Δclock| < 10 m**; broadcast-grade bars, but they catch unit/scale/week bugs decisively.  Unit
tests run off a small in-repo fixture string; the integration test parses the bundled fixture **when present
(skip-if-missing, like the Sp3 fixture test)** — `BRDC00IGS_R_*_MN.rnx` (RINEX 3, `data/2024-129/products/`).
**Measured on those real BRDC/SP3/CLK fixtures: orbit 3.68 m, clock 0.65 m.**

**Card 3 — per-record clock IODE + orbit echo through the store and gates.**  D2 as specified:
`SsrClockCorrection` gains `iode{-1}` + `orbit_iod_ssr{-1}` + `orbit_epoch`; `OrbitClockCorrection` gains
`clock_iode{-1}`; the GnssCorrections gate splits.  Plus the §6.2 `iode{0}`→`iode{-1}` sweep.

**Card 4 — C-line serialization** of the new fields on the RTA-4 optional-trailing-value pattern, with the
twin-file byte-identity test proving an sp3-mode emission (iode −1) writes byte-identical output to pre-card code.

**Card 5 — producer brdc_rts core path.**  Nav load (pre-decomposition site `ApplicationNetwork.cpp:527` area;
**LOG the loaded sat count per constellation**); the SP3 hard error replaced by "LiveEphemerisStore non-empty"; D1
phase-gate suppression on producer station models; D6 anchor-σ resolution; IODE stamping; D10
`apriori_product_id`.  The gate mechanics D1 switches off: `broadcast_only` originates as a
`Geometry::computeSatelliteGeometry` out-param (`ModelGeometry.cpp:587-591`, set at `:882` from `broadcast_grade`,
which comes from `corrections_.getSatelliteClockCorrection(sat, emission_time, &broadcast_grade)` inside
`computeEmissionState`); the consumer end is `PppModel.cpp:699-732`, where the erase block **removes every key
starting with `'L'` from `observed`/`modeled`/`variances`/`zenith_variances`** and a one-shot log fires through the
`sats_phase_gated_` member (`PppModel.cpp:717`).  Landed switch: `PppModel::setBroadcastPhaseGateSuppressed` at
`PppModel.hpp:462`, flag `broadcast_phase_gate_suppressed_` at `:543` (the plan's pre-drift coordinate was
`PppModel.hpp:425-426`), erase guard `PppModel.cpp:709`, suppressed-variant log `:724`.

> **Intentional deviation #1 — the emit-site IODE stamp.**  Instead of threading the per-station emission-time IODE
> out of `computeEmissionState`, Card 5 queries `LiveEphemerisStore::getInstance().computeSatelliteClock(sat,
> se.time)` at the **emit site** — today `src/core/filter/NetworkSolPrep.cpp:167-174`, pre-decomposition
> `ApplicationNetwork.cpp:4254-4260`.  **Do NOT "finish the out-param plumbing"** — it would be dead code.  Justified
> on file: the broadcast clock is station-independent (one shared store) and toe granularity (hours) dwarfs the
> intra-epoch light-travel spread, so `pick()` lands on the version the emission path used.  *Residual risk (OWED
> adjudication, armed data only):* at an IODE-roll boundary epoch a station whose `emission_time` straddles the toe
> cutover could have modeled its C0 against version *N* while the emit-site query at `se.time` returns *N+1* — a
> 1-epoch stamp/baseline mismatch that is **unit-invisible**.  It matters because Card 6 diffs the stamped version
> against its baseline map, so **Cards 5 and 6 must agree on which version-selection defines "the version this epoch
> used"** or Card 6 fires a PHANTOM roll or MISSES a real one.  Judge whether the skew is (a) impossible, (b)
> harmless because the re-level absorbs it, or (c) a real mis-stamp.

> **Correction #1 (field-forced) — the CLK-without-SP3 hole.**  Card 5's original SP3-present refusal was orbit-only
> and missed the CLK-without-SP3 set → silent orbit/clock frame split.  **Fixed** by widening the guard to
> `getOrbitCount() > 0 || getClockCount() > 0` (§6.1 rule 3).

> **Correction #2 (field-forced, first Card-8 smoke) — the progressive nav feed.**  Card 5 **bulk-inserted the whole
> day's BRDC** into LiveEphemerisStore, which retains only **CURRENT + PREVIOUS per satellite**.  Each satellite
> then held its *last two records of the day*, the fit-window guard rejected everything at 05:00, and the producer
> emitted **`n_present = 0` for the entire run**.  The Card-2 integration test validated at 23:00 — the one hour
> where last-two-records works — so **a green suite hid a total producer outage.**  **Fix:** stage the parsed
> records sorted by GPS-scale `toc` and feed them to the store **progressively in the epoch loop** (replay parity,
> D4).  Live sites: staging + sort in `ApplicationNetworkSetup.cpp` ("Card 8 fix (replay parity, plan D4)"), the
> per-epoch feed cursor in `ApplicationNetwork.cpp` ("Card 8 (replay parity)"), availability via the one shared
> `navAvailabilityGpsSeconds` in `app/NetworkSetupResult.hpp` (BDS toc shifted BDT→GPS scale).  The toc proxy is
> conservative by up to one fit half-window — records roll slightly LATER than live, never prematurely.

**Card 6 — ephemeris-cutover re-level (D3).**  **LANDED MECHANISM:** `networkEphCutoverRelevel` in
`include/filter/NetworkRelevel.hpp` / `src/core/filter/NetworkRelevel.cpp`, relocated verbatim out of
`networkSolveGlue` on 2026-07-23.

> **Intentional deviation #2 — PRE-solve carried-PRIOR shift, and it DOES mutate the D2 carry.**  The re-level runs
> **AFTER the observability-gate `continue`** and **BEFORE the D2 / clock-osc prior injection and the solve**.  It
> mutates `ctx.carry.{relevel_iode, eph_cutover_disc_pending, clock_osc, prev_carry, prev_clk, local_carry,
> zwd_carry}` — per rolled sat: `clock_osc[sat].x0 -= Δ` (two-state offset), **every matching `CarryPrior` in
> `prev_carry` with `kind == SharedParamKind::Clock`** (the D2 scalar clock carry), `prev_clk[sat].first -= Δ`
> (clock-monitor jump baseline), and — under `carry_cpl_cond` — every `CoupledShared::s_prev` for that clock in
> `local_carry` and `zwd_carry` (else every coupling row's RHS is off by M·Δ_eph after a cutover, since the row's
> functional u = r + Mᵀs is frame-consistent only if ALL carried references move together).  Drift (c1) and the 2×2
> covariance are frame-invariant and untouched.
>
> **Why, and the two superseded instructions this replaces.**  The plan (D3) placed the shift *after* the solve,
> anchored to where Card 5 stamps `clock_iode`; the Card-6 brief went further, listing as its **"#1 trap"** that one
> must *"do NOT mutate `prev_carry` — an explicit mutation double-shifts."*  **Both are wrong about the live code
> and must not be re-asserted.**  Post-solve, `x_s` was already solved against the OLD-frame prior, so a post-hoc
> `x_s` shift is exact **only** when the prior dominates and **over-corrects** a data-dominated (loose) solve;
> shifting the PRIOR is exact **for any prior weight** and, being a deterministic function of the broadcast
> ephemeris alone, applies identically on emit / σ̂0-withhold / solve-fail.  No double-shift: a gate-withheld epoch
> never reaches the block, so `relevel_iode` and the carried state stay in lock-step; on the σ̂0/solve-fail paths
> the block DID run, the shift landed, and the skipped post-emit carry updates leave it in place.
>
> **The map is `relevel_iode`, not `last_emitted_iode_`.**  `include/app/NetworkRunContext.hpp` says why: *"the plan
> called this `last_emitted_iode_`; 'referenced-to' is the honest name since it advances on withheld epochs too."*
> The baseline advances **at the shift**, not deferred to emit like `prev_pivot` — **deferring it would re-detect
> the same roll next epoch and DOUBLE-shift.**

Δ helper: `LiveEphemerisStore::ephCutoverShiftMetres(sat, t, old_iode)` → `optional<double>` (pure; unit-tested in
`tests/test_rinex_nav_parser.cpp` — known af0 diff → exact metres, missing/unheld previous version → `nullopt`).
GLONASS (`'R'`) skipped (D8).  Fallback: `ctx.carry.eph_cutover_disc_pending.insert(sat)` — a forced per-sat
discontinuity landing at that sat's next actual emit (present/pending split like `ura_retro_sats`; exercised by
`tests/test_network_solprep_disc.cpp`).  Effectively unreachable in steady state (per-epoch checking makes two rolls
between consecutive checks impossible); a defensive belt for a snapshot gap / two-roll corner.  Events: `RELEVEL`
tier, `eph_cutover_relevel` (statistic = −Δ) / `eph_cutover_disc`.  **Clock-model interaction:** with
`network.clock_model` on the 2-state [c0,c1] prior carries, so the `clock_osc` level term must shift identically or
the clock monitor's jump test flags the re-level as a fault.  **Snapshot (RTB-7):** both `relevel_iode` and
`eph_cutover_disc_pending` are in `include/products/NetworkStateSnapshot.hpp` — a warm resume that lost the map
would re-level against a stale baseline.

**Card 7 — consumer: pinned clock eval + echo verification + age skew.**  D7 pinned eval — a *separate* clock-pin
distinct from the orbit anchor (conflating them silently defeats the card; the ORBIT overlay eval stays pinned to
`ssr->iode`, `ModelGeometry.cpp:1278-1287`, and must not be touched).  When the pinned version is gone the Card-3
gate drops the clock and the existing stale-hold ladder rungs govern; the post-processing path (live store empty,
`brdc_iode` -1) must be **confirmed untouched**.  Echo verification counts per-sat skews **unconditionally** with
the *withhold* gated on `orbit_clock_max_skew_s > 0`; the skew counter is cloned from the RTA-13
`bias_attr_mismatch_` pattern (`mutable` + locked, since `getSatelliteClockCorrection` is const); the withhold is
routed through the existing drop path, not a parallel return that skips the broadcast-grade bookkeeping.  Default
OFF → byte-identical.

**Card 8 — armed brdc_rts validation.**  §8.

### 7.1 Point-in-time validation record (Cards 1–5, 2026-07-02/05)

Evidence trail for the card closures; the suite has since grown (CLAUDE.md re-counted 1360 cases / 196 suites / 89
files on 2026-08-12).

- **Cards 1–3:** clean build; full suite **542 pass / 1 known skip / 0 fail** at the Card-3 tree state (Card-2
  fixture tolerances above).  An 8-dimension adversarial review + field-map cross-check against the RTCM decoders
  found **0 confirmed defects**.
- **Cards 4 & 5:** landed concurrently, 3-lens verified; suite **550 / 549 / 1 skip**.  Card 5's one
  CONFIRMED-moderate defect (the SP3/CLK-present refusal missing the CLK-without-SP3 set) fixed as Correction #1.
- **Open from Card 5 — anchor strength.**  Is **3.0 m** the right anchor, or effectively **inert against the
  data**?  It is **60× looser** than the σ = 0.05 that collapses cold-start rms **56 → 2.9 m**.  Armed-data
  question; unresolved.

## 8. Card 8 — armed brdc_rts validation (runbook + measured 2026-07-05 results)

Turnkey validation of Cards 5 + 6 + 7.  Scope: **producer + held-out closed-loop consumer** (criterion 4 is live —
`consume_network_clock` works under brdc_rts because the consumer evaluates the broadcast baseline pinned to the
stamped `clock_iode`).  The armed config was **RE-ARMED 2026-07-05 with the validated all-features network set**
(net305_allfeat em25 — phase leveling + slip detectors, RAIM, clock model/monitor/runaway, SH-VTEC iono,
prioritized phase datum, sum-to-zero clock datum, full product contract) per the Jul-4 audit: **never grade quality
on a stripped config.**

### 8.1 Artifacts

| File | What |
|---|---|
| `config/ppp_rtk_network_brdc_rts.json` | armed config (bundled 3-station roster + CIT100USA held-out consumer, 05:00 UTC start, 1 h) |
| `scripts/stage_brdc_only_root.ps1` | builds `data_brdc_rts/` (products minus SP3/CLK; obs+static junctioned) — gitignored |
| `scripts/check_card6_relevel.py` | judges criteria 2 & 3 from `network_events.csv` + `network_solution.csv`.  **A PROXY only** (emitted-C0 step vs logged shift); the absolute `brdc(clock_iode) + C0` reconstructor is an OPEN C++ tooling gap |
| `pyviz/gnssviz/product_file.py` | patched to parse the brdc_rts C-line `iode=` token (it was silently dropping clock rows) |

### 8.2 Run steps

1. **Confirm the tree is green** (concurrent sessions churn it): `git status`, a Release build via
   `scripts/vs2026_build.ps1 -Config x64-Release` (CLAUDE.md "Build"), then
   `gnss_tests.exe --gtest_filter=-Gpt3Test.*` — the `Gpt3Test` exit-1 is a **pre-existing cross-suite
   logger-registry artifact**, not a Card 5/6 regression.
2. `pwsh scripts/stage_brdc_only_root.ps1` (re-run only if `data/` changed).
3. **10-min smoke** at `max_duration_s: 600.0` on the Release exe.  **GATE STRINGS — the log MUST show** `product
   cache loaded — 0 orbits, 0 clocks` **and MUST NOT show** `precise products are loaded alongside apriori_source =
   brdc_rts`.  A refusal means an SP3/CLK is still on the discovery path — **check the prev/next-day products dirs.**
4. **1 h run:** restore `max_duration_s: 3600.0`, re-run.
5. **Judge:** `python scripts/check_card6_relevel.py <logdir>/network_events.csv <logdir>/network_solution.csv`

### 8.3 Pass criteria

1. **Producer emits phase** (not erased): per-station phase-obs counts > 0 (`network_trace.csv` `solved=1`,
   `n_phase_bias>0`); epochs solve; σ̂0 EWMA in `network_service_health.csv` near sp3-mode levels (not `-1`).
2. **Reconstructed clock continuous at rolls, < 5 cm** — `brdc(iode) + C0` at IODE-roll epochs (the Card-6
   signature; find rolls via the re-level info logs).  C records carry `iode` for G/E/C sats.
3. **Zero clock-disc bumps attributable to eph rolls**; any bump maps to a genuine handover/fault.  A few
   `eph_cutover_disc` forced fallbacks are acceptable (previous eph version evicted); a large count means
   current/previous retention is failing.
4. **Closed-loop consumer converges**; position vs the sp3-mode baseline degrades gracefully (dm-level expected —
   broadcast orbit error rides in the clocks per D9; record the number as the RTF-3-era benchmark).
5. *(optional)* **Restart mid-run** (RTB-7 snapshot) resumes without spurious re-levels.

### 8.4 RESULTS — 2026-07-05, smoke + 1 h run

Conditions: **3-station roster JPLM / P389 / GCGO**, **1 h from 05:00 UTC**, 2024-129, **Release**,
`config/ppp_rtk_network_brdc_rts.json`, evidence `runs/card8_brdc_rts/`.  The **blocking defect** found on the first
smoke and its fix are Correction #2 in §7.

| Criterion | Verdict | Evidence |
|---|---|---|
| 1. Producer emits phase | **PASS** | 3 stations, **10 sat clocks**, **72 phase biases/epoch**; **117/121 epochs emitted**; σ̂0 behaviour **IDENTICAL to the sp3 twin** |
| 2. Reconstruction continuous at rolls < 5 cm | **PASS (direct)** | all **200 C lines carry `iode=`**; **E08's five rolls: reconstruction step ≤ 3.5 cm** against a **0.16 cm median non-roll drift baseline**.  The judge's proxy was **INCONCLUSIVE** (no paired series), so a **direct C0-step-vs-logged-shift** check was used |
| 3. Zero disc bumps from clean re-levels | **PASS** | **26 re-levels**, including the **06:00 32-satellite GPS batch with Δ_eph up to 0.38 m** — **0 disc bumps, 0 forced fallbacks** |
| 4. Closed-loop consumer | **PASS (comparative)** | brdc_rts float median **2.84 m** vs sp3-twin **2.76 m** — degradation **≤ 0.1 m** |
| 5. Restart mid-run | **not exercised** | — |

**Interpretation of criterion 4 — the important read.**  The ~2.8 m vertical-dominant float error **and the 0 % fix
rate are COMMON to both modes**, i.e. they are the **OPEN closed-loop consumer bug**, not a brdc_rts effect — which
now has a cheap **3-station reproducer**: `runs/card8_brdc_rts/{run1h,sp3_twin}`.

**σ̂0 contamination window 05:38–05:50** (climbs **3 → 7.5**, **4 epochs withheld at the 6.0 ceiling**, **G02/G31
flagged**, recovery after) **reproduces IDENTICALLY in the sp3 twin** ⇒ data/geometry driven at 3-station scale
(**RAIM cannot isolate with 3 stations**), **NOT a brdc_rts effect**.  **Producer wall cost: 82 ms/epoch** (3
stations, serial, all features + closed loop).

### 8.5 Why 05:00 UTC — the run-window trap

2024-129 BRDC carries **3906 IODE rolls/day (GPS 429, GAL 2396, BDS 1081)**, but **GPS rolls cluster at ODD UTC
hours**.  The **05:00–06:00 window carries 185 rolls including a full 32-satellite GPS batch**; **a naive 00:00
window has only ~2 GPS rolls** and validates nothing.  Galileo rolls every ~10 min regardless.

### 8.6 Files to watch

- `network_events.csv` — `eph_cutover_relevel` (clean, Δ_eph in the `statistic` column) vs
  `eph_cutover_disc[_landed]` (forced fallback).  **The ONLY machine-readable roll-vs-fault signal.**
- spdlog log — `Network (Card 6): <sat> eph cutover IODE <old>-><new> — re-leveled ... by X m`.
- `network_solution.csv` — per-sat `value_m` (C0) + `disc_counter`.
- the emitted product (`network_products.txt`) — C lines carry `,iode=NN`.

### 8.7 Scaling to 50 / 305 stations

Stage a **SEPARATE** brdc-only root per date window:
`pwsh scripts/stage_brdc_only_root.ps1 -SrcRoot <root> -DstRoot <root>_brdc_rts -Days <YYYY-DOY...>`; copy the
config, repoint `files.internal_data_root` at that root, and swap `network.stations[]` for the larger roster —
**KEEP `apriori_source` / `service_mode` / `trace_events`**.  **Expect Galileo conditioning stress at 305** under
brdc_rts (metre-scale Δδtˢ against a 3 m anchor σ); that discovery plus a possible q/anchor retune is part of the
run, per §1.  The Card-8 deliverable also names **D9 RTS-orbit seam ACTIVATION** as an expected follow-up card
alongside that retune.

## 9. RTF-3 — multi-station streaming front-end (increment 1, LANDED 2026-08-01)

Goal: replace the network producer's batch epoch source (per-station RINEX → `MultiRinexSync` → synced vector) with
live per-station streams, so the producer can run as a service.  This increment landed the full streaming
**mechanics**; §9.5 is the open residue.

### 9.1 What landed

| Piece | Where |
|---|---|
| Green carve-out: epoch-loop body → `processSyncedEpoch(se)` lambda; batch loop + streaming pump both drive it | `src/core/app/ApplicationNetwork.cpp` (`LoopSignal` Next/Stop; **7 top-level `continue` + 1 `break` mapped 1:1**) |
| Per-station transport descriptors | `StationConfig::transport` (`StationTransportConfig`: ntrip / tcp / file) in `include/config/GnssConfigTypes.hpp`; parsed in `GnssConfigLoadNetwork.cpp` |
| `network.streaming` section (default OFF) | `NetworkStreamingConfig` + loader (clamps gated on the master; **inert-arming REFUSAL when zero stations carry transports**) |
| Wait-budget aligner + bounded epoch queue | `realtime/NetworkStreamAligner.{hpp,cpp}` (`NetworkStreamAligner`, `SyncedEpochQueue`) — **pure, clock-injected, transport-free** |
| Per-station decode lanes + ingest thread | `realtime/NetworkStreamFrontEnd.{hpp,cpp}` — one NtripClient/TcpClient/file + `RtcmStreamReader` + **PERSISTENT per-lane `RtcmParser`** |
| Station identity at the framer boundary | `NetworkStreamFrontEnd::tagAndGroup` stamps `RinexObservation::receiver_id` = roster name **the moment a frame decodes**; per-lane parser = per-stream RTCM time continuity |
| RTB-3 reuse | **ONE** station-keyed `LockTimeTracker` across lanes (`network.streaming.msm_lli_from_locktime`) |
| Streaming pump in `runNetwork` | `network.streaming.enable` routes the epoch source; OFF ⇒ original batch iteration, structurally byte-identical |
| Tests (17) | `tests/test_network_streaming.cpp` — aligner dispatch semantics, queue backpressure, identity/LLI tagging, config contract |

### 9.2 Dispatch semantics (the decisions that matter, and their reasons)

- **Deadline-ONLY dispatch.**  An epoch leaves at `first_arrival + wait_budget_s` (default **0.7 s**) with whoever
  arrived.  There is **deliberately NO "all-lanes-present" early path**: a lane counts present after its FIRST frame
  while its remaining constellation frames are still in flight (**MSM is one frame per constellation per tow**), so
  early dispatch would tear epochs **exactly at the multi-constellation boundary**.
- **Merge-APPEND per station.**  Same-station batches for one epoch concatenate; replace semantics would silently
  keep only the last constellation.
- **Head-of-line monotonicity.**  Buckets dispatch strictly in time order; a younger expired bucket waits behind an
  older in-budget one.  Late arrivals for a dispatched (or evicted) epoch are counted and dropped, **never
  retro-merged**.
- **Epoch-granular backpressure.**  The solve queue is bounded in **WHOLE EPOCHS** (drop-oldest).  **BYTES ARE NEVER
  DROPPED ANYWHERE** — the byte-granular alternative desyncs the framer (the documented NtripClient trap).
- **Datum semantics.**  The datum station's time tag is the epoch's representative time (same contract as batch
  walking the reference schedule).  `require_datum_station` (default **on**) drops pivot-less epochs at the aligner
  rather than spending queue slots on known PASS-A skips.
- **Aux content on obs lanes.**  Broadcast eph → `LiveEphemerisStore` (mutex-guarded, designed for stream-thread
  pushes).  SSR content is counted and **DROPPED** with a one-shot log — the producer estimates its own corrections.

### 9.3 Run-end telemetry (invariant #4 — counters, not clean logs)

Aligner: dispatched (full/partial), dropped (min-stations / no-datum / late / stale-bucket), batch merges.  Queue:
overflow drops.  Per lane: frames, obs, nav records, SSR dropped, framer discards, live-at-end.  All logged by
`logRunEndSummary` after the pump ends.  **"0 dispatched" is the loud dead-wiring signal**; the loader refuses an
armed front-end with zero transports outright.

### 9.4 Byte-identity audit (OFF path)

- Carve-out: same statements, same order, same objects; `continue`/`break` mapped 1:1 to lambda returns (**verified
  exit-by-exit**).
- `streaming_on=false` ⇒ the census condition and epoch iteration are the original code; the new TUs are **never
  constructed**.
- Config: new keys parse-only; clamps gated on the master (OFF configs round-trip unclamped — locked by the test
  `OffPathValuesPassThroughUnclamped`).
- Full suite green post-change: **1044 passed / 1 known fixture-gated skip
  (`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`) — the 2026-08-01 gate-time measurement.**  (The suite has
  since grown; CLAUDE.md re-counted 1360 / 196 / 89 on 2026-08-12.  Do not overwrite the gate figure — it is the
  evidence for *this* cut.)
- **Caveat:** only the **BATCH** path is byte-identical.  **Streaming output is arrival-timing-dependent** and never
  will be.

### 9.5 Open on the card — deferral triage (invariant #3)

| Deferral | Triage | Basis |
|---|---|---|
| Header-free station factory (StationMetadata seam; SIF coords) | **FIELD-BLOCKING for the no-files service posture; INERT for replay/validation** | v1 station build still reads roster `rinex_path` for header metadata (antenna type, GLONASS slots, the `PppModel(const RinexParser&)` coupling).  Any run where the files exist (every current fixture/replay) is unaffected. |
| End-to-end ON validation (2-lane file-replay smoke → live NTRIP hour) | **FIELD-BLOCKING for arming ON** | The unit layer is validated; the toggle **must not be armed in a graded run** until the replay smoke passes (staged cadence: 10-ep smoke first). |
| CB-ADM census under streaming | **CLOSED BY REFUSAL** | Census would classify over metadata files, not live epochs — skipped **LOUDLY**, legacy per-epoch gate engages (`ctx.admission.active=false`). |
| Signal handling / graceful SIGINT | **Operational, not estimation** | Session bounds: `processing.max_duration_s` (content time, honored by the body → `Stop`) or stream end.  Live supervision = `docs/rt_service_operations.md`. |
| GLONASS FCN live refresh from 1020 | **INERT** | Network GLONASS is independently blocked (estimator collapse at 305 sta); slots still come from RINEX headers. |
| Teardown "synced epochs" count under streaming | **COSMETIC** | Teardown still receives the file-derived vector for its summary denominators. |
| `RealtimeStream` ↔ front-end `LaneByteSource` consolidation | **HYGIENE** | Spun off deliberately: consolidating mid-card would churn the validated single-rover RT path. |

### 9.6 How to arm (replay smoke shape)

```json
"network": {
  "streaming": { "enable": true, "wait_budget_s": 0.5, "epoch_interval_s": 1.0 },
  "stations": [
    { "name": "AAAA", "position_ecef": [...],
      "transport": { "input_type": "file", "input_path": "captures/aaaa.rtcm3" } },
    { "name": "BBBB", "position_ecef": [...],
      "transport": { "input_type": "ntrip", "host": "igs-ip.net", "port": 2101,
                     "mountpoint": "BBBB00XXX0", "username": "...", "password": "..." } }
  ]
}
```

Roster `rinex_path` files must **STILL EXIST** (v1 metadata constraint, §9.5).  **Perf design point:**
305-sta all-on is **27.7 s/epoch**; **1 Hz live is credible at ~50 stations** for the clock+PB scope
(**559 ms/epoch Release measured**) — the assumption behind the wait-budget defaults.

## 10. Open cards RTF-4 … RTF-9

Colour = the Opus-first economics of the card, as briefed for a fresh Opus 4.8 Ultracode session (🟡 = run
Opus-first, budget real Fable review; 🔴 = Fable-first design or defer).  **No card is pure-green: every one has a
claim build + tests + byte-identity cannot reach — that residue is the whole point of the colour.**  RTF-3's colour
is spent (§9); its 🔴 rationale and landmines survive in §10.6 because they still describe the un-landed remainder.

| Card | Colour | Opus-first economics | The un-self-verifiable claim |
|---|---|---|---|
| **RTF-4** wire protocol | 🟡 *(Tier-1 only)* | Good for Tier 1 | handshake recovers full datum state on reconnect |
| **RTF-6** pivot-station fallback | 🟡 | green long-tier / yellow short-tier | unpinned-gauge conditioning through an outage |
| **RTF-9** MDB integrity | 🟡 *(analytic only)* | Good for the floor | honest per-parameter MDB (DIA separability) |
| **RTF-5** datum re-level / handover | 🔴 | **Poor** — design unmade research | "per-signal bias constants" **estimability-false as stated** |
| **RTF-7** network AR for RT | 🔴 | **Poor** — substrate ~80 % unbuilt; needs live frac_cyc | RT policy tunable only against a running service |
| **RTF-8** atmosphere product | 🔴 | **Poor** — needs dense data not on disk | STEC datum alignment; global-only test = false green |

### 10.1 RTF-4 — wire protocol 🟡 (Tier 1 ONLY)

The existing self-describing line format **IS** the wire format (§4); `NetworkProductTail` already **IS** the
receive-side decoder.  Build: (1) refactor `NetworkProductTail::Impl` so its byte source is an abstraction (file
read vs socket recv) — **reuse the ONE parser**, do not duplicate the torn-line / epoch-completion / contract-heal
logic; (2) a producer socket/caster sink that **TEES** `NetworkProductWriter`'s bytes; (3) connect handshake = push
**S line → latest H → next E block** (mechanically identical to the mid-file tail join).  Default-OFF;
byte-identical when off.  **Tier 2 = do NOT build**: frozen-deferred until a standard-SSR rover exists, and no
`RtcmBitWriter` exists — confirm scope with the user before writing any 4076 encoder.

**Traps.**  Building Tier 2 speculatively; **reimplementing the parser** instead of refactoring the byte source
(**torn lines split ANYWHERE on a socket**, more aggressively than file buffering); **wrong handshake order** —
H-before-S leaves a joiner mis-aligned, **SILENTLY**; editing the writer's bytes instead of teeing (breaks
twin-file/replay tests).

**Open review question.**  Does **{S + H + next E} reconstruct FULL datum state**, or is there stream state a joiner
needs that the push omits?  Disc-counter adoption needs `serialize_disc_counters` ON (`NetworkProductIO.hpp:269` and
`:345`; the brief's pre-drift coordinate was `NetworkProductIO.hpp:370-389`), and the contract is **blind until the
next S**.  **Armed data:** a live socket run across a real reconnect.

### 10.2 RTF-5 — producer datum re-level across handover 🔴 (Fable-first design)

**Why red.**  The card's "per-signal bias constants" is **ESTIMABILITY-FALSE AS STATED**: the bias gauge pins at the
pivot **RECEIVER**, not the pivot **SATELLITE**, so a satellite-clock handover leaves the bias block **invariant in
exact arithmetic**.  Opus would produce a plausible transform Fable must re-derive.  **Path:** one Fable design
session pinning the transform algebra + the RTF-5/RTF-6 boundary + the value/state/carry coherence decision (an
RTF-5 design doc with the estimability proof); **then**, if scoped to clock-common-mode only, a bounded Opus card
mirroring Card-6's helper+shift, plus an armed validation session.  Interim tier — **RTA-1 sticky pivot + RTA-2
scoped handover — already ships.**

### 10.3 RTF-6 — receiver-datum (pivot-station) fallback 🟡

> **THE TWO-PIVOTS DISAMBIGUATION.**  RTF-6 is the pivot **STATION** (`clock_datum_station`,
> `reference_receiver_id`), whose change is **GLOBAL** — it maps onto `markDatumChange()` with **NO argument**
> (`force_all_bump_`) plus a **re-minted generation id**, **NOT** the constellation-scoped satellite-pivot machinery
> (that is RTF-5).  **Conflating them UNDER-SIGNALS the break.**

**Session A — the LONG-tier contract event (bankable, self-validatable, do first).**  A receiver-datum switch must,
**in one atomic emit**: (a) `markDatumChange()` [global]; (b) **re-mint `generation_id`** so the consumer's
gid-change global reset fires — today `RealTimeEngine::reactToGenerationChange` (`RealTimeEngineSsrBreaks.cpp:31`;
the brief's pre-seam-cut coordinate was `RealTimeEngine.cpp:1606-1641`); (c) update `reference_receiver_id` **and
force an S re-broadcast that epoch**; (d) **advance the datum-station baseline AT the emit site** (deferred
discipline).  Add `last_emitted_datum_station`; default-OFF `network.datum_fallback`.  Fully self-validatable
(detector unit test + twin-file + round-trip to the consumer reset path).

**Session B — the fallback DRIVER: an ESTIMABILITY operation, not a flag flip (~1.5 sessions).**  The mechanism is
an **ordered roster + runtime datum-station reassignment**: make `is_pivot` a **per-epoch role chosen from an
ordered roster with N-epoch hysteresis**.  Today `is_pivot` is **construction-time** and `stations.front() ==
pivot` is **assumed** across PASS-A selection, the pin fold, the aborts and the carry-skip (pre-decomposition
coordinates, orientation only, in `ApplicationNetwork.cpp`: pin fold `:2914`, aborts `:3141` / `3152` / `3174`,
carry-skip `:3021-3028`).  A real swap must **re-pin the new datum's ambiguities, un-pin the old, and rederive
`shared_phase_keys`** — with **N-epoch hysteresis**, or a 1-epoch gap causes a **reset storm**.

**Short tier stays Fable-gated:** ride the phase-bias gauge on the D2 carry with no receiver pinning for a
1..M-epoch gap plus a gauge-drift monitor.  **Opus can build the unpinned-gauge mechanism but CANNOT certify it
stays conditioned** — that is Fable + armed data (gauge drift vs threshold + clean-swap convergence under an
induced outage).

**Traps.**  Constellation-scoping a receiver-datum change; swapping the pivot by flipping `is_pivot` (misses the
re-pin sites → singular/drifting solve); no hysteresis; advancing the baseline before the emit; forgetting to
snapshot the new state.

### 10.4 RTF-7 — network AR for real-time 🔴 (defer)

**Why red.**  The batch producer-AR substrate it must extend is **~80 % UNBUILT** — only Phase-0 `IlsCore` + Phase-1
conditional-cov exist; **no DD formation, no cross-station covariance, no re-pin/re-solve, no fixing code**.  The RT
deltas (sticky-leveling thresholds, anti-churn disc policy, wrong-fix tripwire) are **undecided research, tunable
only against frac_cyc evidence from a running service that does not exist**.  The two Opus-safe slices
(cross-station cov; compute+log NL-direct DD/`ilsFix`) deliver **diagnostics, not the card**.  Path: defer until an
armed run yields the frac_cyc/convergence evidence, then Fable-first the RT policy.

### 10.5 RTF-8 — atmosphere product 🔴 (Fable-first + data acquisition in parallel)

**Why red.**  Needs a **DENSE REGIONAL CLUSTER** (~30–50 km, ≥ 5–6 stations, one interior held out) that is **not on
disk and not auto-fetchable** — **a global-set validation is a FALSE GREEN.**  The **STEC datum alignment** (each
station's slant iono carries a free per-(station, constellation) rx-DCB level that must align with the emitted OSB
S-basis) is **deep estimability work**.  Path: one Fable design session freezing atmosphere semantics and rating
**ZWD-first (fully estimable) SEPARATELY from STEC**, plus **dense-data acquisition kicked off in parallel**
(external lead time), then a ~1.5–2 session ZWD-first Opus implementation.  Consumer seams already exist:
`applyGimIonoUpdate`, the RT ZWD pseudo-obs, the B9 `zwd_carry` harvest.

### 10.6 RTF-3 remainder — the landmine list

Increment 1 landed (§9); these are the traps for whoever takes the rest.

- **"The loop already consumes only `se`" is the most misleading line in the card** — it closes over **dozens of
  persistent accumulators**.  (Increment 1's carve-out threaded them; anything further must too.)  The pre-INC epoch
  loop this describes was ~3850 lines (`ApplicationNetwork.cpp:2167-6017`).
- **MSM does carry a station_id** (`RtcmMsmDecoder.cpp:404`) **but DF003 is CASTER-UNRELIABLE** ⇒ identity must come
  from the **stream boundary** (what `tagAndGroup` does).
- **`PppModel`'s ctor hard-requires `RinexParser&`** (GLONASS FCN seeded from the header), making the **header-free
  seam the deepest cut** (§9.5 row 1).
- **Backpressure lives on the assembled-epoch queue**, not in `NtripClient`'s byte callback.
- **Streaming output is ARRIVAL-TIMING-DEPENDENT ⇒ NOT byte-identical**; only the batch path is.

### 10.7 RTF-9 — MDB-anchored integrity 🟡 (analytic floor ONLY)

Buildable half: the analytic Baarda internal-reliability floor, which needs **no** live statistics — it is
closed-form:

**MDBᵢ = σᵢ · δ₀ / √rᵢ,  with δ₀ = `dataSnoopingCritical()` + 0.8416**

Reuse the user-side Baarda machinery (`KalmanFilter.cpp:1723-1789`, **`kRedFloor = 1e-6`**, today spelled
`kBaardaRedundancyFloor` and shared via `filter/BaardaReliability.hpp`); add the MDB floor as a **SECOND `std::max`
at the URA emit site**, next to the warmup floor (today `NetworkSolPrep.cpp:453-454`; the brief's
pre-decomposition coordinate was `ApplicationNetwork.cpp:4496-4519`) — structurally identical, byte-identical when
off.  Default-OFF `network.emit_mdb_ura`.  Consumer: fold the floored per-sat URA into the FFR AR gate
(`AmbiguityResolver.cpp` / `IlsCore.cpp`) behind `network_consumer.fold_ura_into_ar`; keep it thin — **the true
protection-level statistic is Fable/armed territory.**  α/β calibration and integrity *validation* are armed-data
follow-ups.

**The three semantic traps (all also the review focus):**

1. **`rowRedundancy` is PER-STATION-ROW**, but the MDB is on the **EMITTED CLOCK PARAMETER**, fused across all
   stations — **reading one row OVER-OPTIMIZES the floor.**
2. **The σ in δ₀·σ/√r is the A-PRIORI MEASUREMENT σ**, not the posterior clock-state σ already in the URA.
   Conflating them (multiplying `√scov` by δ₀) is **dimensionally plausible but SEMANTICALLY WRONG** — input- vs
   output-referred; it needs the clock-to-input sensitivity.
3. **The analytic single-outlier MDB is OPTIMISTIC exactly in the detectable-but-not-identifiable lock-in regime the
   card exists to bound** (the ep42/G15 mechanism) — Opus cannot know its own floor is too low.

Plus: **δ₀ likely needs Bonferroni/Šidák** over 50–300 stations × ~60 sats; and **do not rename a URA floor a
"protection level."**  **Self-validation checklist:** build + suite + twin-file byte-identity off + pure-helper test
of `δ₀·σ/√r` + **URA-monotonicity**.

## 11. Explicitly out of scope

- **Live RTS-orbit transport into the producer** — RTF-3 territory; **D9 seam only**.
- **RTCM-4076 encoder / Tier-2 wire** (§4) — **no card until an external rover exists.**
- **GLONASS eph anchoring** (D8) → `docs/glonass_enablement_plan.md`.
- **Producer-side datum re-leveling across pivot handover (RTF-5) — a DIFFERENT mechanism; do NOT conflate it with
  the Card-6 EPH re-level.**
- **Consumer AR changes** — RTF-7.

## 12. Owed items, and the meta-rules that earned their place

| Owed / OPEN item | Owner / what settles it |
|---|---|
| The absolute `brdc(clock_iode) + C0` reconstructor | **No C++ tool exists**; `scripts/check_card6_relevel.py` is only a PROXY.  Tooling card. |
| RTB-7 restart drill (Card-8 criterion 5) | **Never run.**  Needs a mid-run restart under `network.state_snapshot_path`, asserting no spurious re-levels (`relevel_iode` is snapshotted). |
| Card-5 emit-site stamp adjudication (§7, deviation #1) | **Armed data only** — an IODE-roll boundary epoch with a straddling `emission_time`. |
| Card-5 anchor strength: is 3.0 m right or inert? | Armed data (§7.1). |
| brdc_rts scale runs 50 → 305 | §8.7; **expect Galileo conditioning stress**, possible q/anchor retune, plus D9 seam activation. |
| RTF-3 end-to-end ON validation + the header-free station factory | §9.5 — both **FIELD-BLOCKING**; the toggle must not be armed in a graded run until the 2-lane replay smoke passes. |
| The closed-loop consumer's ~2.8 m vertical-dominant float / 0 % fix | §8.4 — **mode-independent, still open**; cheap 3-station reproducer on file. |

**What only an armed run can settle** — every yellow card's un-self-verifiable claim funnels to the same place:
Card 5's emit-site equivalence and its 3.0 m conditioning; Card 6's re-level continuity + zero-disc-bump; Card 7's
live divergence branch; RTF-6's gauge drift; RTF-9's realized-bias-respects-floor.  **Sequence the yellow
implementations toward one armed run and let it adjudicate them together.**  Config trap when you do: **brdc_rts
hard-refuses if `getOrbitCount() > 0` (or `getClockCount() > 0`)**, so the armed config must have **no SP3/CLK paths
AND PathResolver must not auto-glob or download one for the day**.

**Two meta-rules this program paid for:**

1. **A green suite is NOT correctness for any estimability claim.**  Say so; do not imply otherwise.  The underlying
   coverage fact: **the network suite has no numerical regression — closed-loop is the only proxy.**  (Correction #2
   in §7 is the proof: a green suite hid a total producer outage.)
2. **Byte-identity is proven by a WRITTEN REDUCTION ARGUMENT, not asserted from a green suite.**  Show every guard.

**Lineage.**  §6–§8 absorb `docs/attic/rtf12_brdc_rts_plan.md` (D1–D10, Cards 1–8 plan) and
`docs/attic/rtf_card8_runbook.md` (the armed validation).  §10 and the §7 deviations absorb
`docs/attic/rtf_opus_first_handoff.md`.  §9 absorbs `docs/attic/SPEC_rtf3_streaming_frontend.md`.  Sibling docs NOT
part of this record: `docs/rt_service_operations.md` (shipped operator surfaces) and
`docs/rt_seam_decomposition.md` (the `RealTimeEngine.cpp` source-refactor gate report).
