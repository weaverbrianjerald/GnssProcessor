# RF-NS — the S-basis null-space read: what the rank gate is actually withholding on (2026-09-15)

⚠ **See §6 (2026-09-15, appended): fix (a) as pre-registered in §4.1 is REJECTED at its own re-coupling gate, and the
"satellite absence" framing of the G07 / G30 / G04 stretches is corrected — those satellites are TRACKED throughout; what
is absent is their tracking STATIONS, rejected by the per-station PD test.  §1–§5 stand as written.**

**STATUS:** closed — **the frozen reading is HALF right, and the half it gets wrong is the one that matters.**  Every mid-run withhold in
both runs — the vehicle's 49-epoch hour-4 and 18-epoch hour-5 stretches AND the golden's own 5-epoch hour-1 cluster — is **ONE satellite's
`C1W` code-bias column sitting in the shared border with EXACTLY zero information** (eigenvector weight 1.00 on that single column, every
other component 0.00, eigenvalue 0.0 to 1e-16).  It is not a phase datum, not a held basis without a successor, not a thin satellite: it is
a CODE-BIAS column that re-enters the border at the start of a satellite's absence and is not covered by any datum row until
`readmit_seed_horizon_s` seeds it at the return.  The identity is exact in all three instances — **the withhold stretch is
`gap_ep − 1` epochs long and closes on the seed epoch**: G04 `gap_ep=6` / 5 withheld (both runs), G07 `gap_ep=50` / 49 (vehicle),
G30 `gap_ep=19` / 18 (vehicle).  The IGNITION deficit is a different animal and IS the frozen reading's class: 14–21 (one epoch 33)
per-satellite `{CLK|Gxx, PB|Gxx|L1C, PB|Gxx|L1W, PB|Gxx|L2W}` lumps that the S-basis completion cannot separate because it has **no carried
level to target** — every ignition `sbc_engage` carries `target=0.0000`, and the deficit dies in ONE epoch (skips 11 → 0, heal 25 → 0) at
TOW 261660, the epoch after the leveling harvest's first carried arcs appear.  Both halves are a **DECLARATION** fix; **no rank read of the
float block is owed**.  Instrument: `scripts/rf_null_space_read.py`.  The census latch is now re-armable
(`network.trace_estimability_budget` / `trace_estimability_deficit_only`, defaults trace-identical).

## 1. The gap in the pre-registration

RF-3-read §7.4 pre-registered the read against `network_estimability.csv` at the vehicle's hour-4/5 stretches.  That file could not answer
it: the eigen-census is a **one-shot dump with a first-N latch** — `ctx.counters.spectrum_diag_budget = 6`
(`include/app/NetworkRunContext.hpp:3275`), decremented at `src/core/filter/NetworkSolveGlue.cpp:659-660`, with the column-descriptor map
built under the same predicate at `src/core/app/ApplicationNetworkAssembly.cpp:2196`.  Both existing runs therefore hold exactly **6 EPOCH
dumps, all at TOW 259200–259350**, and nothing at hours 4–5.  The read is in two parts: Part A off the existing traces, Part B off one
trace-only code change and one Release re-run.

Worth recording, because it is the whole value of Part B: the per-epoch attribution that DOES exist on every epoch — the
`GATE / sbc_skip` events, which name the directions the S-basis completion could not separate — **is misleading for 30 of the 49 hour-4
epochs**.  It reports the flat clock direction (`CLK:G16:0.18`, i.e. 1/√32 on all 32 clocks — the declared gauge) as the skipped one, while
the census shows `CB|G07|C1W = 1.00` present from the stretch's FIRST epoch.  A skipped-direction label is the completion's choice among a
rotation of an unseparable pair; only the spectrum names the span.

## 2. Part A — the ignition read (existing traces, no build)

The 6 dumped epochs are **byte-identical between `_rf0g_field6` and `_rf2d_field6_both`** (`network_estimability.csv` differs only in its
final RUN summary row) — the vehicle's knobs act later, as expected.

**The spectrum at TOW 259200** (n_shared 209, declared_dim 1, ev_max 2.3268e+05, cutoff = 1e-9·ev_max = 2.3268e-04, n_below **16**):

| class | n | ev | shape |
|---|---|---|---|
| broad multi-satellite PB | 2 | 1.91e-04, 1.28e-04 | 5–10 satellites' `PB|*|L2W` at 0.20–0.37 — **the only two the SBC separates and constrains** (`sbc_engage`, both `target=0.0000`) |
| per-satellite lump | 14 | 1.99e-10 … −1.50e-10 (several NEGATIVE ⇒ machine zero) | `PB|Gxx|L2W ≈ 0.44±0.09`, `PB|Gxx|L1W` = `PB|Gxx|L1C` ≈ 0.80× that, `CLK|Gxx` 0.17–0.33, with a second satellite leaking in; two carry a `CB|Gxx|C1W` component (G28 0.129, G30 0.107) |

The L1W and L1C weights are **exactly equal** in every lump: the direction is per-satellite and per-FREQUENCY, not per-signal — one
satellite's clock / phase-bias / slant-iono internal split, with the two-band posture leaving one such freedom per satellite after the code
datum closes the other.  Satellites carrying a lump at 259200: G01, G02/G21, G07, G10, G11/G20, G13, G15, G18, G27 (×2), G28, G29, G30,
G32, plus one all-`CLK` mixture.  Over the full 75-epoch ignition (Part B's census) the recurring lumps are G01 (49 epochs), G28 (45),
G32 (23), G07 (18), G10 (17), G13 (16).

**The gate sees 14, not 16**, because the census runs PRE-completion and the SBC folds the two separable directions before
`assertNullSpaceRank` — `observed 14 … declared 1` in the withhold event.  The drop-and-heal then soft-anchors 17–36 near-null PHASE-BIAS
columns every ignition epoch (17 at 259200, on 12 satellites) and every row reads `soft_anchor_insufficient`.

**Why 16, and what clears it.**  Not admission: `n_present = 59` and `n_clock_sats = 32` are constant across the whole ignition, and no
satellite is thin in the sense the frozen reading named.  What is missing is the completion's TARGET.  The ignition span is epochs 1–82
(TOW 259200–261630): the gate throws on all 82, the heal rescues 7 (the 7 emitted epochs inside it), 75 withhold.  Then, in ONE epoch:

| | ep 82 · TOW 261630 | ep 83 · TOW 261660 |
|---|---|---|
| `sbc_skip` (unseparable orphans) | 11 | **0** |
| `sbc_engage` | 2 | 4 |
| `phase_heal` columns | 25 | **0** (zero heal epochs anywhere after 261660) |
| `sbc_engage` target | `target=0.0000` | `target=-4.7412`, `7.2394`, `2.6067`, … |
| gate | throws | passes on the first evaluation |

`network_amb_state.csv` has **zero rows before TOW 261630** and its first rows at 261630 — the leveling harvest's first carried arcs.  The
S-basis completion re-uses that harvest for its continuity targets (`src/core/app/NetworkRunContext.cpp:406-411`, the `sbasis_complete`
resolution, "the `pb_level_prev` harvest … the completion's continuity targets re-use"), and it constrains each extra direction along its
eigenvector **at the carried level**.  With no carry the target is 0 for every direction, the per-satellite lumps are "machine-zero orphans"
it declines to separate, and the phase-bias heal — a soft anchor — cannot lift an eigenvalue above a cutoff that scales with λmax.  The
ignition deficit is therefore a **carry-cold** condition, consistent with the frozen reading's family (an unpinned per-satellite phase
gauge) though not with its named cause (no successor / thin satellite).

## 3. Part B — the stretch read (trace-only change + one Release re-run)

**The change** (trace-only, no solve-affecting path): `network.trace_estimability_budget` (int, default **6** = the constant it replaces)
and `network.trace_estimability_deficit_only` (bool, default **false**).  `GnssConfigTypes.hpp` NetworkConfig fields with Doxygen; parsed in
`GnssConfigLoadNetwork.cpp` beside `trace_estimability`; echoed in the run manifest (`NetworkRunTraces.cpp`); the budget is seeded once into
`counters.spectrum_diag_budget` by the `NetworkRunContext` constructor; the latch in `NetworkSolveGlue.cpp` now spends the budget AFTER the
dump, and under `deficit_only` a clean epoch (`n_below <= declared_dim`) writes no rows and spends nothing.  With `deficit_only` false the
spend is unconditional — bit-for-bit the previous order, since nothing between the two points reads the counter.

⚠ `deficit_only` is **not** "withheld epochs only", and cannot be: the census precedes the SBC completion, so `n_below > declared_dim` is
true on most epochs (the SBC folds 1–4 extras routinely).  A budget of 200 was consumed by epoch ~250, nowhere near hour 4; the run below
used **2000** (the eigen-decomposition on a 210×210 reduced border is microseconds).  It landed on **168 epochs** — the 147 withheld ones
plus 21 that a completion or heal cured.

**Build / run.**  `scripts/vs2026_build.ps1 -Config x64-Release`, exe copied to `gnss_processor_rfns.exe`,
sha256[:16] **`b8b1cc190e7e5285`**.  Arm `runs/_rfns_field6_both/arm.json` = `runs/_rf2d_field6_both/arm.json` with only the run path and the
two new knobs changed (`diff` is 3 lines).  6 h cold, field DOY-129, 59 stations, exit 0, ~2.5 min wall.

**Byte-identity gate — PASS on all three artifacts** (the change is trace-only):

| artifact | `_rf2d_field6_both` | `_rfns_field6_both` |
|---|---|---|
| `products.txt` (the emitted product) | `1b800c79199d6b1b…` | **identical** |
| `logs/closed_loop_trace.csv` | `c5c7abb73d347faa…` | **identical** |
| `logs/network_epoch_status.csv` | `45ce089e71a7d5e6…` | **identical** |

**The stretch census.**  Both stretches, every epoch, `declared_dim = 1`, `n_shared = 210`:

| stretch | epochs | n_below | direction 1 | direction 2 | directions 3–4 (when n_below = 4) |
|---|---|---|---|---|---|
| hour 4 · 274860–276300 | 49 | 2 on 30 ep, 4 on 19 | `CLK-flat`, ev 1.2e-10…3.4e-10, every weight 0.18 = 1/√32 — **the declared clock gauge** | **`CB\|G07\|C1W = 1.00`**, ev 0.0 … ±7e-16, every other component 0.00 — on **all 49 epochs** | PB pairs on G16 / G17 / G27 (`PB\|G17\|L2W` 0.51, `L1W` = `L1C` 0.41; `PB\|G16\|L2W` 0.42–0.48) |
| hour 5 · 279960–280470 | 18 | 2 on 17 ep, 5 on 1 | `CLK-flat`, all-clock 0.18 | **`CB\|G30\|C1W = 1.00`** — on **all 18 epochs** | one epoch adds PB pairs on G02 / G21 |
| golden hour-1 twin · 264390–264510 (**in BOTH runs**) | 5 | 2 on all 5 | `CLK-flat`, all-clock 0.18 | **`CB\|G04\|C1W = 1.00`** — on all 5 | — |

**The co-incident events — one mechanism, three instances.**  The satellite's `C1W` code-bias column is REMOVED at departure
(`READMIT park … D2 clock + code-bias carry parked at departure` + `TOPOLOGY col_removed label=CB:Gxx:C1W`), the satellite's absence gap
then runs, and the column is present again in the reduced border for the whole gap with no observations and no datum row.  It is pinned only
when the readmission seed fires:

| sat | park / col_removed | withheld stretch | seed epoch | at the seed |
|---|---|---|---|---|
| G04 (both runs) | before 264390 | 264390–264510, **5** ep | 264540 | `READMIT seed_code_bias G04 C1W value=-4.2219` (golden) / `-3.7526` (vehicle) + `TOPOLOGY col_added CB:G04:C1W` + `readmit gap_ep=6` |
| G07 | 274260 | 274860–276300, **49** ep | 276330 | `seed_code_bias G07 C1W value=-0.0508`, `col_added`, `readmit gap_ep=50`, `cta_decouple reason=gap` |
| G30 | 279510 | 279960–280470, **18** ep | 280500 | `seed_code_bias G30 C1W value=3.7694`, `col_added`, `readmit gap_ep=19`, `cta_decouple reason=gap` |

Withheld epochs = `gap_ep − 1` in all three.  Note `TOPOLOGY col_added` appears only at the seed: the topology ledger is a delta against the
last EMITTED epoch, so the column's silent return during a withhold stretch is invisible to it — invariant #7's re-coupling edge with the
column back and the datum not, exactly the class the FT63 leg-19 ledger was built to catch.

**Why no row covers it.**  `C1W` is not in this posture's IF pin: `selectIfPinPair` takes the two lowest DISTINCT-frequency code signals
sorted by band digit then name, so with `{C1C, C1W, C2W}` registered the pair is `C1C + C2W` (confirmed by this run's
`CLK_BASIS reselect … if_pin_reference_moved C1C+C2W->C1W+C2W` events on G01).  The per-satellite geometry-free pin
(`sat_gf_datum: broadcast_tgd`) pins that same pair (`src/core/filter/NetworkSbasis.cpp:848-869`).  `C1W`'s only datum is therefore its
channel's sum-to-zero row — and under this posture's `code_datum_time_linked: true` that row spans only the channel's VETERANS:
`src/core/filter/NetworkSbasis.cpp:1196` (`for (const auto& sat : tl.jac_sats) srow(col_of[sat]) += 1.0;`).  A satellite whose code-bias
carry was parked at departure has no previous value, so it is in `present_sats` and not in `jac_sats`, and its column leaves the row.  The
census proves the column is BARE; that this one predicate is the miss is strongly supported but not separately instrumented — the fix below
carries a coverage counter that settles it.

## 4. Part C — the verdict

The pre-registered rule, verbatim (RF-3-read §7.4):

> Frozen reading: the extra directions are families whose phase datum is momentarily unpinned (a held basis with no elected successor, or a
> thin satellite whose only carrying stations just died) ⇒ a DECLARATION fix (declare them, pin or withhold per family, not the epoch);
> anything else ⇒ a rank read of the float block at that geometry before any build.

Applied honestly:

- **Mid-run stretches (67 vehicle epochs + the golden's own 5): the frozen reading's MECHANISM is FALSIFIED.**  There is no phase datum in
  the direction at all — it is a single satellite code-bias column in the SHARED border, with no float-block participation, and the
  satellite is absent rather than thin.  The held-basis handovers in the window (G07's two continuous handovers at 274080) precede the
  stretch and are not in the span.
- **The REMEDY CLASS the rule names is nevertheless the right one, and for a sharper reason than the rule anticipated:** the defect is per
  FAMILY (one `(sat, signal)` column), it is present on every epoch of the gap, and it is already declared correctly 6–50 epochs late by
  `readmit_seed_code_bias`.  This is a **DECLARATION** fix.
- **Ignition: DECLARATION as well** — the per-satellite `{CLK, PB×3}` lumps are a real gauge freedom with no target, not a rank defect of
  the float block; the deficit clears the epoch the carry exists.
- **No rank read of the float block is owed.**  Neither half is "anything else".

### 4.1 Pre-registered fix — NOT BUILT

**(a) The mid-run class — declare a returning code-bias column when it re-enters the border, not at the readmission seed.**
Preferred site: `NetworkSbasis::buildCodeBiasDatumRows`, the sum-to-zero block at `src/core/filter/NetworkSbasis.cpp:1152-1210`.  Add a
per-epoch COVERAGE census over `registered_code_keys`: any column that no built row spans (IF pin `:700-742`, GF pin `:848-869`, family GF
`:1060-1105`, sum-to-zero `:1181-1210`) gets its own first-sighting row at target 0 — the case the IF pin's own comment already sanctions
("the absolute target survives as the FIRST-SIGHTING anchor").  The counter is the ON-path telemetry invariant #4 demands: `n_uncovered_cb_cols`
per epoch, expected 0 on a settled run and 1 through each readmission gap.  The rejected alternative is to de-register the column while the
satellite is parked (at the `READMIT park` site) — it changes which columns exist, so it is fingerprinted and a migration card, not this fix.
Do **not** touch `NetworkPhaseDatum.cpp`'s election: the census shows the phase datum is not the term for these stretches.

**(b) The ignition class — give the S-basis completion a cold target.**  Site: the completion call at
`src/core/filter/NetworkSolveGlue.cpp:789` (`sb.completeSbasisDimension(…, sbc_x_prev, …, ctx.knobs.sbasis_complete_max)`) and the
`sbc_x_prev` fill above it (`:740-780`), which is empty of `pb_level_prev` before the first harvest.  Two candidates, both default OFF:
constrain a per-satellite lump at its CODE-derived level on a carry-cold epoch, or raise `sbasis_complete_max` (6) above the 11–16 orphans
per ignition epoch so the completion is not capped before it starts.  ⚠ `NetworkSbasis.cpp:1055-1062` records the standing objection —
completing a gauge at a cold carried value passed the rank gate and destroyed the ionosphere on `SPEC_synthetic_minimum_network` §12 rung 3
(a00 = −6292 TECU).  Gate closure is not acceptance; the ionosphere level is the bar, and (a) must land first and alone.

**The bar for (a)** (the card's own): the RF-2d field arm re-run reaches **≥ 715/720 emitted** with the consumer's hours 3–6 unchanged
within noise (0.84 / 0.65 / 1.37 / 0.60 m).  It removes 67 of the vehicle's 147 withholds and 5 of the golden's 80, and touches no other
epoch — the ignition 75 are untouched by (a) and are (b)'s target.

**What this does NOT fix:** the vehicle's remaining 75 ignition withholds and its hour-2 consumer (3.00 m).  (a) alone does not put the
float vehicle back in contention; it removes one of the two reasons its emitted count is 573.

## 5. Run dirs, instruments

- `runs/_rf0g_field6` — the golden (RF-0g posture), 640/720 emitted, 80 withheld.
- `runs/_rf2d_field6_both` — the float vehicle (RF-1 + RF-2b + RF-2d), 573/720, 147 withheld.  Unchanged by this card.
- `runs/_rfns_field6_both` — the re-run of that arm with the re-armed census.  Byte-identical solve; 168 census epochs.
- `scripts/rf_null_space_read.py <run_dir> [--tow-min --tow-max]` — the grader: dispositions and withheld stretches, the eigen-census with
  each direction classified (`CB-orphan` / `CLK-flat` / `sat-lump` / `PB-pair` / `PB-broad` / `mixed`), the per-epoch `sbc_skip` / `phase_heal`
  attribution, co-incident events in (t − 2 ep, t], phase-datum coverage across each stretch, and arc births / deaths.
- Unit suite after the change: **2217 tests from 292 suites, 2216 passed, 1 skipped** (`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`,
  the fixture gate).

## 6. 2026-09-15 — fix (a) as pre-registered is REJECTED at the re-coupling gate; the G07/G30 stretches are vehicle-only observation loss

§4.1(a) was taken to build.  The build stopped at its own pre-registered re-coupling gate and the fix as written is
**rejected**.  Four findings, all from the existing traces and the source; §1–§5 above stand unchanged.

### 6.1 (F1) The pin lands in the same normals as the readmission seed, and loses to it

Gauge rows are folded at `src/core/filter/NetworkSolveGlue.cpp:336` (`buildCodeBiasDatumRows`); the pin's site is the
sum-to-zero block `src/core/filter/NetworkSbasis.cpp:1181-1210` at the gauge weight `kCodeBiasGaugeWeight = 1.0e3`
(`include/filter/NetworkSbasis.hpp:260`).  The null-space gate is at `NetworkSolveGlue.cpp:925` / `:971` →
`:1048` (`gate_withheld`) → `:1125` (`EpochAction::SkipEpoch`); `networkInjectD2CarryPrior` is at `:1159`, **after** the
gate; the parked entry seeds the returned column at `src/core/app/NetworkRunContext.cpp:3527-3531`
(`if (cp.t_park_s > 0.0) { if (!readmitColumnPresent(est, cp)) continue; seed_gap = now_s - cp.t_park_s; … }`,
`var_eff = cp.var + q·seed_gap`); the solve is at `NetworkSolveGlue.cpp:1243`.  A target-0 pin at 1e3 in the same normals
as a seed at `1/var_eff` takes share `1000 / (1000 + 1/var_eff)`:

| instance | seed value (m) | `var_eff` | pin share | realized | error |
|---|---|---|---|---|---|
| G04, golden | −4.2219 | 8.103e-03 | 89.0 % | −0.464 | **+3.76 m** |
| G04, vehicle | −3.7526 | 1.221e-02 | 92.4 % | — | **+3.47 m** |
| G30, vehicle | +3.7694 | 1.559e-04 | 13.5 % | — | −0.51 m |
| G07, vehicle | −0.0508 | 2.620e-04 | 20.8 % | — | +0.011 m |

(values verbatim from each run's `READMIT seed_code_bias` events).  The pin does not politely yield to the seed; on the
G04 class it *wins*, and the returning code bias is realized metres from its own carried value.

### 6.2 (F2) It is permanent, not one epoch — the pinned value becomes the carry

`src/core/app/ApplicationNetwork.cpp:2481` returns before `networkStashCarry` on a withheld epoch, so nothing is stashed
while the gate throws.  The moment the pin lets an epoch be KEPT, `src/core/app/NetworkRunContext.cpp:6402-6417` stashes
the column unconditionally (its only guards are `plan.shared_datum` pins and `var > 0`) and refreshes `code_bias_prev_`
from `x_s(col)`.  The pinned value therefore (a) becomes the live D2 carry, (b) makes the satellite a channel VETERAN
(`src/core/filter/NetworkSbasis.cpp:1193-1197` puts it in `jac_sats`, and the s2z RHS `Σ b̂_prev` carries it forward), and
(c) RETIRES the park (`NetworkRunContext.cpp:6512-6520`, `live.count(...) ⇒ continue`), so the genuine
`seed_code_bias` at the real return never fires.  The invariant-#7 re-coupling edge MOVES (G07 `gap_ep` 50 → ~20) and
`seed_held` then re-validates against a data-free solve.  Consequence: the pin would fire on ONE epoch per absence
(3 column-epochs across the two runs), not 72 — the gate would clear every other epoch through a corrupted s2z RHS.

### 6.3 (F3) The census predicate is not implementable from what the seam sees

§4.1(a) specifies "any column that no built row spans", with "(a) any observation row touches the column" as half of it.
The column registers because ≥ 2 PRESENT stations hold the `getSatBiasStates()` entry
(`src/core/app/ApplicationNetworkAssembly.cpp:2113-2129`) — a fact about station STATE, independent of whether a code row
ever entered the normals.  `NetworkEstimator` (`include/filter/NetworkEstimator.hpp`) exposes no column-occupancy and no
pre-datum diagonal accessor, so the seam cannot ask "did any row touch this column this epoch".  That accessor is a
prerequisite, not an implementation detail.

### 6.4 (F4) The framing "the satellite leaves the network entirely" is FALSE

Measured in `network_pf_keyed.csv`: in `runs/_rf0g_field6` G07 carries 12–14 post-fit CODE rows on EVERY epoch of
274800–276400 and G30 likewise through 279900–280600, and the golden does not withhold there; in the vehicle G07 has 2
rows over 274260–274830 (against the golden's 12) and then none.  So the 49- and 18-epoch stretches are an OBSERVATION-LOSS
pathology that only the vehicle has; only G04 is a coverage dip common to both arms (4 rows golden / 2 vehicle up to
264360 and from 264540).

⚠ **Sharpened the same day — the loss is STATION-level and it is in BOTH arms.**  See
[RF-NS-b](RF-NS-b_vehicle_obs_loss_2026-09-15.md): G07 is TRACKED by 10–13 stations on every epoch of its "absence"
(`network_if_slip.csv`, which is **sha256-identical** between the two runs — the observation stream is bit-identical).
What differs is how many of those stations survive `NetworkNormalEquations::addStation`'s per-station PD test
(`src/core/filter/NetworkNormalEquations.cpp:476-484`): a median of **29 of 59 stations per settled epoch are dropped in
the GOLDEN and 42 in the vehicle**.  G07 reaches the solve at 6–7 stations in the golden and at exactly ONE
(DARW00AUS) in the vehicle.  The post-fit row count is therefore an in-solve-station count, not a coverage count.

### 6.5 What replaces fix (a)

Fix (a) is re-pre-registered as **(b2)** in RF-NS-b — same target-0 pin, but with the pinned column EXCLUDED from the
stash, from veteran promotion and from park retirement, behind the occupancy accessor F3 names, so the park stays parked
and the genuine seed fires unchanged at the real return.  The larger item is **(b1)**, the station-level PD rejection
itself, which is what actually empties these columns.  §4.1(b) (the ignition class) is untouched by this section.
