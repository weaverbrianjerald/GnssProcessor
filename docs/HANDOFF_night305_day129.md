# Handoff — DOY-129 305-station continuous network run

**Status:** run COMPLETE. Full GPS day 2024-129 processed. Artifacts preserved.
**Audience:** a follow-on investigator (Fable) picking this up cold.
**Author's note:** numbers tagged **[M]** I measured and independently re-verified;
**[E]** is estimated/extrapolated; **[?]** is hypothesis I could not confirm.

---

## 1. What exists

| artifact | path | note |
|---|---|---|
| Run tree | `runs/day129_cont/seg01/` | 2160 epochs, 06:00:00→23:59:30, ONE continuous segment |
| Final state | `val_out/night305_day129_eod_immutable.txt` | tow 345570, `n_emitted` 2880 = full day |
| Product stream | `val_out/night305_day129_products.txt` | 40 MB, 2160 epochs, **0 gaps** [M] |
| 6 h seed (untouched) | `val_out/night305_conv_immutable.txt` | tow 280770, `n_emitted` 720 |
| Supervisor | `scripts/chain_run_day129.ps1` | stateless/resumable, checkpoint watchdog |
| Leg config gen | `scripts/chain_make_leg.py` | self-correcting offset recurrence |
| Status | `scripts/chain_status.py` | one-screen position/counters |
| Grader | `scripts/chain_stability.py` | **rewritten** to grade CLK+IF(CB), see §3 |

Run facts [M]: exit 0 · 24.69 h wall · 41.155 s/epoch · `n_skipped` / `n_gate_withheld` /
`n_sigma0_withheld` all **0** · **0 errors, 0 criticals** · zero unplanned seams (the supervisor
never had to resume). Peak RSS 79.4 GB of 137 GB; full-day 305-station parse ≈ 7.5 min.

This is the repo's **first genuinely continuous 305-station reference**. Nothing else in the tree
provides a continuous-vs-resumed control, which is why several prior conclusions drawn from short
resumed legs turn out to be artifacts (§4).

---

## 2. Reproduce anything here

```bash
python C:\GnssProcessor\scripts\chain_status.py
```
```bash
python C:\GnssProcessor\scripts\chain_stability.py --csv hourly.csv
```

Raw inputs: `runs/day129_cont/seg01/logs/{network_solution.csv, network_epoch_status.csv,
network_trace.csv, network_vtec_sh.csv, main_log.log}`.

---

## 3. THE MEASUREMENT TRAP — read this before touching any statistic

I lost a day of monitoring to this. Do not repeat it.

1. **`rms_clock_m` (network_trace.csv) is not an error metric.** It is RMS *about zero of the raw
   satellite clock state* — `NetworkStashStatus.cpp:149-155`. Datum-dependent. Grading on it
   violates standing invariant #5. `scripts/qc_dashboard.py:55` reads only this file.
2. **The datum-free clock stats already exist and nothing reads them.** `clock_median_m`,
   `clock_std_m`, `worst_sat`, `worst_dev_m` — `NetworkStashStatus.cpp:176-195` — written to
   `network_epoch_status.csv` on all 2160 rows.
3. **They are still insufficient.** The clock datum pins one satellite's clock to exactly
   `0.0000` (**G01 on 2051 of 2160 epochs; G02 on 109** [M]). Damage landing in the *pinned
   satellite's code bias* is invisible to every clock-only statistic. On this run that is
   precisely what happened.

**Grade this, and only this:**

```
total_IF(sat) = CLK(sat) + 2.545728·CB(sat,C1W) − 1.545728·CB(sat,C2W)
```
taken **between satellites** (deviation from the per-epoch median over **all** satellites,
including the pivot — a consumer cannot exclude it, it applies its `C` and `B` lines like any
other). The residual gauge freedom is an epoch-common constant, absorbed by the user clock.
Do **not** grade PB this way — it is integer-ambiguous and dominated by whole-cycle branch offsets.

---

## 4. Established findings

### 4.1 The producer does NOT drift over a day — prior "doubling" claim is refuted

Excursion epochs removed, 1714 quiescent epochs over 14.6 h [M]:

| | first third | last third | ratio |
|---|---|---|---|
| between-sat spread of total IF | 7.574 m | 5.956 m | **0.786** |

The spread *shrinks* ~21%. A prior conclusion that `rms_clock_m` "roughly doubles within a leg
(4.9 → 12 m over 4.5 h)" was two artifacts compounded: (a) event contamination, and (b) the
post-seam recovery *undershoot* — after a warm resume the filter is briefly **overconfident**
(restored Pᶻ is DIAGONAL-ONLY, cross-covariances start at zero), so ep20 is a local *minimum*
~30 % below the true floor and any slope fitted from there reads the climb-back as drift.
Measured recovery shape [M]: `ep0=116.8, ep5=17.0, ep20=5.14, ep60=6.49, ep120=7.70, ep240=11.84`.

**Method warning:** a single Theil-Sen slope is invalid on this series — refits over different
windows gave +0.09 / −0.08 / +0.48 / +0.11 m/h [M]. Judge by hourly level bands, not slopes.

### 4.2 Three SH-VTEC collapses; magnitude does not predict damage

Each is an abrupt **single-epoch** `a00` step, no precursor in `n_present`, `n_los`, slip counts,
or `datum_changed` [M].

| onset tow | UTC | a00 | peak `max_clock_m` | persistent damage |
|---|---|---|---|---|
| 286320 | 07:32:00 | +1236 step → up to **2151 TECU**, 80.5 min | 521 m | **none** (4-epoch recovery) |
| 328890 | 19:21:30 | +205.6 step → 239.9 TECU | 1336 m | none |
| **334020** | **20:47:00** | **−341.9 step → −302.6 TECU** | **5636 m** | **YES** |

The *largest* excursion of the day cost nothing; the *smallest* did the lasting harm. What
distinguishes 334020: the excitation was **one-sided in clock space** — all 32 clocks ≤ 0 with the
maximum exactly `0.000` (the pin); common/differential ratio **5.4** vs **0.05** (two-sided) for
the morning event [M].

**Count out-of-band EPOCHS, not warnings.** The warn is edge-triggered (`NetworkShHealth.cpp:87`,
`!in_band && (was_in_band || n_emitted % 60 == 0)`): **9 warnings vs 214 out-of-band epochs
(9.91 %)** — a 23.8× under-read [M].

### 4.3 The persistent damage, and its real shape

At 334020, G01's IF code bias stepped **15.488 → 64.553 m in one 30 s epoch**
(C1W 8.805→39.336, C2W 4.481→23.023), `disc_counter` **not** bumped, clock pinned at 0.0000 [M].
Still **+42.68 m** off the constellation median at 23:59:30 [M]; log-linear decay τ = 3.79 h
(R² = 0.983), extrapolating to ~5.5 h [E] to get under 10 m. The day ends short.

`network.code_bias_disc_jump_test` defaults **false** (`GnssConfigLoadNetwork.cpp:1814-1816`) and
is absent from `leg.json`, so a 49 m step signalled nothing — **even though the disc mechanism
fired 5234 times elsewhere that day** [M]. It is active and it missed the one step that mattered.
Worth understanding why.

**Separating "one satellite moved" from "the constellation redistributed."** Use fully datum-free
pairwise between-satellite differences over *consecutive* hours. Read two numbers: the **span**
(max−min across satellites) is genuine multi-satellite redistribution; a large but **uniform**
min/max offset with a near-baseline span means a *single* satellite moved and everything else
held. Consecutive-hour spans, whole day [M]:

| hours | span | min | max | reading |
|---|---|---|---|---|
| quiet hours 06→18 | **17.94 m** (median), range 10.9–22.1 | | | the producer's *normal* wander |
| hr18→hr19 | **43.12** | −42.43 | +0.69 | 2.4× baseline — real redistribution |
| hr19→hr20 | 33.47 | −23.71 | +9.76 | still elevated |
| hr20→hr21 | 30.71 | **−79.64** | **−48.93** | uniform ≈−64 m offset = **G01 alone moved**; span near baseline |
| hr21→hr22 | 26.60 | +7.78 | +34.38 | relaxing |
| hr22→hr23 | 22.81 | +1.25 | +24.06 | near baseline |

Two things follow, and the second is the uncomfortable one:

1. The event is real but **~2× the baseline, not the ~100× the raw 4643 m peak suggests.** The
   genuine multi-satellite redistribution peaks at hr18→hr19; by hr20→hr21 the signature is a
   *uniform* offset, i.e. the G01 shift with the rest of the constellation intact.
2. **The quiet baseline is already ~18 m of hour-to-hour pairwise wander**, with a 5–8 m
   between-satellite spread. That is the producer's *normal* state, and for PPP-RTK it is poor.
   The event aggravates an already-weak baseline rather than departing from a good one.

*(An earlier read of this as "six damaged satellites" was itself the median artifact warned about
in §3 — G02/G21/G28/G03 each appeared to move 17–23 m while their MUTUAL differences changed by
<5 m. They were fine; the median moved. `chain_stability.py` now reports only pairwise spans.)*

### 4.4 The producer is entirely unguarded

T2–T6 QC tiers had **zero opportunities** all run. `network.raim`, `network.emit_ura`,
`network.phase_leveling` are all `false`. The 1336 m, 4643 m and 5636 m epochs and the 80-minute
non-physical ionosphere all shipped with `gate_ok=1, solved=1, disposition=emitted` [M].

Nothing structural correlates with any of it: `n_present` 301–305, `n_clock_sats` 30–32,
`n_phase_bias` 198–216, `declared_dim ≡ 1`, `datum_changed` fires on exactly **2** epochs (tow
319260, 322530 — both in hours 16–17, the two quietest of the day) [M]. Satellite loss, station
dropout, ambiguity collapse, pivot handover and QC intervention are all **ruled out** as triggers.

---

## 5. Constraints that will bite you

**Fingerprint lock.** The config fingerprint is built at `NetworkRunContext.cpp:172-307`. Changing
anything in it hard-aborts every warm resume against existing snapshots.

- **IN** (cannot change on a warm resume): station roster *and its order*, `carry_invariant`,
  `state_snapshot_carry*`, `estimate_iono_sh`, every q/floor, `apriori_source`,
  `epoch_decimation_n`, datum station, `emit_ura` (`:209`), `phase_leveling` (`:201`),
  `code_bias_disc_jump_test` (`:236-238`).
- **SAFE**: `start_offset_hours`, `max_duration_s`, `state_snapshot_*`, `product_output_path`,
  `write_run_manifest`, `trace_*`, `health_interval_s`, `raim`, `clock_runaway_check`,
  `state_snapshot_resume_shock_m`, closed-loop consumer (`closed_loop` appears **zero** times in
  the fingerprint block).

So the two guards that would have caught this run's failure — `code_bias_disc_jump_test` and
`emit_ura` — **require a fresh convergence**, not a resume.

**Resume arithmetic.** Gate is strict `<` anchored on the first synced epoch of *this run's parse*
(`ApplicationNetwork.cpp:1011`), never restored from the snapshot. Epoch times carry ~2e-05 s of
double residue. Always derive the offset from the previous snapshot's `M` line with a half-epoch
back-off, and assert the log reads **`snapshot age 30.0 s`**:

```
start_offset_hours = (snap_week*604800 + snap_tow − DAY_ANCHOR + 15) / 3600
```

`scripts/chain_make_leg.py:36` hardcodes `DAY_ANCHOR_S = 1399161600.0` for DOY 129 — **change it
for any other day** or the run aborts with "no live epochs to resume against".

**Snapshot aliasing.** `state_snapshot_path` is read AND written. Give every leg its own file.
`scripts/run_warm_resume_arm.sh` re-seeds from the immutable *before every launch* — correct for
A/B arms, backwards for a chain.

**Exit 0 does not prove work.** `n_emitted` is restored on resume, so the zero-emit guard
(`NetworkTeardown.cpp:1059`) is unreachable. Assert the snapshot header delta. HB-1 throws
uncaught → exit `0xC0000409` with 0-byte stderr; Git Bash mangles it to 127.

**Data.** `data/2024-129/obs` has 311 files / 9.5 GB. `data/2024-130/` has **products only, no
obs**. `data/2024-131/obs` has one station. **The 305-station roster exists for DOY 129 and no
other day** [M] — any multi-day continuation is blocked on fetching observations first.

---

## 6. Open questions, ranked — with my hypotheses

### Q1. What triggers an `a00` collapse? *(highest value)*
No precursor in any logged channel at 334020. Single epoch, −341.9 TECU step.

**Hypothesis [?]:** the degree-15 SH field is under-constrained at that geometry and the solve
finds a near-null direction. `kaula_scale_tecu 10.0`, `q_per_degree 0.01`,
`representation_sigma_tecu 5.0`, `mean_vtec_prior_sigma_tecu 30.0` (`leg.json`). The prior on
`a00` is ±30 TECU, which cannot hold a state that reaches 2151.

**First step:** dump the SH normal-equation conditioning per epoch around 334020 (the
estimability spectrum machinery already exists — `NetworkEstimator::dumpEstimabilitySpectrum`) and
check whether the smallest singular value collapses at onset. Cross-check against station
distribution: which stations were contributing iono constraints and where were their pierce
points? `network_vtec_sh.csv` has 552 960 rows (all n,m coefficients per epoch) — the higher-degree
terms at onset should distinguish "genuine field structure" from "null-space excursion".

### Q2. Why did the pinned pivot's code bias absorb it?
Measured, not traced. The pivot's clock cannot move (pinned to 0), so any clock-space excitation
that projects onto it must land somewhere else.

**Hypothesis [?]:** the pin is a hard constraint on the clock state only, leaving the pivot's code
bias as the sole free parameter on that satellite; a one-sided clock excitation therefore
redistributes into it. That would explain why the *one-sided* event (334020) did permanent damage
and the two-sided ones did not — §4.2's common/differential ratio 5.4 vs 0.05 is the fingerprint.

**First step:** trace the pin from `NetworkSbasis` / `NetworkChainDatum` through to the code-bias
datum rows (`NetworkSbasis::buildCodeBiasDatumRows`) and confirm whether pivot clock and pivot code
bias share an estimability constraint. If they do, this is a **datum design issue**, not a
transient — and it will recur on any one-sided excitation.

### Q3. Why is the 11–21 m quiet-baseline pairwise wander so large?
This may matter more than the events. A PPP-RTK network producer whose between-satellite code
corrections wander 15 m hour-to-hour in its *quietest* hours is not fit for cm-level user work.

**Hypothesis [?]:** partly real (code noise at 305 stations with `no_bia_code_downweight 1.0` and
no URA weighting), partly the absent QC tiers. Compare against `docs/REPORT_*.md` expectations and
against a GPS+Galileo arm — the extra constellation may condition the code-bias datum better.

**First step:** decompose the pairwise wander into a common-mode part and a per-satellite part, and
correlate the per-satellite part with elevation/eclipse/block. Check whether it is concentrated in
satellites near the datum.

### Q4. Why did `disc_counter` miss a 49 m step while firing 5234 times elsewhere?
`code_bias_disc_jump_test` is false, so what *is* driving those 5234 bumps? If a different
mechanism produces them, it evidently has no magnitude sensitivity.

**First step:** find the writer of `disc_counter` on CB records in `NetworkProductEmitter.cpp` and
determine its trigger. Cheap, and it tells you whether the emitted stream has *any* usable
integrity signal today.

### Q5. Position-domain impact — unmeasured
No consumer-side artifact exists in this run. Everything above is correction-domain.

**First step:** the closed-loop consumer harness is complete and fingerprint-safe (parse at
`GnssConfigLoadNetwork.cpp:3218-3262`, trace header `NetworkRunTraces.cpp:115-116` with
`pos_err_3d_m, east_m, north_m, up_m`). Replay `products.txt` through it — no producer re-run
needed (`NetworkProductReader::replay`, or `realtime.network_products_path` +
`processing.network_consumer.enable`). Minutes per arm, not a day.

### Q6. DD ambiguity convergence — unmeasurable on this run
`trace_dd_fractional: false`; no `network_dd_fractional.csv` exists. mean|f| and circR cannot be
computed. Note the 2 486 816 slip refloats / 2 421 756 D5 re-taps in `main_log.log:7534/:7537` are
**one causal chain double-counted** (a D5 flag seeds the next epoch's refloat funnel); the
independent physical slip load is 8302 T1 events = 3.84/epoch [M]. Enabling the trace is
fingerprint-safe.

---

## 7. Recommendations

**Do not run an absolute, cm-level PPP-RTK user-bias study against this stream.** The correction
noise floor is 5.2 m between-satellite spread in the single cleanest hour, worst-satellite
deviation 12.09 m even then [M]. PCV, higher-order iono, wind-up and site displacement all sit
50–1000× below that. The study would measure the producer.

**What can proceed today:** an **A/B model-toggle** design where both arms consume the identical
stream. `products.txt` has no station dimension (`# C,sat,clock_c0_m`, `# B,sat,signal,code_bias_m`
— zero station records), so every consumer applies bit-identical per-satellite numbers and the
correction error differences out **exactly**. "User model on vs off" is valid; "user error vs
truth" is not.

**A consumer cannot self-detect the damage.** `C` lines carry no URA/sigma/disc (emitted only
under `emit_clock_disc` / `has_ura`, both off — `NetworkProductIO.cpp:508-510`, `:569-570`);
`B` lines carry nothing; `E` field 7 `datum_changed` is a handover flag that fired on 2 epochs,
neither an excursion. The 5636 m clock epoch is catchable by a magnitude bound (rank 1 of 2160,
~289× median); the +43 m persistent code bias is **not** — it sits inside the normal 12–24 m
worst-satellite band. **Format defect:** the `P` header declares 6 field names while every record
carries 7 [M]; a consumer parsing to the declared schema silently drops sigma.

**Before a re-convergence** (the only way to change fingerprinted knobs), arm together:
`code_bias_disc_jump_test`, `emit_ura`, `phase_leveling`, `raim`, `clock_runaway_check`, and make
the `a00` band check *gate* rather than warn (`NetworkShHealth.cpp:75-97` is read-only today) and
un-throttle it (`:87`).

---

## 8. What I got wrong, so you don't inherit it

During the run I reported hourly on `rms_clock_m` and concluded the network "degrades ~5× in the
evening and stays degraded." That was wrong: it graded a datum-dependent quantity (§3), and the
quiescent trend is *flat-to-improving* (§4.1). I also under-counted `a00` events 23.8× by counting
edge-triggered warnings, reported the peak as 1259.5 TECU when it was 2151, described one evening
event when there are two, and filed the day's **largest** excursion (5636 m at 20:47:00) as a
footnote. The hr23 median I quoted (21.78 m) came from a partial hour and is 16.639 m.

The framing that survives: **the failure is discrete events, not drift**, and **the worst of it
lands where clock metrics cannot see it**.
