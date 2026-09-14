# CC — the continuity half — the consumer's declared-step carry and warm start, landed default-OFF

**STATUS**

- **id:** CC
- **topic:** consumer
- **status:** closed
- **summary:** the continuity half — the consumer's declared-step carry and warm start, landed default-OFF
- **dates in the text:** 2026-09-14
- **knobs / posture:** `network.closed_loop.pb_disc_carry`, `processing.ssr_phase_bias_disc_carry`, `realtime.state_snapshot_path`, `network.closed_loop.state_snapshot`, `network.emit_combined_sigma`
- **hashes named in the text** (commit / exe / byte-cert): `0f510024`, `87a33bf0`, `ef2b134`, `3f421edb`, `7476f0a0`, `545b8bdd2b638231`, `8ba709ddf9191c8a`, `e0d234ee` … (+2 more in the text)
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_event_harness.md` — 24.4

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.4 Card CC — the continuity half, built regardless of §24.3's answer (worker brief `brief_cc.md`)

**(A) Carry the float ambiguity across a DECLARED phase-bias step — `network.closed_loop.pb_disc_carry`** (derived into the
consumer engine as `processing.ssr_phase_bias_disc_carry`; default false ⇒ byte-identical).  A declared step is a
re-realization common to every consumer of that satellite: the phase row is `predicted = … + ssr_bias_term + amb_term`
with both signs +1 (`MeasurementModel.cpp:1608 / 1642 / 1655`), so when the applied bias moves by Δ = PB_new − PB_old the
float ambiguity in the new datum is EXACTLY `amb − Δ` — a translation of one state, covariance untouched, no re-seed.
Mechanism at `resetOnPhaseBiasDisc`: the engine records the bias value it APPLIED at the last update per key (the same
gated/max-age read the model uses); on a counter change with a recorded value it shifts (`StateManager::shiftAmbiguity`,
new, value-only) and logs `carried {} {} by {:+.3f} m`; first sighting, no recorded value, NL/WL basis, or a BSSD consumer ⇒
the existing reset, each counted; the A5 freshness flip (applied ↔ not applied, Δ = applied_now − applied_prev with 0 for
"not applied") is carried by the same bookkeeping and counted separately.  Counters `pb_disc_carried / pb_disc_reset_*`
at run end; the RT snapshot does not carry the last-applied map (v1 format unchanged), so after a restore the first bump
per key falls back to reset — the conservative direction, stated.  The consumer's own slip QC resets are untouched.

**(B) The consumer's WARM START in the harness.**  The engine already has the seam (`RealTimeEngineStateSnap.cpp`:
`realtime.state_snapshot_path`, RtStateSnapshot v1 = x, P, dynamic layout, disc-counter memory, the A5 applied maps;
age horizon 300 s; a fingerprint over the state-meaning config facts) — the closed-loop harness never armed it, which is
why every leg's consumer is cold.  Build: `network.closed_loop.state_snapshot: true` makes the harness write the consumer's
snapshot beside each network history copy with the SAME `.epNNNN` suffix and the same tow (`state_snapshot.txt.consumer.
epNNNN`); `scripts/event_leg.py` copies it into the leg (pristine + disposable, the seed.txt pattern) and sets
`realtime.state_snapshot_path` on the leg config with the write cadence parked, so the leg's consumer resumes at the seam;
a refused restore (age / fingerprint / layout) is logged and the leg's consumer cold-starts — a rider line says which.
The seed is re-written as **v9** (products must hash v8's `0f510024…`; the consumer snapshots present at every history
epoch).  Then the continuous consumer at the matched tows is the reference for every leg (consumer leg − seed at k, the
channel `ev_seam_grade.py` has always REPORTED and never gated because the consumer was cold).

**(C) The combined-σ contract** (M4, `network.emit_combined_sigma` + `closed_loop.consume_combined_sigma`, built and
validated on the 50-station golden in August, NOT armed on the field golden) is a config-only rung on the same ladder:
a refloated satellite's fresh phase-bias σ (0.47 m at the event epoch against 0.039 m) deweights its phase row 150× for
the filter window under the v1 URA + σ RSS the field consumer runs; the combined σ supersedes it.

**Gates (frozen).**  G0 unit: the shift preserves P exactly and the post-update position is unchanged to 1e-9 with vs
without a synthetic declared step; every reset-fallback branch; counter closure.  G1 byte-identity OFF: the certification
smoke (`_cert3_smoke_base`) reproduces `87a33bf0…` AND its `closed_loop_trace.csv` / `rt_engine.log` byte-for-byte; the
v9 seed's products hash v8's.  G2 (B): a null leg from v9 ep0604 whose consumer rider reads RESTORED, no consumer cold
start, consumer leg − seed at k = 1 within the snapshot's own re-linearization (report; cm expected) and flat to k = 60.
G3 (A) on the E3/G24 PIN wire from v9 (the declared G24 L5X re-realization at +10 that WTZZ observes): OFF resets G24 L5X
at the event, ON carries it (the log line with Δ), products byte-identical between the pair; consumer event − null over
+10..+40: ON's max |Δ3D| ≤ OFF's and ON's mean ≤ OFF's.  G4 (the twin, only after §24.3 has named a converging posture and
G2/G3 pass): the v8 recipe with that posture + (A) vs without — declared-step resets 29 → ≤ 2 (the first sightings), settled
p50 / rms not worse, and the instrument's own hour-to-hour p50 spread reported; then and only then §20.3's G-G is re-run on
the fixed slip posture with this consumer, and the four knobs are pinned or not on a bar the instrument can carry.

**§24.4 RESULTS — card CC: the carry is exact and neutral, the warm start works once the restore scope is right, and neither can be
graded on this wire's consumer (2026-09-14 ~04:40 local; worker on worktree-agent-a3344694b164c2e9f `ef2b134`, merged; exe
`F27E9CF9…`; harness `3f421edb…` (the consumer-snapshot additions only); seed v9; manager grade).**

**Build.**  (A) `network.closed_loop.pb_disc_carry` → `processing.ssr_phase_bias_disc_carry`; `StateManager::shiftAmbiguity`
(value-only); `PbDiscCarryLedger` (per-key last-applied value, counters `pb_disc_carried / pb_flip_carried / resets first_sight
/ basis / bssd / unknown_key`, closure at run end); the RtStateSnapshot v1 format unchanged (no value map — after a restore
the first bump per key resets, stated in the header).  (B) `network.closed_loop.state_snapshot`: the consumer's snapshot
written beside every network history copy with the copy's tow; `RealTimeEngine::writeStateSnapshotTo`, a RESTORED rider;
`event_leg.py` copies `state_snapshot.txt.consumer.epNNNN` into the leg (pristine `seed_consumer.txt` + disposable), sets
`realtime.state_snapshot_path` with the cadence parked, records both in leg_meta; `write-seed` arms the knob.  Suite 2083 /
282 / 0 failed (10 new `ConsumerDiscCarryTest` cases; 16 fixture-gated skips on the worker's empty `data/`).  **G1 PASS**:
`_cc_smoke_base` products `87a33bf0…`, consumer trace byte-identical (`7476f0a0…`), `rt_engine.log` identical.  **Seed v9**:
products `0f510024…` = v8, consumer trace = v8's, ep0604 fingerprint `545b8bdd2b638231`, consumer snapshots beside all 12
history copies (tow equal 12 / 12; consumer fingerprint `8ba709ddf9191c8a`).

**G2 — the restore takes, and the loader's default scope makes it useless.**  `cc_null_warm` (RESTORED at tow 277320, age
30 s, 29 states / 24 dynamic; products `e0d234ee…` = `cert_null_v8`).  Consumer leg − seed at matched tows, |Δ ENU|:

| restore posture | k = 1 | k = 11 | k = 30 | k = 60 | max / mean k 1..60 |
|---|---|---|---|---|---|
| `core` (loader default) | **14.43** | 4.28 | 2.12 | 1.21 | 14.43 / 2.61 — the leg then tracks §24.3's COLD trajectory to the digit |
| `full`, seam refloat ON | 0.83 | 6.89 | 3.31 | 2.53 | 6.89 / 2.99 — the refloat re-opens every ambiguity: worse than cold at k = 11 (18.8 m) |
| **`full`, seam refloat OFF** | **0.63** | 1.31 | 1.56 | 1.12 | **1.67 / 1.23** |

`core` restores the five core states and re-creates the 24 dynamic ones, so 48 code rows pull the position 9 m in one epoch;
`full` + `state_snapshot_seam_refloat: false` is the posture that resumes — and the 0.6–1.7 m it still sits from the seed's
consumer is the wire's own seam (clock contrast 0.15 m p50 and nine declared code-bias re-references at the seam epoch,
identical in every posture).  **Decision: the harness leg default becomes `full` + refloat OFF** (a harness default, not a
golden knob; recorded in the harness's default list).

**G3 — the carry does what it says; its consumer benefit is inside the instrument's noise.**  E3/G24 pin pair on v9:
products `adb75dc5…` byte-identical across OFF / CARRY / the fp2 leg.  OFF: `reset G24 L5X` at 277620 and `reset G06 L2W`
at 278250; CARRY: `carried G24 L5X by +0.776 m` and `carried G06 L2W by −12.795 m` (a 12.8 m declared step carried exactly),
no reset for G24, counters `events 2 → carried 2, resets 0, closure HOLDS`.  Consumer event − null over k 11..41: default
posture OFF max 3.380 / mean 2.400, CARRY 3.332 / 2.347 (PASS both bars; carry − off max 0.225, mean 0.093 m); warm posture
OFF 1.050 / 0.514, CARRY 1.054 / 0.513 (mean PASS, max by 4 mm the other way) — mm to cm, because the consumer's error is
wire-driven (§24.3 / §24.5) and the event − null curves are the pin posture's producer-side response common to both arms.

**Decision.**  (A) VALIDATED as exact and neutral (byte-identical OFF, counters closed, the shift lands to the millimetre);
**default OFF, not pinned** — an exact translation is strictly better than a code re-seed, but its benefit cannot be
measured on a wire that leaves a float user at metres, and the standing rule is validation before a pin; it is re-graded on
the §24.5 wire instrument's successor (a consumer-continuity read on a consistent wire) once §24.6's fix lands.  (B) LANDED
as the harness's consumer instrument: every leg from a v9-class seed carries a warm consumer; the `full` + refloat-OFF
default is set on main.  The G4 twin is NOT run: the §24.3 attribution has named no converging posture, because none exists
on this wire — the twin waits on the producer fix.  The pooled "consumer" bars of §20.3 / §21.21 / §23.2 stay suspended.

**§24.7 CERTIFICATION of main after card CC (2026-09-14 ~05:10 local; certification worker on main's own tree at `73cbbab`).**  Release and
Debug rebuilt first attempt (596 s / 274 s); Debug suite **2083 cases / 282 suites / 2082 passed / 0 failed / 1 documented skip**
(`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`; 84.6 s); Release exe `3E768AD2…` (copy `gnss_processor_cert2.exe`); smoke
`_cert4_smoke_base` (knobs OFF) `products.txt` `87a33bf0…` = cert3 and `closed_loop_trace.csv` byte-identical, no unknown-knob line, no
CC-B counter line; the warm null leg on main's exe (`cert4_null_warm`, v9 ep0604) products `e0d234ee…` = `cert_null_v8`, consumer
RESTORED (scope full, 29 states / 24 dynamic, age 30 s), leg − seed 0.6305 / 1.311 / 1.564 / 1.119 m at k = 1 / 11 / 30 / 60 (its consumer
trace byte-identical to the worker's `cc_null_warm_full_norefloat`).  One cross-exe note: the v8 and v9 `ep0604` snapshots differ by nine
bytes (the header write time and the URA quality-ramp counter 598 → 604, read only under `warmup_ura_floor_m > 0`; both seeds' products
`0f510024…`).  The per-row residual trace (§24.6) merged after this certification is trace-only; its own smoke pair (`_rows_smoke_base` /
`_rows_smoke_on`, both `87a33bf0…`) was run by its worker on the same code.

**§24.3 RESULTS (2026-09-14 ~01:20 local; the five legs, products byte-identical `e0d234ee…` in every arm; manager read with
`scratchpad/ci_consumer_read.py`).**  The consumer's own modeling is NOT the defect, and neither the iono supply nor the
network clock alone is:

| arm | k = 1 | k = 11 | k = 30 | k = 60 | p50 3D k 30–60 | \|up\| | north | east | slope m/ep |
|---|---|---|---|---|---|---|---|---|---|
| L0 as golden (FULL) | 3.96 | 16.13 | 11.37 | 7.18 | 8.44 | 5.98 | −5.92 | +0.79 | −0.137 |
| L1 no iono supply | 4.25 | 15.34 | 10.74 | 7.60 | 8.37 | 4.49 | −6.90 | +1.62 | −0.102 |
| L2 no network clock (bias-only) | 9.39 | 4.93 | 6.13 | 6.44 | 6.52 | 5.30 | −2.08 | +3.26 | +0.005 |
| L3 neither (CODE clock + network biases) | 6.21 | 6.65 | 8.16 | 7.47 | 8.24 | 6.61 | −2.81 | +3.88 | −0.029 |
| **L4 no products (plain PPP on CODE SP3/CLK/.BIA)** | **1.48** | **1.32** | **0.28** | **0.25** | **0.25** | **0.17** | −0.17 | +0.08 | 0.000 |

The same 6–8 GPS satellites (15° mask), the same filter, the same 30 minutes: on CODE's products the cold float converges to
0.25 m 3D / 0.17 m up by k = 30 and stays there; on ANY combination of the network's products it sits at 6–8 m at k = 60
(FULL mode first walks to 16 m by k = 10 — its ZWD state runs 0.07 → −1.37 → +3.41 m over eleven epochs while the frozen
`troposphere_q 1e-9` then holds whatever it latched; the continuous seed's consumer holds ZWD ≈ 0.6 m for six hours
against a physical ~0.15 m, which is the height bias of §24.1 in another coordinate).  L3 is the decisive row: CODE's clock
with the network's phase and code biases is as bad as FULL — so the damage is in the BIAS wire or in how the consumer
applies it, and a per-satellite constant cannot be the reason (a float ambiguity absorbs any constant).  Either the
network's biases, taken with a clock, differ from CODE's by something that MOVES within an arc at the metre level, or the
consumer's application of the network products is defective at the metre level — `project_closedloop_consumer_vertical_bug`
concluded the latter in August ("the defect is CONSUMER-side application; products are sound in isolation") and it was never
fixed.  The two are separated without a consumer, by the products themselves (§24.5).

**Corrections to §24.1's reading.**  The consumer is a healthy float PPP user (L4); its wander on the network wire is a
TRUE statement about the wire AS APPLIED, not an instrument artifact — but the bar problem stands as written: a wire that
leaves a float user at 6–8 m makes every consumer figure a random walk, and the resets are not the channel.  The
consumer-side bars stay suspended; the replacement instrument is §24.5.

