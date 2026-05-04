# GnssProcessor scenario configs

Each `.json` here is a self-contained config tuned for a single processing
scenario. Pick one at run time:

```
gnss_processor.exe config/ppp.json
gnss_processor.exe config/ppp_rtk.json              (default if no argv)
gnss_processor.exe config/baseline_single.json
gnss_processor.exe config/baseline_multi.json
```

| File | Mode | Observation source | Default tuning highlights |
| --- | --- | --- | --- |
| `ppp.json` | PPP | `data/observations/absolute/*.rnx` (auto) | Float-only, BSSD off, large initial std (true unknown position) |
| `ppp_rtk.json` | PPP-RTK | `data/observations/absolute/*.rnx` (auto) | BSSD on, pivot ≥ 25°, AR float (mode=0), estimate sat code biases |
| `baseline_single.json` | Baseline-Single | `IGS_Pasadena` fixture (CIT1 + JPLM, ~10 km) | IF DDs, Δzwd state, pivot handover, AR float-only |
| `baseline_multi.json` | Baseline-Multi | Custom (drop in `baseline.rovers`) | Same tuning as Baseline-Single, star topology |

For per-field documentation see [`docs/CONFIG.md`](../docs/CONFIG.md).
