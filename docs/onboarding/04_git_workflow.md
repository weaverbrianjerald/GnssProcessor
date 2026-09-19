# 4 · Git workflow: branch, commit, merge request

Summary: **no commits on `main`. One branch per change. Small commits. A merge request
for every branch. A maintainer merges.**

The commands on this page cover the first months of work, including for contributors
new to git.

---

## The loop

```powershell
# 1. Start from an up-to-date main
git switch main
git pull

# 2. Create a branch for ONE change
git switch -c feature/elevation-floor-model

# 3. Work. Inspect the changes often.
git status
git diff

# 4. Stage the intended files by name
git add include/model/StochasticModel.hpp src/core/model/StochasticModel.cpp

# 5. Review what is staged, then commit
git diff --cached --stat
git commit

# 6. Publish the branch and open a merge request
git push -u origin feature/elevation-floor-model
```

`git push` prints a GitLab link that creates the merge request (MR). The MR is assigned
to a maintainer. Further commits pushed to the same branch update the MR.

## Branch names

| Prefix | Use | Example |
| --- | --- | --- |
| `feature/` | New capability | `feature/elevation-floor-model` |
| `fix/` | Bug fix | `fix/sp3-header-leap-second` |
| `docs/` | Documentation only | `docs/troposphere-section` |
| `test/` | Tests only | `test/klobuchar-edge-cases` |

## Commit messages

Format: **`area: what changed`**, a blank line, then a few lines on the **reason**.

```
model: add ElevationFloor stochastic model (default OFF)

The pure 1/sin^2(el) law assigns all zenith variance to elevation-dependent
effects.  ElevationFloor keeps a fraction f elevation-independent:
var = var_zen * (f + (1-f)/sin^(2p)(el)).  f = 0 is bit-identical to Elevation.

Selected with filter.stochastic_model = "elevation_floor"; the shipped default
is unchanged.
```

- `area` names where the change lives: `model`, `filter`, `config`, `realtime`, `rinex`,
  `products`, `tests`, `docs`, `scripts`, `build`. `git log --oneline` shows the
  established style.
- The message states whether behaviour changes for a run that does not opt in. The
  answer is normally "no" ([CONTRIBUTING rule 1](../../CONTRIBUTING.md#the-five-rules)).
- **No AI attribution lines** (`Co-Authored-By: Claude…`, "Generated with…"). The
  `commit-msg` hook enabled during setup strips them. This is a project rule.

## Commits for one change

A change such as the [worked example](03_worked_example.md) is three commits, in this
order. Each commit builds and passes the tests on its own.

| # | Commit | Contents |
| --- | --- | --- |
| 1 | `model: add ElevationFloor stochastic model (default OFF)` | Headers, `.cpp`, config loader, sample-config comment |
| 2 | `tests: ElevationFloor closed form, limits and loader contract` | The new test file and its `CMakeLists.txt` line |
| 3 | `docs: ElevationFloor in the stochastic-model reference` | `.tex` and rebuilt `.pdf` |

## Common problems

| Situation | Resolution |
| --- | --- |
| `git status` shows untouched files (`*.aux`, `logs/`, `build/`) | These are outputs and are not added. An output that is not ignored is reported to a maintainer. |
| `git add .` staged too much | `git restore --staged <file>` un-stages a file. The edits are kept. |
| A commit was made on `main` by accident | `git switch -c feature/my-change` moves the work to a branch. `main` is not touched further without asking. |
| `main` advanced during the work | `git switch main; git pull; git switch -; git merge main`, then resolve conflicts, build, test, commit. |
| A merge conflict that is not understood | `git merge --abort` restores the previous state. Then ask. |
| Local edits to one file are to be discarded | `git restore <file>`. This **discards** the edits to that file. |

`git push --force`, `git reset --hard` and `git clean` are **never** used on this
repository without prior agreement. They destroy work, possibly someone else's.

## Before the merge request

The [checklist in CONTRIBUTING.md](../../CONTRIBUTING.md#the-checklist) is completed
first. The MR description states what changed, how it was tested (with the test summary
line), and, if data was run, one before/after number.
