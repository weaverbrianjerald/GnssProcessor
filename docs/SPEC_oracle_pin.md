# SPEC — the ORACLE PIN: hold the ambiguities at truth and read where the error goes

Card opened from `docs/SPEC_event_harness.md` §17 (stubbed 2026-09-11), built and
graded the same day.  **Numbers first; the reading is §8.**

Knob: a new injector kind `oracle_pin` in `network.inject.events[]`.
**SIM-ONLY DIAGNOSTIC — field-inert by construction, never a golden knob, never in
`docs/CONFIG_promotion_ledger.json`.**

---

## 0. Headline

| | |
|---|---|
| **G1 OFF byte-identity** | **PASS** — 7/7 artifacts bit-identical to the UNMODIFIED exe, 652/652 counter log lines identical |
| **G2 actuation** | **PASS on every arm the DD instrument can see, once each arm is past its own onset transient.** At a settled epoch: `L1C+L2W` 100.0 % of pinned chords < 0.01 cyc (max 1e−4), `L5Q` 100.0 % (max 0.0000), `all` 100.0 % on L1C/L2W/L5Q (max 4e−4) and **57.7 % on Galileo L7Q** (max 0.103 cyc) — the one band the pin does not fully take |
| **G3 the coupling table** | built, both windows, every cell from a truth grader — §6 |
| **G4 slipped seed** | **PASS** — 109 truth slips land on pinned arcs after the pin; on chords carrying one, **99.7 %** of DD errors are < 0.01 cyc at slip+1 and **99.0 %** at slip+5.  The pin follows `N(t)`. |
| **THE READING** | **The DD-invariant ionosphere collapses 10.1× under a complete pin — 0.4233 → 0.0419 m rms, 2.61 → 0.26 TECU.  The standing 99.3 %-iono root cause is CONFIRMED, and the `none` column's 0.423 m IS the integer-information deficit.**  §8.1 |

⚠ **Row 1 of the §6 table (RAW per-LOS iono error) cannot test the root cause and must
not be read as if it could.**  In the producer's realized S-basis the free per-LOS
slant iono absorbs a station-common lump (receiver GF/DCB, the Table-8 identity) and a
satellite-common lump (satellite code bias / S-basis) that NO ambiguity information
can touch — measured here at **a_r rms 1.13–1.28 m and b_s rms 1.85–1.93 m against
0.42 m of DD-invariant interaction**, i.e. ~97 % of the 2.73 m raw rms.  Row **1b** is
the two-way de-meaned error, which is exactly what a double difference sees, and it is
the row that carries the verdict (standing invariant #5).

**The three findings that were not in the pre-registration:**

1. **Holding the ambiguities at truth is a metre-class prefit event, and the
   standing QC stack treats it as one.**  On `all`, the onset epoch's fit residual
   is **6.43 cyc rms / 38.1 cyc max** over 2 295 carried arcs — that is how far the
   converged float ambiguities sit from their true integers in the run's own
   single-difference frame — and `raim_within_epoch` + the A4 σ̂₀ ceiling
   **withhold 4 epochs** (σ̂₀ 28.3 → 22.2 against a ceiling of 6.0) while the rest
   of the state moves.  By the run end the residual is **1e−4 cyc rms / 2.7e−3
   max**: the solve lands exactly on the pins.  The withhold is a RESULT, not a
   grading inconvenience — it is the price of the information, paid up front.
   A band-scoped pin is much gentler (`L5Q` withholds **nothing**).
2. **The null leg's ZWD is a WARM-RESUME plateau, not the producer's converged
   level.**  The SEED's own converged ZWD (its epochs 640–665, immediately before the
   snapshot every leg resumes from) is **0.0076 m rms**; the null leg sits at
   0.067–0.081 m for all 57 epochs and never works it off.  A complete pin removes
   ~80 % of that within two epochs and lands at 0.0130 m — 1.7x the seed.  So finding
   2's direction stands but its magnitude must be quoted against the seed, not against
   the null.  §8.2.
3. **On the SLIPPED bed the pin does not settle.**  Same knob, same σ: the
   last-epoch residual is **6.96 cyc rms / 118 cyc max** and `no_station_term` is
   **87 645** arc-epochs against 0 on the band arms of the quiet bed.  It still
   helps — DD-invariant iono 2.2001 → 0.4145 m (5.3×), ZWD 1.348 → 0.526 m (2.6×) —
   but lands an order of magnitude above the quiet bed on both, and the consumer gets
   worse.  The fit rests on the leveling carry, and on a slipped bed that carry is
   continuously refloated out from under it.  §7.

---

## 1. What was built

### 1.1 Config surface

```json
{"kind": "oracle_pin", "tow": 279330.0, "scope": "all", "sigma_m": 1e-4}
```

- `scope` — `all` | `band:<sig>[+<sig>…]` | `station:<name>` | `sat:<id>` |
  `arc:<sta>/<sat>/<sig>`.  Anything else is refused **by name** at load
  (`include/filter/NetworkOraclePinScope.hpp` owns the one grammar, shared by the
  JSON loader and the run-scope seam so they cannot drift).
- `sigma_m` — the pin prior σ in metres, default **1e−4 m** (≈ 0.0005 cyc, Λ = 1e8):
  stiff, but two decades BELOW the 1e10 datum pins, so the gauge stays the anchors'.
  Must be > 0.
- PERSISTENT from `tow` on (the `sat_clock_step` semantics); `n_applied` increments
  every epoch the table was non-empty, so the injector's never-fired hard error
  applies unchanged.
- A `station` / `sat` / `signal` field beside `scope` is **refused**: which arcs are
  pinned is said by `scope` alone, and a second, silently-ignored selector would arm
  a different set than the operator wrote.

**Field-inertness.**  An `oracle_pin` event with `network.cycle_slip.oracle_truth_root`
empty or missing is a HARD load failure, and `networkOraclePinLoad` hard-errors when
any ROSTER station lacks `truth_ambiguities.csv` or `truth_slips.csv`.  A field
lineage has no truth root, so the knob cannot be armed on one at all.  A HARD error
rather than warn-and-disarm because a silently unpinned run would report the model's
error budget WITHOUT the ambiguities held and be read as the ceiling this card exists
to measure.

### 1.2 THE GAUGE — why the pin is a fit and not `λ·N_true`

A raw pin `value_abs = λ·N_true` fights the datum.  The network fixes ONE arc per
(satellite, band) family — the ANCHOR — at whatever level it carried when it was
elected, and station-level phase biases ride INSIDE the ambiguities.  In the run's
own frame a non-anchor arc's ambiguity column is

    α_rsj = β_sj + u_rj + λ_j · N_rsj

with `u_rj` station r's band-j phase offset (receiver phase bias plus whatever
code-datum lump the clock absorbed — station-common, NOT integer, and
data-determined, so the pin must not overwrite it) and `β_sj` the family's frame
term fixed by the anchor a(s,j) and its ENFORCED pin target `t_sj`
(`NetStation::pinned_amb_targets` — READ, never assumed 0):

    β_sj = t_sj − λ_j·N_{a s j} − u_{a j}.

Eliminating β between an arc and its own family's anchor leaves a pure
station-offset DIFFERENCE with every frame term gone:

    g(r,s,j) = value_abs(r,s,j) − t_sj − λ_j·(N_rsj − N_{a s j}) = u_rj − u_{a j}

so the fit is a **levelling network** over the per-(station, band) offsets,
determined up to one constant per band — the same free constant the pinned values
are invariant to.  The pinned value is then

    pin(r,s,j) = t_sj + (u_rj − u_{a j}) + λ_j·(N_rsj − N_{a s j}).

Three properties, all asserted by the unit suite:
1. every pinned DOUBLE DIFFERENCE is exactly `λ·DD(N_true)` — β and u cancel, so the
   pin asserts INTEGER information and nothing else;
2. anchor arcs are NEVER in the table (their 1e10 datum pin owns the family frame);
3. re-parameterising the truth as (β − c, u + c) leaves every pinned value unchanged.

**⚠ The alternating fit named in the card's own pre-registration does not converge,
and was replaced.**  "β from the anchors, u from the medians, iterate to a
tolerance" has the error recursion `e_r = median over the station's families of
e_{anchor(family)}`, and a median is a SELECTION, not an average — so the anchors'
errors cycle among their own values forever.  Reproduced on the unit suite's 6×8×2
bed: 20/20 iterations, oscillating between two values.  Since the pinned values are
invariant to the free constant, the fix is to FIX IT: pick a ROOT anchor per band
(the one owning the most families, ties by station index), declare its offset 0, and
PROPAGATE — each remaining station's offset is estimated ONCE, as a median over ≥ 3
arcs whose family anchors already have a value, and then frozen.  Terminates in at
most one round per station; measured **2–3 rounds** on every 60-station leg here.

The ≥ 3-arc rule is load-bearing: an offset fitted on one or two arcs is not a
station term, it is that arc's own error re-labelled, and pinning on it would inject
that error into every other arc of the station.  A MEDIAN, not a mean, because on the
slipped bed a single un-repaired arc is a multi-cycle outlier.

### 1.3 Where it actuates

Built SERIALLY once per epoch (`networkOraclePinEpoch`, after the phase-anchor
election and after column registration, before PASS B — so the fit reads THIS
epoch's anchors and a handover moves the family's frame with the pin following it),
then read READ-ONLY inside the parallel PASS-B produce loop, where each pinned
ambiguity column takes ONE DIAGONAL LOCAL PRIOR row of the same shape as the
phase-leveling carry row:

```cpp
const double lam = 1.0 / (pin.sigma_m * pin.sigma_m);
inj.emplace_back(col, lam, lam * (pin.value_m - x_lin(amb_idx)));
```

⚠ **The x_lin trap** (`SPEC_lane_slip_response` §7.2): shifting `x_lin` alone is a
proven EXACT solve no-op.  The row above is a real (Λ, η) pair — that is why it
actuates — and the pin deliberately does NOT also write `LocalCarry::value_abs`.

The row is INDEPENDENT of the leveling carry: it fires whether or not the arc has a
`LocalCarry` entry (a newborn leg is pinned too), whether or not the slip response
freed the arc this epoch, and regardless of the F3 block route.  Both the row-build
gate and the `addStationPrior` INSTALL gate were widened for it — widening only the
build gate would have produced rows that are coalesced into `R.carry_cols` and then
dropped, i.e. a run-end counter that reports folds which never reached the normal
equations.

**Closure, stated in the run-end clause and verified on every arm:**
`arc-epochs offered = rows_injected + gauge_pinned_skipped + no_column`.
Measured `144025 = 144025 + 0 + 0` (`all`), `81695 = 81695 + 0 + 0` (`L1C+L2W`),
`51251 = 51251 + 0 + 0` (`L5Q`), `93641 = 93641 + 0 + 0` (slipped `all`).

### 1.4 Telemetry — both edges of invariant #7, with IDENTITIES

`network_events.csv` carries a per-arc row on the ENTRY edge (`pinned`, with
`before` / `pinned` / `delta_cyc` / `N`) and on the EXIT edge (`released`), plus one
summary row per active epoch.

⚠ **Counters alone were measured to be insufficient here.**  The first build emitted
the per-arc rows only at the ONSET epoch.  A grader reconstructing "which arcs are
pinned at epoch t" from those under-counts by every arc that joined later — **7 828
at onset vs 9 381 at the run end** on the first quiet arm — and then reads a working
pin as a failure (it did: the actuation gate came back 0 % on L2W with the pin
working perfectly).  Both edges are now emitted, so the pinned set at any epoch is
exactly (entries − exits) up to that epoch.  This is what invariant #7 means by
"with counters **and identities**".

---

## 2. The legs

All nine, 60 epochs, pin at **+5**, generated by `scripts/ev_oracle_pin_legs.py`
(concurrency cap 2, completion read from `exitcode.txt`, never the process list).
Quiet seed `runs/evseed_quiet6h` ep0665 (tow 279150, fingerprint `6a3531a1afb2b40b`)
on fixture `runs/simnet60_quiet_wu93_24h`; slipped seed `runs/evseed_slipped6h`
ep0665 (tow 322320) on `runs/simnet60_campaign_wu93`.
`network.cycle_slip.oracle_truth_root` is armed on **every** leg, the nulls
included, so the only difference between a null and an arm is the event.

| leg | exe | scope | rc | wall s | withheld (A4) | rows injected | arcs first pinned / released |
|---|---|---|---|---|---|---|---|
| `opin_null_base` | base | — | 0 | 146.6 | 0 | — | — |
| `opin_null` | ON | — | 0 | 151.4 | 0 | — | — |
| `opin_all` | ON | `all` | 0 | 164.9 | **4** | 144 025 | 9 577 / 3 392 |
| `opin_l12` | ON | `band:L1C+L2W` | 0 | 154.8 | **4** | 81 695 | 5 084 / 128 |
| `opin_l5` | ON | `band:L5Q` | 0 | 145.8 | 0 | 51 251 | 3 193 / 66 |
| `opin_s21` | ON | `station:S21000SIM` | 0 | 151.2 | 0 | 2 755 | 162 / 34 |
| `opin_g08` | ON | `sat:G08` | 0 | 151.3 | 0 | 2 889 | 177 / 48 |
| `opin_slip_null` | ON | — | 0 | 131.7 | 0 | — | — |
| `opin_slip_all` | ON | `all` | 0 | 148.5 | **4** | 93 641 | 4 535 / 3 839 |

Standing invariant #4: every armed leg reports `rows_injected > 0`, and the PASS-B
closure is exact on every one of them.

---

## 3. G1 — OFF byte-identity (PASS)

`opin_null_base` (the UNMODIFIED worktree exe, SHA-256 `803C61D9…`) against
`opin_null` (the ON exe, `D5193E1A…`), same config, same snapshot:

| artifact | | bytes | sha256[:16] |
|---|---|---|---|
| `products.txt` | IDENTICAL | 1 458 419 | `f3f37bc36051983a` |
| `logs/network_dd_fractional.csv` | IDENTICAL | 16 016 413 | `852c4dc38cdc1929` |
| `logs/network_zwd.csv` | IDENTICAL | 189 998 | `ddc169a23ad780b7` |
| `logs/closed_loop_trace.csv` | IDENTICAL | 4 973 | `18280f8cd729b00f` |
| `logs/network_ionosphere.csv` | IDENTICAL | 4 113 695 | `acea8d872fe34b19` |
| `logs/network_rx_states.csv` | IDENTICAL | 677 647 | `889b59bc51ce244a` |
| `logs/network_events.csv` | IDENTICAL | 22 022 | `690927f6addbcd41` |

plus **652 / 652** run-end and counter log lines identical (timing figures and leg
paths excluded, which are the only legal differences), and the ORACLE PIN clause
ABSENT from the null leg's log.  Every file's mtime post-dates its own launch —
memory `feedback_stale_trace_dir_fake_ab`: an A/B against a stale trace directory
reads as byte-identity and is worth nothing.

---

## 4. G2 — actuation

Instrument: `logs/network_dd_fractional.csv`.  A chord counts as PINNED when all
FOUR of its legs (both stations × sat_i/ref_sat) are in the leg's own pinned set,
reconstructed from the producer's ENTRY/EXIT event rows at that epoch — what the
producer DID, not what the scope asked for.

**At the pre-registered pin+1** (`--at-ep` default):

| leg | read at | withheld first | sig | pop | p50 | p90 | max | < 0.01 cyc | unpinned p50 / p90 (pop) |
|---|---|---|---|---|---|---|---|---|---|
| `all` | +4 | 2 | L1C | 838 | 0.0001 | 0.0017 | 10.69 | 90.6 % | — |
| `all` | +4 | 2 | L2W | 498 | 0.0002 | 0.0031 | 24.78 | 91.4 % | — |
| `all` | +4 | 2 | L5Q | 838 | 0.0001 | 0.0026 | 11.23 | 90.6 % | — |
| `all` | +4 | 2 | L7Q | 33 | 2.7009 | 13.05 | 18.56 | 36.4 % | 0.4071 / 7.917 (307) |
| `L1C+L2W` | +4 | 2 | L1C | 838 | 0.0000 | 0.0000 | 0.0001 | **100.0 %** | — |
| `L1C+L2W` | +4 | 2 | L2W | 498 | 0.0000 | 0.0000 | 0.0000 | **100.0 %** | — |
| `L1C+L2W` | +4 | 2 | L5Q | — | | | | | **0.0882 / 0.3360 (838)** |
| `L1C+L2W` | +4 | 2 | L7Q | — | | | | | **0.1561 / 0.3876 (340)** |
| `L5Q` | +1 | **0** | L5Q | 878 | 0.0000 | 0.0000 | 0.0002 | **100.0 %** | — |
| `L5Q` | +1 | 0 | L1C | — | | | | | **0.4355 / 2.409 (878)** |
| `L5Q` | +1 | 0 | L2W | — | | | | | **0.0778 / 0.5836 (531)** |
| `L5Q` | +1 | 0 | L7Q | — | | | | | **0.0937 / 0.2448 (347)** |

**The contrast is the instrument and it is unambiguous**: in `L1C+L2W` the pinned
bands read their true integer to 1e−4 cycles and the L5Q / L7Q chords of the SAME
epoch do NOT snap (p50 0.088 / 0.156 cyc); in `L5Q` it is the mirror image.

**At a settled epoch** (`--at-ep 33`, i.e. +28): `L1C+L2W` 100.0 / 100.0 %,
`L5Q` 100.0 %, `all` **100.0 % on L1C, L2W and L5Q** (max 0.0004 cyc) and
**57.7 % on Galileo L7Q** (p50 0.0027, max 0.1027 cyc).  So `all`'s pin+1 shortfall
is its own onset transient, not a wiring failure — but **L7Q is a real, open
exception**: it is the band with by far the thinnest arc population (40 onset rows
against 3 009 for L1C), so the fit's ≥ 3-arc station-term rule binds hardest there,
and `all` is the only arm whose `no_station_term` is non-zero (34 988 arc-epochs
against 0 on both band arms).

**`sta S21` and `sat G08` cannot be graded on this instrument at all**, and that is
structural, not a gap: a DD chord needs FOUR pinned legs, and a single-station scope
supplies at most two while a single-satellite scope supplies at most two (the
reference satellite is never G08).  Those two arms are graded by the §6 table alone.

---

## 5. G4 — the slipped seed (PASS)

`opin_slip_all` on `runs/evseed_slipped6h` / `simnet60_campaign_wu93`.
**109 truth slips land on a pinned arc after the pin** (first fifteen):

| tow | station | arc | Δ cyc | flagged |
|---|---|---|---|---|
| 322560 | S09000SIM | E07/L7Q | −26 | 1 |
| 322560 | S53000SIM | G04/L5Q | −38 | 0 |
| 322560 | S56000SIM | G06/L1C | −56 | 1 |
| 322590 | S03000SIM | G15/L5Q | −24 | 0 |
| 322590 | S55000SIM | E26/L5Q | +18 | 1 |
| 322620 | S01000SIM | E34/L7Q | −62 | 0 |
| 322620 | S21000SIM | G30/L1C | +44 | 0 |
| 322620 | S44000SIM | G23/L5Q | −60 | 0 |
| 322620 | S51000SIM | E14/L5Q | +98 | 1 |
| 322650 | S09000SIM | E19/L7Q | −81 | 0 |
| 322650 | S10000SIM | G01/L5Q | +65 | 0 |
| 322680 | S07000SIM | E25/L5Q | −54 | 0 |
| 322710 | S27000SIM | G19/L1C | +63 | 0 |
| 322710 | S43000SIM | E26/L1C | +96 | 0 |
| 322710 | S43000SIM | G19/L5Q | −52 | 1 |

DD error on chords carrying a slipped pinned arc, `|dd_cyc − DD(N_true)|`:

| offset | pop | p50 | p90 | max | < 0.01 cyc |
|---|---|---|---|---|---|
| slip + 1 | 355 | 0.0002 | 0.0019 | 32.44 | **99.7 %** |
| slip + 5 | 303 | 0.0001 | 0.0012 | 35.00 | **99.0 %** |

**The pin follows `N(t)` through the jump.**  A pin that did not would show the
slip's own Δ (24–115 cycles here) in every one of those chords; the p50 is 2e−4 cyc
and the single-chord tail is one or two arcs, not a population.

---
## 6. G3 — the six-column coupling table

`python scripts/ev_oracle_pin_grade.py g3` → `runs/opin_table.json`.  Every cell is
a TRUTH grader against the fixture, never a raw wire step (invariant #5).  Windows
are the pre-registered ones: **W1 = epochs +28..+32 (pin+23..+27)** and
**W2 = +53..+57 (pin+48..+52)**, both well past every arm's onset transient.
4 774 LOS / 300 station-epochs / 5 emitted epochs per cell.

⚠ **Row 1 is BASIS-DEPENDENT: it contains the station- and satellite-common DCB
absorption of the realized S-basis and CANNOT test the root cause** — it is context
only.  **Row 1b is the datum-invariant one** (the two-way de-mean; §6.1).  Read §8.0
before reading any row of this table as an estimation error.

### window W1 — epochs +28..+32 (pin+23..+27), tow 280020..280140

| row | none | all | L1C+L2W | L5Q | sta S21 | sat G08 |
|---|---|---|---|---|---|---|
| 1 slant iono rms (m) [basis] | 2.6553 | 2.6783 | 2.7093 | 2.6852 | 2.7102 | 2.6323 |
| 1 slant iono p50 \|e\| (m) [basis] | 1.6411 | 1.6701 | 1.7850 | 1.8200 | 1.6314 | 1.6498 |
| 1 slant iono p90 \|e\| (m) [basis] | 4.6200 | 4.6873 | 4.4754 | 4.6501 | 4.6920 | 4.5681 |
| **1b DD-invariant iono rms (m)** | 0.4312 | 0.0427 | 0.5537 | 0.2987 | 0.4632 | 0.4738 |
| 1b DD-invariant iono p50 \|eps\| (m) | 0.2323 | 0.0303 | 0.2245 | 0.2159 | 0.2539 | 0.2697 |
| 1b DD-invariant iono p90 \|eps\| (m) | 0.6981 | 0.0688 | 0.8114 | 0.4827 | 0.7103 | 0.7759 |
| 1b station effect a_r rms (m) | 1.1381 | 1.1810 | 1.2687 | 1.1351 | 1.1965 | 1.1434 |
| 1b satellite effect b_s rms (m) | 1.8544 | 1.8907 | 1.8659 | 1.8946 | 1.8608 | 1.8302 |
| 2 ZWD rms (m) | 0.0725 | 0.0138 | 0.0160 | 0.0927 | 0.1000 | 0.2203 |
| 2 ZWD p90 \|e\| (m) | 0.1304 | 0.0219 | 0.0280 | 0.1263 | 0.1788 | 0.2353 |
| 3 rx DCB contrast rms (m) | 1.3063 | 1.1710 | 1.6514 | 1.4543 | 1.2339 | 1.3131 |
| 3 rx clock contrast rms (m) | 2.0648 | 1.9787 | 2.3373 | 2.0296 | 2.1017 | 2.1120 |
| 4 sat clock rms (m) | 1.1955 | 1.2015 | 1.2720 | 1.1935 | 1.1706 | 1.2199 |
| 4 sat clock p50 \|e\| (m) | 0.7769 | 0.6183 | 0.7276 | 0.7741 | 0.7285 | 0.7825 |
| 4 sat clock SD contrast rms (m) | 1.6497 | 1.6934 | 1.6849 | 1.6400 | 1.6006 | 1.6313 |
| 5 code bias rms (m) | 0.1986 | 0.2323 | 0.1995 | 0.8324 | 0.1953 | 0.1990 |
| 5 DD L1C med \|e\| (cyc, ref-free) | 0.1366 | 0.0000 | 0.0000 | 0.0752 | 0.1763 | 0.1505 |
| 5 DD L1C at own integer (%) | 82.5 | 100.0 | 100.0 | 88.1 | 70.1 | 74.3 |
| 5 DD L2W med \|e\| (cyc, ref-free) | 0.1371 | 0.0000 | 0.0000 | 0.0196 | 0.2018 | 0.1864 |
| 5 DD L2W at own integer (%) | 83.1 | 100.0 | 100.0 | 100.0 | 68.9 | 72.5 |
| 5 DD L5Q med \|e\| (cyc, ref-free) | 0.1469 | 0.0000 | 0.0210 | 0.0000 | 0.1885 | 0.1744 |
| 5 DD L5Q at own integer (%) | 81.0 | 100.0 | 100.0 | 100.0 | 71.8 | 73.1 |
| 5 WL(1,2) med \|e\| (cyc, ref-free) | 0.0776 | 0.0000 | 0.0000 | 0.0549 | 0.1249 | 0.1077 |
| 5 WL(1,2) at own integer (%) | 93.5 | 100.0 | 100.0 | 88.1 | 80.6 | 82.7 |
| 5 EWL(2,5) med \|e\| (cyc, ref-free) | 0.0226 | 0.0000 | 0.0182 | 0.0196 | 0.0312 | 0.0292 |
| 5 EWL(2,5) at own integer (%) | 100.0 | 100.0 | 100.0 | 100.0 | 90.9 | 90.7 |
| 6 consumer 3D p50 (m) | 1.7645 | 1.3421 | 2.3813 | 1.2180 | 4.0578 | 1.9353 |
| 6 consumer Up p50 (m) | 1.5822 | 1.3223 | 0.8747 | 1.1320 | 3.9807 | 1.7851 |
| n LOS / sta-ep / epochs | 4744/300/5 | 4744/300/5 | 4744/300/5 | 4744/300/5 | 4744/300/5 | 4744/300/5 |

### window W2 — epochs +53..+57 (pin+48..+52), tow 280770..280890

| row | none | all | L1C+L2W | L5Q | sta S21 | sat G08 |
|---|---|---|---|---|---|---|
| 1 slant iono rms (m) [basis] | 2.7281 | 2.7706 | 2.7892 | 2.7571 | 2.7896 | 2.7260 |
| 1 slant iono p50 \|e\| (m) [basis] | 1.6624 | 1.7326 | 1.8983 | 1.8787 | 1.6835 | 1.6847 |
| 1 slant iono p90 \|e\| (m) [basis] | 4.7208 | 4.8383 | 4.5976 | 4.7404 | 4.7852 | 4.7822 |
| **1b DD-invariant iono rms (m)** | 0.4233 | 0.0419 | 0.5632 | 0.2793 | 0.4476 | 0.4741 |
| 1b DD-invariant iono p50 \|eps\| (m) | 0.2387 | 0.0289 | 0.2185 | 0.1979 | 0.2501 | 0.2881 |
| 1b DD-invariant iono p90 \|eps\| (m) | 0.7064 | 0.0683 | 0.8152 | 0.4382 | 0.7122 | 0.7762 |
| 1b station effect a_r rms (m) | 1.1327 | 1.1810 | 1.2798 | 1.1252 | 1.2111 | 1.1370 |
| 1b satellite effect b_s rms (m) | 1.8995 | 1.9247 | 1.8978 | 1.9290 | 1.8985 | 1.8846 |
| 2 ZWD rms (m) | 0.0669 | 0.0130 | 0.0144 | 0.0634 | 0.0900 | 0.2138 |
| 2 ZWD p90 \|e\| (m) | 0.0923 | 0.0212 | 0.0246 | 0.0928 | 0.1173 | 0.3618 |
| 3 rx DCB contrast rms (m) | 1.3192 | 1.1416 | 1.6332 | 1.3998 | 1.2388 | 1.2972 |
| 3 rx clock contrast rms (m) | 2.0922 | 1.9796 | 2.3384 | 2.0595 | 2.2701 | 2.2789 |
| 4 sat clock rms (m) | 1.1892 | 1.1930 | 1.2659 | 1.1881 | 1.1526 | 1.2620 |
| 4 sat clock p50 \|e\| (m) | 0.7859 | 0.6196 | 0.7269 | 0.7789 | 0.6524 | 0.6669 |
| 4 sat clock SD contrast rms (m) | 1.6330 | 1.6701 | 1.6668 | 1.6239 | 1.5662 | 1.6442 |
| 5 code bias rms (m) | 0.1898 | 0.2918 | 0.2170 | 0.7627 | 0.1836 | 0.1946 |
| 5 DD L1C med \|e\| (cyc, ref-free) | 0.1876 | 0.0000 | 0.0000 | 0.0366 | 0.2793 | 0.2668 |
| 5 DD L1C at own integer (%) | 73.4 | 100.0 | 100.0 | 96.5 | 61.5 | 61.6 |
| 5 DD L2W med \|e\| (cyc, ref-free) | 0.2386 | 0.0000 | 0.0000 | 0.0141 | 0.4615 | 0.4335 |
| 5 DD L2W at own integer (%) | 68.6 | 100.0 | 100.0 | 100.0 | 52.2 | 52.9 |
| 5 DD L5Q med \|e\| (cyc, ref-free) | 0.2059 | 0.0000 | 0.0138 | 0.0000 | 0.3130 | 0.2826 |
| 5 DD L5Q at own integer (%) | 71.5 | 100.0 | 100.0 | 100.0 | 58.7 | 60.9 |
| 5 WL(1,2) med \|e\| (cyc, ref-free) | 0.0669 | 0.0000 | 0.0000 | 0.0309 | 0.1058 | 0.1426 |
| 5 WL(1,2) at own integer (%) | 91.7 | 100.0 | 100.0 | 96.4 | 77.3 | 79.3 |
| 5 EWL(2,5) med \|e\| (cyc, ref-free) | 0.0209 | 0.0000 | 0.0119 | 0.0141 | 0.0300 | 0.0349 |
| 5 EWL(2,5) at own integer (%) | 100.0 | 100.0 | 100.0 | 100.0 | 87.8 | 91.5 |
| 6 consumer 3D p50 (m) | 1.6561 | 1.0862 | 3.2027 | 0.3117 | 3.3884 | 1.4189 |
| 6 consumer Up p50 (m) | 1.4389 | 0.6913 | 0.1534 | 0.2920 | 3.3067 | 1.1983 |
| n LOS / sta-ep / epochs | 4774/300/5 | 4774/300/5 | 4774/300/5 | 4774/300/5 | 4774/300/5 | 4774/300/5 |

**Rows 3, 4 and 5 (code bias) caveat.**  `rx DCB contrast` and `rx clock contrast`
are the BETWEEN-STATION contrasts (`simnet_grade.grade_rx_states`) — the absolute
per-station DCB is datum-bearing by the Table-8 identity and is not gradeable.
`sat clock` is TRUTH-DIFFERENCED in the realized basis (`grade_clock`; this bed's
satellite clocks are NOT null, so `simnet_sat_clock_census.py` is the wrong tool).
The DD rows are the REF-FREE lane grade (`simnet_amb_integer --ref-free`): the DD
trace has ONE reference satellite and it is in every chord (`SPEC_amb_iono` §6.12).

**Row 1 (marked `[basis]`) carries a datum-bearing common level too** — the
fleet-mean receiver GF bias absorbed into the ionosphere, **+1.58 m** on the `none`
column — on top of the per-station and per-satellite lumps rows 1b quantify.  A
ONE-way de-mean is not enough to repair it: removing only the global mean leaves
2.2246 (`none`) against 2.2442 (`all`), still flat, because the station and satellite
factors survive.  Only the TWO-way removal of row 1b exposes the estimable part.

---

### 6.1 How row 1b is computed, and the check it doubles as

Per EPOCH, over every LOS (r, s) with both an estimate and a truth value, fit the
unbalanced two-way ADDITIVE model (no interaction term)

    e_rs = mu + a_r + b_s + eps_rs

by **alternating means** — Gauss–Seidel on the normal equations of the two-factor
model — re-centring `a` and `b` to sum-to-zero after each sweep and iterating to
`max|Delta a|, max|Delta b| < 1e-12 m` or 500 sweeps.  Measured convergence: **40–43
sweeps** on every epoch of every leg (60 stations x 53 satellites, ~950 LOS).  For an
unbalanced design the sweep is a contraction on the connected component, so it
converges — unlike the median recursion of the PIN's own fit (§1.2), which is a
selection and cycles.  `eps` is then pooled over the window; `a` and `b` are pooled
separately so the reader can see which factor carries the metres.

`eps` is exactly what a double difference sees: `DD(e) = eps(r1,s1) − eps(r1,s2) −
eps(r2,s1) + eps(r2,s2)`, with `a_r` and `b_s` cancelling identically.  So row 1b is
**also a sanity check on the trace**: under a DD-consistent pin the algebra forces
`eps` to the phase floor, and if it had not collapsed the right response would have
been to check what `network_ionosphere.csv` actually emits on this posture before
drawing any conclusion.  It collapsed (0.4233 → 0.0419 m), so the trace and the join
are sound; the residual 4 cm is discussed in §8.1.

**Row 1b is NOT confounded by the warm resume** (unlike row 2 — §8.2).  The SEED
run's own converged window reads row 1b **0.4647 m** with a_rms 1.317 / b_rms 1.831,
the same level and factor structure as the null leg, and the null column is flat at
0.443–0.449 across all 57 epochs.

---

## 7. G4's second half — the slipped bed, `none` vs `all` (window W2)

| channel | none | all | |
|---|---|---|---|
| **1b DD-invariant iono rms (m)** | **2.2001** | **0.4145** | **5.3x better** |
| 1b DD-invariant iono p50 \|eps\| (m) | 1.0944 | 0.1357 | 8.1x better |
| 1b station effect a_r rms (m) | 2.5245 | 3.6179 | (basis) |
| 1b satellite effect b_s rms (m) | 6.5664 | 10.1308 | (basis) |
| ZWD rms (m) | 1.3476 | **0.5260** | **2.6x better** |
| ZWD p90 \|e\| (m) | 1.0734 | **0.6534** | 1.6x better |
| ~~slant iono rms (m), RAW~~ | 7.5358 | 10.9152 | ⚠ basis-dependent — see below |
| consumer 3D p50 (m) | 3.6726 | 4.5606 | 1.2x WORSE |
| consumer Up p50 (m) | 1.3322 | 3.8237 | 2.9x WORSE |

⚠ **The RAW iono line is the §8.0 trap a second time, and this is the cleanest
demonstration of it in the card.**  Raw rms goes the WRONG way (7.54 -> 10.92) while
the DD-invariant part goes the RIGHT way by 5.3x — because the absorbed lumps GREW
(satellite effect b_s rms 6.57 -> 10.13 m, station effect 2.52 -> 3.62 m).  The pin
pushed error out of the estimable interaction and into the datum-bearing directions,
which is a re-realization, not a degradation, and the raw row cannot tell the two
apart.  Do not quote it.

The stub asked: *does a perfect integer hold cure the slipped bed's ZWD?*
**Partially, and it is not free.**  The metre-class ZWD sink halves — but it stays
at 0.53 m, **69x the seed's converged 0.0076 m** (§8.2), and the consumer gets
worse.  The DD-invariant ionosphere DOES respond (5.3x), but at 0.41 m it lands an
order of magnitude above the quiet bed's pinned 0.042 m.

The mechanism is in the run-end clause: on the slipped bed the pin's own fit reports
`no_station_term` **87 645** arc-epochs (against **0** on both quiet band arms) and
a last-epoch residual of **6.96 cyc rms / 118 cyc max** — the solve never lands on
the pins.  The fit's observations are the leveling carry, and on a slipped bed that
carry is being continuously refloated out from under it: 3 839 arcs released against
4 535 first pinned in 50 epochs, versus 128 against 5 084 on the quiet `L1C+L2W` arm.
**A truth-fed integer pin is not a cure for a bed whose carry is churning; it is
downstream of the same defect the slip-response cards are about.**

---

## 8. WHAT THE COLUMNS DECIDE

### 8.0 Read the rows in this order — they are not equally trustworthy

Standing invariant #5 is a ranking, not a footnote, and this table spans the whole
range:

1. **DD ambiguities and row 1b are DATUM-INVARIANT.**  A double difference removes
   the station-common and satellite-common terms identically, and row 1b is that
   same removal applied to the ionosphere (`DD(e) = eps(r1,s1) − eps(r1,s2) −
   eps(r2,s1) + eps(r2,s2)`, with `a_r` and `b_s` cancelling).  These two rows carry
   the card's verdicts.
2. **ZWD is absolute and carries no datum** — it is gradeable as an error, and it is
   the only non-invariant row that is.  ⚠ But on these legs it is dominated by the
   warm-resume re-convergence, so it must be quoted against the SEED, not against
   the null leg (§8.2).
3. **The consumer is a COLD consumer on every leg** — each leg starts its held-out
   station from scratch, so the 5-epoch cells are ignition-dominated and differ
   between columns partly for reasons that have nothing to do with the pin.  Read
   them as a direction, never as a number.
4. **Satellite clocks, receiver clocks/DCBs, satellite code biases and RAW row 1 are
   BASIS-DEPENDENT CONTEXT.**  Each is reported in the producer's realized S-basis,
   where the datum and the absorbed lumps move between arms.  Their flatness across
   columns is informative (nothing large moved into them) but their VALUES are not
   estimation error and must not be quoted as such.

### 8.1 The ionosphere: the root-cause inference is CONFIRMED, and the deficit is 2.6 TECU

**The number.**  Row 1b (`none` → `all`, window W2): DD-invariant per-LOS slant iono
rms **0.4233 → 0.0419 m**, a **10.1× collapse**; p50 \|eps\| 0.2387 → 0.0289, p90
0.7064 → 0.0683, max 1.741 → 0.143.  W1 says the same (0.4312 → 0.0427, 10.1×).
At 6.16 TECU per metre of L1 slant that is **2.61 → 0.26 TECU**.

**It is not a resume artifact, and that was checked.**  The SEED run's own converged
window (its epochs 640–665, immediately before the snapshot every leg resumes from)
reads row 1b **0.4647 m** with a_rms 1.317 / b_rms 1.831 — the same level and the
same factor structure as the null leg.  The null column is flat at 0.443–0.449
through all 57 epochs; `all` drops from 0.450 before the pin to 0.159 at +6..+10 and
**0.0455 by +11..+15**, then holds 0.042.  The collapse is the pin, it happens
within ~6 epochs of onset, and the baseline it collapses from is the producer's
CONVERGED level.

**Why the raw row 1 said the opposite, and why it never could have said anything.**
Raw per-LOS iono error is **basis-dependent**: in the producer's realized S-basis the
free per-LOS slant iono absorbs a STATION-common lump (the receiver GF/DCB share —
the Table-8 identity) and a SATELLITE-common lump (the satellite code-bias / S-basis
share).  The two-way fit measures exactly how big they are: **a_r rms ≈ 1.13–1.28 m
and b_s rms ≈ 1.85–1.93 m**, against **0.42 m** of DD-invariant interaction.  The
2.73 m raw rms is therefore ~97 % absorbed lumps, and **no ambiguity information can
touch either of them** — so raw row 1's insensitivity to the pin is forced by the
basis, not measured.  It is kept in the table as context and must not be read as a
result.  *The earlier draft of this section drew a falsification from it; that
inference was wrong and is withdrawn.*

**What this settles.**  Memory `project_why_non_integer_rootcause` reads "99.3 %
ionosphere", needing only 0.117 TECU to move a DD fraction.  Both halves now stand
together: the DD-invariant ionosphere error on this bed is **2.61 TECU — 22× the
0.117 TECU that moves a fraction** — which is why the float DD fractions are
uniform; and holding every ambiguity at its true integer **determines that
ionosphere to 0.26 TECU**.  **The `none` column's row 1b IS the integer-information
deficit, in metres: 0.423 m of L1 slant per LOS.**

**One caveat, stated rather than smoothed.**  The collapse is to centimetres, not to
the millimetre phase floor: 0.0419 m rms with a 0.143 m tail.  The pre-registered
algebra says a fully DD-consistent pin forces eps to the phase floor, so ~4 cm is
residual leakage, not the floor.  The most likely source is in this table: Galileo
L7Q is the one band the pin does not fully take under `all` (57.7 % at own integer,
max 0.103 cyc — §4), and the constellation split of row 1b is Galileo 0.0463 m
against GPS 0.0386 m, in the same direction.  Tropo/code leakage into the per-LOS
state is the other candidate.  Not separated here; it is an open item, and nothing
in this section depends on the difference between 4 cm and the floor.

**The partial columns do NOT interpolate, and one of them is worse than nothing.**
Row 1b: `L5Q` alone 0.4233 → **0.2793** (1.5× better, and still decaying at +57),
but `L1C+L2W` 0.4233 → **0.5632 — 1.3× WORSE than not pinning at all**, flat from
+6 onward, on both constellations (GPS 0.363 → 0.466, Galileo 0.500 → 0.683).
`sta S21` (0.4476) and `sat G08` (0.4741) are also slightly worse.  A consistent
reading, not a proof: on this posture the B bands of one LOS share ONE free
slant-iono state, so holding a proper SUBSET of them exactly over-determines the iono
in the pinned directions while leaving the lane directions free, and the tension
lands in the iono (the B-fold over-count of `SPEC_amb_iono_split_overcount`).  Only
the complete pin removes it.  **Integer information is worth having only when it is
complete on the LOS; a partial hold is a net loss.**

### 8.2 Where the error goes in the ZWD — and the seed number that reframes it

`none` → `all`, W2: ZWD rms **0.0669 → 0.0130 m (5.1×)**, p90 **0.0923 → 0.0212 m
(4.4×)**; consumer 3D p50 1.656 → 1.086 m, Up 1.439 → 0.691 m.  W1 agrees (0.0725 →
0.0138, 5.3×).  The ZWD is the ONE non-invariant channel that moves by a factor
rather than a percent, and it is the channel this project keeps finding at the end of
every level-error path (`project_zwd_sink_census_2026-09-09`: "FIELD consumer =
producer ZWD sink via the wire").

**⚠ The context number, and it changes the claim.**  The SEED run's own converged ZWD
over its epochs 640–665 — the epochs immediately before the snapshot every leg
resumes from — is **0.0076 m rms** (p50 0.0047, p90 0.0125, mean +0.0003), the known
quiet-twin level.  **The null leg's 0.0669 m is therefore the WARM-RESUME
re-convergence, not the producer's converged ZWD**: the leg jumps to 0.0675 at its
first five epochs, peaks at 0.0809 around +11..+15, and is still 0.0669 at +53..+57 —
**it does not recover inside the 57-epoch leg.**

So the honest statement of finding 2 is not "the pin improves the ZWD 5×".  It is:

- a warm resume costs this producer ~**0.07–0.08 m** of ZWD error that it does not
  work off within 57 epochs;
- a COMPLETE integer pin removes ~80 % of that within **two epochs** of onset
  (0.0653 pre-pin → 0.0241 at +6..+10 → 0.0136 by +11..+15, flat thereafter), landing
  at **1.7× the seed's own converged 0.0076 m** — close to, but not at, the
  converged level;
- `L1C+L2W` does the same (0.0160 plateau), so the ZWD recovery is bought by the
  **L1/L2 pair**, not by L5;
- `L5Q` alone makes it **3× WORSE first** (0.238 at +6..+10) and then decays back to
  roughly where it started (0.0634 at +57) — the single-number W2 cell "barely moves"
  hides a large transient;
- `sta S21` (0.0900) and `sat G08` (0.2138) are worse than the null throughout.

Everything else is flat to a few percent — rx DCB contrast 1.319 → 1.142, rx clock
contrast 2.092 → 1.980, sat clock rms 1.189 → 1.193, sat clock SD contrast 1.633 →
1.670 — and per §8.0 those are basis-dependent context, so the reading is only "no
large quantity moved into them", not a number.

### 8.3 The coupling map — what a PARTIAL pin does

**`L1C+L2W` (pin two of three GPS bands).**  The pinned bands go to 100 %, and so
does the FREE L5Q (71.5 -> 100.0 %, med \|e\| 0.206 -> 0.014 cyc): once two bands of a
GPS LOS carry exact integers, the third is determined through the shared slant-iono
state, so the pin PROPAGATES ACROSS BANDS on the same LOS.  ⚠ `L1C` is a GALILEO code
too, so this scope pins only ONE of Galileo's three bands — its L7Q chords are
untouched (0.131 med \|e\|), and Galileo is where row 1b degrades hardest (0.500 ->
0.683 against GPS's 0.363 -> 0.466).  ZWD follows `all` almost exactly (0.0144 vs
0.0130).  But the CONSUMER splits: Up collapses to **0.153 m** — the best vertical in
the table — while 3D *rises* to 3.203 m, i.e. the horizontal gets worse.  Code-bias
rms also rises slightly (0.190 -> 0.217).

⚠ **The two invariant rows disagree in sign on this column, and both readings are
real**: the DD INTEGERS all reach 100 % while the DD-invariant IONOSPHERE gets 1.3x
WORSE (0.4233 -> 0.5632, §8.1).  Exact integers on a subset of a LOS's bands do not
imply an exact iono on that LOS — the free ambiguity lands on its integer and the
iono absorbs the remaining tension.  This column is the sharpest open question the
card raises (§9).

**`L5Q` alone.**  L5Q -> 100 %, L2W -> 100 %, L1C 73.4 -> 96.5 %: the same cross-band
propagation, from one band this time.  ZWD ends where it started (0.0669 -> 0.0634)
after a 3x transient excursion to 0.238 (§8.2) — **so the ZWD recovery of `all` is
bought by the L1/L2 pair, not by L5**.  Two things get
conspicuously worse: **code bias rms 0.190 -> 0.763 m (4x)** — pinning the third band
alone forces the L5 satellite code bias to absorb what the L5 ambiguities no longer
can.  Its row 1b is the best of the partial columns (0.4233 -> 0.2793, 1.5x, and still
decaying at +57), i.e. one band of exact integers does buy some DD-invariant
ionosphere, unlike `L1C+L2W` (§8.1).  And the consumer is the BEST of the whole
table (3D **0.312 m**, Up **0.292 m**), which on a bed whose producer ZWD is
unimproved is a warning, not a win: the consumer is a single held-out station over one
5-epoch window, and this column is the one to re-measure before quoting.

**`station:S21000SIM` and `sat:G08` — a partial pin is NET-HARMFUL here.**  Both make
the network-wide DD integer rate WORSE (L1C at own integer 73.4 -> 61.5 % / 61.6 %,
L2W 68.6 -> 52.2 % / 52.9 %), both worsen the ZWD (0.0669 -> 0.0900 / 0.2138) and
`sta S21` worsens the consumer 2x (1.656 -> 3.388 m).  The run-end clauses say why:
neither arm settles — last-epoch fit residual **0.202 cyc** (S21) and **1.596 cyc**
(G08) against 1e-4 on the quiet full and band arms.  **A pin on a handful of arcs
(162 and 177) does not hold the network at truth; it asserts integers on a few arcs
while their neighbours contradict them, and the inconsistency is paid by the shared
states.**  That is the coupling map's real answer: on this posture integer information
is only useful when it is CONSISTENT across a band or the whole network — one station
or one satellite is worse than nothing.

### 8.4 The price of the information

The pin is the cleanest available measurement of how far the converged float
ambiguities sit from truth: **6.43 cyc rms / 38.1 cyc max** in the run's own SD frame
at the onset epoch of `all` (2 295 carried arcs).  Yanking that to zero at Lambda = 1e8
is a metre-class prefit event and the standing QC stack reacts exactly as it should —
sigma-hat-0 28.3, five within-epoch re-solves, five stations excluded, and **4 epochs
withheld at the A4 ceiling**.  A band-scoped pin costs nothing (`L5Q`: 0 withheld).
Any future card that feeds integer information into this producer must budget for that
transient or ramp the weight.

---

## 9. Deliberate non-goals and open items

- **`sigma_m` was not swept.**  Every arm here runs the default 1e-4 m.  The withhold
  transient of §8.4 is a direct function of it; a ramped or looser pin is the obvious
  follow-up and is not measured.
- **Galileo L7Q never fully takes under `all`** (57.7 % at own integer, max 0.103 cyc
  at a settled epoch) while L1C/L2W/L5Q reach 100.0 %.  L7Q is the thinnest band on
  this roster and `all` is the only arm with non-zero `no_station_term`; whether the
  >= 3-arc station-term rule is the binding constraint is untested.  This is also the
  leading candidate for the 4 cm that row 1b does NOT collapse (§8.1) — the
  constellation split is Galileo 0.0463 m against GPS 0.0386 m — and the two should be
  closed together.
- **Why a PARTIAL band pin makes the DD-invariant ionosphere WORSE than no pin at all**
  (`L1C+L2W` 0.4233 -> 0.5632 m) is offered as a consistent reading in §8.3, not as a
  proof.  It is the sharpest open question this card raises, and it is testable: a
  two-band pin on a posture with only two bands should not show it.
- **The ZWD contrast is against a warm-resume plateau** (§8.2).  A seamless (never
  resumed) arm would say what a complete integer pin is worth against a genuinely
  converged 0.0076 m ZWD; this card does not.
- **`arc:<sta>/<sat>/<sig>` scope is built and unit-tested but has no harness leg.**
- **`sta S21` / `sat G08` cannot be graded on the DD instrument at all** — a chord
  needs four pinned legs and those scopes supply at most two.  They are graded by the
  §6 table alone.
- **The consumer numbers rest on one held-out station over a 5-epoch window** and are
  the least robust cells in the table; the `L5Q` column in particular (3D 0.31 m)
  should be re-measured before it is quoted.
- **This knob must never be promoted.**  It is not in `docs/CONFIG_promotion_ledger.json`,
  it is not in any golden, and the loader hard-errors without a simulator truth root.

---

## 10. Reproducing

```powershell
# base (G1 reference) and ON exes, from THIS worktree
Copy-Item scripts\vs2026_build.ps1 $env:TEMP\vs2026_build_opin.ps1   # edit $proj to the worktree
& $env:TEMP\vs2026_build_opin.ps1 -Config x64-Release

python scripts\ev_oracle_pin_legs.py make
python scripts\ev_oracle_pin_legs.py run opin_null_base opin_null    # cap 2, always
python scripts\ev_oracle_pin_legs.py run opin_all opin_l12
python scripts\ev_oracle_pin_legs.py run opin_l5 opin_s21
python scripts\ev_oracle_pin_legs.py run opin_g08 opin_slip_null
python scripts\ev_oracle_pin_legs.py run opin_slip_all opin_slip_all
python scripts\ev_oracle_pin_legs.py status

python scripts\ev_oracle_pin_grade.py g1
python scripts\ev_oracle_pin_grade.py g2                 # pre-registered pin+1
python scripts\ev_oracle_pin_grade.py g2 --at-ep 33      # settled
python scripts\ev_oracle_pin_grade.py g3                 # -> runs/opin_table.json
python scripts\ev_oracle_pin_grade.py g4
```

Unit suite: `gnss_tests.exe --gtest_filter=OraclePin*` (22 cases, 4 suites in
`tests/test_oracle_pin.cpp`).

Both graders added a `--tow-min` / `--tow-max` pair to `scripts/simnet_grade.py` and
`scripts/simnet_amb_integer.py` (`--last-frac` can only name a TRAILING fraction, and
this card grades two short windows inside one 60-epoch leg).  Default (None, None) =
not one row filtered; `simnet_amb_integer --selftest` still PASSes.

---

## 11. Rebase certificate (onto `cc689d5`, Study 7's closure)

The card was rebased onto main after `bd97b9b` / `b268ac2` / `cc689d5` landed the F3
anchor clause and pinned `common_band_scope` + `if_phase_td_pairs` in the sim golden.
**Zero conflicts** — main's hunks in the three shared files
(`NetworkRunContext.hpp`, `ApplicationNetworkAssembly.cpp`, `NetworkTeardown.cpp`)
sit ~1500 lines away from this card's.

Smoke leg `opin_all_rb` = `opin_all` re-run on the post-rebase exe
(`F4FF8EF0…`).  The leg derives from the FROZEN seed config, so main's golden edits
do not reach it, and the result certifies that:

- the run-end clause reproduces **character for character** — 57 active epochs,
  **144 025** rows injected, **9 577** first pinned, **3 392** released, offers
  144 025 = 144 025 + 0 + 0, no anchor 17 100, no station term 34 988, fit 173 (3 on
  the last epoch), last-epoch residual **0.0001 / 0.0027 cyc** over 2 523 arcs;
- `Network producer complete: 723 epochs emitted, 4 skipped (… 4 by the A4 σ̂0
  ceiling)` — the same four withheld epochs;
- and **every solve artifact is bit-identical** to the pre-rebase run:
  `products.txt` `B3E149F2…`, `network_dd_fractional.csv` `5FEEB22F…`,
  `network_zwd.csv` `0877BA3B…`, `network_ionosphere.csv` `FBF86A96…`,
  `closed_loop_trace.csv` `5C507871…`, `network_events.csv` `7675CD40…`.

So every number in this document stands unchanged on the rebased tree.
