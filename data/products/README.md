# data/products/

This directory holds the cached GNSS product files used by **internal
mode** (`data_source = "internal"` in `src/core/config/gnss_config.json`).

It is intentionally **empty in git** — the contents are far too large
(hundreds of MB) to commit.  Populate it by running the bootstrap
script from the repo root:

```bash
python scripts/bootstrap_internal_data.py
```

The script downloads:

* **SP3** orbits, **CLK** clocks, **BIA** code biases, **ERP** Earth-rotation
  parameters, and **BRDC** broadcast nav for the bundled test dates
  (2024-DOY129 for the PPP fixture, 2023-DOY321 for the baseline
  fixture).  Source: CDDIS (`https://cddis.nasa.gov`) — requires a
  free Earthdata Login account and a `.netrc` entry; see the script's
  module doc-comment for details.
* **igs20.atx** antenna phase-centre model from `files.igs.org` (no auth).
* **EOT20_load.nc** ocean-tide loading from DGFI-TUM (no auth, ~280 MB).
  Skipped automatically when ocean-tide loading is disabled in your
  config.

After bootstrap, the layout is:

```
products/
  orbit/unzip/   *.SP3
  clock/unzip/   *.CLK
  dcb/unzip/     *.BIA
  erp/unzip/     *.ERP
  brdc/unzip/    BRDC00IGS_R_*_MN.rnx
  rinex/unzip/   *.crx       (Hatanaka observation cache)
  atx/           igs20.atx
  nc/            EOT20_load.nc
```

`PathResolver` globs these paths automatically — you do not need to
list them in the JSON config.

## Production mode

If you set `data_source = "production"` in the config, this directory
is unused.  `ProductResolver` downloads on demand into
`data_download.data_root` instead (typically a separate location like
`C:/GnssProcessorSupport/products/`).
