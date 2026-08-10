# SPEC — RTF-3 multi-station streaming front-end (increment 1, landed 2026-08-01)

Card: RTF-3 (RT service queue). Goal: replace the network producer's batch
epoch source (per-station RINEX → `MultiRinexSync` → synced vector) with live
per-station streams, so the producer can run as a service. This increment
lands the full streaming *mechanics*; see §5 for what remains open on the
card and the field-blocking triage.

## 1. What landed

| Piece | Where |
|---|---|
| Green carve-out: epoch-loop body → `processSyncedEpoch(se)` lambda; batch loop + streaming pump both drive it | `src/core/app/ApplicationNetwork.cpp` (`LoopSignal` Next/Stop; 7 top-level `continue` + 1 `break` mapped 1:1) |
| Per-station transport descriptors | `StationConfig::transport` (`StationTransportConfig`: ntrip / tcp / file) in `include/config/GnssConfigTypes.hpp`; parsed in `GnssConfigLoadNetwork.cpp` |
| `network.streaming` section (default OFF) | `NetworkStreamingConfig` + loader (clamps gated on the master; inert-arming refusal when zero stations carry transports) |
| Wait-budget aligner + bounded epoch queue | `realtime/NetworkStreamAligner.{hpp,cpp}` (`NetworkStreamAligner`, `SyncedEpochQueue`) — pure, clock-injected, transport-free |
| Per-station decode lanes + ingest thread | `realtime/NetworkStreamFrontEnd.{hpp,cpp}` — one NtripClient/TcpClient/file + `RtcmStreamReader` + persistent `RtcmParser` per lane |
| Station identity at the framer boundary | `NetworkStreamFrontEnd::tagAndGroup` stamps `RinexObservation::receiver_id` = roster name the moment a frame decodes; per-lane parser = per-stream RTCM time continuity |
| RTB-3 reuse | ONE station-keyed `LockTimeTracker` across lanes (`network.streaming.msm_lli_from_locktime`) |
| Streaming pump in `runNetwork` | `network.streaming.enable` routes the epoch source; OFF ⇒ original batch iteration, structurally byte-identical |
| Tests (17) | `tests/test_network_streaming.cpp` — aligner dispatch semantics, queue backpressure, identity/LLI tagging, config contract |

## 2. Dispatch semantics (the decisions that matter)

- **Deadline-only dispatch.** An epoch leaves at `first_arrival + wait_budget_s`
  (default 0.7 s) with whoever arrived. There is deliberately NO
  "all-lanes-present" early path: a lane counts present after its FIRST frame
  while its remaining constellation frames (MSM is one frame per constellation
  per tow) may still be in flight — early dispatch would tear epochs at
  exactly the multi-constellation boundary.
- **Merge-append per station.** Same-station batches for one epoch concatenate;
  replace semantics would silently keep only the last constellation.
- **Head-of-line monotonicity.** Buckets dispatch strictly in time order; a
  younger expired bucket waits behind an older in-budget one. Late arrivals
  for a dispatched (or evicted) epoch are counted and dropped, never
  retro-merged.
- **Epoch-granular backpressure.** The solve queue is bounded in WHOLE epochs
  (drop-oldest). Bytes are never dropped anywhere — the byte-granular
  alternative desyncs the framer (the documented NtripClient trap).
- **Datum semantics.** The datum station's time tag is the epoch's
  representative time (batch walks the reference schedule; same contract).
  `require_datum_station` (default on) drops pivot-less epochs at the aligner
  instead of spending queue slots on known PASS-A skips.
- **Aux content on obs lanes.** Broadcast eph → `LiveEphemerisStore` (mutex-
  guarded, designed for stream-thread pushes). SSR content is counted and
  DROPPED with a one-shot log — the producer estimates its own corrections.

## 3. Run-end telemetry (invariant #4 — counters, not clean logs)

Aligner: dispatched (full/partial), dropped (min-stations / no-datum / late /
stale-bucket), batch merges. Queue: overflow drops. Per lane: frames, obs,
nav records, SSR dropped, framer discards, live-at-end. All logged by
`logRunEndSummary` after the pump ends. "0 dispatched" is the loud dead-wiring
signal; the loader refuses an armed front-end with zero transports outright.

## 4. Byte-identity audit (OFF path)

- Carve-out: same statements, same order, same objects; `continue`/`break`
  mapped 1:1 to lambda returns (verified exit-by-exit).
- `streaming_on=false` ⇒ census condition and epoch iteration are the
  original code; new TUs are never constructed.
- Config: new keys parse-only; clamps gated on the master (OFF configs
  round-trip unclamped — locked by test `OffPathValuesPassThroughUnclamped`).
- Full suite green post-change: 1044 passed / 1 known fixture-gated skip.

## 5. Open on the card — deferral triage (invariant #3)

| Deferral | Triage | Basis |
|---|---|---|
| Header-free station factory (StationMetadata seam; SIF coords) | **Field-blocking for the no-files service posture; INERT for replay/validation** | v1 station build still reads roster `rinex_path` for header metadata (antenna type, GLONASS slots, `PppModel(const RinexParser&)` coupling). Any run where the files exist (every current fixture/replay) is unaffected. |
| End-to-end ON validation (2-lane file-replay smoke → live NTRIP hour) | **Field-blocking for arming ON** | The unit layer is validated; the toggle must not be armed in a graded run until the replay smoke passes (staged cadence: 10-ep smoke first). |
| CB-ADM census under streaming | **Closed by refusal** | Census would classify over metadata files, not live epochs — skipped LOUDLY, legacy per-epoch gate engages (`ctx.admission.active=false`). |
| Signal handling / graceful SIGINT | **Operational, not estimation** | Session bounds: `processing.max_duration_s` (content time, honored by the body → `Stop`) or stream end. Live supervision = docs/rt_service_operations.md. |
| GLONASS FCN live refresh from 1020 | **INERT** | Network GLONASS is independently blocked (est. collapse at 305 sta); slots still come from RINEX headers. |
| Teardown "synced epochs" count under streaming | **Cosmetic** | Teardown still receives the file-derived vector for its summary denominators. |
| `RealtimeStream` consolidation (front-end `LaneByteSource` duplicates the driver-local class) | **Hygiene** | Flagged as a spun-off task; consolidating mid-card would churn the validated single-rover RT path. |

## 6. How to arm (replay smoke shape)

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

Roster `rinex_path` files must still exist (v1 metadata constraint, §5).
Perf context: 305-sta all-on is 27.7 s/ep — 1 Hz live is credible at ~50
stations for the clock+PB scope (559 ms/ep Release measured), which is the
design point the wait-budget defaults assume.
