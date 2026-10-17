# Card NOM3: the step designs (inputs to the generators)

The buildable specifications of card NOM3's next steps (`docs/refound/NOM3_network_giants_2026-10-09.md`).
The card holds each step's binding summary, its frozen gates and its decisions; these files hold the detail a
generator and its verifier are built from: types in code, every cut range, every binding, every gate arm.

| file | step | measured at | status |
| --- | --- | --- | --- |
| `EP1_spec.md` | EP1: `NetworkRun` and `NetworkEpoch` replace the per-epoch lambda; every spine seam takes `(NetworkRun&, const NetworkEpoch&, outputs...)` | main `0e14eb9` (EP0) | pre-registered in NOM3 (2026-10-13); its tooling is `scripts/nom_ep1/` with `scripts/nom_cpp.py`, `scripts/nom_ep1_cut.py`, `scripts/nom_ep1_verify.py`, `scripts/nom_epoch_lint.py`; the build waits for the REMOVE-key deletion |
| `SG_design.md` | SG: the solve glue as a linear orchestrator of typed stages; the five exits a declared table | main `31c393f` plus the deletion's track branches merged | designed and adversarially checked (2026-10-14); not pre-registered |
| `PB1_design.md` | PB1: PASS B's stages in the epoch form, then the produce loop as a declared per-station step table with the parallel contract as a type | as SG | as SG |
| `HV1_design.md` | HV1: the station-local carry harvest cut by carried kind under one orchestrator | as SG | as SG |
| `N4_design.md`, `N4_fields.tsv` | NOM N4 and N5: entity tables own the run state (one entity per sub-step, persistence declared per field, the snapshot records filled from the declarations), then `EpochEvents` and entity-table bindings; the TSV places every `NetworkCarryState` member | `nom/wave3` `4816d1d` | designed and pre-registered (2026-10-15); defaults stated, not built |
| `N6_design.md`, `N6_sites.tsv` | NOM N6: the network owns its local parameter set (`StationParams`, no borrowed `StateManager`) and one observation-equation function serves the producer's H, the consumer's H and the seeds; a byte-identical body (N6.0 to N6f) and a behaviour-changing tail (N6g to N6i); the TSV counts the user-side state use per network file | main `b701d01` | designed and pre-registered (2026-10-16); defaults stated, not built |

How to read them:

- Every count and line number is measured at the commit named in the table. The REMOVE-key deletion (another
  workstream, branches `rmk/*`) lands before EP1's build, so EP1's counts are re-measured on the post-deletion main
  (the generator's `--survey` mode does it), and SG, PB1 and HV1 were measured on a reconstruction of the
  post-deletion files (main plus the deletion's track branches, merged); each is re-surveyed when it starts.
- Each design names its decisions for the author; the defaults it states are not decided.
- The measurement evidence (frozen copies, reconstructions of the post-deletion files, survey and inventory scripts
  and their outputs) is outside the repository, at `D:\GnssProcessorSupport\nom3_evidence\` (folders `ep1design`,
  `nextsteps`, `ep`, `giants`). A path in these files under a session scratchpad (`.../scratchpad/ep1design/...`,
  `.../scratchpad/nextsteps/...`) resolves there.
