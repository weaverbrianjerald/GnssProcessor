# wind-up census — the fractional-mode census — the two continuity horizons are INVERTED, `trace_windup_census`, and the restart response built default-OFF

**STATUS**

- **id:** wind-up census
- **topic:** slip
- **status:** open
- **summary:** the fractional-mode census — the two continuity horizons are INVERTED, `trace_windup_census`, and the restart response built default-OFF
- **dates in the text:** 2026-08-22 → 2026-08-23
- **knobs / posture:** `estimation.trace_windup_census`, `network.phase_leveling_max_gap_s`, `estimation.phase_windup_max_gap_s`, `network.cycle_slip.windup_restart_refloat`, `estimation.accumulate_phase_windup`, `network.phase_leveling`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/_wd`
- **source:** `docs/archive/SPEC_phase_windup_sign.md` — §10, §10.1, §10.2, §10.3, §10.4, §10.5, §11

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_phase_windup_sign.md -->
## §10 The fractional-mode CENSUS — §7's premise gets an instrument (2026-08-22)

§9.5 filed the `accumulate_phase_windup` loader default as "the follow-on card,
and it should be next".  Working that card surfaced a blocker that has to be
measured before the default can be discussed at all, so this increment builds
the measurement rather than flipping the knob.

### §10.1 The accumulator is NOT structurally broken by its key — verified

`prev_windup_` is keyed by **satellite alone**
(`include/corrections/GnssCorrections.hpp`), which would corrupt accumulated
mode outright if one instance served several receivers.  It does not: each
network station owns its own `PppModel` (`ApplicationNetworkSetup.cpp:576`) →
its own `Antenna` by value → its own `GnssCorrections` by value, and the sole
production caller is `Antenna::computeAntennaCorrection`.  One instance per
receiver makes the satellite-only key injective.  The place it *is* structurally
unsafe is the simulator's shared-provider fan-out, which already pays a
process-per-station cost for exactly this reason.  The contract is now written
into the header above the member so the next reader cannot re-derive it wrong.

### §10.2 The real blocker: the two continuity horizons are INVERTED

The accumulator restarts when the processed-epoch gap exceeds its horizon,
historically a hard-coded `kMaxGapSeconds = 300.0`.  A restart drops the
maintained value (`raw + 2πN`) back to the raw `atan2` angle, i.e. it applies an
**exact integer-cycle step to every signal of that satellite**.  Nothing resets,
refloats or flags any ambiguity in response; the whole event is a `debug` line
and a CSV column in a trace the network path never opens (`enableWindUpTracing`
has one caller, in `Application::run`, which `runNetwork` never reaches).

The FT63/EXT63/FLD63 chain sets `phase_leveling_max_gap_s: 900.0` — **3× the
accumulator's horizon**.  So a mid-arc gap in **(300, 900] s** steps the model by
an integer while the carried ambiguity and the zero-q leveling level SURVIVE and
are re-injected at their pre-gap values.  That is standing invariant #1 verbatim
(a stiff time-linked quantity carried past a jump with no wired response) and #7
(both edges silent).  The MW detector cannot close it: the step is band-common,
so `ΔWL = 0` exactly and MW is structurally blind — the same blindness that
makes the fractional staircase invisible.

⇒ **Arming `accumulate_phase_windup` on a leveling lineage today trades a
CHARACTERISED 753/day staircase for an UNCHARACTERISED, uninstrumented one.**
The default flip is therefore deferred behind the response, and the response is
deferred behind the measurement.

### §10.3 What landed — `estimation.trace_windup_census`

A read-only per-receiver census, armable in **BOTH** postures.  That is the
point: the fractional posture is what every field lineage runs, and no
instrument had ever reached it on the network path — §7's headline numbers come
from ONE PPP station's `windup_trace.csv`, scaled to 63 by hand.

Counted per consecutive PROCESSED epoch (deduped on epoch exactly as the CSV
trace is, because the function runs once per signal and once per rebuild pass):

| counter | meaning |
|---|---|
| `steps` | FORWARD consecutive-epoch transitions — the denominator |
| `backwards` | transitions with gap ≤ 0 (epochs out of order); excluded from every bucket, and the reference still advances so one of them cannot spoil the NEXT measurement too |
| `wraps` | \|Δ applied\| ≥ 0.9 cyc across an in-horizon gap — **the fractional staircase**. ⚠ **Identically zero in accumulated mode**, because every in-horizon transition is unwrapped into ±0.5 cyc; a zero there is a structural property, not evidence |
| `restarts` | **the accumulator's OWN verdict**, not a threshold on the applied step — the only counter that can see restarts in accumulated mode |
| `gap_steps` | \|Δ applied\| ≥ 0.9 cyc across an out-of-horizon gap |
| `gap_lt_wu` | transitions inside the wind-up horizon |
| `gap_wu_to_carry` | out-of-horizon transitions between the two horizons — **the §10.2 exposure, but only when the carry horizon EXCEEDS the wind-up one** |
| `gap_gt_carry` | out-of-horizon transitions beyond both |
| `max_abs_step_in_cyc` / `max_abs_step_gap_cyc` | step magnitude, in- and out-of-horizon kept apart so a single restart is never reported as an in-horizon step size |

⚠ **The exposure bucket is only meaningful when the carry outlives the
accumulator.**  It does on the field chain (900 s vs 300 s) but NOT at the loader
default: `network.phase_leveling_max_gap_s` defaults to **120 s**, *below* the
300 s wind-up horizon, and on that posture the ordering is inverted and the
bucket is structurally empty.  The teardown prints the ordering it observed and
**suppresses the exposure narrative when it does not hold**, so a structural zero
can never be read as a clean bill of health — the invariant-#4 failure mode this
card exists to avoid, which it would otherwise have committed itself.

Plus `estimation.phase_windup_max_gap_s` (double, default **300.0** — the value
that was hard coded, so the default is behaviour-preserving; non-positive is
rejected-to-default because a zero horizon restarts every epoch and silently
collapses accumulated mode to fractional while still reporting "accumulated").

Run-end readouts: `NetworkTeardown.cpp` (roster sum) and `Application.cpp`
(single receiver, the cross-check against §7's own 43/day figure).  Telemetry
only — arming it cannot move a byte of the solve.

### §10.4 Pre-registered reading of the first armed run

On a FRACTIONAL field lineage (the canonical posture), armed on the existing
`_fld63_relevel4h_ctlw` control so no other knob moves:

- **`wraps`/station/day should land near §7's 43** (measured at GCGO, 24 h,
  115 satellites).  Materially below ⇒ the census under-counts (dedupe or
  first-sighting defect) — check `steps` against the epoch count first.
  Materially above ⇒ §7's single-station figure did not generalise, and the
  63-station scaling in §7 is an underestimate.
- **`gap_wu_to_carry` is the number that decides the accumulate card.**  Zero on
  a settled field run ⇒ the inverted-ordering hole has an empty population on
  this posture and the default flip can be argued on §9.5's residual algebra
  alone.  Nonzero ⇒ the response (refloat the arcs the restart stepped, at both
  edges, with identities) is a BLOCKING prerequisite, and no accumulate arm
  should run until it lands.
- `steps` = 0 on an armed run is **dead wiring**, not a null result
  (invariant #4).
- Read the census on the arm that matches the question: `wraps` answers the
  FRACTIONAL question, `restarts` answers the ACCUMULATED one, and neither
  substitutes for the other.

Arm config: `config/test_suite/_fld63_wucensus4h.json` (knob-verbatim clone of
`_fld63_relevel4h_ctlw` plus the census; byte-identical on the solve, so it needs
no A/B twin).

⚠ **Provenance caveat for SIM lineages.** `estimation.phase_windup_max_gap_s` is
honoured by the PROCESSOR but is NOT threaded into the simulator's wind-up
injector, which still restarts on the historic 300 s.  A sim run that sets the
knob to anything else puts fixture and producer on different continuity horizons
— the same fixture/producer provenance class the §9.9 sign-fix rule was paid for.
Until the value is threaded (or asserted equal at synthesis), **leave the horizon
at its default on any lineage whose observations came from the simulator**; the
field arm above is unaffected.

**Two wiring caveats worth knowing before quoting a number.**  The census is
armed only on the network producer and the single-receiver `Application::run`
path — it is silently inert in baseline and real-time modes.  And the user-path
readout goes to the `ppp_model` logger, which `logging.enable_ppp_model` can
gate, so a PPP run can accumulate counters and print nothing.

**Fingerprint verdict:** `phase_windup_max_gap_s` should NOT be added to the
state-snapshot fingerprint now.  It is inert in the only posture anyone runs, the
posture knob above it is itself unfingerprinted, and the real resume hazard is
not a knob at all — **the accumulator state is not snapshotted**, so every
accumulated-mode warm resume applies an unconditional integer-cycle step while
the carried levels survive.  The ordered prerequisites for the default flip are
therefore: (1) the census result, (2) a wired restart response at both edges,
(3) conditional-append BOTH `accumulate_phase_windup` and
`phase_windup_max_gap_s` to the fingerprint using the established
append-only-when-armed pattern so existing fingerprints stay byte-identical, and
(4) either serialize the accumulator or refuse a warm resume in that posture.

**Not landed, deliberately:** the `<` → `<=` boundary change at the horizon
comparison (its motivating population is empty — no config that arms accumulate
sets any decimation or rate knob), the unwrap-residual guard (its threshold
would have to be derived from a per-gap \|Δψ\| distribution that does not exist
yet — the census is what produces it), and any loader interlock that DISARMS
accumulate, which would silently flip the ~119 committed simulator producer
configs that arm it from accumulated to fractional: a larger and less examined
behaviour change than the default flip this card declines to make.

### §10.5 The census EXECUTED on the field lineage (2026-08-22) — both numbers measured

`runs/_wd/wucensus` (`_fld63_wucensus4h.json`, 4 h, 61 stations, GPS dual,
fractional mode, horizons **wind-up 300 s / leveling carry 900 s** — the ordering
under which the exposure bucket is meaningful).

```
237 243 forward transitions (0 out-of-order)
IN-horizon : 237 201 transitions, 51 step(s) >= 0.9 cyc, mean |step| 0.0007 cyc, max 1.000
OUT-of-horizon: 42 total — 20 BETWEEN the horizons, 22 beyond both, 0 of which stepped
Accumulator RESTARTS: 0   (fractional mode — correct by construction)
```

**(1) The fractional staircase is real on the network path, and it reproduces §7
once normalised.**  51 one-cycle steps in 4 h across 61 stations = **306/day
network-wide**, mid-pass, band-common (ΔWL = 0 exactly ⇒ MW structurally blind).
Per sample: 51/237 201 = **2.15e-4 per satellite-epoch**, against §7's GCGO trace
at 43/100 983 = 4.26e-4.  The factor ~2 is expected and in the right direction —
§7's trace samples at ~98 s against this run's 30 s, and wrap probability per
transition scales with cadence.  §7's headline was one PPP station scaled to 63 by
hand; this is the first direct network-path measurement, and it lands in the same
class.

**(2) `gap_wu_to_carry` = 20 — NONZERO, and that is the answer the card was
built to get.**  Twenty transitions in 4 h across 61 stations (≈120/day) fall in
the (300, 900] s band where arming accumulated mode would apply an exact
integer-cycle step while the carried ambiguity and the zero-q leveling level
survive at their pre-gap values, with no response wired.  Per §10.4's
pre-registration, **nonzero ⇒ a wired restart response is a BLOCKING prerequisite
and no accumulate arm runs until it lands.**

**The consequence is sharper than §9.5's algebra suggested.**  §9.5's table gives
the accumulated corrected arm a residual of 0 — true for a CONTINUOUS arc, but a
restart segments the arc and the leveling carry bridges the segment boundary
uncompensated.  So the honest trade on this lineage is not "306 staircase steps
→ 0"; it is **~306 fractional wraps/day → ~120 uncompensated restarts/day**, i.e.
a ~2.5× reduction that MOVES the defect from a characterised channel to an
uninstrumented, unresponded one.  Arming accumulation is therefore an improvement
of bounded size that trades a known problem for a less-visible one — which is
precisely why the response, not the flip, is the next build.

Also confirmed: `restarts` = 0 in fractional mode and `wraps` bounded by the
unwrap in accumulated mode are BOTH structural, so neither counter alone answers
the question — the pair does.  The horizon-ordering disclosure printed
"carry outlives the accumulator", i.e. the exposure bucket was meaningful on this
posture (it is NOT at the 120 s loader default).
## §11 The §10.4 RESTART RESPONSE — BUILT (2026-08-23, default-OFF)

The blocking prerequisite is now wired: `network.cycle_slip.windup_restart_refloat`.

**Mechanism.**  The accumulator itself keeps a restart LEDGER
(`GnssCorrections::wu_restarts_`, filled ONLY in accumulated mode at the §10.2
restart branch — satellite → {gap_s, epoch_s}, bounded at the visible
constellation, inert until drained).  A serial PASS-A drain
(`networkSlipDrainWindupRestart`, `slip/NetworkSlipQc.cpp`) runs after the
per-station model evaluations filled the ledgers and BEFORE the lane
observatory's PASS-A half, so a restart-freed pair enters the same epoch's
freed set.  Per fresh entry it applies the SAME response the raw-slip drains
use: mark pending-reseed, latch `free_ambs` with the new cause bit
`kCauseWindup`, feed the C3c funnel under carry_invariant, and VOID any
outstanding bridge pend for the (station, satellite) — a restart is a MODEL
step, and a pend's pre-slip capture predates it.

**Refusals, both counted** (closure printed at teardown):
- `epoch_s != now` → STALE (the station was absent at its restart; the step
  was absorbed at re-entry — responding to history would refloat a healthy
  arc);
- `gap_s > phase_leveling_max_gap_s` → BEYOND-CARRY (the carry died with the
  gap; nothing stale survives to refloat).

**Both edges (invariant #7):** `T0_windup/wu_restart` event rows per
(station, satellite) with gap and horizon (`refloat` / `no_arc` /
`diagnostic` / `stale_refused` / `beyond_carry`), run-end closure line
(`wu_restart_events = refloated + cold + beyond_carry + stale`), and the lane
observatory measures the response's own lane cost under `cause=WU` — the
§10.2 step is band-common (ΔWL = 0), so the observatory is the ONLY
instrument that can grade it.

**Interlocks (warn-and-disarm, invariant #4):** without
`estimation.accumulate_phase_windup` the verdict does not exist (fractional
mode wraps instead of restarting — a structural zero); without
`network.phase_leveling` there is no zero-q level to refloat.  The census's
accumulated-mode READING line now reports whether the response is armed.
Config-contract tests: `tests/test_windup_restart_response_config.cpp`
(defaults-off, both disarms, arm-does-not-drag-accumulation).

**Pre-registered validation ladder (in order; the accumulate flip stays
BLOCKED until (c) passes):**
(a) unit suite green;
(b) fractional field lineage: the knob DISARMS by loader (warn line present)
    and the run is byte-identical — trivial by construction, asserted once;
(c) ACCUMULATED sim arm on a gap-bearing fixture (⚠ §10.4's provenance
    caveat: leave `phase_windup_max_gap_s` at 300 on sim lineages — the
    injector's horizon is not threaded): gates = `wu_restart` counters
    NONZERO with closure OK; responded events consistent with the census's
    restart population on the same run; the relevel walk stays dead
    (SPEC_slip_relevel_walk quiet-twin gates); lane observatory `cause=WU`
    rows present and their |δ| mass read against the §7.6 null;
(d) only then: the accumulate-flip field arm (fresh convergence,
    conditional-fingerprint semantics), graded against §7's fractional
    staircase baseline (~306 wraps/day network-wide) with the response's own
    counters as the restart-side ledger.

**Deferral, named:** a restart at the legacy DATUM station (stations[0]) is
not responded (its ambiguities are gauge-pinned — the same scope every other
detector drain has); prioritized/eq34 lineages have no such station.
Field-inert on the FT63 chain (eq34).
