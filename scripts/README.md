# scripts/

One-off utilities. Index seeded with the RTB-7(d) warm-resume ("RS card") tooling
(`docs/SPEC_warm_resume_event_card.md`); older scripts are self-documenting via
their module docstrings.

## Warm-resume / branch-faithfulness (RS card)

| script | purpose |
|---|---|
| `run_warm_resume_arm.sh` | Run ONE warm-resume arm under the HB-2 protocol: re-seed the working snapshot from the immutable converge artifact **before every resume launch**, then assert the expected `WARM RESUME` / `BRANCH-FAITHFUL` verdict in the log. Guards the §0.4 hazard — a resume overwrites the snapshot it read, so an un-re-seeded relaunch silently full-cold-starts (warn only) and would be graded as warm. |
| `warm_resume_sd_metric.py` | Between-satellite SD-of-SD divergence of emitted products (CLK/CB) vs an inline reference, per epoch, with post-transient slope fitting and an optional plot. Datum-invariant by construction; PB excluded (integer-ambiguous). Read levels **and** slopes — a higher level inside the adaptive-window transient with a shallower slope is a transient, not divergence. |
| `diff_carry_blobs.py` | Branch-faithfulness instrument: compares the Card-4 carry blobs of two snapshots per (group, chord) **after translating ids to names** via each file's own `carry_sat_vertex` / `carry_group_id` maps (raw-id comparison is a silent mis-binding trap). Reports branch-agree / branch-flip fractions and set churn. |

Typical use (see the spec's §5 run matrix):

```bash
bash scripts/run_warm_resume_arm.sh config/test_suite/_warm_resume.json run.log warm+carry
python scripts/warm_resume_sd_metric.py --ref solA.csv \
    --arm cold=solB2off.csv --arm warm=solB2on.csv --plot rs_curves.png
```
