# NBF: the reference setup without the CODE bias file

**STATUS**

- **id:** NBF
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-23 on the author's ruling of that morning ("Yes, drop the CODE bias file from the reference"). The reference files apply no value of the CODE bias file (`model_corrections.enable_code_phase_bias` false), but PathResolver still finds the observation day's `.BIA` by a glob with no key to stop it, and its presence changes the run (clock reference observables, coverage, seeds). A key `files.use_bias_file` (default true, identical) stops PathResolver from returning any `.bia` or `.bsx`; the two reference files take it false with `model_corrections.bias_coverage_requires_apply` true. Readings fixed before any run (section 4).
- **dates in the text:** 2026-11-23
- **knobs / posture:** new key `files.use_bias_file` (default true: today's behaviour). Reference files `config/golden/golden_{field,sim}_refound.json`: `files.use_bias_file` false and `model_corrections.bias_coverage_requires_apply` true, through the manifest, the ledger and the generator; the frozen lineages `golden_{field,sim}_rf0hv18` and the BeiDou lineage unchanged.
- **code:** branch `config/bias-file-key-2026-11-23` from `int/2026-11-23` (`42efa23`), worktree `D:/GnssProcessorSupport/worktrees/biakey1123`.
- **run dirs:** `D:/GnssProcessorSupport/runs_irm/_rap_nbf_*`; reports `D:/GnssProcessorSupport/gates/adopt_2026-11-23/`.
- **source:** the author's ruling of 2026-11-23, relayed by the managing session (`D:/GnssProcessorSupport/gates/adopt_2026-11-23/manager_ORDER_2.md`); the evidence of card BRP (branch `card/brp-broadcast-poster`, not yet merged).

**Outcome.** Pre-registered; no result yet. The key is built on its branch with a unit test of its own. Readings fixed in section 4: identity at the default against main's engine on the two reference files (2 h), equivalence of the key false against the data tree copy without the file used by card BRP's arms (2 h), then the regenerated reference files' health over 6 h on days 129, 130, 132, the simulator and 2026 day 145, and the 16 receivers of the three days replayed on the new products.

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
