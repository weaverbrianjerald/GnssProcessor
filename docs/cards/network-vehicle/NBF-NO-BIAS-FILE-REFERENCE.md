# NBF: the reference setup without the CODE bias file

**STATUS**

- **id:** NBF
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-23 on the author's ruling of that morning ("Yes, drop the CODE bias file from the reference"). The reference files apply no value of the CODE bias file (`model_corrections.enable_code_phase_bias` false), but PathResolver still finds the observation day's `.BIA` by a glob with no key to stop it, and its presence changes the run (clock reference observables, coverage, seeds). A key `files.use_bias_file` (default true, identical) stops PathResolver from returning any `.bia` or `.bsx`; the two reference files take it false with `model_corrections.bias_coverage_requires_apply` true. Readings fixed before any run (section 4). Step 2 (2026-11-23): identity at the default and equivalence with card BRP's data copy without the file both hold over 2 h; the two reference files take the keys in commit 16197c6.
- **dates in the text:** 2026-11-23
- **knobs / posture:** new key `files.use_bias_file` (default true: today's behaviour). Reference files `config/golden/golden_{field,sim}_refound.json`: `files.use_bias_file` false and `model_corrections.bias_coverage_requires_apply` true, through the manifest, the ledger and the generator; the frozen lineages `golden_{field,sim}_rf0hv18` and the BeiDou lineage unchanged.
- **code:** branch `config/bias-file-key-2026-11-23` from `int/2026-11-23` (`42efa23`), worktree `D:/GnssProcessorSupport/worktrees/biakey1123`.
- **run dirs:** `D:/GnssProcessorSupport/runs_irm/_rap_nbf_*`; reports `D:/GnssProcessorSupport/gates/adopt_2026-11-23/`.
- **source:** the author's ruling of 2026-11-23, relayed by the managing session (`D:/GnssProcessorSupport/gates/adopt_2026-11-23/manager_ORDER_2.md`); the evidence of card BRP (branch `card/brp-broadcast-poster`, not yet merged).

**Outcome.** The key `files.use_bias_file` is built (89b5148) and does what card BRP's arms did: at its default the engine is identical to main's on both reference files over 2 h (products, every trace, the run manifest; only the loader's summary line gains its field), and false on the unchanged data tree gives products and traces identical to the run on card BRP's data copy without the file. The two reference files take it false with `model_corrections.bias_coverage_requires_apply` true (16197c6). Open: the 6 h health of the regenerated files on four beds and 2026 day 145, and the 48 receiver replays on the new products.

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
