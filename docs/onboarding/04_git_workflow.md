# 4 · Git workflow — branch, commit, merge request

The short version: **never commit to `main`. One branch per change. Small commits.
Open a merge request. Brian merges.**

If you are new to git, that sentence plus the commands on this page are all you need
for the first months.

---

## The loop, every time

```powershell
# 1. Start from an up-to-date main
git switch main
git pull

# 2. Make a branch for ONE change
git switch -c feature/elevation-floor-model

# 3. Work. Look at what you changed, often.
git status
git diff

# 4. Stage the files you mean to commit — by name
git add include/model/StochasticModel.hpp src/core/model/StochasticModel.cpp

# 5. Check what is staged, then commit
git diff --cached --stat
git commit

# 6. Publish the branch and open a merge request
git push -u origin feature/elevation-floor-model
```

`git push` prints a GitLab link — open it to create the merge request (MR). Assign it
to Brian. Push more commits to the same branch to update the MR.

## Branch names

| Prefix | Use for | Example |
| --- | --- | --- |
| `feature/` | new capability | `feature/elevation-floor-model` |
| `fix/` | bug fix | `fix/sp3-header-leap-second` |
| `docs/` | documentation only | `docs/troposphere-section` |
| `test/` | tests only | `test/klobuchar-edge-cases` |

## Commit messages

Format: **`area: what changed`**, then a blank line, then a few lines on **why**.

```
model: add ElevationFloor stochastic model (default OFF)

The pure 1/sin^2(el) law assigns all zenith variance to elevation-dependent
effects.  ElevationFloor keeps a fraction f elevation-independent:
var = var_zen * (f + (1-f)/sin^(2p)(el)).  f = 0 is bit-identical to Elevation.

Selected with filter.stochastic_model = "elevation_floor"; the shipped default
is unchanged.
```

- `area` is where the change lives: `model`, `filter`, `config`, `realtime`, `rinex`,
  `products`, `tests`, `docs`, `scripts`, `build`. Look at `git log --oneline` for the
  house style.
- Say whether behaviour changes for someone who does not opt in. Usually the answer
  must be "no" — see [CONTRIBUTING rule 1](../../CONTRIBUTING.md#the-five-rules).
- **No AI attribution lines** (`Co-Authored-By: Claude…`, "Generated with…"). The
  `commit-msg` hook you enabled during setup strips them; this is a project rule.

## Good commits for one change

A change like the [worked example](03_worked_example.md) is three commits, in this order.
Each one builds and passes the tests on its own.

| # | Commit | Contains |
| --- | --- | --- |
| 1 | `model: add ElevationFloor stochastic model (default OFF)` | headers, `.cpp`, config loader, sample-config comment |
| 2 | `tests: ElevationFloor closed form, limits and loader contract` | the new test file + its `CMakeLists.txt` line |
| 3 | `docs: ElevationFloor in the stochastic-model reference` | `.tex` + rebuilt `.pdf` |

## Things that go wrong

| Situation | What to do |
| --- | --- |
| `git status` shows files you did not touch (`*.aux`, `logs/`, `build/`) | They are outputs. Do not add them. If one is not ignored, tell Brian. |
| You used `git add .` and staged too much | `git restore --staged <file>` un-stages it. Your edits are kept. |
| You committed on `main` by accident | `git switch -c feature/my-change` (the commit comes with you), then ask before touching `main`. |
| `main` moved on while you worked | `git switch main; git pull; git switch -; git merge main` — fix conflicts, build, test, commit. |
| A merge conflict you do not understand | Stop. `git merge --abort` puts everything back. Ask. |
| You want to undo local edits to one file | `git restore <file>` — this **discards** your edits to it. |

**Never** use `git push --force`, `git reset --hard`, or `git clean` on this
repository without asking first. They destroy work — possibly someone else's.

## Before you open the merge request

Run the checklist in [CONTRIBUTING.md](../../CONTRIBUTING.md#the-checklist). The MR
description should say: what changed, how you tested it (paste the test summary line),
and — if you ran data — one before/after number.
