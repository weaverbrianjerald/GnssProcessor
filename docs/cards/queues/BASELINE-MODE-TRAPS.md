# BASELINE-TRAPS: baseline modes (Baseline-Single, Baseline-Multi): data-flow traps and the static DD review register

**STATUS**

- **id:** BASELINE-TRAPS
- **topic:** queues
- **status:** reference
- **summary:** Traps traced through the baseline-mode data flow on 2026-09-03 (SP3-only orbits, inert gnss_rate_s, coordinates taken as ARP) and the unvalidated register of the 2026-06-23 static DD review.
- **dates in the text:** 2026-06-23 to 2026-10-07
- **source:** agent memory note `project_baseline_requires_sp3`, copied 2026-10-24

**Outcome.** Baseline modes load orbits only from SP3 (an implementation gap), ignore `processing.gnss_rate_s`, and take a configured coordinate as the ARP without reducing it from the mark. The fixes of the 2026-06-23 review were never validated in a dedicated run, so the latent-item register below is to be weighed before recommending DD on real data. Anchors were re-checked 2026-10-07; the status note records what was re-checked on 2026-10-24.

> Results are appended TO THIS FILE, below the existing text.

---

## 2026-10-24: baseline modes: SP3-only orbits, inert gnss_rate_s, coordinates as ARP, and the static DD review register

These results were recorded only in the agent memory note `project_baseline_requires_sp3` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: re-checked 2026-10-24: the misleading orbit error text is still in `ApplicationBaseline.cpp`; `findEpochWithin` (now in `MultiRinexSync.cpp`) still takes the first epoch within tolerance, not the closest; the rest of the latent register is unchecked since 2026-06-23; `use_iono_free` is false in both shipped baseline configs.

Baseline modes (`Baseline-Single` / `Baseline-Multi`). Traps traced through the data flow on
2026-09-03 (USGS 1.35 km vector, April 2025 data), plus the 2026-06-23 static DD review (merged here
2026-10-07). Anchors re-checked 2026-10-07. Other real-data DD runs on record: the bundled
CIT1+JPLM preset in `docs/test_suite.md` (8.5 cm from JPLM truth, June 2026). The 2026-06-23 review
fixes were never validated in a dedicated run: weigh the register below before recommending DD on
real data.

**Orbits are SP3-only (an implementation gap, not physics).** `ProductParser::loadProducts` fills
`orbits_` only via `loadMulti("SP3", ...)`; the broadcast RNX feeds `parseKlobuchar` alone.
⚠ `runBaseline` still errors "no orbit data available ... Set navigation_path to a valid RINEX nav
file", which is misleading: nav cannot fix it. (The wrong code comment near
`ApplicationBaseline.cpp:113` was corrected in the comment rewrite `b3a8e48`, 2026-09-28; the error
text was not.) Orbit error enters as db/b = dr/rho (1.5 m on 1352 m is ~0.1 mm), so the need for
precise products is the loader's, not the geometry's; consequence: a short baseline cannot run
offline (it needs reachable CDDIS credentials). Open capability gap: a broadcast-to-`orbits_` adapter.
- To run: `data_source: "production"`, `data_download.{enabled, download_products}: true`,
  `year`/`doy`. `ProductResolver` fetches day-1/day/day+1 and `parseSp3` merges them, so midnight
  crossings are covered. ⚠ Check the epoch count: a missing day drops epochs silently. Satellite CLK
  cancels in DD; it is fetched only to satisfy the loader.

**`processing.gnss_rate_s` is INERT in baseline mode** (applied only in `Application::run`). A 1 Hz
file solves every epoch (42,727 over 12 h) and the formal sigmas come out optimistic (correlated
epochs treated as independent). Pre-decimate with `scripts/decimate_rinex3_obs.py`. ⚠ Decimate on
seconds-of-day, never by index from each file's start (two stations starting 473 s apart gave ZERO
matched epochs). Even at 30 s the sigmas stay optimistic by ~2 to 4x (multipath, residual
troposphere); never set a formal sigma beside a control-network floor.

**Coordinates are taken at face value.** A config `position_ecef` (reference or rover) is used as the
ARP, not reduced from the mark. ⚠ An NGS datasheet coordinate is the monument: pre-reduce it
(mark + antenna height x local up). A raw paste put 2.075 m into the base: 0.14 mm on the VECTOR,
2.075 m on the POSITION, so a vector-only check passes it clean. The header `ANTENNA: DELTA H/E/N`
reaches only the rover a priori, and only when no config coordinate is given (`getArpPositionXyz`
fallback); it never changes the solve (zeroing it in both files: bit-identical rover ECEF, AR ratio
93.28). Supply ARP coordinates and grade ARP to ARP.

**Diagnostic lesson.** A -73.5 mm Up residual matched "0.075 m antenna height applied twice" to
1.5 mm and was FALSE. Swapping MARK for ARP in the config moved the vector 0.7 mm (config path
clean); zeroing the header deltas was bit-identical (header path inert). Both tests are needed: a
header-driven application is identical in both arms of the first. Most of the Up residual is
DIFFERENTIAL RECEIVER PCO, structural whenever `enable_receiver_pco` is false
(vector = V_ARP + pco_rover - pco_base). Guessed ATX types (IGAIG8, CHCI93; predicted +52.5 mm IF
differential) with rx PCO/PCV on: Up -73.5 to -33.7 mm, horizontal unchanged, AR ratio 93.28 to
209.70 (independent corroboration). ⚠ The 39.8 mm (54 %) recovery is a lower bound on the mechanism,
not its size; diagnostic only (headers say "Unknown"; IGAIG9 and IGAIG3S are not in igs20.atx). Still
open: a ~-24 mm across-track residual. The datasheets carry only NETWORK accuracy (the wrong statistic
for a 1.35 km vector; do not quote 3 mm), and HN0157 was adjusted by USGS in June 2026, possibly from
these same observations, so that comparison may be processing-vs-processing; HN0156 (NGS, 2012) stays
independent.

**DD core math VERIFIED CORRECT (2026-06-23; do not re-derive):** BRSD, DD = BRSD_i - BRSD_pivot,
H_pos = -(los_i - los_p), R = diag(var_i) + var_p 11^T, innov = innov_pre - x(amb), pivot
S-transform T P T^T (`DdPivotManager`); the differential troposphere state is fully wired. Rejected
as non-defects: the rover-only troposphere mapping partial (valid short-baseline approximation) and
float-only output (AR ships `mode=0`; baseline is FLOAT-ONLY and static, `position_q=0`, by default).
Lessons from the committed fixes (`requestBaselinePivotRerank` -> `DdPivotManager::requestForcedRerank`):
- Never prune a DD ambiguity for a row merely excluded mid-epoch: union `excluded_measurements`
  into `expected` before the stale-state prune (it destroyed the soft-reset float and covariance).
- A KF accumulator nobody drains poisons the run: the undrained pivot-slip request left the slipped
  DD pivot un-re-referenced (constellation dropped) and grew `requested_pivot_handovers_` for the
  rover's lifetime, muting cross-band detectors. Drain all three lockstep accumulators after every
  KF update.

Latent register (as of 2026-06-23): with `use_iono_free=true` (now false in
`config/baseline_single.json` / `baseline_multi.json`: DD cancels iono to mm on short baselines,
keeps integer ambiguities, avoids ~3x code noise) the IF exclusion token `"L1C+L2W"` never matches the
per-band token in `differenceBrsd` (exclusion dead) and IF rows are not integer-capable (AR no-ops);
mode-3 `holdAmbiguitiesDd` tightens unfixed ambiguities and double-applies `hold_variance` (both
gated `ar_mode==3`, off); the `x == 0.0` freshness gate in `resolveDd` is bogus (seed is
`innov_pre`); a zero variance gives a non-PD R with no floor; `findEpochWithin` returns
first-not-closest (still so 2026-10-07); the RINEX clock-offset column is ignored; PathResolver's
substring station match can collide.

downloads), memory note `project_antenna_eccentricity_arp_frame`, memory note `project_pivot_handover_flipflop_fix`.
