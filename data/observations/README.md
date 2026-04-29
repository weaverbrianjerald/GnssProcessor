# data/observations/

Cached observation RINEX files for the bundled test cases.  Empty in
git for the same reason as `data/products/` — populated by:

```bash
python scripts/bootstrap_internal_data.py
```

Layout after bootstrap:

```
observations/
  absolute/                       # PPP / PPP-RTK fixture
    GCGO00USA_R_20241290000_01D_30S_MO.rnx
  baseline/short/static/          # Baseline-Single / -Multi fixture
    C1033210.23o
    NARY3210.23o
    233_3210.23o
```

`PathResolver` matches files here by station name (the part of the
filename that contains the 4-character station ID).  Add new fixtures
under any subdirectory — the resolver scans recursively.
