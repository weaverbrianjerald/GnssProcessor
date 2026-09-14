# Cards A+B — the `require` hard-fail and the rx_clock_step event, plus the jump gate in metres

**STATUS**

- **id:** Cards A+B
- **topic:** harness
- **status:** closed
- **summary:** the `require` hard-fail and the rx_clock_step event, plus the jump gate in metres
- **dates in the text:** 2026-09-10
- **knobs / posture:** `network.state_snapshot_require`, `network.carry_rx_clock_jump_k`, `network.carry_rx_clock_in_block`, `network.product_output_path`
- **hashes named in the text** (commit / exe / byte-cert): `dee2824`, `6a3531a1afb2b40b`, `8a849480e7ef02af`
- **run dirs named in the text:** `runs/_hb1probe`, `runs/evseed_quiet6h`, `runs/evleg_s2b_rxstep50_off`, `runs/evleg_s2b_null_off`, `runs/simnet60_quiet_wu93_24h`
- **source:** `docs/archive/SPEC_event_harness.md` — 11, 11.1, 11.2, 11.3, 11.4, 11.5, 11.6, 11.7, 11.8

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 11. Cards A and B — the `require` hard-fail, and the receiver-clock step

**Date 2026-09-10, branch `worktree-rxstep`.**  Two gaps §6b and §8 named, closed
together because the second one cannot be graded without the first: Study 2b's legs
are relaunch-heavy and every relaunch hits §6b.

### 11.1 Card A — `network.state_snapshot_require` now MEANS refuse

§6b reported that the non-positive-age refusal "is not wired to the HB-1 hook: it
warns, falls through to a COLD START, and the run then died `0xC0000409` 25.8 s
later."  Re-read against the source, the diagnosis sharpens and the fix changes:

* The branch **was** wired (`NetworkRunContext.cpp`, `hb1("gate rejected the
  snapshot: " + gate)` — `dee2824`).  The run was genuinely refused.
* What was broken is that **the refusal was unreadable and unsurvivable**.  The
  `log->warn(... COLD START ...)` line fired FIRST and unconditionally, so the last
  thing in the log claimed a cold start; then `hb1` threw a `std::runtime_error`
  that **nothing catches** — `main()` has no handler and `Application::run()` does
  not wrap `runNetwork()` — so MSVC's `std::terminate`→`abort()` fast-failed the
  process with `0xC0000409` **and the exception text, which held the reason, was
  never printed anywhere.**  The 25.8 s is the 60-station RINEX load, i.e. the
  refusal fired where it should: before the first epoch.

So "not wired" was the right symptom read from the outside and the wrong mechanism.
The fix is three parts, all on the error path only:

1. **One policy, one place.**  `NetworkStateSnapshotIO::warmResumeRefusalMessage
   (require, WarmResumeRefusal, detail)` is now the only thing that decides what a
   refused resume means; `warmResumeAllowed` gained an optional out-parameter that
   returns the refusal **class** (`Fingerprint` / `NonPositiveAge` / `Stale`) so the
   caller acts on the class, not on a substring of the prose.  Every branch —
   missing file, parse failure, no live epochs, gate rejection, emitter-restore
   failure, and the invariant-carry blob that will not bind — goes through it, so
   `require` cannot mean different things on different branches again.
2. **The COLD-START warnings are suppressed under `require`.**  Announcing a cold
   start the run is about to refuse is exactly the confusion §6b reported.
3. **The throw is caught at the `runNetwork()` call site**, which logs `e.what()` as
   the ONE error line and returns 1.  A refused leg now ends with a readable line
   naming the reason and the remedy, and `exitcode.txt` says `1`, not `-1073740791`.

The non-positive-age class carries its own remedy text (`event_leg.py rerun`,
§6b) because it is the one refusal an operator reaches by doing something
reasonable.  Nothing on the success path is touched, and `require: false` behaves
exactly as before (the message builder returns empty, the warnings still fire).

Tests: `tests/test_warm_resume_require.cpp`, 6 cases — the gate classifies a
relaunch-in-place as `NonPositiveAge`, each gate refusal gets its own class, every
class is fatal under `require` and none is without it, the harness remedy appears
on that class and on no other, and the gate's own prose survives into the fatal
message.

### 11.2 Card B — `rx_clock_step`, the dual of `sat_clock_step`

```json
{"kind":"rx_clock_step","station":"S21000SIM","at_ep":10,"step_m":0.5}
```

From the event epoch on, **every** code and phase observable of **every** satellite
at that station carries an additional constant `+step_m` metres.  Same site as the
other observation-side kinds (§2.1), same persistence rule as `slip` and
`sat_clock_step` (a step, re-applied every epoch — a receiver that jumped its clock
does not jump back), same never-fired hard error, same `INJECT` event row
(`rx_clock_step_onset;sats=N;rows=M`) and the same generic run-end ledger.

It is EXACT for the same reason `sat_clock_step` is.  With
`y = L̃ − (ρ + c·δt_r − c·δtˢ + …)`, adding `s` metres to every code and `s/λ`
cycles to every phase observable

* of ONE satellite at every station **is** `+s` on the a-priori satellite clock, and
* of EVERY satellite at ONE station **is** `+s` on that station's receiver clock —

the same scalar write swept over the other index of the same residual.  The two
kinds now share one helper (`addRangeStep`), so they cannot drift apart.

The loader **refuses** a `sat` or a `signal` on this kind rather than ignoring it: a
receiver-clock jump is station-common and non-dispersive by construction, and a
field that reads as a scope but is not one would arm a different event than the
operator wrote — the same rule as the rest of the parse contract.

Tests: 5 new cases in `tests/test_network_inject.cpp` — the kind parses with no
sat/signal, all four of those malformed forms fail the load, one station's every
satellite moves by the same metres on code and the same cycles on a shared band
while a second station does not move at all, the delta is constant across epochs
and absent before `tow`, and an absent station is a hard run-end error.

### 11.3 A new instrument: the jump gate in metres

`network.carry_rx_clock_jump_k` is 10 σ on the one-step prediction, and `var_pred`
is per station-epoch (harvested variance + drift extrapolation + q), so "the gate"
is a distribution, not a constant — and `k` alone cannot say whether a given step is
above or below it.  The run-end F3 line now carries, armed-only, the census
`k·√var_pred` mean / min / max, the largest |innovation| seen, and the refusal
count.  Measure-only; an OFF run moves none of it.

### 11.4 Study 2b — PRE-REGISTRATION (written before any ON leg was run)

Six legs, quiet bed, `state_snapshot.txt.ep0665` (tow 279150), 61 epochs, event at
+10 (tow 279480), station **S21000SIM**, graded on satellite **G10** for continuity
with §6 and §10.  OFF legs resume the OFF seed (fingerprint `6a3531a1afb2b40b`), ON
legs the ON seed (`8a849480e7ef02af`) — §10.1's rule, re-verified on the copies used
here.  Each event leg is graded against **its own arm's null**; ON-null − OFF-null is
the noise floor.

Predictions, in advance:

* **OFF absorbs ≥ 99 % of the step in the estimated receiver clock by +1 epoch**,
  with the other arcs' frame reaction at the null floor.  Precedent: §6(6)'s
  `sat_clock_step` absorbed **100.4 %** in one epoch with P(L1C) moving 0.6 mm.
  ⚠ One station's step is 1/N network-common, so a 60-station contrast can only
  show `(1 − 1/N)` ≈ 98.3 % of it; the RAW station clock-error difference is the
  unbiased channel and both are reported.
* **ON above the gate**: exactly **1** jump refusal, at the event epoch and at that
  station (`n_rxclk_carry_refused` 0 → 1); absorption and frame reaction equal to
  OFF within the null-vs-null floor.
* **ON below the gate**: the measurable cost.  The carried prior does not refuse the
  step, so it fights it; the leak is quoted into the other arcs' levels and the
  consumer.
* 0.5 m is expected **above** the gate and 0.05 m **below** it; §11.3's census
  decides that from the run rather than from the knob.

Verdict shape, fixed in advance: (a) harmless above the gate and harmful below it —
then the gate value matters and the leak is reported against step/σ; (b) inert on
its own target event too — then KEEP_OFF **with a mechanism**; (c) helpful.

### 11.5 The gate census refuted half the pre-registration before the ON legs were graded

`network.carry_rx_clock_jump_k` is 10 σ, and §11.3's census says what that is in
metres on this seed — **identical in all three ON legs** (variances do not depend on
observation values, so this is a property of the posture, not of the event):

```
rx-clock JUMP GATE: k = 10.0 sigma over 3660 station-epoch test(s)
  => gate mean 7.9863 m, min 3.0261 m, max 300.0161 m;
     largest |innovation| seen 9.5735 m; 0 refusal(s).
```

The 300 m tail is the documented first-increment case (an uninformative rate prior,
σ_d = 1 m/s over 30 s ⇒ ~30 m, ×10); 60 of the 3660 tests are those, and removing
them puts the SETTLED gate at ≈ **3.1 m**, consistent with the 3.0261 m minimum.

So **0.5 m is not above the gate — it is 6× below the tightest station-epoch on the
seed**, and 0.05 m is 60× below.  The pre-registered "ON above the gate" arm was
therefore unreachable at the default `k`, and both event sizes land in the
"below the gate" arm.  Rather than report a vacuous half-experiment, one more pair
was added, **pre-registered here before it was run**:

* **7. `rx_clock_step` 10 m OFF, 8. `rx_clock_step` 10 m ON** — 10 m (33 ns) is a
  perfectly ordinary receiver-clock jump and is above the ≈3.1 m settled gate.
  Predictions: OFF absorbs 100 % again (a sink with no gate cannot notice size); ON
  produces **exactly 1** `n_rxclk_carry_refused` at the event epoch, and the question
  the card actually asks is whether that refusal buys anything the OFF arm did not
  already have.

### 11.6 The table

Eight legs, 61 epochs each, all exit 0, all `1 event(s) armed, 1 fired
(rx_clock_step=1), 0 never fired`, all ledgering
`INJECT,rx_clock_step,,S21000SIM,<step>,,injected,rx_clock_step_onset;sats=18;rows=108`
(18 satellites × 3 bands × {code, phase} = 108 observables at that station, and at no
other).  `absorb` is `clk_raw_d / step_m` — the fraction of the injected step the
station's estimated receiver clock took, on the RAW clock error, not the contrast
(§11.4's 1/N caveat: the contrast can structurally show at most 59/60 of it, and it
does, to five digits: `clk_d` 0.49167 for a 0.5 m step).  `oth_d` is the station's
OTHER arcs' mean |level error|, event − null — the frame reaction.

| arm | absorb +0 | +1 | +2 | +45 | res rms (m) | oth_d rms (cyc) | ZWD_d pk (m) | ion_d pk (m) | Call_d rms (m) | cons_d +45 (m) |
|---|---|---|---|---|---|---|---|---|---|---|
| **null floor (ON null − OFF null)** | — | — | — | — | — | **0.0016575** | −0.000588 | −0.005882 | 0.0283 | **0.0218** |
| 0.05 m OFF | **1.000000** | 1.000000 | 1.000000 | 1.000000 | 1.4e-06 | 6.8e-06 | 8e-06 | 1.1e-05 | 1.1e-07 | 0.0000 |
| 0.05 m ON | 0.998960 | 1.000080 | 1.000080 | 1.000020 | 1.4e-06 | 4.9e-06 | 2.5e-05 | 3.5e-05 | 1.9e-06 | 0.0000 |
| 0.5 m OFF | **1.000000** | 1.000000 | 1.000000 | 1.000006 | 2.4e-06 | 1.5e-05 | 1.4e-05 | 5.4e-05 | 1.2e-06 | 0.0000 |
| 0.5 m ON | 0.998960 | 1.000090 | 1.000090 | 1.000026 | 1.5e-05 | 1.7e-05 | 2.5e-04 | 3.5e-04 | 1.9e-05 | −0.0001 |
| 10 m OFF | **1.000000** | 1.000000 | 0.999997 | 0.999998 | 3.1e-05 | 3.8e-05 | 3.9e-05 | 4.5e-05 | 4.6e-06 | 0.0000 |
| 10 m ON *(1 refusal)* | 0.998962 | 0.999945 | 0.999979 | 0.999999 | 5.6e-05 | 1.8e-04 | 5.0e-03 | 7.0e-03 | 4.0e-04 | −0.0018 |

**Read the first row against the rest.**  Every number in every event row except the
clock's own is **at or far below** the two nulls' difference, which is the floor below
which nothing means anything.

**1. OFF absorbs the step 100.000 % in the epoch it lands, at every size.**  Not at
+1 — at **+0**, the epoch the corrupted observables arrive.  Faster and cleaner than
§6(6)'s `sat_clock_step` precedent (100.4 % in one epoch, P(L1C) −0.6 mm).  Settled
residual 1.4–31 µm on steps of 0.05–10 m, i.e. **3–6 ppm of the step**.  And nothing
else in the network moves at all: the station's other arcs 5e-06…3.8e-05 cyc (**44–220×
below the null floor**), its ZWD ≤ 3.9e-05 m, its slant iono ≤ 5.4e-05 m, the emitted
satellite clocks 1e-06…5e-06 m rms with a per-epoch MEAN shift of ≤ 5e-06 m, and the
consumer 0.0000 m to four decimals.

That is the mechanism §10.5 named, now measured on its own event: **the network's
receiver clock is a free per-epoch parameter with no temporal prior, so a
range-direction step at one station is EXACTLY estimable there and leaks nowhere.**
The event is not hard; it is *free*.

**2. ON is not neutral — it is worse, by exactly the carried prior's share.**
`absorb_0` is **0.998962 at all three sizes**, to five digits.  That scale-linearity is
the whole story: the carried prior takes a fixed **0.104 %** of whatever step arrives
(0.5 mm at 0.05 m, 5.2 mm at 0.5 m, 10.4 mm at 10 m), because that is its weight in
that epoch's normal equations.  The frame reaction grows with the leak (`oth_d` rms
3.8e-05 → 1.8e-04 cyc at 10 m; ZWD peak 3.9e-05 → 5.0e-03 m; consumer 0.0000 →
−0.0018 m).  All of it is still below the null floor, so the honest statement is **no
measurable benefit and a measurable-but-sub-floor cost** — but the SIGN is unambiguous
and it is the same sign at every size.

**3. The jump gate is too loose to see a realistic receiver step, and its response is
one epoch late by construction.**  §11.5's census puts the settled gate at ≈ 3.1 m
(k = 10 σ ⇒ σ ≈ 0.31 m), so 0.5 m is 6× below it.  The 10 m pair reached it and behaved
exactly as pre-registered on the counter — **`n_rxclk_carry_refused` 0 → 1**, and the
gate-test count 3660 → 3659 because the erased station has no predecessor to test
against on the following epoch, which is the decoupling and re-coupling edges both
showing up in one number.  But the refusal fires at **harvest, after the solve**: the
epoch the step lands on has already folded the stale prior, so the 10.4 mm leak at +0
happens **with the refusal armed**, and the gate only cleans up from +1 (0.55 mm) on.
A jump response that cannot act on the epoch of the jump cannot beat having no prior
at all — which is what the OFF column is.

⚠ **OPEN (invariant #7):** the refusal has a counter and **no identity** — no event
row, no station, no epoch.  On this bed the attribution was available only because
exactly one station was stepped and the count went 0 → 1.  On a field lineage
"N refusals" cannot be attributed to a receiver, and that is the gap to close if this
gate is ever armed for real.

### 11.7 Verdict

**`network.carry_rx_clock_in_block` is KEEP_OFF — outcome (b), inert on its own target
event, with a mechanism.**

The receiver-clock step was the one event this card was designed for, and the producer
already handles it perfectly without the card: the station's free per-epoch clock
absorbs 100.000 % of the step in the epoch it arrives, at 0.05 m, 0.5 m and 10 m
alike, and no other state in the network moves by more than a few hundredths of the
null-vs-null floor.  Arming the carry makes that *worse* by a scale-linear 0.104 % of
the step, because the carried prior is a stiff opinion about a quantity the data
determines exactly, and the jump gate — 3.1 m at the settled k = 10 σ, i.e. blind to
anything smaller than a 10 ns receiver step — fires only after the solve that already
paid the leak.  §10 found the knob inert on slip, `arc_erase` and a cold handover and
closed with "inert where measured, untested where designed"; it is now tested where
designed, and the answer is the same with the sign against it.

What would change this verdict is a receiver step the OFF arm CANNOT absorb — one
large enough to break the linearization or to trip the producer's own admission
screens (a 1 ms / 299 792 m jump is five decades above the gate), or a station whose
geometry is too thin for the clock to be estimable epoch-by-epoch.  Neither is
reachable on this bed, and neither is this knob's advertised regime.

### 11.8 Byte identity, the suite, and reproducing

* **Byte identity (invariant #6).**  `config/test_suite/_golden_sim_off60.json` with
  ONLY `network.product_output_path` repointed, 61-epoch cold golden smoke, this
  branch's Release exe (`gnss_processor_rxstep.exe`, sha256 `207C5374…`):
  `products.txt` sha256
  **`3BF7831B29916C7A1E20434AC91F2ADFEB74EB06974560DF6E050FC43AC3F357`** — the exact
  hash §6 and §10.7 record for main HEAD.  Both cards are error-path-only or
  strictly armed-only, so an unarmed run cannot reach either.
* **Suite: 1796 cases / 245 suites, 1779 passed, 0 failed, 17 skipped** (fixture-gated;
  `data/` is not bootstrapped in this worktree) — main's 1785 plus this branch's 11.
* **Card A verified end to end**, not only by unit test: a leg relaunched in place
  (`runs/_hb1probe`, a copy of a run `null_off` leg whose `seed.txt` its own run had
  already overwritten) now exits **1**, prints the single line

  > `[main_log] [error] Network (RTB-7d/HB-1): network.state_snapshot_require is ARMED
  > and the snapshot is NOT OLDER than the first epoch this run would process — … (age
  > 0.000 s …).  The run is REFUSED BEFORE ITS FIRST EPOCH … Re-seed … event_leg.py
  > rerun <legdir> …`

  and emits **no** `COLD START` line.  Before the fix the same input printed
  `COLD START` and fast-failed at `0xC0000409`.

```powershell
# the legs (OFF arm on the OFF seed, ON arm on the ON seed — §10.1's fingerprint rule)
python scripts\event_leg.py make-leg --seed-run runs/evseed_quiet6h --snapshot-ep 665 `
       --epochs 60 --name s2b_rxstep50_off --at-ep 10 `
       --event '{"kind":"rx_clock_step","station":"S21000SIM","step_m":0.5}'
python scripts\event_frame_grade.py runs/evleg_s2b_rxstep50_off --null runs/evleg_s2b_null_off `
       --fixture C:/GnssProcessor/runs/simnet60_quiet_wu93_24h --sat G10
```

`--sat` is required for this kind: a station-wide step names no satellite, which is the
event.  `--step-m` overrides the leg's own `step_m` (use it to grade a non-step leg on
the same axis, or to check the arithmetic).

