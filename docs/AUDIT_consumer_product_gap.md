# AUDIT — consumer behaviour across a producer product gap (read-only)

*2026-07-27 (Opus 5). Read-only: no file in `src/`, `include/`, `tests/` or `config/` was
modified. No binary was run. The 305-station guards chain was live throughout and was not
disturbed.*

**Question.** With the Phase-2.2 SH health gate now withholding emission for hours at a time,
what does a consumer actually do inside the gap — does it apply C1, how long does it coast,
and does it ever learn that the producer is deliberately withholding?

**Convention:** `[M]` measured/read first-hand this session · `[E]` estimated · `[?]` unverified.
Line numbers re-grepped 2026-07-27; they drift under concurrent sessions.

---

## 0. Answer in five lines

1. **C1 is dead weight today.** The full path exists and is arithmetically correct, but the
   only producer that populates a nonzero `c1` is `network.clock_model` (a documented NO-GO)
   or `network.emit_clock_rate` (built, default-OFF, never validated). Every stream on disk
   ships `c1 = 0`, so `c0 + c1·dt` degenerates to ZOH silently.
2. **The coasting horizon is 30 seconds**, not hours — `processing.ssr_max_age_s` default
   `30.0`, with the clock stale-hold rung **off by default**. C1 therefore only ever gets to
   extrapolate across ≤ 30 s, the regime where it is worth least.
3. **Expiry is graceful and loud**: the satellite falls back to broadcast-grade, it is not
   dropped. But the warn is **unthrottled per satellite per epoch** — a 2 h gap would emit
   ~7,700 warn lines `[E]`.
4. **The producer signals the withhold correctly.** Tonight's stream carries **278
   `withheld_sh` H heartbeats and zero E records** through the era `[M]`. The wire is honest.
5. **The consumer does nothing with it — this is the RTF-9 gap, and the code says so.** The
   sole reader is a one-shot log line; and the **replay driver never receives the status at
   all**, so the very tool that would grade tonight's stream is blind to it.

---

## 1. The gap that exists tonight `[M]`

`runs/day129_guards/leg01`, from `logs/network_epoch_status.csv` + `products.txt`:

| | |
|---|---|
| leg span | tow 327600 → 342780 (19:00 → 23:13), 507 epochs at 30 s |
| **first withhold era** | tow **327600 → 334740**, **239 epochs = 1.99 h**, contiguous |
| later withholds | 14, 19, 2, 4 epochs (0.12 / 0.16 / 0.02 / 0.03 h) |
| total withheld | **278 / 507 = 54.8 %** |
| `products.txt` | 116,170 lines · **521 H records** (278 `withheld_sh`, 243 `ok`) · **243 E records** |
| E records inside the era | **0** |
| H records inside the era | **237 of 239 epochs** |

The leg **resumed at 19:00 into an already-out-of-band SH state** and withheld continuously
for 1.99 h, releasing at 334770 — 750 s *after* the 20:47:00 (tow 334020) excursion it was
armed to catch.

**The producer side is sound.** `NetworkEmitStore.cpp:120-140` early-returns on
`sh_gate_engaged` — no store push, no product write, no closed-loop consume — but still
writes the H heartbeat (`:129-137`), exactly so "a live tail can tell 'SH gate withheld' from
'producer died'." The bytes confirm it worked: 237 heartbeats across a 2 h silence.

---

## 2. Does the consumer apply C1? The path, end to end

**Yes, the arithmetic is live and correct.** `SsrCorrectionStore.cpp:651-654`:

```cpp
double SsrCorrectionStore::applyClockToBias(double c0, double c1, double c2,
                                            ..., double dt, ...) {
    const double dC_m = c0 + c1 * dt + c2 * dt * dt + hr_clock;   // metres
```

- **Units/sign:** `c1` is m/s; `dt` is `query − content-epoch` seconds, forward-positive.
- **The dt base is the SSR *content* epoch, not receive time** — deliberate, and correct:
  `getOrbitClock` sets `out.epoch_time = combined->epoch_time` (`:157-158`) on the combined
  branch and `= clock->epoch_time` (`:179-180`) on the separate-clock branch, with the
  comment that a separate clock "MUST use the clock epoch (its dt is c1/c2-sensitive)". The
  network producer pushes **separate** clock records, so the second branch is ours.
- **Where c1 is set:** `:141` (combined) and `:175` (separate clock). Both unconditional on
  the record's own `c1` field.
- **Wire round-trip:** writer `NetworkProductIO.cpp:545-548`, reader `:142-151`, header
  declaration `:483`. Positional field 4, written unconditionally when disc counters are on,
  otherwise only when nonzero.

**And it is inert.** The single upstream assignment is `NetworkProductEmitter.cpp:241`
(`rec.c1 = s.clock_c1_m_s`), fed from `NetworkSolPrep.cpp:214` (clock_model) or `:236`
(emit_clock_rate). Both toggles default false, so `clock_c1_m_s ≡ 0` and the writer's
`else if (s.clock_c1_m_s != 0.0)` at `:547` omits the field entirely.

> **Finding A1 — the ZOH is silent.** A consumer applying `c0 + 0·dt` is doing ZOH and
> nothing anywhere says so. There is no counter, no log line, and no trace column recording
> "this stream carries no rate term." Combined with §3's 30 s horizon, a user could
> reasonably believe corrections are being propagated when they are being held constant and
> then dropped.

---

## 3. The staleness horizon

All defaults from `src/core/config/GnssConfigLoad.cpp` `[M]`:

| channel | age-checked? | knob | default | at expiry | observable? |
|---|---|---|---|---|---|
| clock (C0/C1/C2) | **yes** | `processing.ssr_max_age_s` `:407` | **30.0 s** | fall back to broadcast-grade | **yes** — unthrottled warn |
| clock stale-hold rung | **off** | `ssr_clock_stale_hold_factor` `:454` | **0.0 = off** | hard cliff at 30 s, no middle rung | n/a |
| orbit (RAC + rates) | yes | same `ssr_max_age_s` | 30.0 s | RAC overlay not applied | via the same path |
| code/phase bias | **NO by default** | `enable_ssr_bias_max_age` `:414` | **false** | *served indefinitely* | no |
| bias, when armed | yes | `ssr_bias_max_age_s` `:422` | = `ssr_max_age_s` (30 s) | drop past `factor ×` | yes |
| bias stale-hold | yes | `ssr_bias_stale_hold_factor` `:433` | **2.0** | held to 60 s, then dropped | yes |
| high-rate clock | yes | same `max_age_s` | 30.0 s | term omitted | no |

**The clock/bias asymmetry is real but has been fixed on the paths that matter.**
`enable_ssr_bias_max_age` defaults **false**, so on a generic path the clock expires at 30 s
while biases coast forever. Two auto-arms close it under actual network consumption:

- `GnssConfigLoadNetwork.cpp:3542` — `deriveClosedLoopConsumerConfig` arms the gate whenever
  `consume_network_products` is set (respecting an explicit user `false`).
- `:3577` — `applyNetworkConsumerProfile` does the same for a standalone consumer.

The `:3531-3540` comment names precisely this audit's scenario: without the arm the consumer
*"silently applied unboundedly stale biases across producer withhold gaps."* It was found in
the Jul-2026 closed-loop audit and fixed. **Both paths a user would realistically take are
covered.** ✔

**The consequence for C1, stated once:** with `ssr_clock_stale_hold_factor = 0`, the clock is
either fresher than 30 s (applied) or rejected. **The `c1·dt` term is therefore only ever
evaluated for `dt ∈ [0, 30] s`.** Arming the rung at, say, 4.0 widens it to 120 s — still far
inside the random-walk-dominated regime (`SPEC_c1_clock_rate_validation.md` §3.2 measures the
drift/RW crossover at ~9.5 min). There is no configuration in which C1 extrapolates across a
2 h era: the store expires first, by design.

---

## 4. Coasting — what happens inside the era, per path

The three consumer paths behave **differently**, which matters for anyone grading tonight's
stream.

### 4.1 Real-time tail (`ApplicationRealtime`)

`GnssCorrections.cpp:180-222` `[M]`:

```cpp
const double apply_window = SsrCorrectionStore::clockHoldWindow(ssr_max_age, clk_hold);
const auto ssr = ssrStore().getOrbitClock(satellite, time, apply_window);
if (!ssr) {
    ... log->warn("SSR for {} ... REJECTED stale: |age|={:.1f}s > {:.1f}s "
                  "(obs↔SSR desync) — falling back to broadcast-grade", ...);
}
...
if (std::abs(clock_corr) < 1e-12 && bc) { clock_corr = bc->bias; used_broadcast = true; }
```

Expiry has **three outcomes**, and which one applies is decided by whether a precise CLK
sample covers the epoch (`interpolateClock`, `:160`, MAX_WINDOW 3600 s at `:398`):

- **(a) Precise CLK covers the epoch — the DOY-129 / post-mode case.** `used_broadcast`
  stays false (`:219-222`), so `broadcast_grade = false` (`:378`), the phase gate at
  `PppModel.cpp:721-736` never fires and no satellite is dropped. **The satellite keeps code
  AND phase at full weight and silently swaps its clock datum from the network frame to the
  CLK-product frame.** No flag, no deweighting, no reset. *This is the operative failure
  mode, and the warn's wording — "falling back to broadcast-grade" — is misleading here:
  the fallback is to the precise CLK product, not to broadcast.*
- **(b) No precise CLK, empty live store (post-processing).** `clock_corr` stays 0.0 and
  **returning 0.0 is the drop signal**: `ModelGeometry.cpp:496-508` / `:553-559` turn
  `|sat_clk| < 1e-12` into `drop_reason = DropReason::NoProduct`.
- **(c) Real-time on a broadcast baseline.** `broadcast_grade = true` ⇒ carrier phase
  stripped per satellite (`PppModel.cpp:721-729`); full drop only under
  `drop_broadcast_grade` (default false, armed in `config/realtime_*.json`).

Tonight's configuration is settled as **case (a)**: the guards config is `app_mode: "post"`,
`deriveClosedLoopConsumerConfig` copies it verbatim (`GnssConfigLoadNetwork.cpp:3520`), and
`realtimeUsesLiveOrbitClockSource()` is false outside RealTime (`GnssConfig.hpp:4259-4263`),
so precise SP3/CLK are loaded.

> **Correction to an earlier reading in this audit.** I first recorded the expiry as a
> uniform "degrade to broadcast-grade, never drop." That is right only for case (a), and even
> there the destination is the CLK frame rather than broadcast. Cases (b) and (c) do drop or
> strip. The generalisation was wrong.

> **Finding A2 — the stale warn is unthrottled.** The `log->warn` sits in the per-satellite,
> per-epoch query path with no `once` flag, no rate limit, and no epoch-level dedup (contrast
> the deliberate edge-triggering used for `T5_clock_rate` at `NetworkSolPrep.cpp:238` and the
> producer-status transition log at `RealTimeEngine.cpp:1673`). 32 satellites × 39 internal-era
> epochs ⇒ ~1,250 identical warns `[E]`; on a sustained mid-stream outage it scales with the
> gap. It buries the *one* line that matters (the producer-status transition, §5).
>
> **And it inverts on a cold start.** The warn is nested inside
> `if (auto se = ssrStore().getSsrEpoch(satellite))` (`:187`), so with an **empty** store —
> era 1 — there is no stored epoch, no warn, and the consumer degrades in total silence. The
> logging is loud exactly where the store is merely stale and mute exactly where it is empty.

> **Finding A3 (resolved, was `[?]`) — a 30 s bias-without-clock window on every outage.**
> Clock and bias expire on different clocks: 30 s vs 60 s under the armed gate (orbit shares
> the clock's). Between t+30 s and t+60 s a satellite carries **network phase and code biases
> at full weight against an a-priori CLK clock** — biases estimated in the network clock
> frame, now stacked on a different frame, with no R inflation. It compounds: the
> state-allocation coverage predicates read the same 60 s bias product
> (`MeasurementModel.cpp:199-203`, `:227-232`), so the consumer *also* keeps suppressing its
> own satellite-bias states on the strength of a bias whose clock partner is already gone.
> With the gate unarmed this window is unbounded rather than 30 s.

### 4.2 Consumer replay (`ApplicationConsumerReplay`) — the tool for grading tonight

The replay loop iterates **RINEX observation epochs**, not product epochs
(`ApplicationConsumerReplay.cpp:288-330`):

```cpp
while (rec_cursor < records.size()
       && continuousGpsSeconds(records[rec_cursor].epoch.epoch_time) <= now_s + sync_tol) {
    NetworkProductReader::replay(records[rec_cursor], store, emitter);
    ++rec_cursor; ++replayed_here;
}
if (replayed_here == 0) ++n_no_products;
```

**Era 1 is not a coasting case at all — it is a cold-start famine.** `products.txt` line 6 is
already `H,2313,327599.99998664856,withheld_sh,0.0000,0,*`; lines 6–244 are all withheld; the
first E record is line 246 at tow 334770 `[M]`. **No C/B/P/U record exists before it**, so the
consumer's store is *empty*, not stale — there is nothing to coast from. And the driver
handles it: `ApplicationConsumerReplay.cpp:288-291` fast-forwards past every observation epoch
earlier than `records.front()`:

```cpp
// Nothing to consume before the product stream starts ... Fast-forward instead.
if (now_s + sync_tol < continuousGpsSeconds(records.front().epoch.epoch_time)) continue;
```

⇒ **the 1.99 h leading era is skipped entirely and never enters the grade.** (The comment's
stated rationale — "every satellite dropped" — is itself wrong for this setup, which is
case (a); but the skip behaviour is correct and is what matters.)

The exposure is the **four internal eras**, which fall inside the product stream and are
*not* skipped: 14 + 19 + 2 + 4 = **39 epochs = 19.5 min** `[M]`. There, `replayed_here == 0`,
`n_no_products` counts up, and `consumer.processEpoch(...)` still runs against an expired
store — §4.1 case (a).

> **Finding A4 (revised down) — internal-era epochs are graded with expired corrections and
> nothing marks them.** `n_no_products` is reported once, at `:411`. `closed_loop_trace.csv`
> (`:257-258`) carries `week,tow,n_sats,ar_ran,…,pos_err_3d_m,east_m,north_m,up_m,ar_gate` —
> **no column distinguishes "graded against fresh corrections" from "graded against expired
> ones."** The C++ hour-band accumulator (`:351-353`) is unconditional after `processEpoch`,
> and `q5_reanchor_hourly.py` does not filter on freshness `[M]`. So ~19.5 min of
> CLK-frame positioning folds into the hourly bands unlabelled.
>
> **Severity correction.** I first wrote this as "1.99 h silently averaged in," which was
> wrong — the fast-forward excludes the leading era. The real exposure is ~19.5 min inside a
> ~4 h grade: still worth excluding, no longer grade-corrupting.
>
> **Workaround tonight, no code change:** both files are keyed by `tow` — join
> `closed_loop_trace.csv` against `logs/network_epoch_status.csv` and drop
> `disposition == sh_gate_withheld` before grading.

### 4.3 In-process closed-loop harness

Different again: `NetworkEmitStore.cpp:120` early-returns **before** the closed-loop consume
(the `:113-119` comment: *"no store push, no product write, no closed-loop consume/iono
supply, no n_emitted advance"*). So the harness consumer **does not run at all** on withheld
epochs — it has no coasting behaviour, it simply has no samples there.

> **Finding A5 — the two harnesses disagree about what a withheld epoch means.** Replay
> *runs* the consumer with expired corrections and folds the result into the grade; the
> in-process harness *skips* it. Grading the same era through the two paths gives different
> answers, and neither is labelled. Pick one convention and make it explicit before comparing
> any A/B across a withhold era.

---

## 5. The `withheld_sh` H-state — emitted, parsed, and inert (**the RTF-9 gap**)

**Emitted ✔.** `NetworkEmitStore.cpp:129-137` writes `state = WithheldShGate`, `withheld = "*"`.
Wire grammar `NetworkProductIO.cpp:490`:
`H,week,tow,status,sigma0,n_sats_published,withheld`. 278 such records tonight `[M]`.

**Parsed ✔.** `SsrCorrectionStore.cpp:520-528`:
`{Ok, Partial, WithheldSigma0, WithheldGate, WithheldShGate, Unknown}`, forward-compatible
(unknown tokens → `Unknown`, never fatal).

**Delivered — on one path only.** `NetworkProductIO.cpp:212-235`: an H line always begins a
new epoch unit, updates `latest_status`, and is stashed as `pending_h`. Then `:127-134`:

```cpp
// Attach only on an exact week/tow match — the emitted epoch's own heartbeat.
// A withheld epoch's standalone H (different time) is dropped here;
// it already updated latest_status when consumed.
```

So a withheld epoch's status survives **only** in `latest_status`, reachable via
`NetworkProductTail::lastStatus()` (`:916`) — the **live tail** API.

- **RT path:** `ApplicationRealtime.cpp:633-644` pulls it and calls
  `network_store->setProducerStatus(*hst)`. This is the **only** `setProducerStatus` call site
  in the tree `[M]`.
- **Replay path:** `ApplicationConsumerReplay.cpp` calls `NetworkProductReader::read(path,
  &file_contract, &products_ok)` — no status out-param — and never calls `setProducerStatus`.
  **The replay consumer's store status is permanently `nullopt`.**

**Acted upon — no.** The sole reader in the entire tree is `RealTimeEngine.cpp:1671-1688`:

```cpp
const auto pst = ssrStore().getProducerStatus();
if (pst && (!last_producer_status_ || *last_producer_status_ != pst->state)) {
    ...
    if (pst->state == State::Ok)  log->info ("Network producer status: {} -> {} ...");
    else                          log->warn ("Network producer status: {} -> {} ...");
    last_producer_status_ = pst->state;
}
```

An edge-triggered log line. Nothing else. The comment directly above states the boundary:

> *"Deeper integrity wiring — gating measurement usage or R-inflation on the producer status —
> is deliberately out of scope here (**RTF-9**)."*

> **Finding A6 — the RTF-9 gap, precisely.** The producer's integrity signal reaches the
> consumer's store and is *observable*, but is **inert in the estimator**: it gates no
> measurement, inflates no R, triggers no reset, and sets no flag any downstream code reads.
> Every non-`Ok` state — `partial`, `withheld_gate`, `withheld_sigma0`, `withheld_sh`,
> `Unknown` — takes the same `else` branch and is indistinguishable in effect. `withheld_sh`
> is a **new** state (Phase 2.2, Jul-27) added after RTF-9 was scoped, and it inherits that
> inertness by default rather than by decision.

> **Finding A7 — the replay path cannot see the state at all.** Even the log line is
> unavailable to `runConsumerReplay`. The consequence compounds A4: the tool the user will
> reach for to grade tonight's stream has **neither** the H-state **nor** a per-epoch gap
> column. It will produce an hourly position grade over the era with no indication that the
> era was corrections-free.

### 5.1 Four more gaps on the emit side (from the parallel fan-out, verified)

> **Finding A8 — the heartbeat is OFF by default.** `network_emit_epoch_status_record_{false}`
> (`GnssConfig.hpp:4698`, loader `GnssConfigLoadNetwork.cpp:1766-1768`), force-armed only by
> the service-mode bundle (`:3041-3042`). Tonight's leg arms it explicitly
> (`_day129_guards_conv.json:2931`). **The completed 0-gap reference stream contains zero H
> lines** `[M]`. So in a default configuration the entire signalling channel this section
> analyses does not exist, and a gap is byte-indistinguishable from producer death.

> **Finding A9 (latent, and this leg's exact shape) — a v2-contract leg opening inside a
> withhold era writes nothing at all.** Under `network.serialize_sbasis_contract`,
> `ApplicationNetworkSetup.cpp:1059-1071` sets `contract_pending` and constructs **no**
> writer; the deferred open is at `NetworkSolPrep.cpp:810-823` — ~670 lines *after* that
> function's SH-gate early return at `:140-141`. All four H write sites require a non-null
> `product_writer` (`NetworkEmitStore.cpp:130`, `:325`; `NetworkSolveGlue.cpp:566`, `:1220`).
> **A leg that opens with a 239-epoch withhold would therefore produce an empty directory for
> two hours** — losing precisely the signal RTA-10 exists to provide, at precisely the moment
> it is needed. Tonight escapes only because `serialize_sbasis_contract` defaults false
> (`GnssConfigTypes.hpp:729`) and `leg.json` omits it — confirmed by `products.txt:1` reading
> `# gnss_network_product v1` `[M]`. **But `frame=` / `cbpin=` / `agek=` are exactly what a
> real consumer needs, so the first configuration that adds them inherits this hole.**

> **Finding A10 — half the skip dispositions emit no H at all.** `sync_skip`
> (`ApplicationNetworkAssembly.cpp:528`, `:537`, `:1090`, `:1759`), `datum_failed`
> (`NetworkSolveGlue.cpp:300-307`) and `solve_failed` (`:681-691`) return without writing a
> heartbeat. Four of eight trace dispositions get one. Those are silent wire holes with the
> same byte signature as producer death — the gap RTA-10 closed for the withhold tiers is
> still open for the failure tiers.

> **Finding A11 — the withheld heartbeat drops the diagnosis it already has.** A withheld H
> ships `sigma0 = 0.0000, n_sats = 0, withheld = "*"` — one enum token of information.
> `sigma0` is 0 by **omission, not unavailability**: `sigma0_adapt` is in scope at
> `NetworkEmitStore.cpp:52` and used 210 lines later at `:340` for the *emitted* branch, and
> the `SH_GATE` trace event three lines earlier (`:122-126`) already carries
> `sh_gate_last_a00`, `sh_gate_last_rms` and `sh_gate_oob_run` — none of which reaches the
> wire. Since the grammar overloads 0 as "unavailable", a consumer cannot even distinguish
> "not measured" from "not written."

> **Finding A12 — SH-gate state does not survive a chain-leg boundary.**
> `grep sh_gate_ src/core/products/NetworkStateSnapshot.cpp` returns **zero matches** `[M]`,
> so neither `n_sh_gate_withheld` nor the gate's engaged / out-of-band-run state crosses a
> resume — while the rank-gate's `n_gate_withheld` does (`NetworkStateSnapshot.hpp:50`,
> `:117`; `.cpp:122`, `:261`). Relatedly, `network_service_health.csv` publishes
> `n_gate_withheld` (the rank/null-space counter) and reads **0 on every row** while 278
> epochs were withheld `[M]`; the SH tier's counter is a different member
> (`NetworkRunContext.hpp:594`). This is a **documented** coverage boundary
> (`docs/rt_service_operations.md:44-48`, `:109-114`), not an invariant-#4 defect — the
> run-end counter does exist and fire (`NetworkTeardown.cpp:775-793`) — but any operator
> alarm keyed on the health CSV's withhold counters is blind to the SH tier.

**Note on scope.** RTF-9 as scoped in `docs/rtf_opus_first_handoff.md` is *MDB-anchored
integrity* — an analytic Baarda floor folded into the emitted URA and thence the AR gate. That
is a **producer-side** integrity product. The gap found here is the **consumer-side**
consumption of an integrity signal that already exists on the wire. They are complementary,
and the second is far cheaper: the signal is already emitted, already parsed, and already in
the store on the RT path.

---

## 6. Findings, ranked

| # | finding | severity | cost to close |
|---|---|---|---|
| **A9** | v2-contract leg opening inside a withhold era writes **nothing** for the whole era | **high (latent)** — triggered by the first `frame=`/`cbpin=` config | construct the writer eagerly, or write H through a pending contract |
| **A3** | 30 s bias-without-clock window at full weight, also suppressing consumer sat-bias states | **high** — silent frame mismatch on every outage | tie the bias gate to the clock gate, or inflate R across the window |
| **A7** | replay never receives producer status; blind to `withheld_sh` | high | thread `latest_status` out of `NetworkProductReader::read` → `setProducerStatus` |
| **A8** | heartbeat OFF by default; the reference stream has zero H lines | high | default it on under `emit_epoch_status`-capable configs |
| **A6** | producer status inert in the estimator (RTF-9) | medium — by decision, but the decision predates `withheld_sh` | scope a consumer-side integrity card |
| **A10** | `sync_skip` / `datum_failed` / `solve_failed` emit no H — silent wire holes | medium | one heartbeat write per skip path |
| **A5** | replay vs in-process harness disagree on withheld-epoch semantics | medium — invalidates cross-harness A/B | pick a convention, document it |
| **A4** | ~19.5 min of internal-era epochs graded with expired corrections, unlabelled | medium *(revised down from high)* | freshness column in `closed_loop_trace.csv`; join workaround available now |
| **A2** | stale warn unthrottled when stale, **absent** when the store is empty | medium | edge-trigger per satellite; warn on empty-store too |
| **A11** | withheld H drops `sigma0` / a00 / oob_run it already has in scope | low–medium | populate the fields |
| **A12** | SH-gate state and counter do not cross a chain-leg boundary | low–medium | add to `NetworkStateSnapshot` |
| **A1** | `c1 = 0` ZOH is silent | low | one summary line when a stream carries no rate term |

**Not a finding — verified sound:** the wire grammar (535/535 H lines at exactly 7 fields);
the withheld-epoch heartbeat (zero missing beats, modal spacing 29.999998 s); `#E == #ok-H`
in every scan with zero E under any withheld H; the content-epoch `dt` base; the
negative-age/future-record rejection (`SsrCorrectionStore.cpp:24-42`); the bias-gate auto-arm
on both consumer paths (`GnssConfigLoadNetwork.cpp:3542`, `:3577`); the SH gate skipping **no**
QC seam; the run-end counter existing and firing (`NetworkTeardown.cpp:775-793`).

**Superseded by verification — do not carry forward:** "expiry always degrades, never drops"
(three cases, §4.1); "the 1.99 h era is silently graded" (fast-forwarded, §4.2); "the clock/orbit
window asymmetry bites tonight" (the network stream is **clock-only** — `pushOrbit`/`pushCombined`
have no non-test caller outside the RTCM decode path, so there is no orbit overlay to expire).

---

## 7. What I could not verify

1. **A3's magnitude** — the *existence* of the 30 s bias-without-clock window is now
   confirmed in code, but whether the resulting frame mismatch is common-mode (absorbed by
   the receiver clock) or per-satellite is **not** established. That decides whether it is
   cosmetic or a real position error. `[?]`
2. **Live RT behaviour** — everything in §4.1 is read from source; no RT session was run.
3. **Position-domain magnitude** of the internal eras — needs a replay pass over
   `leg01/products.txt` via `network.closed_loop.replay_products_path`. Note `leg.json` has
   **zero** `closed_loop`/`consumer` keys and `closed_loop_trace.csv` is absent from
   `leg01/logs/` `[M]`, so nothing about consumer behaviour can be read out of tonight's run
   as it stands — it requires a separate replay arm.
4. **Sub-30 s behaviour** — the product cadence is 30 s, so no measurement here covers
   `dt < 30 s`, which is the C1 card's actual stated target. See the companion spec §3.6.
5. **Authorial intent** on the emitted-vs-solved split in the C1 estimator — whether the
   warmup gate's emitted-epoch keying was a choice or an inherited copy. Unverifiable from
   code; it does not change the defect.
6. **The two H records missing from era 1** (237 of 239). Not chased; a later scan counted
   the era's heartbeats as complete with zero missing beats, so this is likely a scan-boundary
   artifact of reading a growing file rather than a real hole. `[?]`

---

## 8. Method note

Sections 1–5 were established first-hand. They were then cross-checked by a 77-agent
read-only fan-out (5 parallel area audits → 71 load-bearing claims → one adversarial
refutation pass each → synthesis): **38 confirmed, 33 corrected.** Three of those corrections
changed conclusions in this document and are marked inline (§4.1 expiry cases, §4.2 severity,
§5.1 A8–A12). The full dossier, including the claims that were refuted outright, is at
`docs/REPORT_consumer_gap_dossier.md`. Independent confirmation of the companion spec's central measurement
arrived by a different method (p90 of datum-reduced c0 rather than robust rms of pair
differences): ZOH error growing as `dt^0.581` — the random-walk signature — and an optimal
shrinkage weight on the rate term of **α\* = 0.00 at every window tested**.
