# RT service root conventions (RTF-1 / RTF-2 freeze + RTF-4 protocol decision)

Status: **FROZEN** (Fable, 2026-07-02). These are the convention decisions the RTF
implementation cards inherit. Scope: the self-sufficient real-time PPP-RTK service —
network producer estimates satellite clocks / phase biases / code biases (later
atmosphere); the ONLY external real-time product is orbits. Downstream cards
(RTF-1..6 implementation, RTF-4 tier-2 encoder) implement against this document;
deviations require editing this file first.

---

## 1. Producer a-priori orbit/clock source (RTF-1)

Two supported modes, selected by a new `network.apriori_source`:

- **`brdc_rts`** (primary service mode): broadcast ephemeris (LiveEphemerisStore,
  fed from an RTCM nav stream or a RINEX-nav loader for replay parity) plus external
  IGS-SSR (4076) orbit corrections ingested with `corrections_accept = orbit_only`
  (RTA-8). The producer's per-satellite a-priori clock is the broadcast polynomial;
  the estimable clock state absorbs broadcast-clock error.
- **`sp3`** (current behavior, default): resolved SP3(+CLK) files. With predicted
  ultra-rapids (RTB-4) this remains a legitimate low-infrastructure service mode;
  the product is then datum-locked to the file identity.

Consequences frozen here:
- The `broadcast_only` phase-drop gate in PppModel is **suppressed when the config
  estimates satellite clocks** (network producer mode). The metre-level broadcast
  clock error is exactly what the estimable clock state absorbs; erasing phase there
  is wrong-for-producer. User-mode behavior is unchanged.
- Clock anchor prior re-bases to broadcast grade in `brdc_rts` mode: sigma ~3 m
  (config default; the <100 m binding gate still admits it). Cold-start conditioning
  and the D2 carry q get re-tuned in the RTF-1 card's validation, not here.
- The W6 code-bias iono constraint in `brdc_rts` mode is SH-VTEC co-estimation
  (`estimate_iono_sh`); the GIM file path is post-processing-only.
- Frame consistency comes from the shared code path: producer and user both resolve
  satellite state through Geometry/GnssCorrections against the same store types.

## 2. Clock product convention (RTF-2)

- **The emitted C0 is a delta against broadcast(IODE) + the applied RTS orbit**
  (IGS-SSR convention) in `brdc_rts` mode; against the SP3/CLK identity in `sp3`
  mode (today's convention, unchanged).
- **Per-record ephemeris anchor:** `SsrClockCorrection` gains an `iode` field
  (int, -1 = unknown), stamped by the producer from the eph version its baseline
  used, serialized as a new optional C-line field, propagated through
  `getOrbitClock` (replacing the clock-only inherit-default), and consumed by the
  existing `iodeMatches`/`pickForSsr` machinery. The `iode{-1}` sentinel is
  extended to `SsrOrbitCorrection` and `SsrCombinedCorrection` (closes the
  verified RT0-1 sibling nit).
- **Ephemeris cutover = re-level, not reset:** when the producer's broadcast eph
  rolls to a new IODE for a satellite, the producer re-levels that satellite's C0
  to the new baseline (it holds both eph versions — current/previous in
  LiveEphemerisStore) so the emitted stream is continuous, and stamps the new IODE.
  The per-sat disc bump is the fallback ONLY when re-leveling is impossible
  (missing previous version). Disc counters remain fault/datum signals, consistent
  with the RTF-5 philosophy (continuity by construction, discs for faults).
- **Orbit coherence echo:** in `brdc_rts` mode each emitted clock record also
  carries the RTS orbit reference it was estimated against (IOD-SSR + orbit
  content epoch, optional C-line fields), so the consumer can verify it applies
  the same orbit update. Age skew between orbit and clock gets its own staleness
  budget on the consumer (extends the RTA-9 ladder).
- **`apriori_product_id` becomes a live identity string:**
  `"brdc+rts:<provider-or-mount>"` or `"sp3:<basename>[+clk:<basename>]"`. The
  existing checkContract semantics (compare only when consumer declares) apply.

## 3. Reference point (RTF-1/2, interacts with RTB-5)

The service standardizes on **CoM end-to-end**: SP3+ATX (CoM) or IGS-SSR 4076
(CoM). Legacy APC-referenced SSR mountpoints are supported for the USER engine only
via the RTB-5 tag + `honor_ssr_reference_point`; the PRODUCER's corrections stream
must be 4076/CoM (config validation rejects legacy-dialect mounts for
`network.apriori_source = brdc_rts`). The contract's `oref` field (RTA-12)
becomes dynamic accordingly.

## 4. Wire protocol (RTF-4 decision)

- **Tier 1 (now): the existing line protocol IS the wire format.** With RTA-3/4/5/10
  landed, the file records already satisfy the streaming requirements: absolute
  values + disc-counter VALUES per record, periodic S-contract re-broadcast,
  generation ID, per-epoch H heartbeat, flush-per-epoch, and a tail reader with
  mid-file join. Transport = the same byte stream over a TCP socket or an NTRIP
  caster mountpoint; one text line = one message; connect/resync handshake = send
  current S line + latest H + next epoch block (identical to a mid-file tail join).
  ~160 kbps/connection at 1 Hz x 60 sats: acceptable for the server-to-server tier.
- **Tier 2 (deferred until external/standard rovers need it): RTCM/IGS-SSR 4076
  encoder** for fan-out bandwidth, plus one proprietary extension message carrying
  {generation ID, clock disc counters, code-bias disc counters}; phase-bias disc
  maps onto the standard 2-bit field with a max-one-bump-per-update-interval
  invariant (retro-bumps are edge-triggered; handovers are rare — holds by
  construction). No implementation card is scheduled until a consumer that cannot
  speak the line protocol exists.

## 5. Sequencing note

Tier-1 transport means RTF-3 (multi-station ingestion), RTF-5/6 (datum
continuity), and closed-loop live testing proceed WITHOUT any encoder work. The
implementation order stays: RTF-1+RTF-2 (one card, both sides of this convention)
-> RTF-3 -> live end-to-end -> RTF-5/6 hardening.
