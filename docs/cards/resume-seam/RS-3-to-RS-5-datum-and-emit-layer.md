# RS-3 … RS-5 — both datums continue at the seam, the phase-bias level is the EMIT layer, and the seam closes on the wire

**STATUS**

- **id:** RS-3 … RS-5
- **topic:** resume-seam
- **status:** closed
- **summary:** both datums continue at the seam, the phase-bias level is the EMIT layer, and the seam closes on the wire
- **dates in the text:** 2026-09-12 → 2026-09-13
- **knobs / posture:** `network.trace_sat_bias_states`, `network.resume_pb_seam_mark`, `network.serialize_disc_counters`, `network.state_snapshot_emit_capture`, `network.resume_emit_restore`
- **hashes named in the text** (commit / exe / byte-cert): `794d10e`, `b9da8cc1`, `545b8bdd2b638231`, `83a48de`, `5f671fba`, `87a33bf0`, `265d122`, `dcf04dd` … (+4 more in the text)
- **run dirs named in the text:** `runs/evseed_field6h_v3`, `runs/evseed_field6h_v3raw`, `runs/evseed_field6h_v3t`, `runs/evseed_field6h_v4`, `runs/evleg_hfix_trap`, `runs/evleg_close2_null_main`
- **source:** `docs/archive/SPEC_event_harness.md` — 21.9, 21.10, 21.11, 21.12

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 21.9 RESULTS — RS-3: both datums continue at the seam; the wire still re-realizes ~10 m per phase-bias family, without the branch too, and UNDECLARED; RS-4 pre-registered (2026-09-12 ~16:30 local; seed `runs/evseed_field6h_v3`, exe `gnss_processor_rs3.exe` D233ADBB…; manager re-grade)

**Build (merged `794d10e`).**  As §21.7 plus one deviation the worker found by measurement: the code-bias half has a
THIRD container — the IF-pin reference pair `code_bias_ifpin_pair_`, against which `NetworkSbasis` decides
`pair_same`; restored empty it reads "pair changed" for every satellite, the split re-seeds to 0 and (with the
seen-set now restored) every one bumps — restoring the values without the pair was strictly worse than today
(probe: 32 of 32 still re-seeding).  Row `CBI` added; discs 32 → 0.  Rows ride the existing header (the
optional-`var_first` precedent).  Debug suite 1982 / 0 failed, `ResumeDatumRestore` (7); cold smoke products
`b9da8cc1…` both arms, snapshots differing only by the 824 new rows; seed v3 720/720, ep0604 at the same tow
with 793 datum rows (6 EQD / 176 EQP / 126 EQS / 302 EQT / 121 CBP / 32 CBS / 30 CBI), fingerprint
`545b8bdd2b638231` unchanged.

| first post-resume epoch | restore | norestore (control) | nocarry_restore |
|---|---|---|---|
| `eq34_reref` / `eq34_reattach` (seed 2 / 1) | **8 / 0** | 110 / 0 | 8 / 0 |
| `CB_DATUM/reseed_disc_landed` (seed 0) | **0** | 30 | 0 |
| R record satellites | **0** | 30 | 0 |
| D5 flags at +1 / births +1 / +2 | 40 / 64 / 7 | 301 / 64 / 7 | 0 / 701 / 7 |
| G2 / G3 / G4 | PASS / PASS / PASS | PASS / PASS / PASS | PASS / PASS / PASS |
| per-sat-median-removed \|ΔPB\| p50 at +2 / +60 | **11.20 / 11.30 m** | 14.35 / 13.78 | 9.33 / 10.52 |
| clock contrast pooled p50 / p95 | **1.74 / 9.28 m** | 3.61 / 11.94 | 1.82 / 8.85 |
| W6 code bias pooled p50 | 0.470 | 0.656 | 0.464 |
| ZWD pooled p50 / p95 | 0.64 / 4.99 | 1.65 / 8.62 | 1.24 / 7.84 |
| a₀₀(+60) − seed | −1.19 | −0.84 | −1.22 |
| cold-consumer replay p50 from +5 (§18.14) | **6.66 m, falling** | 10.04 m, flat | — |

The control reproduces §21.7's K = 1 leg exactly (110 / 30 / 30 sats / 14.35 m / 3.61 / 0.656 / 1.65), so v3
is a faithful replacement for v2.  The restored tree at +1 (the leg's own first-epoch snapshot — see the trap
below) is 93 % the writer's: EQP 164/176 parent edges, EQS 115/126 roots, EQT 282/302 pins (all targets 0),
CBS/CBI identical.

**Verdicts.**  P5a: the two hard bars pass (re-seed discs 0 ≤ 3, R record 0 ≤ 3); `eq34_reref` 8 against a
bar of 5 and a seed rate of 2 — a near-miss that reads as the 7 % of the tree that genuinely could not be
kept (arcs that changed across the seam).  P5c PASS.  **P5b FAIL on every channel**, all moving the right way.
Cold-consumer replay: the restore leg's wire converges (6.66 m and falling) where the control's does not (10.0
flat) — a real product improvement, not yet continuity.

**The residual, measured (`products.txt` at +2, leg vs seed).**  Satellite clocks agree to a common −0.118 m
with a datum-free contrast of 0.54 m p50.  Phase biases do NOT: raw |ΔPB| p50 **17.6 m** over 120 keys,
**not whole wavelengths** (|Δ/λ − round| p50 0.258 — uniform noise), **not common to a satellite** (spread
across a satellite's signals 48.7 m p50; G02 L1C +11.5 / L1W +37.5 / L2W +13.8 m).  The emitter's own per-key
baselines (`epb`, restored from the writer) moved by 16.5 m p50 at the first emit.  The same order of offset
is present WITHOUT the DD branch (`nocarry_restore` 9.3 m) and with the branch held (§21.7), and the tree and
targets are the writer's.  So the phase-bias LEVEL per family is set by something that restarts cold in every
posture — the station layer (the rx-CB carry: 26 % of priors withheld at its 10σ gate on the leg vs 13 % on
the seed; the T5 per-LOS iono deviation carry: 400 cold seeds at +1; the SH a₀₀ +4.6 TECU at +1 decaying in
five epochs; the receiver phase-bias states) — or a definition of the family's level the pins do not fix.
That is RS-4's measurement.

**The seam is UNDECLARED on the wire — an invariant-#7 gap in its own right.**  The first emitted E record
carries `datum_changed 0`, no R record is written, and this lineage does not serialize the optional per-key
`phase_disc` field (legend: `P,sat,signal,bias_m,signal_integer,wide_lane_integer[,phase_disc]`), so a file
consumer sees a 17 m phase-bias step with no signal.  Why: the CLK-R `resume_seam` datum mark fires only when
the held-frame engine reports `epochs == 0` (`NetworkEmitStore.cpp:338`), and the frame restore brings that
engine back with its 604 epochs — the mark was designed for the clock frame and is suppressed exactly when the
clock IS continuous; the emitter's per-key jump test (`NetworkProductEmitter.cpp:383`) does bump the
in-process counters on a fresh baseline, which is what the in-process consumer reacts to, but nothing reaches
the file.  ⚠ Harness trap (worker, verified on `rs2_null_rebase_d5w1` too): a leg's first emitted epoch
overwrites the `seed.txt` it resumed from — `event_leg.py` sets `state_snapshot_interval_s = 1e9`, but
`NetworkRunContext.cpp` gates the write on `last_snapshot_write_s < 0.0 || …`, and the initial −1 fires
unconditionally.  Every leg today copied a fresh snapshot and ran once, so no result is affected; `event_leg.py
rerun` exists for exactly this; a re-run in place would silently resume from the leg's own +1 snapshot.

**RS-4 pre-registered — the phase-bias level at the seam: measure, then declare.**
* **Measurement (no solve change):** arm the raw satellite-bias state trace (`network.trace_sat_bias_states`,
  trace-class) on a `rs3_null_restore`-posture leg and on a 60-epoch re-run window of the seed, and attribute
  the raw estimator PB difference leg − seed at +1 per (satellite, signal) to: (a) the family's pinned arc kept
  vs changed (the EQT diff), (b) the pinning station's rx-CB carry restart and its receiver phase-bias state,
  (c) the signal and its per-LOS iono deviation at the pinning station, (d) emitted vs raw (whether the
  emitter's re-base/filter moves the level).  Deliverable: the table, and a one-sentence mechanism the next
  memory card can be built on.  Also the rx-CB carry and T5 deviation as candidate rows for a capture increment
  if (b)/(c) dominate.
* **Declaration (small build, default-OFF, byte-identical when off):** `network.resume_pb_seam_mark` — at the
  first emit after a warm resume, if the emitted phase bias differs from the restored per-key baseline by more
  than `discThresholdM` on more than 10 % of a constellation's keys, mark `datum_changed` for that
  constellation exactly as the CLK-R `resume_seam` rule does (counted, evented `PB_FRAME/resume_seam`); and
  register the per-key discontinuity serialization for the field golden as its own row (product bytes change
  — a documented product-contract change, certified by a smoke pair).  Gates: on the restore leg the first E
  record carries `datum_changed 1` (or an R record) whenever P5b's PB bar fails; byte-identical products when
  no key exceeds the threshold (the seed-window control); the cold-consumer replay unchanged (the mark reaches
  the file, the replay driver's reaction is measured, not assumed).

### 21.10 RESULTS — RS-4: the seam's phase-bias level is the EMIT LAYER, and it closes exactly; the seam is now declared; RS-5 pre-registered (2026-09-12 ~19:00 local; worker on worktree-agent-ac918e1c0b5f083e4, merged `83a48de`; manager grade)

**The instrument.**  `network.trace_sat_bias_states` does not carry the phase-bias state (its writer at
`NetworkSolPrep.cpp:1253` emits the satellite CODE bias), and no trace carries it — `network_solution.csv` is
written downstream of the emit transforms and mirrors `products.txt`.  The worker found the exact
instrument in existing knobs: `NetworkEstimator.cpp:1050` sets `npb.bias_m = x_s(c)` (the raw state) and two
OUTPUTS-ONLY transforms then overwrite it in `NetworkEmitStore.cpp` — the Card-4 eq34 emit re-base (`:256`,
`carry_invariant_emit_rebase`, default true) and the PB emission filter (`:295`, `emit_pb_filter`, armed on
the golden) — so a twin with both off (`runs/evseed_field6h_v3raw`) emits the raw state verbatim.  Proof it is
outputs-only: the v3t and v3raw ep0604 snapshots differ in 0 lines outside the emitter `epb`/`ep` rows, and the
two restore legs have max |ΔCLK| = 0.0 over 1830 clock keys.  The trace-only twin `runs/evseed_field6h_v3t`
hashes `products.txt` `5f671fba…` = v3's (trace-class confirmed).

| p50 \|Δ\| per (sat, signal), leg − seed | +1 | +2 | +30 |
|---|---|---|---|
| **EMITTED** | **17.666 m** | **17.611** | **17.652** |
| **RAW** (transforms off on both sides) | **1.695 m** | **1.862** | **3.263** |
| emit layer on the LEG (emitted − raw) | **0.000 (0 of 129 keys nonzero)** | 0.628 | 5.714 |
| emit layer on the SEED at the same tow | 18.410 | 18.766 | 23.062 |
| closure `emitted − (raw + legLayer − seedLayer)` | — | **0.000 over 120 keys** | — |

In §21.9's own units (per-sat-median-removed |ΔPB| p50 at +2): restore 11.20 (§21.9: 11.20), nocarry 9.33
(§21.9: 9.33), **raw 0.43 m**.  Clocks reproduce §21.9 (common −0.1177 m, contrast p50 0.537).

**Mechanism.**  The eq34 emit re-base engine (`NetworkEmitRebase`, a bare run-local at
`ApplicationNetwork.cpp:915`; memory `last_emit_` = one double per key, the previous emission frame) and the
PB emission filter (`NetworkPbEmitFilter`, `:930`; `state_` mean/var/last_seen per key + per-constellation
step accumulators) are in no snapshot and cold-start at the seam: the leg's first epoch passes the raw
solve-frame value through untouched while the seed's wire at the same tow carries 604 epochs of accumulated
re-base (18.4 m p50).  The estimator's own level moves **1.70 m p50 at +1** — 1.68 m excluding the six L2X
keys (24.7 m), whose eq34 component datum receiver GAMB00PYF vanished at the seam (`→ None/CPVG00CPV`; 19 of
134 families changed root); grouped by pinned-arc kept vs changed 1.59 vs 2.44 m, by signal L1C 2.10 / L1W
1.10 / L2W 2.41 / L5Q 2.37 / L5X 0.82, L2W/L1W ratio −0.55 (not iono-like: +1.65).  Receiver clocks leg −
seed at +1 p50 1.21 / p95 5.11 m over 61 stations; receiver code bias 0.57 / 1.68.  **Cold-consumer replay**
(p50 3D from +5 / +30): restore 6.66 / 5.54, nocarry 15.41 / 15.44, the seed's own continuous wire over the
same window 9.30 / 8.08 — a constant per-key level costs a cold consumer nothing; the consumers the seam
hurts are the ones that carried state across it.

**Declaration built and measured (`network.resume_pb_seam_mark`, default OFF, one-shot, outputs-only, not
fingerprinted).**  Marked leg: run-end `keys tested 129, over threshold 129 (100.0 %), constellations marked
1`, one `PB_FRAME/resume_seam` event (`keys_over=129/129, max|dPB|=119.3 m`), first E record `datum_changed 1`
(`…,604,0,0,1,G`) vs the control's 0 — **the whole 61-epoch products file differs by that one line** (P
records max |Δ| 0); cold smoke on and off both `b9da8cc1…`; Debug suite 1988 / 271 suites / 0 failed;
`ResumePbSeamMark` (6).  The replay of the marked wire is byte-identical to the unmarked one (`NetworkProductIO
.cpp:773` re-applies the flag, but every key is its own first sighting at the first replayed epoch).  The
complementary half for a stateful file consumer is **`network.serialize_disc_counters`** (RTA-4): no ledger row
until now; its smoke pair changes only the P (2700) / B (2595) / C (640) records (`b9da8cc1 → 87a33bf0`, one
integer inserted before the `sigma=` token of every P line, legend unchanged).  Registered this commit:
the mark RECOMMEND_SCOPED and armed on every harness leg; the serialization RECOMMEND_SCOPED with the golden
pins DEFERRED to the RS-5 close-out, where the smoke baselines of all of today's cards are re-recorded once.

**RS-5 pre-registered — emit-layer continuity across the seam.**  Write knob
`network.state_snapshot_emit_capture` (default false): the snapshot carries `NetworkEmitRebase::last_emit_`
(row `ER,<sat|signal>,<last_emit_m>`) and `NetworkPbEmitFilter`'s `state_` (`PF,<sat|signal>,<mean>,<var>,
<last_seen_s>`) and `step_` (`PS,<const>,<sum_log_e2>,<n_eff>`) as format-forward rows under the existing
header; both engines become parameters of `networkWriteStateSnapshot` / `networkTryWarmResume` exactly as the
CLK-R engine was made one (§18.10), with `exportState` / `restoreState` on each.  Read knob
`network.resume_emit_restore` (default false; forward-only resume response policy, fingerprint-EXEMPT; requires
`resume_frame_restore`): restore both engines before the first emit; rows absent ⇒ today's behaviour, loudly;
counters + run-end line (`emit re-base keys restored N, filter keys N, step accumulators N; first-epoch emit
layer p50 vs the writer's`).  Legs from a new seed `runs/evseed_field6h_v4` (write knob via `--extra`; the
harness will arm both knobs by default once merged): `rs5_null_restore`, `rs5_null_norestore` (control — must
reproduce RS-4's 17.6 m emitted / 0.000 layer), and the raw-transform twin is not needed (the closure is the
instrument).
* **P7a the emit layer continues:** emitted − seed at +1 p50 ≤ raw − seed p50 + 0.05 m (i.e. ≈ 1.7 m, the
  estimator's own residual); the leg's emit layer at +1 within 0.05 m of the seed's on ≥ 95 % of keys; the
  seam mark's `keys over threshold` fraction reads the RAW residual's (recorded — with a 1.7 m raw step it is
  expected still to fire, honestly).
* **P7b nothing else moves:** availability, a₀₀, clocks, D5, births, eq34 rerefs and CB discs as RS-3's restore
  leg; cold-consumer replay unchanged (a level is invisible to a cold consumer).
* **P7c the stateful consumer (the point of the card):** a consumer replay STARTED on the seed's wire and
  continued across the seam onto the leg's (splice the seed's products up to ep0604 with the leg's; §18.14's
  `splice_wire.py`) — its ambiguity resets at the seam and its 3D p50 over +5..+30 vs the same consumer on the
  seed's continuous wire; recorded for the control and the restore leg, with the mark's declaration on.
* Then RS-6 — the estimator's own 1.7 m level at the seam (the station layer: receiver clocks 1.2 m p50 at +1,
  the rx-CB carry, the T5 deviation, the vanished L2X component datum receiver) — is pre-registered from
  RS-5's residual table, not before.

### 21.11 RESULTS — RS-5 closes the seam on the phase-bias wire; the day's close-out; RS-6 pre-registered (2026-09-12 ~21:00 local; worker on worktree-agent-a1a4a2dd4a4bbb596 `265d122`; manager grade)

**Build.**  `NetworkEmitRebase::exportState/restoreState` (the `last_emit_` map) and
`NetworkPbEmitFilter::exportState/restoreState` (`state_` + `step_`; counters and config stay run-local);
write knob `network.state_snapshot_emit_capture` (rows `ER` / `PF` / `PS`, format-forward under the existing
header; the pooled `PS` bucket's key is legitimately blank), read knob `network.resume_emit_restore` (throws
without `resume_frame_restore`); both engines threaded through `networkWriteStateSnapshot` /
`networkTryWarmResume` as the CLK-R engine is.  Debug suite **2001 / 273 suites / 0 failed** (17 fixture
skips in the worktree); `ResumeEmitRestore` + `ResumeEmitRestoreConfig` 13/13; cold smoke `products.txt`
**`b9da8cc1…` in both arms**, snapshots differing by 135 ER + 135 PF + 1 PS rows; seed
`runs/evseed_field6h_v4`: 720/720, `products.txt` **byte-identical to v3t's** (`5f671fba…`), ep0604 with 147
ER / 147 PF / 1 PS, fingerprint `545b8bdd2b638231` unchanged.

| emitted PB, leg − seed, p50 / p95 / max \|Δ\| | +1 | +2 | +30 |
|---|---|---|---|
| `rs5_null_norestore` (control) | 17.666 / 82.80 / 119.34 m | 17.633 / 82.82 | 17.652 / 82.84 |
| **`rs5_null_restore`** | **0.000 / 0.003 / 0.053 m** | 0.001 / 0.011 | 0.038 / 0.153 |

Keys within 0.05 m at +1: restore **120/121**, control 0/121; per-sat-median-removed p50 at +2: 0.0005 m vs
11.20.  **P7a PASS — and past its own bar:** the restored re-base aligns each key to the writer's last emitted
value, so it absorbs the estimator's 1.7 m raw level as integer + common-fractional; the PB seam mark correctly
stays silent (`keys tested 129, over threshold 6 (4.7 %), marked 0`, first E `datum_changed 0`) where the
control fires it (129/129, marked 1).  **P7b PASS, byte-exactly:** the two legs' `products.txt` (19 049 lines)
differ ONLY in 7 840 P rows and the one E flag — every C / B / U / H record identical, and a₀₀ (worst |Δ| 4.39
TECU at +39; −1.19 at +60), clocks (contrast pooled p50 1.743), code bias (0.470), ZWD (0.636), D5 flags at +1
40 / re-taps 0, births 64 / 7, eq34 rerefs 8, CB re-seed discs 0, `637 re-based` are the same numbers on both.
**P7c — the stateful consumer** (`scripts/splice_wire.py`, re-written and verified to reproduce the seed's
bytes up to the seam; the same WTZZ00DEU consumer replayed over the same 665-epoch window):

| wire | PB resets at the seam | code re-refs | clock re-refs | PB resets over the window |
|---|---|---|---|---|
| seed, continuous | 0 | 0 | 0 | 29 |
| spliced control | **19** | 24 | 6 | 59 |
| **spliced restore** | **0** | 21 | **0** | 30 |

The restore wire is indistinguishable from the continuous wire on the phase-bias channel; the 6 clock
re-references in the control are downstream of its `datum_changed 1`; the code-bias re-references (21 / 24) are
in BOTH legs whose B records are byte-identical — the seam's CODE-BIAS channel, the estimator's own 0.47 m
level, declared per key by the code jump test.  (The replay's 3D column is not an instrument here: the file
replay floats its own iono and reads ~15 m on the continuous wire.)

**Decision.**  Both knobs RECOMMEND_SCOPED and registered; the goldens pin the write knob; the harness arms
both by default (seeds and legs).  **The deferred product-contract pin lands now:** `network.serialize_disc_
counters` is pinned on both goldens and the smoke baselines are re-recorded once in the close-out
certification below (the field golden's canonical 20-epoch hash moves from `b9da8cc1…` to the serialized
one measured in §21.10's pair, `87a33bf0…`, if nothing else in the day moved products — the certification
says).  The harness posture for a field resume leg is therefore: frame + branch (re-based, one held D5 epoch) +
both datums + the emit layer + the PB seam mark, every one of them default-OFF, byte-identical when off, and
outside the warm-resume fingerprint.

**What the seam still costs, and where (RS-6, pre-registered as a MEASUREMENT first).**  The estimator's own
level at the first post-resume epoch: phase bias 1.70 m p50 raw (1.68 excluding the six L2X keys whose eq34
component datum receiver vanished), code bias 0.47 m p50 (the 21 code re-references), clock contrast 0.54 m at
+2 growing to 1.7 m pooled, ZWD 0.64 m, receiver clocks 1.21 m p50 — all set by the station layer that
restarts cold (the rx-CB carry: 26 % of its priors withheld at the 10σ gate on a leg vs 13 % on the seed; the
T5 per-LOS iono deviation carry: 400 cold seeds at +1; the SH a₀₀ +4.6 TECU decaying over five epochs; the
receiver phase-bias states).  With RS-5 armed none of this is visible on the PB wire any more (the re-base
absorbs it), so RS-6 needs a STATE-side instrument: extend `network_sat_bias_states.csv` (today CODE bias only)
with the per-(sat, signal) phase-bias state (trace-class, byte-identical products), and a trace of the rx-CB
carry values per (station, signal).  Legs from v4 ep0604 on today's posture, each with ONE station-layer memory
restored in turn once captured (rx-CB carry; T5 deviation; RPB) against the control — the channel that moves
names the memory.  Bars, frozen now: raw PB residual at +1 p50 ≤ 0.2 m; code bias ≤ 0.05 m p50 (and the
stateful consumer's code re-references at the seam ≤ 3); clock contrast pooled p50 ≤ 3× the seed's own step
(0.27 m); ZWD ≤ 3× own step (0.32 m); SH a₀₀ at +1 within 2 TECU.  Also owed from the day: the six L2X families
whose component datum receiver GAMB00PYF is dropped at the seam (a re-election edge, not a memory).

⚠ **Harness trap, confirmed on every leg today and to be fixed in the harness, not the producer:** a leg's
first emitted epoch overwrites the `seed.txt` it resumed from (`NetworkRunContext.cpp` gates the first write on
`last_snapshot_write_s < 0.0`, which the initial −1 satisfies unconditionally, whatever
`state_snapshot_interval_s` says).  The `.epNNNN` library is intact; `event_leg.py rerun` restores the pristine
copy; never re-run a leg in place.  **FIXED in the harness 2026-09-13 (main `dcf04dd`, worker commit `a207193`):**
`make-leg` / `null` now lay down TWO copies — `seed.txt` (pristine, never named by any config; its sha256 recorded in
`leg_meta.json`) and `state_snapshot.txt` (what `state_snapshot_path` names, disposable) — so the exe never reads and
writes the same file; `rerun` restores only the disposable copy after verifying a pristine source.  Validated on
`runs/evleg_hfix_trap`: first run and in-place rerun both WARM RESUME, products byte-identical (`95cda87d…`),
`seed.txt` unchanged.  The same commit arms `resume_coupling_restore` on a leg only when the seed's lineage carries
the loader's Card-1f interlocks (the field golden does not), so field legs no longer need `--extra` for it.

### 21.12 CLOSE-OUT CERTIFICATION (2026-09-12 ~22:30 local; worker on main's own tree at `17f854f`)

Debug and Release rebuilt with the wrapper, first attempt.  **Full Debug suite: 2001 cases / 273 suites,
2000 passed, 1 skipped (the documented fixture gate), 0 failed.**  Three-way 20-epoch smoke of the shipped
field golden on `gnss_processor_close2.exe` (106D0205…): `base` (as it ships) **`87a33bf0…`**; `noser`
(`serialize_disc_counters` false) **`b9da8cc1…`** — the previous canonical hash, so nothing but the
serialization moved products today; `nowrite` (the three write knobs `state_snapshot_carry_lin` /
`_datum_capture` / `_emit_capture` false) **== `base`** — the write side is byte-identical.  The base/noser
diff is exactly P 2700 / B 2595 / C 640 records, one appended or inserted integer each, S/E/H/U untouched
(RS-4's census reproduced).  **The field golden's canonical 20-epoch hash is `87a33bf0…` from here on.**  All
nine knobs registered today read correctly in the POSTURE block (the four write/contract knobs armed; the
resume knobs `POSTURE OFF [SCOPED]` on a cold seed writer; `resume_datum_restore` and `common_band_ref_arc`
out of regime), no unknown-knob warnings.  Closing leg `runs/evleg_close2_null_main` (v4 ep0604, harness
defaults): 60/60, every seam and run-end line as §21.11's restore leg, and its `products.txt`
**byte-identical to `rs5_null_restore`'s** (`5fca04e2…`) on main's merged exe.

Two notes for the next session.  (1) **A leg inherits its SEED's config:** `runs/evseed_field6h_v4` was
written before the `serialize_disc_counters` pin, so every v4 leg keeps the unserialized product contract —
write a v5 seed from the current golden before grading anything that needs the per-key counters on the wire.
(2) Two pre-existing golden properties, not today's: the loader's regime label reads "WARM-RESUME start" on a
cold smoke (it keys on `state_snapshot_path` being set), and the POSTURE CHAIN advisory about
`emit_generation_id` under `state_snapshot_require: false` fires on every cold seed writer.

