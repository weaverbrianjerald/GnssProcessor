# RF0HV18: the end-of-week combined golden candidate

**STATUS**

- **id:** RF0HV18
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-06, before any 6 h run. Question: can the built-off keys of the week be armed together as the next golden baseline `_rf0hv18`, and what does each add to or cost the combination? Candidate C is the field golden plus the three signal decisions of card N3c (D1 `network.n3c_bia_pin_pair`, D6 `network.n3c_wire_per_signal`, D7 `network.n3c_ledger_lifecycle` with `network.roster_tracker` and `network.ledger_reentry`) and the refloat maturity gate (`network.raim_refloat_maturity_gate`, 120 epochs). One Release exe runs every arm: control, C, four leave-one-out arms and three add-one arms (pivot election, IF-TD post-flag re-seed, full-equation seeds) on DOY 129, control, C and C plus re-seed on DOY 132, control and C on the sim bed. Nothing is armed by this card.
- **source:** `scripts/nom3_designs/N3b_decisions.md` sections 9 and 10 (D1 to D7 and their 6 h grades); cards [NOM5](NOM5_station_parameter_set_tail_2026-10-30.md) (the recommendation bar, G3), card NSP (census noise floor, quoted in CLAUDE.md), [PIV](PIV_pivot_election_coverage_2026-10-17.md), [ARL](ARL_integer_ambiguity_ladder_2026-10-09.md) (re-seed key), card ICL5 (`docs/refound/ICL5_reseed_key_over_a_day_2026-11-06.md` on branch `icl5/reseed-key-day`, unmerged), [MDAY](MDAY_multiday_candidate_2026-10-19.md) (the DOY 132 setup), [BOF](BOF_built_off_follow_through_2026-10-28.md) (the baseline `_rf0hv17`)
- **knobs / posture:** see section 2. No golden, manifest or ledger row is edited.
- **dates in the text:** 2026-11-06
- **hashes / run dirs:** branch `golden/rf0hv18` from main `284ec29`, worktree `D:/GnssProcessorSupport/worktrees/rf0hv18`; run root `D:/GnssProcessorSupport/runs_rf0hv18` (`RAPID_RUNS_ROOT`); scratch, arm scripts and graders `D:/GnssProcessorSupport/gates/rf0hv18/`; exe and sha256 recorded in section 4
- **gates (frozen at pre-registration):** section 3

**Outcome.** Pending. Pre-registered 2026-11-06.

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

The cards of the week left several default-off keys graded neutral or better on their own, each against the golden. Arming them one at a time re-bases the goldens once per key. The author's directive is that built work is activated rather than left in open cards. This card grades the combination on one exe, so that one re-base can replace several, and grades each member by removing it from the combination. The add-on keys (pivot election, post-flag re-seed, full-equation seeds) are graded on top of C. The decision to arm anything is the author's.

## 2. Arms

All arms are copies of a golden with only the listed keys changed (`scripts/rapid_arm.py make`, tags `rf18_<bed>_<arm>`), 6 h, one Release exe from the branch build. Keys were checked against the loader (`src/core/config/GnssConfigLoadNetwork.cpp`) and the cards:

- D1 `network.n3c_bia_pin_pair`: pins the bias product's clock-reference pair (GPS C1W with C2W). No companion.
- D6 `network.n3c_wire_per_signal`: one wire bias per modelled signal. Armed in the BeiDou golden; not in the GPS + Galileo goldens.
- D7 `network.n3c_ledger_lifecycle`: arms `network.ledger_screens` and `network.ledger_anchor` itself (loader). Its third site, gap re-entry, is `network.ledger_reentry`, which needs `network.roster_tracker` and `network.cycle_slip.gap_reentry_relevel` (already true in the field golden). As graded (`N3b_decisions.md` section 9, "the arm sets them"), D7 is the three keys `n3c_ledger_lifecycle`, `roster_tracker`, `ledger_reentry`.
- Gate: `network.raim_refloat_maturity_gate` true with `network.raim_refloat_maturity_epochs` 120. The ledger row states it was never A/B tested.

| arm | field DOY 129 (bed f) | field DOY 132 (bed g) | sim (bed s) |
| --- | --- | --- | --- |
| ctrl | golden unchanged | day golden unchanged | golden unchanged |
| c | D1 + D6 + D7 + gate | same | same |
| noD1, noD6, noD7, nogate | c minus that member | | |
| cpiv | c + `network.pivot_election` `coverage` | | |
| crs | c + `network.cycle_slip.if_phase_td_reseed_on_flag` | same | |
| csfe | c + `estimation.seed_full_equation` | | |

The DOY 132 golden is derived by `scripts/mday_golden_config.py` (observations from `D:/GnssProcessorSupport/data_mday`, 56 stations, as card MDAY). The sim bed starts at the fixture's start (the golden carries no start offset; the baseline `_rf0hv17_sim6` ran the same way) and is graded from its first epoch (00:30:15).

**Code check of the add-on keys against main.** The re-seed key was graded on branches `icl4/post-flag-slip` and `icl5/reseed-key-day`. The diff of that branch against main (`git diff main...icl5/reseed-key-day`) adds default-off code (frame hold, quorum, apriori clock track, two-way centring sink, tallies) and leaves the key's own path (`SlipQc.cpp` re-seed lines, `IfPhaseSlipDetector`, loader) as on main. Main therefore carries the graded behaviour of the key; the branch's other additions are not part of this card. Pivot election (card PIV) and `estimation.seed_full_equation` (card NOM5) are on main.

## 3. Gates (frozen 2026-11-06, before any 6 h run of this card)

Existing bars quoted: the recommendation bar of card NOM5 G3 (decision D6): "hours 1 to 6 not worse at the held-out consumers by more than 1 mm in any hour, the census median site not worse, and no census site worse by more than 1 cm (card NSP: a neutral change moves no site by more than 0.19 mm, so a site past 1 cm is a response)". Standing grading rules of CLAUDE.md: hour 1 is ignition and settled claims come from hours 2 to 6; a delta is against the same-exe control; never adopt on one instrument. This card applies the bar to hours 2 to 6 and reports hour 1; "census median not worse" is read at the tool's printed resolution, a paired median change of at most +0.1 mm. No bar moves after this section is committed. A failed clause stays failed.

Instruments: held-out consumer WTZZ00DEU, hourly p50 of `pos_err_3d_m` in `logs/closed_loop_trace.csv` (`rapid_arm.py score`); the 16-site census by `scripts/clock_level/gcensus.py` (GPS + Galileo consumers, each replay uses the arm's own `arm.json`), paired change of each site's mean hourly p50 over hours 2 to 6 against the control of the same exe; on the sim bed the held-out consumer SZZ000SIM by hour and the census of `simcensus6h.py` with `SIMCENSUS_BASE` set to the control's arm.

| gate | bar |
| --- | --- |
| R0, reproduction | The field DOY 129 control against the baseline `_rf0hv17_field6` (exe `gnss_processor_bofarm.exe`): each of the six WTZZ00DEU hourly p50 within 1 mm. Byte identity of `products.txt` is reported, not a bar (main moved since the baseline; the exe differs). Same for the sim control against `_rf0hv17_sim6`. |
| R1, health | Every arm exits 0 and emits as many epochs as its control; epochs withheld at most the control's. |
| R2, held-out consumer | Each of hours 2 to 6 of WTZZ00DEU (SZZ000SIM on the sim) at most +1 mm against the control (candidate and leave-one-out arms) or against C (add-one arms). |
| R3, census median | Paired median change over hours 2 to 6 at most +0.1 mm. |
| R4, census sites | No site worse by more than 1 cm over hours 2 to 6. Sites worse by more than 0.19 mm are listed (a move past the noise floor). |
| R5, ON path | Each key's run-end counters are read; 0 driven means inert wiring and the key's verdict is "inert", whatever the grade. Reported: IF-TD refloats, T4 arcs, decouplings, withheld epochs and slip handovers per arm. |

**Candidate verdict.** C passes when R1 to R4 pass on DOY 129 (against ctrl), on DOY 132 and on the sim. The candidate may become `_rf0hv18` only if C passes; the author decides.

**Key verdicts.** Leave-one-out, from `c` against `c minus X` on the field bed: X is *contributing* when C is better than `c minus X` by more than 1 mm in a settled WTZZ00DEU hour or by more than 0.19 mm at the census median or any site; *harmful* when C is worse by the same margins (then `c minus X` is the candidate to read); *neutral* when the products differ and no margin is exceeded; *inert* when the products are byte-identical or the key's counters read 0 driven. Add-one keys are passed or failed by R1 to R4 against C (DOY 129; the re-seed key also on DOY 132) and then read as for the leave-one-out arms.

Reported without a bar: WTZZ00DEU hour 1 and the mean of hours 2 to 6; the census by hour; the full per-site table; key counters.

Nothing is armed by this card. Arming, any golden, manifest or ledger change and the merge are the author's decisions.
