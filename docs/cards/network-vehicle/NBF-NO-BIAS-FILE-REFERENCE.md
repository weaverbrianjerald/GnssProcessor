# NBF: the reference setup without the CODE bias file

**STATUS**

- **id:** NBF
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-23 on the author's ruling of that morning ("Yes, drop the CODE bias file from the reference"). The reference files apply no value of the CODE bias file (`model_corrections.enable_code_phase_bias` false), but PathResolver still finds the observation day's `.BIA` by a glob with no key to stop it, and its presence changes the run (clock reference observables, coverage, seeds). A key `files.use_bias_file` (default true, identical) stops PathResolver from returning any `.bia` or `.bsx`; the two reference files take it false with `model_corrections.bias_coverage_requires_apply` true. Readings fixed before any run (section 4). Step 2 (2026-11-23): identity at the default and equivalence with card BRP's data copy without the file both hold over 2 h; the two reference files take the keys in commit 16197c6. Step 4 (2026-11-23): every epoch emitted on four 6 h beds and 2026 day 145, the simulator 0 wrong held, integers identical in the cycle form except two GPS links on day 130 that the run without the file decided early on marginal floats and holds one narrow-lane cycle off (it fits worse there by 2 to 4 mm); the 48 receiver replays within 10.3 mm.
- **dates in the text:** 2026-11-23
- **knobs / posture:** new key `files.use_bias_file` (default true: today's behaviour). Reference files `config/golden/golden_{field,sim}_refound.json`: `files.use_bias_file` false and `model_corrections.bias_coverage_requires_apply` true, through the manifest, the ledger and the generator; the frozen lineages `golden_{field,sim}_rf0hv18` and the BeiDou lineage unchanged.
- **code:** branch `config/bias-file-key-2026-11-23` from `int/2026-11-23` (`42efa23`), worktree `D:/GnssProcessorSupport/worktrees/biakey1123`.
- **run dirs:** `D:/GnssProcessorSupport/runs_irm/_rap_nbf_*`; reports `D:/GnssProcessorSupport/gates/adopt_2026-11-23/`.
- **source:** the author's ruling of 2026-11-23, relayed by the managing session (`D:/GnssProcessorSupport/gates/adopt_2026-11-23/manager_ORDER_2.md`); the evidence of card BRP (branch `card/brp-broadcast-poster`, not yet merged).

**Outcome.** The key `files.use_bias_file` (89b5148) does what card BRP's arms did: identical to main's engine at its default, and false on the unchanged data tree identical to the run on card BRP's data copy without the file. The two reference files take it false with `model_corrections.bias_coverage_requires_apply` true (16197c6). Over 6 h the reference without the file emits every epoch on four beds and 2026 day 145, holds 0 wrong integers on the simulator and the control's integers on days 129, 132 and 145, cuts day 129's marks from 1,053 to 298, and leaves the built-in users and 48 receivers within about 1 cm. Day 130: two GPS links held one narrow-lane cycle off (early decisions on marginal floats; fit 2 to 4 mm worse), open.

> Results are appended TO THIS FILE, below the text, under dated headings.

---

## 1. Ruling and question

The author, 2026-11-23 morning, to the managing session's statement of card BRP's evidence: "Yes, drop the CODE bias file from the reference. Continue working, prioritize the most meaningful improvements first."

Question: can the reference files run without the CODE bias file reaching the run at all, by a key of the one class that locates input files, so that the measured arms of card BRP (made from a copy of the data tree with the file taken out) become the reference without a private copy of the data?

## 2. What the file does today (read on `int/2026-11-23`, `42efa23`)

- `PathResolver::resolveInternal` globs `.bia` and `.bsx` from `<root>/<YYYY-DOY>/products` of the observation day and returns them with the other products; the application driver replaces the configuration's product list with the resolver's (`Application::resolveData`).
- `ProductParser` loads the file named by `GnssConfig::getProductPath("BIA")` with `parseBia`, which fills both the bias table and the clock reference map (the file's `SATELLITE_CLOCK_REFERENCE_OBSERVABLES` lines).
- The reference files set `model_corrections.enable_code_phase_bias` false, so no value of the file corrects an observation; the clock reference map, the coverage the user's fixing reads, the Melbourne-Wubbena correction and the seeds still see the file (card BRP and card SCE, from the run session's traces).
- The file cannot be deleted from `C:/GnssProcessor/data` (other configurations read it), and a reference file cannot point at a private copy of the data tree.

## 3. The key

`files.use_bias_file` (default true). False: PathResolver returns no `.bia` and no `.bsx` (internal mode: no glob; production mode: removed from the downloader's list), with a log line naming the key. `PathResolver::isBiasFile` is the one test of a bias file. A bias file named in `files.product_list_path` together with false is refused by the loader with a message naming the file and the key. The summary line of the loader reports `bias_file=`. Unit test: `tests/test_path_resolver_bias_file.cpp` (suite `PathResolverBiasFile`).

## 4. Readings fixed before any run

Step 2 (the key does what the arms did):

- I1, identity at the default: `golden_field_refound` and `golden_sim_refound` (sim from 00:30:15), 2 h, 4 threads, the new engine against main's engine `gnss_processor_int1122.exe` (sha256[:16] a0fadbb1564612b9): products and every trace identical (`scripts/nom_td_gate.py --whole`). The loader's summary line differs by the new `bias_file=` field, expected; any other difference stops the order.
- I2, equivalence: `golden_field_refound` with the key false on the unchanged data tree against the same file with `files.internal_data_root` = `D:/GnssProcessorSupport/runs_nom/_brp_data_full_nobia` and the key at its default, 2 h, the new engine: products and every trace identical apart from lines that name the data root. Anything else is reported with what else reads the file, and the order stops.

Step 3: the two reference files through the manifest, the ledger and the generator; the generator leaves the frozen lineages and the BeiDou lineage byte-identical.

Step 4 (health, no bar: the ruling is made): 6 h runs of the regenerated files on days 129, 130, 132 and the simulator against the adopted baselines `runs_irm/_rap_a22h_{f129,f130,f132,s}_6`, and 2026 day 145 as a pair on the new engine. Readings: exit codes; every epoch emitted; the built-in user by hour; held share by hour; the simulator's truth audit (held, right, wrong); the integer check against the control in the cycle form; marks per hour in each product. Then the 16 receivers of days 129, 130 and 132 replayed on the new products against `runs_irm/_rap_n20_<day>_<site>`: by hour, the median over hours 2 to 6, fix epochs, and the integer check at common epochs. Reported at once, before the next step: an epoch not emitted, a wrong held integer on the simulator, a receiver worse by more than 20 mm on its 6 h median, a loss of held share of more than 1 point.

## 2026-11-23, results: steps 1 and 2

**Build.** Engine `gnss_processor_nbf1123.exe` of 89b5148, sha256[:16] 4799852583448865; the unit test
`PathResolverBiasFile.*` 4 of 4 passed (Debug).

**I1, identity at the default** (2 h, 4 threads; `nom_td_gate.py --whole`; runs `runs_irm/_rap_nbf_id_{f,s}_{new,ref}`,
the reference on `gnss_processor_int1122.exe`, sha256[:16] a0fadbb1564612b9). Field DOY 129 and the simulator from
00:30:15: products (da1cbd9ae808b3f2 and d773dda85ce00a10), every trace, the run manifest, `main_log` and `rt_engine.log`
identical; `stdout.txt` differs in one line on each bed, the loader summary with its new `bias_file=` field. Every run
exit 0. Reads: `gates/adopt_2026-11-23/nbf_identity_{f,s}.txt`.

**I2, equivalence** (2 h, field DOY 129, both on the new engine; runs `runs_irm/_rap_nbf_eq_kf` with the key false on
`C:/GnssProcessor/data` and `runs_irm/_rap_nbf_eq_nb` with the key at its default on
`runs_nom/_brp_data_full_nobia`). Products (fd7240b3aff5a221) and every trace identical, the run manifest identical,
`rt_engine.log` identical; 63 lines of `stdout.txt` and 1 of `main_log` differ only by the data root (normalised and
compared in full after the gate's masks), and one line by the key's own summary field. The key acts: the run that reads
the file has other products (da1cbd9ae808b3f2) and resolves 11 product files against 10. Reads:
`gates/adopt_2026-11-23/nbf_equivalence_f.txt`, `nbf_equivalence_f_full.txt`.

**Step 3.** Commit 16197c6: the manifest pins the two keys on `golden_sim_refound` and `golden_field_refound` only; both
files regenerated with `make_golden_config.py --only`, each changed only by the two keys; the frozen lineages and the
BeiDou lineage byte-unchanged. Ledger: `files.use_bias_file` ARMED, `model_corrections.bias_coverage_requires_apply` a new
ARMED row, each note in three parts.

## 2026-11-23, results: step 4 (health over 6 h) and the day-130 analysis

Engine of 89b5148 (sha256[:16] 4799852583448865); runs `runs_irm/_rap_nbf_h_*_6`, 4 threads; controls the adopted
baselines `_rap_a22h_{f129,f130,f132,s}_6` and, for 2026 day 145, the same arm reading the file (`_rap_nbf_h_n145_ref_6`,
card BRP's `brp_n145_c0_6` rerun on this engine: products identical to card BRP's run). Reads:
`gates/adopt_2026-11-23/REPORT_nbf_step4.md`, `REPORT_nbf_day130.md`, `nbf_health_<bed>.txt`.

| Bed | Epochs | Built-in user 3D median by hour (m), run / control | Held share, hours 3 to 6 (points) | Integers, cycle form | Marks |
|---|---|---|---|---|---|
| Simulator (from 00:30:15) | 720 of 720 | 0.69 0.10 0.03 0.03 0.02 0.02 / 0.68 0.10 0.03 0.03 0.02 0.02 | E 0.00; G +0.09 to +0.17 | 0 of 292,901; truth audit 406,385 right, 0 wrong | 63 / 63 |
| Day 129 | 720 of 720 | 2.28 0.09 0.03 0.01 0.01 0.01 / 2.31 0.09 0.01 0.03 0.04 0.01 | E -0.11 to +0.40; G +0.16 to +0.35 | 0 of 224,104 | 298 / 1,053 |
| Day 130 | 720 of 720 | 2.04 0.11 0.02 0.04 0.04 0.04 / 2.06 0.11 0.02 0.04 0.04 0.04 | E -0.51 to +0.18; G -1.17 to -0.15 | 1,025 of 184,716 | 40 / 40 |
| Day 132 | 720 of 720 | 0.60 0.05 0.03 0.03 0.02 0.01 / same | within 0.11 | 0 of 207,208 | 70 / 70 |
| 2026 day 145 | 720 of 720 | 1.03 0.17 0.20 0.27 0.19 0.11 / 1.05 0.20 0.22 0.23 0.17 0.11 (float user of that file) | within 0.10 | 0 of 276,204 | 13 / 13 |

**Day 130.** The 1,025 differing link-epochs reduce to two links (fewest edits on the cycle form's forest, at most two
per epoch): KOKV00USA G31 (+1, +1) and GAMB00PYF G08 (-1, -1). The run without the file decided both early (04:02:00,
frac_if 0.19, sigma 0.110; 03:03:30, frac_if 0.20, sigma 0.120, wide lane at 0.241 cycle) and holds them to the end;
the control's fraction gate refused them until 04:49:30 and 04:47:30 and then took the neighbouring narrow lane. On both
links the run without the file carries the steady bias of a narrow-lane cycle (KOKV G31 L1C -2.6 to -4.1 mm, GAMB G08
+2.0 to +2.9 mm; L2W about -0.6 times L1C), the control within about 1 mm: the run without the file holds the other
integer. The differences surface when the control's own decisions make the cycles comparable, which is why the cause
search lists none for 43 of 44 episodes. The GPS clock reference change at 04:39:00 (G01 to G02) is the same in both
runs. The GPS held share of hour 6 (-1.17 point) is not explained by this reading.

**Receivers.** The 16 receivers of days 129, 130 and 132 replayed on the new products against `_rap_n20_<day>_<site>`
(`gcensus.py grade`): hours 2 to 6 paired median +0.7, -0.0 and -1.6 mm; worse at 1 cm: 1 (SCRZ day 129, +10.3 mm),
0, 0; better at 1 cm: 0, 1 (SCRZ day 130, -120.8 mm), 0. Every replay exit 0. The users' held integers against the
controls (`brp_held_read.py pairs --rule union`): frame-free flagged 11,653, 3,120 and 0 on the three days; the flagged
rows are constant whole-cycle offsets per satellite and signal (G11 L2W -1 at HARB and DAV1 on day 129), the two
networks' satellite integer frames, which the frame-free form does not remove; card BRP's same-products replays are not
comparable with them.
