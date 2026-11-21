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

## Stills for a poster or a slide

```
py -3.14 scripts\netview\netview.py still <page.html> --panel <view>/<part> [--t HH:MM] [--size WxH] [--type-scale S] [--scale N] [--set KEY=VALUE ...] [--placed-width INCHES] -o <picture.png>
py -3.14 scripts\netview\netview.py still [<page.html>] --batch <stills.json> [-o <folder>]
```

`still` writes one part of one view of a built page as a picture: the part alone on white, without the rail, the
header, buttons or notes, at exactly the asked size, so nothing is cropped. Headless Microsoft Edge draws it (a batch
of 13 stills of a 24 h page of 86 MB took 32 s, four side by side). The page file is not changed: the command works
on a temporary copy that carries this checkout's page code around the page's data, so a page built before stills
existed can be used.

**The part** is named `<view>/<part>`. The view is written as in the page's address (`network`, `overview`,
`receivers`, `satellites`, `atmosphere`, `receiver/GCGO00USA`, `satellite/G05`, `los/<receiver>/<satellite>` and the
other layers). The part is one of:

| Part | What is written |
| --- | --- |
| `links` | the network picture of the `network` view: rings by hops, or the world map with `--set l=map` |
| `timeline` | the event lanes with the held share and the hour labels; on any view (`network/timeline` opens fastest), and the view of one receiver or satellite adds that entity's lane |
| `tiles` | the view's key figures as data, to a `.json` file: label, value, unit, note and level of each tile |
| a panel's title, or its beginning | any titled panel of the view: `overview/Receivers over time`, `network/Share of the links`, `receiver/GCGO00USA/Wet delay`, `satellite/G05/Emitted clock`, `satellite/G05/Ground track`, `atmosphere/Vertical ionosphere at the cursor` |

A name that fits no part of the view, or more than one, is refused; the message lists the view's parts. A still that
fails leaves no picture.

**Size and type.** `--size` is the picture in CSS pixels and `--scale` the picture pixels per CSS pixel.
`--type-scale S` is a magnification: the part is laid out at 1/S of the size and drawn S times larger, so type, lines
and marks all grow by S against the picture (as the browser's zoom does). The page's labels are 10 to 12 CSS pixels
(9 for the bar labels of the jump chart). With the size given as inches times 100, a type scale of 2.5 puts a 10
pixel label at 18 pt, and `--scale 3` gives 300 pixels per inch. With `--placed-width` (the width in inches the
picture has where it is placed) the command also prints the type sizes in points: pixels x placed width x 72 /
layout width. The drawing takes the height the legend leaves. The rows of a matrix share it (rows thinner than
4 pixels carry no names), the lanes of the timeline share it (when they are thinner than their labels the command
prints the height the timeline needs), and a map keeps its 2:1 shape inside it.

**Choices.** What the page keeps in its buttons is given with `--t` and `--set`:

| Key | Values | Effect |
| --- | --- | --- |
| `--t` | an epoch index, or a GPS time of day `HH:MM` | the time cursor |
| `l` | `map` | `links`: the world map instead of the rings |
| `f`, `b` | index in the page's "Signals" list (0 first); 0 or 1 in its "Tree of" list | the signals and the tree of `links`, of the timeline's held share and of the network view's charts |
| `names` | `0`, `sat` | `links`: no names, or the satellites and the reference receiver only |
| `held`, `float` | `0` | `links`: without the held, or the float, links |
| `m` | a metric's key | a matrix: `code`, `phase`, `zcode`, `nsat`, `raim`, `slip`, `qc`, `held`, `zwd`, `clksd` for receivers; `code`, `phase`, `nrx`, `anchor`, `frac`, `held`, `hops` for satellites (those whose trace the run has). An unknown key is refused with the list |
| `iono` | `vertical`, `slant` | the ionosphere map of `atmosphere` (the panel's title follows the choice) |
| `key` | `0` | without the legend or the colour scale |
| `title` | `1` | with the panel's title line |
| `cursor` | `0` | without the line of the time cursor |
| `colours` | `turbo`, `viridis` | the colour map of magnitudes |
| `s` | `node:NAME`, `link:RECEIVER:SAT` | the selection of the network view, as in its address |

**Several stills.** A batch file lists them; each still takes the defaults it does not set, and files are written to
the batch file's folder or to `-o`:

```json
{
  "page": "D:/GnssProcessorSupport/netview_out/agu3_n145.html",
  "defaults": {"t": "12:00", "type_scale": 2.5, "scale": 3},
  "stills": [
    {"file": "links_map.png", "panel": "network/links", "size": [1930, 1000], "placed_width": 19.3, "set": {"l": "map", "key": 0}},
    {"file": "wet_delay.png", "panel": "receiver/GCGO00USA/Wet delay", "size": [965, 312], "placed_width": 9.65},
    {"file": "tiles.json", "panel": "overview/tiles"}
  ]
}
```

The command prints one line per still (the picture's pixels, the layout, the magnification, the smallest and largest
type) and, for a batch, writes `stills.json` beside the pictures: per still what it shows (signals, tree, layout,
metric, the cursor's time), its sizes and `axis`, the span of its time axis as fractions of the picture's width. Two
stills placed so that these spans coincide share one time axis (the timeline and a matrix have label columns of
different widths).

**Words.** A still replaces words that are the page's makers' own: "Held-out user receiver" reads "Test receiver"
(the table `STILL_WORDS` of `scripts/netview/page/still.js`). The page itself keeps its words.

**The address.** A page built since 2026-11-20 shows the same still in a browser, for a look before a batch:
`<page.html>#/network?t=12:00&l=map&still=links` (the part fills the window; the browser's zoom is the type scale;
`w` and `h` fix the part's size in CSS pixels).

Limits: one part per still; a still is a magnified part, so marks grow with the type (the rings picture of 56
receivers holds every name at 18 pt only when it is placed about 15 in wide or more; `names=sat` thins it); a panel
that scrolls on the page (a table, an event list) is cut at the still's height.

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

The test of the stills draws six stills of the small synthetic page with Microsoft Edge and is skipped on a machine
without it.
