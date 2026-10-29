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
- The seam A/B certifies TRAVERSAL, not the reactions. The verbatim verifier that covered
  them (`scripts/rt_seam_verify.py`) was retired on 2026-10-17; a reaction the arms do not
  drive needs a test of its own when its seam body is edited.

**Current posture (2026-09-14)** — unchanged by [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md), which is a
producer program; the RT consumer inherits whatever RF-3's wire contract declares.

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [INAV-ODD](../cards/realtime/INAV-ODD-PAGE.md) | live E23 poisoning: SBF I/NAV odd-page data read 6 bits late (orbit off up to ~300 m); fixed, tested on the receiver's GALNav | closed | 2026-10-23 |
| [EPH-JUMP](../cards/realtime/EPH-JUMP-RESEED.md) | re-seed a satellite whose broadcast record change moves its range > 10 m: BUILT default OFF; 6 h armed replay byte-identical, healthy changes <= 0.83 m; ungraded on a real jump | closed | 2026-10-23 |
| [SSR-PB-HDR](../cards/realtime/SSR-PB-HEADER.md) | SSR phase-bias messages read with the header's two consistency bits per satellite: every block after the first misaligned, the live coverage ramp and resets were artifacts; fixed, unit-tested; live after the fix 99.9 % coverage, 0 resets (28 before) | closed | 2026-10-29 |

**Standing documents** (still at their original paths, indexed not moved):

- [rt_seam_decomposition.md](../rt_seam_decomposition.md)
- [rt_service_conventions.md](../rt_service_conventions.md)
- [rt_service_operations.md](../rt_service_operations.md)

---

Back to [INDEX.md](../INDEX.md).
