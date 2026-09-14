# `realtime` — The real-time engine — streaming ingest, SSR consumption, service

The streaming path: `Application::runRealTime` → `RealTimeEngine`, its seam
decomposition, the NTRIP / RTCM / SBF ingest, SSR consumption and the producer-datum
reactions, and the service work queue.

**Conventions and invariants**

- Post-processing-only features (the smoother, `gnss_rate_s`) must guard against
  `AppMode::RealTime`.
- New code goes in the seam matching its concern (`RealTimeEngineAdmit`, `…Prepare`,
  `…SlipQc`, `…SsrBreaks`, `…Estimate`, `…Ingest`, `…Init`); admission seams are the only
  ones that may end an epoch.
- `RtEpochContext` owns nothing that outlives the epoch — a value that must survive is a
  member.
- Order is load-bearing in `SsrBreaks`: `reactToGenerationChange` applies the union
  globally and must precede the per-signal handlers.
- The seam A/B certifies TRAVERSAL, not the reactions — re-run `scripts/rt_seam_verify.py`
  after touching a seam body.

**Current posture (2026-09-14)** — unchanged by [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md), which is a
producer program; the RT consumer inherits whatever RF-3's wire contract declares.

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

_No split cards in this topic._

**Standing documents** (still at their original paths, indexed not moved):

- [rt_seam_decomposition.md](../rt_seam_decomposition.md)
- [rt_service_conventions.md](../rt_service_conventions.md)
- [rt_service_operations.md](../rt_service_operations.md)

---

Back to [INDEX.md](../INDEX.md).
