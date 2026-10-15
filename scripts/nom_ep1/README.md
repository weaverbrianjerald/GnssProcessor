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
`--base` is not given: main `926dafa`, the REMOVE-key deletion as it landed (card RKD). EP1 was
first generated on that deletion's branch head `180aa28`, which main does not contain (the branch
was rebased before it landed); the EP1 commits were rebased onto `926dafa` at the review round
(section "The review round" below). The tooling was built on `c483a11` and `0e14eb9` (EP0).
No row, table or check carries a line number: the survey re-derives every line from whichever
base it is given. The base also decides which table entries apply: an entry naming an object
the REMOVE-key deletion takes is marked `retired_by=REMOVE_KEY` and applies only at a base
that still has the object (section "The REMOVE-key deletion" below). Any other row whose
seam, header or anchor is gone at a later base is reported by the survey and refuses the
generation, never a crash.

## Files

| file | role |
| --- | --- |
| `scripts/nom_cpp.py` | The C++ reading layer: lexer, statement splitter, declaration and parameter parsers, per-file function index, scope walker, call extraction, the resolver that follows reference declarations to their initializers, `GitTree` (the frozen revision) and `Driver` (the shape of `runNetwork` and its lambda). `--selftest` reproduces the EP1 inventory's counts on the frozen driver. |
| `scripts/nom_ep1/__init__.py` | `DEFAULT_BASE`, the row-file list and the row loader; `REMOVE_KEY`, `RENAMED` and `bind_base()`, which binds every table to the entries that apply at a base. |
| `scripts/nom_ep1/model.py` | The row type `Seam`, the realization tables (lambda local to epoch member, driver name to run member, the engine constructors and the spec fields) and the fold grammar. Generator input only: the verifier does not import it beyond the row type. |
| `scripts/nom_ep1/declared.py` | The closed list of declared substitutions of spec 5.6 (eleven classes, and the twelfth, `UNREAD_ALIAS`, of the review round), each with its reason and a locator that finds its frozen object at any base; the write proofs of COPY_SOURCE and VALUE_EQUAL (`Writers`), which the survey and the verifier evaluate; the declared member order of `NetworkEpoch` (`EPOCH_LAYOUT`, check V3c); and the declared non-conversions (the input stage, the calls inside the EP2 blocks, the method calls). |
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
| `scripts/nom_ep1/docs.py` | The comment rewrites of the review round (`DOC_HUNKS`: comment-only hunks the generator applies to its finished text, each checked to change no token) and the stale-spelling census (`STALE`), the docs pass's worklist. |
| `scripts/nom_ep1/manifest_<rows>.json` | Written by each generator run under `--root`: the base, the rows, every written file with its digest, the frozen ranges each transform resolved, each converted call and preamble. The verifier reads it for pairing only. |
| `scripts/nom_ep1_cut.py` | The generator: `--survey`, `--stage ep1a1`, `--stage ep1b`, `--shared-only`, `--presolve-writer`, `--check`. |
| `scripts/nom_ep1_verify.py` | The relocation verifier: checks V1 to V13, the two write proofs, and 69 negative controls (the sixteen of spec 5.7, seventeen added at the review round, seventeen at hardening round 1, eleven at hardening round 2, seven at hardening round 3 and one at the EP1 review round). It reads the rows and `declared.py` and nothing the generator computes. |
| `scripts/nom_epoch_lint.py` | The permanent lint: checks V0 (the reading), V4, V10 and V11 on any tree, with no git revision. It also holds the readers both tools share: the tree, the preprocessor lines, a translation unit's include closure and the declarations in it, and the CMake commands. |

## Step EP1a-1, the base

One worker, serial. The commands run from the repository root, in this order.

1. Re-point `DEFAULT_BASE` to the merge commit of EP0 on main, or pass `--base <B>` to every
   command below.
2. Prove the reading layer and the table at the base:
   `python scripts/nom_cpp.py --selftest --base <B>` (at a base whose driver changed, add
   `--expect KEY=N,...` with the counts expected there), then
   `python scripts/nom_ep1_cut.py --survey --base <B>`. Both must exit 0. The survey lists
   every row's call sites and bindings, the six fallback seams, every declared entry's
   frozen file:line with the two write proofs, and the engine and specification tables
   against the base. The design's frozen tables can be compared with `--calltable`
   (self-test) and `--compare-compact` / `--compare-renames` (survey); a declaration that only
   moved to another line is listed, not failed.
3. Write `tests/test_network_engine_spec.cpp` by hand (spec 5.9; the generator does not write
   it, and lists it in `CMakeLists.txt` only when the file exists).
4. Generate: `python scripts/nom_ep1_cut.py --stage ep1a1 --base <B>`. It writes 14 files at
   `c483a11` (every call still explicit; `ApplicationNetwork.cpp` 2 940 to 1 094 lines, the
   epoch function 1 754 lines) and `scripts/nom_ep1/manifest_ep1a1.json`. A refusal writes
   nothing and names the shape it did not recognize.
5. Build Debug and Release with `scripts/vs2026_build.ps1`, then run the unit suite.
6. Verify: `python scripts/nom_ep1_verify.py --tree . --base <B> --controls`. This is the
   positive control of spec 5.7: every applicable check passes with no row converted. 51
   controls fire (N5 to N8, N10 to N12, N16, R1, R2, R5 to R11, R13 to R20, R22 to R34, R38,
   R40 to R48 and R50 to R52; N7, N16, R24 to R26, R46 to R48 and R50 also fail the lint); the
   other seventeen are pending (their target is a converted seam). A control whose target this
   stage must have but the tree lacks is MISSING and fails the run.
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

The controls column follows from each control's target (N5 to N8, N10 to N12, N16, R1,
R2, R6 to R11, R13 to R20, R22 to R34, R38, R40 to R48 and R50 to R52 run on every tree with
an epoch function; R5, the
epoch preamble's third-band forwarder, runs until W2 converts the DD build, which takes the
forwarder with it; R21 runs once a worker's rows convert an adapter, R49 once they convert a
free seam); the W2 row was confirmed by a run at `c483a11`, R49 on it at `0e14eb9`.

| worker | `--rows` | seams | negative controls its own rows make runnable |
| --- | --- | --- | --- |
| W1 app | `app` | 10 | N2, N9, R39, R49 |
| W2 pre-solve and solve | `solve` | 13 | N1, N9, N14, N15, R4, R21, R35, R49 (R5 becomes pending) |
| W3 emit and post-fit | `emit` | 15 | N3, N9, N13, R3, R12, R21, R36, R37, R49 (N4 also needs W2's solve) |
| W4 diagnostics | `diag` | 24 | N9, R21, R49 |

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
--controls` (67 controls fire and none is MISSING; R5 is pending by design, its forwarder having
gone with the DD build's conversion; at a base after the REMOVE-key deletion R4 to R6 are pending
and 66 fire), `python scripts/nom_epoch_lint.py` without `--partial`,
the full G-EP and the cost pair.

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
| V1a | Every top-level statement of the frozen lambda is carried, character-exact and shifted four columns, by the epoch function, or is the old side of a declared transform of spec 5.3 whose new side is checked by rule (the epoch's construction, the dropped workspace, an alias, a sized or a result store at the local's frozen position, the clock store, `ep.emitted`, the brackets, the converted calls, the two replay closures). Each carried statement and store is paired with its frozen text token by token: an `ep.<path>` that stands for a local must pair with a frozen token that is that lambda local (the DD-carry predict's own `dt` is not `ep.clock.dt`), `ep.emitted` with the frozen comparison. The epoch function's definition has the frozen lambda's head: `NetworkRun& run`, then the lambda's parameter list, its trailing return type (`Application::LoopSignal`), and after the list the lambda's specifiers but `mutable` (none, so no `noexcept`: an exception that escapes an epoch reaches `Application::run`'s handler, which logs it and returns 1, instead of calling `std::terminate`). The VALUE_EQUAL proof. |
| V1b | Each converted seam's body below its bindings equals the frozen body, the solve's two replay lines and three hand-off lines excepted; the bindings open the body (no code before the opening marker) and hold declarations only. |
| V1c | Every unconverted seam and every adapter's explicit form is byte-identical to frozen. |
| V1d | `runNetwork` equals frozen apart from the spec 4.1 edits and the `UNREAD_ALIAS` lines (each gone line one declarator, a reference or a const builtin copy of a member path, and no token left in `runNetwork` names it), every declaration left in its outermost scope is read, and what those edits took out is where the specification puts it: the epoch function's one declaration in `Application`, with the lambda's head as in V1a; the predicate and the two brackets as functions with the closures' parameters, return type, specifiers after the list (none) and body, each declaration with its definition's head, under a preamble that binds exactly the closures' captures (by reference where the capture was, each to the captured object); the timer scratch; the hand-off fields with their frozen declarations; `NetworkRunEngines` with the engines in frozen construction order and frozen types, one constructor with an empty body and nothing after its parameter list; `LoopSignal`. |
| V1e | Every changed line of the build's inputs is accounted for. The inputs are every file the base has under `src/`, `include/` and `tests/`, every C, C++ or CMake file the candidate has there whatever its extension (so an untracked `.inl` or `.cpp` left there fails as a new file), `CMakeLists.txt`, and, when the tree carries them, `third_party/` and the root CMake configuration files. A C++ file is read without its comments, any other file as written (a `/*` in CMake hides nothing). A new file of the step is diffed against nothing; any other new file fails. A changed line is an include or forward-declaration line, or every token of it lies in a token range another check verified, so code appended on a verified line (after a definition's `}`, beside a namespace brace) is not accounted with it. No changed line is a preprocessor directive but `#include`, a new header's `#pragma once` or a conditional group of `#include` lines: a `#define` of a seam's name or an `#if 0` around the read copy changes what the compiler builds without changing a token any check reads. No frozen `#include` is removed. An `#include` that enters the tree names its header literally, without `..`, and reaches a header (`.hpp` or `.h` under `include/` or `third_party/`) the base has or one of the step's two new headers, a C++ standard library header, or a library header the base includes by that spelling; never a source, a test, an inline file or a file outside the include directories. `CMakeLists.txt` changes by the step's build-list lines alone: each new source named once, as a bare line of its target's list (`gnss_core`, `gnss_tests`), and nowhere else, so no per-source compile option or definition, no forced include and no test source in `gnss_core` enters. The new source files hold only what the specification puts there: `NetworkRun.cpp` the three definitions, `ApplicationNetworkEpoch.cpp` the one epoch function, the contract test its probe, static_asserts and one TEST case (the hand-written engine specification test's text is the suite's; its one listing in `gnss_tests`, and that nothing includes it, are checked here). Of `NetworkRunTiming` and `NetworkSolveGlueResult` only the moved and new members are claimed, so any other edit there (a default, a stray field) fails. No stray edit enters the tree. |
| V2 | Each converted seam has one epoch-form declaration and one definition with the parameter list its row states; kept parameters keep their frozen declaration; after the solve only the hand-off and the prepared solution are outputs. The head around the list is the frozen form's (an adapter's, that of the explicit form it forwards to): the return type and every specifier before the name, every specifier after the list, and no default argument but a kept parameter's frozen one. So no epoch form returns a reference to its local or to the explicit form's temporary (the spine's store would copy a destroyed object), and none is `noexcept`. |
| V3a | At every frozen call site paired with its new site (and at the solve's two replay calls), every frozen parameter binds the same storage object, derived independently from the frozen and the new source and equal modulo the declared substitutions; the binding line is `<the frozen declarator> = <expression>;`, so a reference stays a reference, a copy stays a copy and no type changes; `// written` marks exactly the epoch writes; the DD build's third-band binding is the one declared forwarder to `networkIsUncarriedThirdBand` over the knobs; adapters' other callers keep their text. |
| V3b | Every read of a captured driver name in the epoch function resolves to the frozen object (a reference, or a copy only where the frozen name was one), over every token V1a paired; each preamble line keeps the frozen declarator (a run reference under its spec type, an owned engine as a reference of its frozen type, `const` alone addable on a reference); the epoch preamble's third-band forwarder is the declared form. The COPY_SOURCE proof. |
| V3c | Each epoch member is realized at its local's frozen position, with the local's type and initializer, and is bound nowhere before it; `NetworkEpoch` holds only the declared members, in the declared layout (`declared.EPOCH_LAYOUT`), and each group exactly its declared locals in their declared order, so construction and destruction order are declared; `NetworkEpoch`, `EpochClock` and the groups are plain structs (no base clause, no friend, nested type, alias or static member; no member function in a group), so no base takes an aggregate initializer's values, no nested alias re-types a member and no hidden friend captures a seam call; the epoch's constructor binds `se`, does nothing else and has nothing after its parameter list, and `x_emit()`, with exactly `const` after its list, returns the frozen `x_emit` initializer under the epoch-member mapping. |
| V3d | The `NetworkRun` construction binds each reference member to the frozen object of its role; `NetworkRun` is a plain struct whose members are the references, `engines` and the run's groups in the spec's order; each reference member is declared with its spec type and bound to a constructor parameter of that type; the owned members have the frozen types and no defaults; the constructor builds only what the spec declares, has an empty body and has nothing after its parameter list. |
| V3e | No body reads a free `run`, `ep`, `nrun` or `nep` other than its parameters. |
| V3f | Every folded argument parses in the fold grammar (member reads, `&` or `*`, `.get()`, `ep.x_emit()`, literals, side-effect-free conditionals), so folding moves no side effect. |
| V4 | After the solve, the epoch function writes `ep` only through its four stores and the three post-solve output arguments; `ep.clock` is stored once, before its first read. |
| V5 | Const is deep: no member of `NetworkEpoch`, or of a project type held by value inside it, is a non-const reference, a pointer to non-const, a smart pointer, a `std::function` or a `std::reference_wrapper`. |
| V6 | The ordered events (calls, log calls with level and format literal, brackets with their `SB_*`, `LoopSignal` returns, the inserted stores) of the epoch function and of `runNetwork` equal the frozen ones. |
| V7 | Each converted declaration's Doxygen carries the frozen `@param` texts and names every epoch member its bindings read. |
| V8 | Every call of the frozen call table is covered exactly once, by a row or a declared non-conversion; the contract test lists the converted seams, each cast to one alias of the file whose type is the seam's epoch form (the frozen return type and the row's parameters), and every alias is one seam's, so the compiled contract keeps on later trees what V2 checked here; the manifest of this stage pairs with what the verifier found. |
| V9 | No call resolves otherwise than at the base. For every translation unit whose include closure (every tree file it reaches, of any type) is not its frozen counterpart's (its own frozen file; `ApplicationNetwork.cpp` for `ApplicationNetworkEpoch.cpp` and `NetworkRun.cpp`, which hold code cut from `runNetwork`), over the names its code calls: every free-function declaration newly visible is a converted seam's epoch form with the parameter types V2 checks, or one of the step's new functions; none visible at the base is gone but a converted seam's explicit form; no newly reached file defines a macro of an identifier of the unit, brings in a called name by a using-declaration or holds a using-directive. The two cut units reach no project header the frozen driver did not but the two new headers, and include no library header it did not read. So a narrowed overload of a seam (`NetworkEpoch&` for `const NetworkEpoch&`, the better match for the spine's non-const `ep`), in whatever file the unit includes, fails. The two headers hold only their plain structs (no base clause), forward declarations, static_asserts, the declarations of `networkEngineSpec` and the two brackets (each its definition's signature: return type, parameter types, default arguments, specifiers after the list) and the observer alias, and define no member function but the two constructors and `x_emit()`; every struct they define is scanned, with a base clause or without. |
| V10 | Every epoch group a seam's bindings read is already written when its spine line runs; a converted seam that a seam calls with its own run and epoch (the solve's replay builders) reads only groups written before the caller's spine line. |
| V11 | The spine lines that pass an epoch group as an output equal the group's "written by" list in the banner of `NetworkEpoch.hpp`. |
| V12 | `networkEngineSpec` holds the frozen setup statements of the declared ranges, character-exact and in order, reading only the configuration, the network block, the knobs and their own locals, calling only an allowlist; a replaced local is re-pointed at its own field (`T& name = spec.name;`, spec 2.2), so two locals cannot trade fields; the function has the head of spec 2.5 (it returns the spec by value, nothing follows its parameter list), opens with `NetworkEngineSpec spec;`, ends with `return spec;`, assigns a field whole at most once, never a field a reference binding fills, and an argument field only with its frozen argument; each engine constructor argument is the frozen argument or the spec field holding it, and each `NetworkEngineSpec` field has the type of what it holds (no argument is narrowed on its way to its engine). |
| V13 | The six seams whose frozen bodies use `run` or `ep` take the fallback names `nrun` / `nep`; no other seam needs them. |

The two write proofs (spec 5.6), on the frozen base and on the candidate: COPY_SOURCE, no
write to `n_threads`, `seam_threads`, `carry_par` or `brdc_rts` of the setup result outside
`networkSetup_`; VALUE_EQUAL, no writer of `ctx.counters.n_emitted` that the per-epoch body
reaches but `networkEmitStore`, which the body uses once. Each write's object is resolved by a
scope walk of its function, so a snapshot's `n_emitted` is another object. A failed proof fails
V3b or V1a and withdraws the substitution, so every binding that relied on it fails too.

The negative controls are mutations of the candidate tree built in memory; each must fail its
named check before a pass is believed. N1 to N16 are spec 5.7's (N4 must fail both V3a and V10;
its mutation keeps the trip's adapter and gives the solve's internal call the epoch form, which
binds `ep.solve.plan` there exactly as the spec's direct conversion would; N11 cuts the
`debug_r_scale` warning while the base has it, else a synthetic warning). R1 to R17 were added
at the review round, R18 to R34 at hardening round 1, R35 to R39 at hardening round 2 and R46
to R48, R51 and R52 at hardening round 3 and R53 at the EP1 review round, one per mutation an
earlier verifier accepted; R40 to
R45 (hardening round 2) put the edit of R35 to R39 on the other function heads the step
writes, and R49 and R50 (hardening round 3) carry the overload of R46 and the build-flag
channel of R48 by another route. R35 to R37 must fail both V2 and V8 (the mutation edits the
seam's head and its contract-test alias together), R38 both V1a and V1d (the epoch function's
definition and its declaration), R46, R47 and R49 both V1e and V9 (the include or the new file,
and the overload it carries):

| id | mutation | check |
| --- | --- | --- |
| R1 | the DD-carry predict's two reads of its own `dt` become `ep.clock.dt` | V1a |
| R2 | an epoch-preamble copy declared with another type (`const bool pl_warmup`) | V3b |
| R3 | a seam binding direct-initialized with a narrower type (`float pl_now_s(...)`) | V3a |
| R4 | the DD build's third-band forwarder negated | V3a |
| R5 | the epoch preamble's third-band forwarder negated | V3b |
| R6 | a predicate binding that shadows a function its body calls | V1d |
| R7 | a bracket's by-reference capture bound as a copy | V1d |
| R8 | `x_emit()` returns the float vector | V3c |
| R9 | `NetworkEpoch`'s constructor writes a member | V3c |
| R10 | a run reference member held by value | V3d |
| R11 | `NetworkRun`'s constructor logs | V3d |
| R12 | code before a converted seam's bindings | V1b |
| R13 | a default of `NetworkSolveGlueResult` changed | V1e |
| R14 | a `NetworkEngineSpec` field narrowed | V12 |
| R15 | two engines' construction order swapped | V1d |
| R16 | a second in-loop writer of `ctx.counters.n_emitted` | V1a |
| R17 | a setup field written inside the loop | V3b |
| R18 | two setup locals bound to each other's spec fields (and the run's lanes built crosswise while the base has them) | V12 |
| R19 | a spec field its local fills re-assigned from another field before `return spec;` | V12 |
| R20 | a spec field its local fills reset to its default (`spec.clk_rebase_cfg = ...Config{};`) | V12 |
| R21 | an adapter's epoch form forwards and, by the comma operator, empties the slip funnel | V1b |
| R22 | a static object with a side-effecting initializer in `NetworkRun.cpp` | V1e |
| R23 | the same after the epoch function's closing brace | V1e |
| R24 | a better-matching overload of a called seam in the epoch's translation unit | V1e, lint |
| R25 | a function-like macro of a called seam's name above the epoch function | V1e, lint |
| R26 | the epoch function under `#if 0`, a compiled copy under `#else` | V1e, lint |
| R27 | a nested derived funnel type and a member alias in `NetworkEpoch` | V3c |
| R28 | `EpochClock` with an aggregate base holding its three members | V3c |
| R29 | `EpochClock` with an empty base and a constructor that stores two members crosswise | V3c |
| R30 | a hidden friend of a called seam's name in `NetworkEpoch` | V3c |
| R31 | two run-owned groups declared in swapped order | V3d |
| R32 | two `NetworkEpoch` members declared in swapped order (`ws`, `solve`) | V3c |
| R33 | two members of `EpochPassA` declared in swapped order | V3c |
| R34 | two members of `EpochAssembly` declared in swapped order | V3c |
| R35 | the solve's epoch form returns `const NetworkSolveGlueResult&`, a reference to its local result (declaration, definition and contract alias) | V2, V8 |
| R36 | `networkFixedCycles` returns `const FixedCycleEmission&`, a reference to its local | V2, V8 |
| R37 | the `networkSolPrep` adapter's epoch form returns `const NetworkSolPrepResult&`, a reference to the explicit form's temporary | V2, V8 |
| R38 | `Application::networkProcessEpoch_` declared and defined `noexcept` | V1a, V1d |
| R39 | the epoch form of `networkWriteStateSnapshot` declared and defined `noexcept` | V2 |
| R40 | `networkSubBegin` (a moved bracket) declared and defined `noexcept` | V1d |
| R41 | `networkEngineSpec` returns `const NetworkEngineSpec&` (declaration and definition) | V12 |
| R42 | `NetworkEpoch`'s constructor `noexcept` | V3c |
| R43 | `NetworkRun`'s constructor `noexcept` | V3d |
| R44 | `NetworkRunEngines`' constructor `noexcept` | V1d |
| R45 | `x_emit() const noexcept` | V3c |
| R46 | a new `include/app/NetworkEpochCompat.inl` holds an overload of a called seam with its epoch parameter narrowed to `NetworkEpoch&` (once `networkEmitStore` is converted, `networkEmitStore(NetworkRun&, NetworkEpoch&, NetworkSolutionEpoch&)`, which empties the discontinuity bumps and forwards); the epoch's unit includes it on its blank line before the namespace | V1e, V9, lint |
| R47 | the same overload in the hand-written `tests/test_network_engine_spec.cpp` (listed in `gnss_tests`), included by the epoch's unit as `"../../../tests/test_network_engine_spec.cpp"` | V1e, V9, lint |
| R48 | `set_source_files_properties(src/core/app/ApplicationNetworkEpoch.cpp PROPERTIES COMPILE_OPTIONS "/fp:fast")` appended to `CMakeLists.txt` | V1e, lint |
| R49 | the narrowed overload of a converted seam in a new header `include/app/NetworkEpochCompat.hpp`, included by the epoch's unit | V1e, V9, lint |
| R50 | `target_compile_definitions(gnss_core PRIVATE networkIonoDiag=networkDdFrac)` appended to `CMakeLists.txt` | V1e, lint |
| R51 | `add_compile_options(/fp:fast)` in `CMakeLists.txt` between a comment line ending in `/*` and a line `# */` (two CMake comments, one C++ block comment) | V1e |
| R52 | the last quoted `#include` of `ApplicationNetwork.cpp` removed | V1e |
| R53 | an `UNREAD_ALIAS` line kept in `runNetwork` at its frozen place (the review round's defect: nothing reads it once the lambda has moved) | V1d |

A control whose target the stage cannot have yet is pending; one whose target the stage must
have but the tree lacks is MISSING and fails the run, so no control falls silent at a later
base. N7, N16, R24 to R26 and R46 to R50 must also fail the standalone lint, which must pass
on the unmutated candidate. The lint (`nom_epoch_lint.py`) is the permanent form of V4, V10 and
V11, with V0, its reading: the epoch function is defined once, its translation unit holds no
directive but `#include` and conditional groups of them and declares no name the function
calls, and each spine seam has one epoch-form definition of its arity; every file the unit
reaches through `#include` is a header (`.hpp` or `.h` under `include/` or `third_party/`)
named literally; no declaration there whose first parameter can bind `run` exactly
(`NetworkRun`, an alias of it, a template parameter, `auto`) differs from the seam's epoch
form; `CMakeLists.txt` lists the unit once as a target's source and names it nowhere else (no
per-source property but `COMPILE_FLAGS "/bigobj"`), defines no macro of a name the function
calls and forces no header into the units. It runs on any later tree, before a commit that
touches a seam signature, `ApplicationNetworkEpoch.cpp` or `CMakeLists.txt`.

## What the checks cannot prove (spec 5.8)

The verifier proves the text and order of every moved statement, the object every name in
every moved body denotes, the reference or copy semantics of every parameter, the initial state
of every epoch member where its local was declared, the write discipline after the solve, that
the engines are built from the arguments they had, and that no stray edit entered the tree. It
reads text; it does not compile or run anything. Not covered, and covered elsewhere:

- that default construction followed by a move assignment equals the frozen direct
  initialization (transforms T-B4 and T-B7): gate G-EP, every arm;
- construction order against the frozen code (the engines are built right after the traces
  instead of through setup; heap layout differs): gate G-EP, every arm, where a pointer-keyed
  container would show (the declaration order of the run's, the epoch's and the groups'
  members, which fixes their construction order among themselves, is declared and checked:
  V1d, V3c, V3d);
- destruction order against the frozen code and exception unwinding: gate G-EP;
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
- `tests/test_network_engine_spec.cpp`, which is hand-written and checked by the suite (its
  one listing in `gnss_tests` and that no file includes it are V1e's);
- the contents of library headers: V9 compares the declarations of the tree's own files, and
  V1e admits a new `#include` of a library only for a C++ standard header or a header the
  base already includes by that spelling.

## The REMOVE-key deletion: one set of tables for both bases

The REMOVE-key deletion (another session, branch `rmk/delete`) takes objects the tables name:
six seams of the per-epoch lambda (`networkRxCbDemeanPrepass`, `networkFastClockTick`,
`networkFastClockHarvest`, `networkBuildEnsembleGauges`, `networkClockLevelRebase`,
`networkIonoDwRelease`), the lane layer, FC-1, the PB emit filter, the iono-deweight gate, the
ensemble gauges and the third-band predicate; it also renames the counter snapshot
`fc_n_emitted_before` to `n_emitted_before`. The tables are not edited per base. They are
base-aware:

- Every entry naming a retired object is marked `retired_by=REMOVE_KEY`
  (`nom_ep1/__init__.py`): the six rows; in `declared.py` EPOCH_MEMBER `carry_arc_amb_key2`,
  OWNER_MOVE `iono_deweight`, `fc_ppp_utils`, `lane_run`, SPEC_CUT `mw_wl_cfg`, `ewl_cfg`,
  `pb_filter_cfg`, `idw, iono_deweight`, CALLABLE `isUncarriedThirdBand` and CALLBACK_DIRECT
  `buildEnsembleGauges`; in `model.py` the same objects' realization entries (the epoch
  member, the three run members and engines, the four specification fields, the two removed
  parameters) and the PB filter's argument of the emit engines (`RetiredArg`); since the
  review round also UNREAD_ALIAS `phase_datum_ensemble`, `phase_hold_on` and
  `sh_converged_latch`, and in `templates.RETIRED_TEXT` the key `D:sh_converged_latch` (the
  T1d latch, which only the designed text names: such a key is decided by its own
  declaration at the base, `model.bind_base`).
- `nom_ep1.bind_base(fz)` decides each marked entry from its object at the base. Every tool
  calls it once, in its `Frozen`, before reading a table. Object present: the entry applies as
  written. Object gone: the entry does not apply and is listed (the survey's section "Retired
  at this base", the generator's summary and its manifest's `retired_at_base`, the verifier's
  header). Object partly gone (a half-applied deletion): the survey fails and the generator
  refuses. An unmarked entry must find its object at every base, as before.
- `RENAMED` re-spells `fc_n_emitted_before` wherever the base declares `n_emitted_before`
  (the VALUE_EQUAL entry, `model.VALUE_EQUAL_EXPR`, the epoch's store, control N16).
- The designed text follows the base: `templates.RETIRED_TEXT` holds, per retired object, the
  edit that takes its mention out of `NetworkRun.hpp`, `NetworkEpoch.hpp` and the epoch
  function's documentation; `templates.SOLVE_BANNERS` holds the solve banner's rewrite for two
  replay builders and for one; the headers `NetworkRunGroups.hpp` gains are tied to the engines
  that need them (`gen_setup.GROUPS_INCLUDES`).

At the EP0 base `0e14eb9` every object is present: the survey prints what it printed before,
plus an empty retired section, and the generated tree was byte-identical to the dry run's
(`nom/ep1-dry` `42729f0`) until the review round, which removes the dead aliases there too (35
of them; `carry_innov_gate` is still read at that base and stays) and lists six `docs.py` hunks
written for the text after the deletion as not applied (`after_deletion`); the generator and
the verifier (`--rows all`) pass there. After the deletion the controls R4 to R6 (the third-band
forwarder) are pending and N11 uses its synthetic warning. On a later base:

1. `python scripts/nom_cpp.py --selftest --base <B> --expect KEY=N,...` with the new driver's
   counts, then `python scripts/nom_ep1_cut.py --survey --base <B>`; both must exit 0.
2. An object the survey reports missing that the deletion took is marked
   `retired_by=REMOVE_KEY` at its entry (and in `templates.RETIRED_TEXT` where the designed
   text names it), never deleted, so the tables keep serving the EP0 base; a renamed one goes
   into `RENAMED`.
3. On scratch exports of `<B>`: the generator (`--stage ep1a1`, then `--stage ep1b --rows
   all`), the verifier with `--controls` on the frozen tree and on both generated trees (no
   control MISSING), the lint, and `--check` on both trees.
4. The specification's texts that name these objects (sections 2.2, 4.1) stay as written; the
   tables are their executable form at each base.

## The review round (2026-10-14)

An outside review of the generated EP1 tree made seven findings; each is fixed in the tooling,
none by hand in the generated text.

1. **The base.** EP1 was built and verified on `180aa28`, the deletion's branch head, which main
   does not contain. The EP1 commits were rebased onto main `926dafa` (`git rebase --onto
   926dafa 180aa28 nom/ep1`; the two file sets are disjoint), `DEFAULT_BASE` names `926dafa`, and
   the tree is regenerated and verified there. Outside `scripts/` and `docs/` the two bases differ
   only in `src/core/config/GnssConfigNetworkPosture.cpp` and the goldens (the deleted keys
   unset), which EP1 does not touch.
2. **The dead aliases.** Moving the lambda out left 33 aliases in `runNetwork` that nothing
   reads (the defect INC-12 and EP0 removed). `declared.UNREAD_ALIAS` lists them;
   `gen_setup.unread_aliases` removes each one that nothing reads after the other edits and
   refuses a generation that would leave any outermost declaration of `runNetwork` unread; V1d
   accepts a gone line only as one declarator, a reference or a const builtin copy of a member
   path, with no token left naming it, and fails any unread declaration (control R53). Their
   orphaned comments are `docs.py` hunks.
3. **The banner's grep recipes** in `NetworkEpoch.hpp` now hold (`ep\.pass_a[,)]`, `n?ep\.pass_a\.present`)
   and name the lint's V11 as the authority.
4. **`EpochClock::carry_warm`'s documentation** named the T1d SH-convergence latch, which the
   deletion removed: a `templates.RETIRED_TEXT` entry keyed on `D:sh_converged_latch` (a key no
   table names is decided by its own declaration at the base, `model.bind_base`).
5. **Stale comments** (two findings) about the removed closures, the solve's callbacks, `sg_`, the
   driver's epoch locals and the engines as run-TU locals: `docs.py` `DOC_HUNKS`, applied by the
   generator to its finished text (each must change no token, each conditioned on the seam
   whose conversion makes it true), and the `STALE` census the generator prints and records in
   its manifest (`stale_spellings`) for the docs pass of the last merge.
6. **The maintenance banners.** `ApplicationNetworkEpoch.cpp`, the contract test and the epoch
   function's declaration say the generated text is maintained by hand once EP1 lands, guarded
   by `nom_epoch_lint.py` and the contract test; the CLAUDE.md seam-contract draft in the
   specification (section 7) is amended with the run-only form, the adapter form and the
   fallback names.

## Run times

Measured at `c483a11` on the development machine, the lower figure on a quiet machine and the
upper under load: the generator 1 to 3.5 minutes; the verifier 15 to 35 seconds quiet, 2 to 7
minutes loaded; with `--controls` 4 to 7 minutes quiet (33 controls, 6 to 13 seconds each,
all rows) and 9 to 25 minutes loaded; the lint under a minute quiet, 2 to 3.5 minutes loaded;
the survey 1 to 2 minutes. With the 50 controls of hardening round 1, measured at `0e14eb9` on
a lightly shared machine: `--controls` 12 minutes for all rows (11 to 18 seconds each) and 6.5
minutes for EP1a-1 (38 controls run), under a minute on the frozen tree (10 run). With the 61
controls of hardening round 2, measured at `0e14eb9` with the three acceptance runs side by
side: `--controls` 30 minutes for all rows (60 run, 15 to 38 seconds each), 15.5 minutes for
EP1a-1 (45 run) and 2 minutes on the frozen tree (10 run). With the 68 controls of hardening
round 3, measured at `0e14eb9` with the three acceptance runs side by side: the verifier 35 to
55 seconds (V9 reads every translation unit's include closure once, about 25 seconds; a
control that edits no file of a unit's closure reuses that unit's result), `--controls` 20
minutes for all rows (67 run, 12 to 45 seconds each), 10 minutes for EP1a-1 (51 run) and 1.5
minutes on the frozen tree (13 run); the lint 5 to 15 seconds.
