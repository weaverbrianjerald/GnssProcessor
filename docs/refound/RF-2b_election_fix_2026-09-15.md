# RF-2b — the composed election, the column-scoped depose, and the clock-level read (2026-09-15)

**STATUS:** closed — **the sim is fully restored (720 / 720, WL integers 99.1 %), the field keeps the phase-wire win at a sane handover rate with the
clock ramp gone and the most stable field consumer in the record (0.77–1.48 m every hour), and R-CL names the next term: the clock's own
level channel.  RF-1 + RF-2b is the vehicle candidate; the goldens' re-base on it is the author's call (on RF-0g's posture once that is the
baseline).**  Worker branch `worktree-agent-a2915f195d8518bf6` (`d271f4b`), merged.  Suite 2192 / 290 (0 failed; +6 cases, 21 in the suite); OFF
smoke `47bc4e5a…` on the tree it was built on; `--verify` PASS.

**The fix, and what its counters found.**  The composed rank (highest elevation among ledger-usable candidates, elevation-only as a counted
fallback) landed — and the per-cause deposition counters immediately exposed two structural defects: (1) **an incumbent's ledger verdict is
unavailable by construction** — a gauge-pinned column is excluded from the carry at inject and harvest, so the handover ledger has no live
entry for the station holding the basis (the first sim arm: 44 008 "ledger" deposals against 1 293 held family-epochs — a basis held in name
only); re-validating a held CONSTANT against a ledger that does not track it is a category error — test removed, counter kept (reads 0); (2)
**the age gate is a candidate gate, not an incumbent one** — a pinned column is not harvested, its carry goes stale and is gap-pruned, and the
age test then deposed the holder for having held — the incumbent is now tested on its own basis column's flag only, arc death counted at the
sticky loop's real tracking test.  Also a wiring gap in RF-2's item 2: the declaration loop sat inside an unrelated predicate, so only 9 of 100
sim handovers reached it — hoisted, with the rule **trace every handover, mark only those whose basis value changed** (a continuous handover
inherits the frame, Δ = 0; marking it forces a consumer reset for a wire that did not move — what took RF-2's marks to 5.3×).  Deposals: field
5 flag / 159 arc-death / 0 ledger; sim 0 / 100 / 0.  Held : elected — field 63 870 : 282, sim 44 998 : 174 (**99.6 % held**).

## The arms (6 h cold, both beds, the RF-0 configs; A3 + RF-2b)

| bar | SIM | FIELD |
|---|---|---|
| availability | **720 / 720** (RF-2: 358) | **720 / 720** |
| S3 WL at the integer ≥ 98 % | **99.1 %** (RF-2: 35.2 %); L1C med |e| 0.086 cyc | — |
| S4 / D1 `b` | 1.072 ✓ | 1.081 ✓ |
| D_phase / RF-0 by hour | 1.03 / 0.62 / 0.68 / 0.81 / 0.51 / 0.84 (5 of 6; hour 1 ignition) | 1.63 / **0.54 / 0.33 / 0.23 / 0.30** / 0.86 (5 of 6) |
| consumer p50 by hour (m) | 0.46 / 0.59 / 1.09 / 1.44 / 6.27 / 2.68 | **0.77 / 0.95 / 1.48 / 0.88 / 1.25 / 0.93** (RF-0: 3.02 / 3.00 / 1.55 / 2.14 / 0.98 / 3.29 — 4 of 6 better; settled 0.93×, bar 0.6×) |
| PB marks ≤ declared + 1.5× RF-0's | — | **163** ≤ 146 + 113 ✓ (RF-2: 399) |
| handover rate | 29 / h | **47 / h** ✓ (RF-2: 546) |
| unsignalled clock steps > 2 m | 0 | 0 |
| erases retired / basis drift | 0 / 0 | 0 / 0 |

**The field's clock-level ramp is gone**: `clk_level_p50` 0.78 / 0.64 / 0.70 / 0.73 / 0.74 / 0.70 (RF-0 0.70–0.74 flat; RF-2 1.22 → 2.29);
unsignalled clock steps > 0.5 m 102 (RF-2) → 26 (RF-0 29).

## R-CL — the clock-level read (`scripts/clock_level_read.py`)

Per satellite per 10-epoch window, the median (emitted clock − COD) de-meaned across that window's satellites (the free common gauge cancels),
joined to the basis tenure and the Q3 trips; the verdict on |level step| at a handover boundary against the same run's non-handover
boundaries: **RF-0 0.690 vs 0.690 m (ratio 1.00, 30 handovers / h); RF-2 0.756 vs 0.693 (1.09, 546 / h); RF-2b 0.700 vs 0.694 (1.01, 52 / h).**
**The level RUNS THROUGH handovers** — a handover boundary is indistinguishable from any other in every arm — so the held value does NOT carry
the basis station's bias at election and the ledger screen / a higher `min_age` is not the lever.  Sharper than the card's dichotomy: RF-0
itself shows the same per-boundary motion with the churning phase bias fully in place — **the churning PB never supplied a level datum; the
clock's per-satellite level channel has always moved at ~0.69 m per 100 s on its own, and the handover RATE multiplies that motion into the
wire** (546 / h → the 1.2 → 2.3 m ramp; 52 / h → RF-0's flat 0.70).  The net-drift statistic is arc-confounded and decides nothing (printed,
labelled).

## Grade and reading

The sim's bars all pass; the field passes the wire, marks, rate, contract and integrity bars, misses the consumer's 0.6× settled bar (0.93×)
and hour 1 of D_phase (ignition).  **RF-1 + RF-2b is the vehicle candidate** — the first field posture with every hour's consumer under 1.5 m —
and the remaining ~1 m has two named terms: the RF-0 posture's own (the GIM, the C2X stations — RF-0g in flight) and **the clock's level
channel** (0.69 m per 100 s, present on every arm since RF-0, held to mm per step by the Q3 anchor's increments yet moving at the level).

## RF-2c (pre-registered): what moves the per-satellite clock level at 0.69 m per 100 s — a read before any build

On `runs/_rf2b_*field*` and RF-0's field run: every per-satellite level step ≥ 0.3 m between consecutive 10-epoch windows, attributed within
± 1 window to the nearest (a) Q3 anchor trip / re-couple on that satellite (`cta_decouple` / `cta_recouple`; ~325 trips per 6 h ≈ one per 66 s
network-wide), (b) IF-pin re-seed bump (`this_epoch_cb_reseed`, the datum's first-sighting / pair-change re-anchoring), (c) a datum-station or
pivot event, (d) a gross-LOS or T4 flag on that satellite — against a shuffled null (1 000 shuffles); the rate of steps per satellite per hour
beside each satellite's Q3 trip count; and the same for the Q3 arm with `clock_anchor_gauge_free: true` (one 6 h field arm, config-only:
the anchor's prior on the gauge-projected state instead of the raw state — the loader's own §2.1 advice).  **Frozen reading:** (a) enriched > 3×
over the null ⇒ the anchor's decouple / re-couple edges are the level's motion — the card is the re-coupling edge (re-validate before
re-engaging, or a smaller `clock_anchor_k` ladder) and `gauge_free` is graded on the same read; (b) enriched ⇒ the IF-pin re-seed is the motion
— the level datum row of RF-0e (e1) is re-graded on the consumer with the ISC term now gone; none enriched and `gauge_free` flat ⇒ the motion
is the clock's own random walk between the anchor's floors and needs a per-constellation level row — built then, not now.
