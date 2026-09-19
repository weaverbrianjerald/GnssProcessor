# Onboarding

This path leads from an empty machine to a merged change. The pages are read in order.
Each page ends with a working result.

| # | Page | Result | Time |
| --- | --- | --- | --- |
| 1 | [Setup](01_setup.md) | Code built, one PPP position solution, unit tests passing | ½ day |
| 2 | [Code tour](02_code_tour.md) | A map of the PPP and real-time paths, and a reading list | 1 to 2 days |
| 3 | [Worked example](03_worked_example.md) | One complete small change: code, test, data run, LaTeX, commits | 1 to 2 days |
| 4 | [Git workflow](04_git_workflow.md) | The branch, commit and merge-request loop | 1 hour |

Afterwards, [CONTRIBUTING.md](../../CONTRIBUTING.md) is the one-page reference: the five
rules, the merge-request checklist, and a table that maps each kind of change to its
code, test and reference files.

## Suggested schedule

| When | Task |
| --- | --- |
| Day 1 | Page 1. First run completed and tests passing. A failed install step is reported to a maintainer right away. |
| Days 2 to 3 | Page 2. The headers on the reading list are read next to the matching chapters of `docs\report\main.pdf`. `ppp_internal.json` and `ppp_rtk_internal.json` are run, and the CSV output is inspected and plotted. |
| Days 4 to 5 | Pages 3 and 4. The worked example is re-created on a personal branch and compared with `example/stochastic-model`. |
| Week 2 | A first task, agreed with a maintainer, similar in size to the worked example. |

## The three kinds of documentation

| Document | Question answered |
| --- | --- |
| The onboarding pages and `CONTRIBUTING.md` | How is work done in this project? |
| `docs\report\main.pdf`, the LaTeX mathematical reference | What does the code compute, and why? |
| Doxygen comments in `include\` | What does a given function take and return? |

## Out of scope at the start

About two thirds of the repository belongs to a multi-station network research program:
`docs\refound\`, `docs\cards\`, `docs\archive\`, most of `config\` and `scripts\`, every
`Network*` source file, and `CLAUDE.md`. It is a careful scientific record, but it is not
the starting point. The [code tour](02_code_tour.md#what-to-skip) lists what to skip.
