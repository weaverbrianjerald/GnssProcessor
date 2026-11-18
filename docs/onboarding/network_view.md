# The network dashboard: one network run on one page

`scripts/netview/netview.py` turns the traces of one network run into a single HTML page laid out as a dashboard:
layers over the whole network (overview, network links, receivers, satellites, cycle slips, stochastic model,
atmosphere, products and user check, events) that lead to the page of one receiver, one satellite and one line of
sight, all on one time cursor. The page opens by double-click (no server, no network access). It is made on request
from a finished run folder, so normal runs pay nothing. The quantities, their sources and the decisions behind the
page: [NETVIEW](../cards/tools/NETVIEW-network-view.md) (the dashboard: section 11).

## Make a page from a finished run

```
py -3.14 scripts\netview\netview.py build <run folder> -o <page.html>
```

Without `-o` the page is written to `<run folder>\netview.html`. The build reads `logs\*.csv`,
`logs\network_run_manifest.json` and `products.txt` (about 40 s for a 6 h run of 56 receivers) and writes a page of
about 22 MB. The overview is ready about a second after the page loads; the other layers decode their data the first
time they are opened (0.1 to 0.3 s). Parts whose trace is absent are left out; the build and each layer name the
file and the key that writes it.

## Make a run that can be viewed

```
py -3.14 scripts\netview\netview.py arm <config.json> -o <armed config.json>
```

`arm` writes a copy of the config with the trace keys of the page switched on and changes nothing else (the
fixing and the chain datum change the estimate and are never switched on by it). The reference configurations
(`config\golden\`) already arm every trace the dashboard reads but two: `network.trace_chain_datum` (the trees and
the links) and `network.trace_rpb_states` (the receiver phase bias); both write only on a run with the chain datum
(`network.phase_datum_eq34`). `arm` also switches on `network.trace_live_record`, the record the live monitor follows
(below), for a configuration that is not a copy of the reference ones (they arm it). The run is then made as usual
from the armed copy.

| Trace | Key | Size per 6 h, 56 receivers, GPS + Galileo |
| --- | --- | --- |
| `network_chain_edges.csv` | `network.trace_chain_datum` | 49 MB |
| `network_chain_frame.csv` | `network.trace_chain_datum` | 50 MB |
| `network_rpb_states.csv` | `network.trace_rpb_states` | about 14 MB (1.15 MB in the first 30 min of DOY 144) |
| `network_ring_fix.csv` | written by the fixing itself (`network.ring_fix.enable`) | 14 MB (one family held) to 34 MB (two) |
| `network_live.jsonl` (the live monitor's record) | `network.trace_live_record` | 12 MB (about three quarters of it the events) |
| post-fit rows, carried floats, slip tests and observables, ionosphere, residual test, events and the rest | already armed in `config\golden\` | about 600 MB together |

The two added traces cost about 1 % of the run time and leave `products.txt` byte-identical (measured on 30 min of
DOY 144: [NETVIEW](../cards/tools/NETVIEW-network-view.md), section 5).

## Compare two runs

```
py -3.14 scripts\netview\netview.py compare <page A.html> <page B.html> -o <compare.html>
```

The comparison page shows the two pages side by side; time, the view and its selection follow each other, and each
side keeps its own data (for example the same day from two reference receivers). The three files keep their
relative places.

## The live monitor

```
py -3.14 scripts\netview\netview.py live <run folder, live record or built page> [--port 8770] [--speed N] [--start HH:MM] [--replay]
```

The command starts a local server (bound to 127.0.0.1) and prints its address. The page there shows a rolling window
of the network: key figures with their trend, receivers and satellites over the window for a chosen metric, a map,
what happened (newest first, by family) and the selected receiver or satellite with every value over the window. The
page's controls pause, change the rate or jump. Ctrl+C stops the server.

- **Following an engine.** With `network.trace_live_record` on (armed in the reference configurations since
  2026-11-17, so every run copied from them has it), the engine writes `logs\network_live.jsonl`: a header with the
  receivers, then one line per epoch, flushed as each epoch ends. `live` on such a run folder, or on the file
  itself, opens on the newest epochs and shows each new epoch as soon as the engine has written it, while the engine
  runs or after it has finished. A run folder without products yet is taken as an engine that is starting: the page
  waits for its first epoch. After a jump back, `--speed` (default 50 epochs per second) sets how fast the window
  catches up. When no epoch has arrived for 10 s, the window label says for how long (an engine that has finished,
  or stalls).
- **Replaying a finished run.** A built page, or a run folder without the record (or with `--replay`), is replayed as
  a live engine would send it: `--speed` (default 1) sets the epochs released per second and `--start` the first
  epoch. A built page starts in seconds; a run folder is built first.

Per epoch the record holds how the epoch ended, the post-fit sigma0 of code and phase, the receivers in the solution
and the satellites with a clock; per receiver the post-fit code and phase RMS, its satellites, the robust scale of its
normalized code residuals, its largest normalized residual, its wet delay and its clock sigma; per satellite the
post-fit code RMS, its receivers, its emitted clock and its declared discontinuities; the held-out user's position
error; and every event. With the integer fixing armed (`network.ring_fix.enable`) it also holds, per family, receiver
and satellite, the links that are not tree links and how many of them are held at whole numbers: the page then shows
a held tile per family and the held share of each receiver and satellite, as the replay of the same run does. Its
values are those of the run's traces ([NETVIEW](../cards/tools/NETVIEW-network-view.md), sections 13 and 15). A line
holds about 4 KB of values for 58 receivers, plus the epoch's events (and about 0.6 KB with the integer fixing). Rows that record without
acting (reports, diagnostics, coincident duplicates) count neither as cycle slips nor as quality-test flags and are
listed under bookkeeping.

## Reading the page

1. The rail on the left picks a layer; the badge beside it is its count. The path above the view leads back
   (Overview > Receivers > MBAR00UGA > G05), and so does the browser's back button.
2. The overview's tiles show the cursor epoch with their history; a click on a tile opens the layer behind it. Its
   matrices (receivers x time, satellites x time) take a metric from the buttons above them, worst rows first.
3. A click on a receiver or a satellite (a table row, a point on a map, a cell of a matrix) opens its page at that
   time. On a receiver or satellite page, a click on an arc or on a dot of the sky plot opens the line of sight.
4. The slider, the play button, the arrow keys (Shift: ten epochs) and a click on any chart move the time; every
   view follows. The lanes under the slider count the events of the run; a click on a lane jumps to its nearest
   event and lists the events of that epoch. On an entity page the lane "Selected" holds that entity's events.
5. Event lists name the receiver and the satellite of each row; a click on a row opens its line of sight at that
   time.
6. The network links layer is the first page design: the tree of each signal as rings of hops, held and float
   links, and on the right the detail of a link, satellite or receiver. A double-click on a node or a link opens its
   page.
7. The legend button explains the terms (layers, going deeper, tree link, held link, wide and narrow lane, jump,
   post-fit residual, sigma0, variance factor, MW, GF, wet delay).
8. "Colours" in the header picks the colour map of magnitudes and counts: turbo (the default) or viridis (better in
   print and for colour-vision deficiency); signed values keep a blue-red map. A signed quantity whose values nearly
   all share one sign (the wet delay) gets a scale from 0, its lowest colour marked "<0". The atmosphere layer's
   ionosphere map shows the vertical-equivalent delay or the slant delay.

## Tests

```
py -3.14 scripts\netview\test_netview.py
```
