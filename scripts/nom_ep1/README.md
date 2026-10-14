# scripts/nom_ep1/: the EP1 seam conversion

Tooling of card NOM3 step EP1 (`docs/refound/NOM3_network_giants_2026-10-09.md`, section 3
"EP" and the gates of section 5). Step EP1 moves the network producer's per-epoch lambda
`processSyncedEpoch` out of `Application::runNetwork` into
`Application::networkProcessEpoch_(NetworkRun&, const SyncedEpoch&)` and converts the 62
per-epoch seams it calls to `R networkX(NetworkRun& run, const NetworkEpoch& ep[, kept...][,
outputs...])`. Nothing in the moved code is retyped by hand. A generator writes every edited
file from the frozen source, and an independent verifier proves that every moved statement,
every binding and every declarator denotes what it denoted before (gate G-R). Section numbers
below ("spec 5.5") refer to the EP1 specification, the buildable design of step EP1.

Step EP1a-0 (this tooling) changes no C++ file. All tools use the Python 3 standard library
only. Run them with `PYTHONIOENCODING=utf-8` on a cp1252 console (PowerShell:
`$env:PYTHONIOENCODING = "utf-8"`). None of them builds or runs C++.

## The frozen base

Every tool reads the frozen source `<B>` from git (`git show`, `git cat-file --batch`) at
`--base <rev>`, never from a working tree. `DEFAULT_BASE` in `__init__.py` names it when
`--base` is not given. At the time of writing it is `c483a11` (`nom/ep0`: main `366385b` plus
step EP0); before step EP1a-1 starts it is re-pointed to the commit that merges EP0 into main.
No row, table or check carries a line number: the survey re-derives every line from whichever
base it is given. A row whose seam, header or anchor is gone at a later base (the REMOVE-key
deletion takes the lane layer, FC-1, the PB emit filter and some `iono_dw` / `sh_health_gate`
code) is reported by the survey and refuses the generation, never a crash.

## Files

| file | role |
| --- | --- |
| `scripts/nom_cpp.py` | The C++ reading layer: lexer, statement splitter, declaration and parameter parsers, per-file function index, scope walker, call extraction, the resolver that follows reference declarations to their initializers, `GitTree` (the frozen revision) and `Driver` (the shape of `runNetwork` and its lambda). `--selftest` reproduces the EP1 inventory's counts on the frozen driver. |
| `scripts/nom_ep1/__init__.py` | `DEFAULT_BASE`, the row-file list and the row loader. |
| `scripts/nom_ep1/model.py` | The row type `Seam`, the realization tables (lambda local to epoch member, driver name to run member, the engine constructors and the spec fields) and the fold grammar. Generator input only: the verifier does not import it beyond the row type. |
| `scripts/nom_ep1/declared.py` | The closed list of declared substitutions of spec 5.6 (eleven classes), each with its reason and a locator that finds its frozen object at any base, and the declared non-conversions (the input stage, the calls inside the EP2 blocks, the method calls). |
| `scripts/nom_ep1/rows_app.py` | Canonical-table rows of worker W1 (10 seams). |
| `scripts/nom_ep1/rows_solve.py` | Rows of worker W2 (13 seams). |
| `scripts/nom_ep1/rows_emit.py` | Rows of worker W3 (15 seams). |
| `scripts/nom_ep1/rows_diag.py` | Rows of worker W4 (24 seams). |
| `scripts/nom_ep1/gen.py` | The generator's seam plans and the reads over edited text. |
| `scripts/nom_ep1/gen_epoch.py` | The epoch function (transforms T-A, T-B1 to T-B9, T-C, T-E, T-F). |
| `scripts/nom_ep1/gen_seams.py` | Converted signatures, generated Doxygen (check V7), binding preambles, fallback names, adapters' epoch forms, the solve's replay, forward declarations and includes, the contract test. |
| `scripts/nom_ep1/gen_setup.py` | `runNetwork` (spec 4.1), `NetworkRun.cpp` (`networkEngineSpec`, the bracket functions), `NetworkRunGroups.hpp`, the post-fit hand-off, the third-band predicate, `Application.hpp`, `CMakeLists.txt`. |
| `scripts/nom_ep1/templates.py` | The designed text of the new types (spec 2), checked against the model's tables. |
| `scripts/nom_ep1/textops.py` | Non-overlapping edits, line ends, comments, Doxygen blocks, the target tree. |
| `scripts/nom_ep1/manifest_<rows>.json` | Written by each generator run under `--root`: the base, the rows, every written file with its digest, the frozen ranges each transform resolved, each converted call and preamble. The verifier reads it for pairing only. |
| `scripts/nom_ep1_cut.py` | The generator: `--survey`, `--stage ep1a1`, `--stage ep1b`, `--shared-only`, `--presolve-writer`, `--check`. |
| `scripts/nom_ep1_verify.py` | The relocation verifier: checks V1 to V13 and the sixteen negative controls of spec 5.7. It reads the rows and `declared.py` and nothing the generator computes. |
| `scripts/nom_epoch_lint.py` | The permanent lint: checks V4, V10 and V11 on any tree, with no git revision. |

## Step EP1a-1, the base

One worker, serial. The commands run from the repository root, in this order.

1. Re-point `DEFAULT_BASE` to the merge commit of EP0 on main, or pass `--base <B>` to every
   command below.
2. Prove the reading layer and the table at the base:
   `python scripts/nom_cpp.py --selftest --base <B>` (at a base whose driver changed, add
   `--expect KEY=N,...` with the counts expected there), then
   `python scripts/nom_ep1_cut.py --survey --base <B>`. Both must exit 0. The survey lists
   every row's call sites and bindings, the six fallback seams and every declared entry's
   frozen file:line. The design's frozen tables can be compared with `--calltable` (self-test)
   and `--compare-compact` / `--compare-renames` (survey); a declaration that only moved to
   another line is listed, not failed.
3. Write `tests/test_network_engine_spec.cpp` by hand (spec 5.9; the generator does not write
   it, and lists it in `CMakeLists.txt` only when the file exists).
4. Generate: `python scripts/nom_ep1_cut.py --stage ep1a1 --base <B>`. It writes 14 files at
   `c483a11` (every call still explicit; `ApplicationNetwork.cpp` 2 940 to 1 094 lines, the
   epoch function 1 754 lines) and `scripts/nom_ep1/manifest_ep1a1.json`. A refusal writes
   nothing and names the shape it did not recognize.
5. Build Debug and Release with `scripts/vs2026_build.ps1`, then run the unit suite.
6. Verify: `python scripts/nom_ep1_verify.py --tree . --base <B> --controls`. This is the
   positive control of spec 5.7: every applicable check passes with no row converted. N5, N6,
   N7, N8, N10, N11, N12 and N16 fire (N7 and N16 also fail the lint); the other eight are
   reported as pending (their target is a converted seam).
7. Lint: `python scripts/nom_epoch_lint.py --partial` (no seam has an epoch form yet, so the
   writer banner is checked only for writers that exist).
8. Idempotence: `python scripts/nom_ep1_cut.py --stage ep1a1 --base <B> --check` exits 0
   (a second run writes nothing).
9. Gate G-EP on the arms of spec 7 (`f2`, `s2`, `zero`, `cil`, `snapw`, `snapr`, `fc`).

## Step EP1b, one worker per rows file

Each worker branches from EP1a-1 and owns the files of its rows (no file is owned by two
workers). The two shared generated files, `src/core/app/ApplicationNetworkEpoch.cpp` and
`tests/test_network_epoch_contract.cpp`, are scratch on a worker branch: they are regenerated
at each merge, never hand-merged.

The controls column follows from each control's target (N5 to N8, N10 to N12 and N16 run on
every tree with an epoch function); the W2 row was confirmed by a run at `c483a11`.

| worker | `--rows` | seams | negative controls its own rows make runnable |
| --- | --- | --- | --- |
| W1 app | `app` | 10 | N2, N9 |
| W2 pre-solve and solve | `solve` | 13 | N1, N9, N14, N15 |
| W3 emit and post-fit | `emit` | 15 | N3, N9, N13 (N4 also needs W2's solve) |
| W4 diagnostics | `diag` | 24 | N9 |

On a worker branch, from the repository root:

1. Generate: `python scripts/nom_ep1_cut.py --stage ep1b --rows <w> --base <B>`. It rewrites
   the worker's seam files, the EP1a-1 files (unchanged) and the two shared files, and writes
   `scripts/nom_ep1/manifest_<w>.json`.
2. Build Debug and Release, run the unit suite.
3. Verify: `python scripts/nom_ep1_verify.py --tree . --base <B> --rows <w> --controls`.
   EP1a-1's manifest stays in the tree; the verifier pairs only the manifest of the stage it
   verifies and lists the others in V8.
4. Lint: `python scripts/nom_epoch_lint.py --partial`.
5. Idempotence: `python scripts/nom_ep1_cut.py --stage ep1b --rows <w> --base <B> --check`.
   A manifest the tree does not have is reported as absent, not failed.
6. Gate G-EP on the four core arms and the worker's witness arms (spec 7).

A worker's run writes, on its own files, the same text as a run with all four rows files,
and on every other file the EP1a-1 text (checked for all four workers at `c483a11`). A merge
by file of the worker branches followed by `--shared-only` therefore gives the all-rows tree;
nothing is merged by hand.

Integration, in the order W2, W1, W3, W4. After each merge:

1. `python scripts/nom_ep1_cut.py --shared-only --rows <merged> --base <B>` (for example
   `--rows solve,app`; the order typed does not matter, row files are taken in canonical
   order) regenerates the two shared files from the base and the union of the merged rows.
2. Release build, suite, `python scripts/nom_ep1_verify.py --tree . --base <B> --rows
   <merged>`, `python scripts/nom_epoch_lint.py --partial`.

After the last merge: `python scripts/nom_ep1_verify.py --tree . --base <B> --rows all
--controls` (all sixteen controls fire, none pending), `python scripts/nom_epoch_lint.py`
without `--partial`, the full G-EP and the cost pair.

The card's pre-solve form (author decision 1, `NetworkEpoch& ep` for the writers before the
solve) is generated, verified and linted from the same rows with `--presolve-writer epoch` on
the generator, the verifier and the lint. The verifier and the lint report V11 as n/a there: a
pre-solve writer takes the whole epoch, so its spine line names no group and the writer banner
is not machine-checked. In the default form the lint fails a seam that takes `NetworkEpoch&`.

## What each check proves

The verifier exits 0 only when every check that applies passes; "n/a" (a check with nothing to
check yet, such as V2 before any seam is converted) is not a failure. Comments and blank lines
are ignored throughout.

| check | what it proves |
| --- | --- |
| V1a | Every top-level statement of the frozen lambda is carried, character-exact and shifted four columns, by the epoch function, or is the old side of a declared transform of spec 5.3 whose new side is checked by rule (the epoch's construction, the dropped workspace, an alias, a sized or a result store at the local's frozen position, the clock store, `ep.emitted`, the brackets, the converted calls, the two replay closures). |
| V1b | Each converted seam's body below its bindings equals the frozen body, the solve's two replay lines and three hand-off lines excepted; the bindings region holds declarations only. |
| V1c | Every unconverted seam and every adapter's explicit form is byte-identical to frozen. |
| V1d | `runNetwork` equals frozen apart from the spec 4.1 edits, and what those edits took out (the predicate, the two bracket closures, the timer scratch, the hand-off fields, `LoopSignal`) is where the specification puts it. |
| V1e | Every changed code line of `src/`, `include/`, `tests/` and `CMakeLists.txt` lies in a region another check verified, a new file of the step, an include or forward-declaration line, or a declared hunk. No stray edit enters the tree. |
| V2 | Each converted seam has one epoch-form declaration and one definition with the parameter list its row states; kept parameters keep their frozen declaration; after the solve only the hand-off and the prepared solution are outputs. |
| V3a | At every frozen call site paired with its new site (and at the solve's two replay calls), every frozen parameter binds the same storage object, derived independently from the frozen and the new source and equal modulo the declared substitutions; the binding line keeps the frozen declarator, so a reference stays a reference and a copy stays a copy; `// written` marks exactly the epoch writes; adapters' other callers keep their text. |
| V3b | Every read of a captured driver name in the epoch function resolves to the frozen object (a reference, or a copy only where the frozen name was one). |
| V3c | Each epoch member is realized at its local's frozen position, with the local's type and initializer, and is bound nowhere before it. |
| V3d | The `NetworkRun` construction binds each reference member to the frozen object of its role. |
| V3e | No body reads a free `run`, `ep`, `nrun` or `nep` other than its parameters. |
| V3f | Every folded argument parses in the fold grammar (member reads, `&` or `*`, `.get()`, `ep.x_emit()`, literals, side-effect-free conditionals), so folding moves no side effect. |
| V4 | After the solve, the epoch function writes `ep` only through its four stores and the three post-solve output arguments; `ep.clock` is stored once, before its first read. |
| V5 | Const is deep: no member of `NetworkEpoch`, or of a project type held by value inside it, is a non-const reference, a pointer to non-const, a smart pointer, a `std::function` or a `std::reference_wrapper`. |
| V6 | The ordered events (calls, log calls with level and format literal, brackets with their `SB_*`, `LoopSignal` returns, the inserted stores) of the epoch function and of `runNetwork` equal the frozen ones. |
| V7 | Each converted declaration's Doxygen carries the frozen `@param` texts and names every epoch member its bindings read. |
| V8 | Every call of the frozen call table is covered exactly once, by a row or a declared non-conversion; the contract test lists the converted seams; the manifest of this stage pairs with what the verifier found. |
| V9 | No translation unit that newly reaches `app/NetworkRun.hpp` or `app/NetworkEpoch.hpp` calls, in its frozen code, a function name that only the new headers declare, so no new overload can capture a frozen call; the two headers declare only their types, `networkEngineSpec`, the two brackets and the observer alias. |
| V10 | Every epoch group a seam's bindings read is already written when its spine line runs. |
| V11 | The spine lines that pass an epoch group as an output equal the group's "written by" list in the banner of `NetworkEpoch.hpp`. |
| V12 | `networkEngineSpec` holds the frozen setup statements of the declared ranges, character-exact and in order, reading only the configuration, the network block, the knobs and their own locals, calling only an allowlist; each engine constructor argument is the frozen argument or the spec field holding it. |
| V13 | The six seams whose frozen bodies use `run` or `ep` take the fallback names `nrun` / `nep`; no other seam needs them. |

The negative controls (spec 5.7) are mutations of the candidate tree built in memory; each must
fail its named check before a pass is believed. N7 and N16 must also fail the standalone lint,
which must pass on the unmutated candidate. The lint (`nom_epoch_lint.py`) is the permanent
form of V4, V10 and V11: it runs on any later tree, before a commit that touches a seam
signature or `ApplicationNetworkEpoch.cpp`.

## What the checks cannot prove (spec 5.8)

The verifier proves the text and order of every moved statement, the object every name in
every moved body denotes, the reference or copy semantics of every parameter, the initial state
of every epoch member where its local was declared, the write discipline after the solve, that
the engines are built from the arguments they had, and that no stray edit entered the tree. It
reads text; it does not compile or run anything. Not covered, and covered elsewhere:

- that default construction followed by a move assignment equals the frozen direct
  initialization (transforms T-B4 and T-B7): gate G-EP, every arm;
- construction order (the engines are built right after the traces instead of through setup;
  heap layout differs): gate G-EP, every arm, where a pointer-keyed container would show;
- destruction order and exception unwinding: gate G-EP;
- code generation after about 1 400 lines move to a new translation unit (inlining,
  `/fp:precise`): gate G-EP and the cost pair;
- bindings that differ from their by-name alternative only on some postures: the witness arms
  of spec section 6;
- reachability: spec section 6 names what each arm reaches and what no arm reaches;
- that the generated text compiles and that the contract test's static asserts hold: the
  Debug and Release builds;
- the banner's prose "available from" column, which V10 replaces by each group's first write
  in the epoch function, and, on the card's pre-solve form, the banner's "written by" column
  (V11 n/a: the pre-solve writers' spine lines name no group);
- `tests/test_network_engine_spec.cpp`, which is hand-written and checked by the suite.

## Run times

Measured at `c483a11` on the development machine, the lower figure on a quiet machine and the
upper under load: the generator 1 to 3.5 minutes, the verifier 2 to 7 minutes, with
`--controls` 9 to 17 minutes (all rows; 20 to 60 seconds per control), the lint 2 to 3.5
minutes.
