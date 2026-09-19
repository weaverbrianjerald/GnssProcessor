# Onboarding — your first two weeks

Welcome. This path takes you from an empty machine to a merged change. Do the pages in
order; each one ends with something that works.

| # | Page | You finish with | Time |
| --- | --- | --- | --- |
| 1 | [Setup](01_setup.md) | the code built, a PPP position solution, the unit tests passing | ½ day |
| 2 | [Code tour](02_code_tour.md) | a map of the PPP and real-time paths; a reading list | 1–2 days |
| 3 | [Worked example](03_worked_example.md) | a complete small change made by your own hands: code, test, data run, LaTeX, commits | 1–2 days |
| 4 | [Git workflow](04_git_workflow.md) | the branch → commit → merge-request loop | 1 hour |

Afterwards, [CONTRIBUTING.md](../../CONTRIBUTING.md) is the one-page reference: the five
rules, the merge-request checklist, and a table of where each kind of change goes.

## A suggested schedule

| When | Do |
| --- | --- |
| Day 1 | Page 1. Get the first run and the tests green. Ask for help the moment an install step fails. |
| Days 2–3 | Page 2. Read the headers on the reading list next to the matching chapters of `docs\report\main.pdf`. Run `ppp_internal.json` and `ppp_rtk_internal.json`; open the CSVs; plot them. |
| Days 4–5 | Pages 3 and 4. Re-create the worked example on your own branch. Compare with `example/stochastic-model`. |
| Week 2 | Your first real task, agreed with Brian — similar in size to the worked example. |

## The three documents, and when to use each

| Document | Answers |
| --- | --- |
| These onboarding pages + `CONTRIBUTING.md` | *How do I work here?* |
| `docs\report\main.pdf` — the LaTeX mathematical reference | *What does the code compute, and why?* |
| Doxygen comments in `include\` | *What does this function take and return?* |

## What you can ignore for now

About two thirds of the repository belongs to a multi-station network research program
(`docs\refound\`, `docs\cards\`, `docs\archive\`, most of `config\` and `scripts\`, every
`Network*` source file, and `CLAUDE.md`). It is a careful scientific record, but it is not
where you start. The [code tour](02_code_tour.md#what-to-skip) lists exactly what to skip.
