"""The live monitor (prototype): a local server that streams one record per epoch to a page that keeps a rolling
window of the network's state.

    telemetry.py  the per-epoch record (what a live engine would send) and a source that builds it from the blocks
                  of a finished run, so a run can be replayed as if it were live
    server.py     the local HTTP server: the page, the run's static description, the stream of records (server-sent
                  events), and the replay controls (speed, pause, jump)

The page's sources are page/live.html, page/live.css and page/live.js.  Start it with
    py -3.14 scripts/netview/netview.py live <run folder or built page> [--speed 1] [--port 8770]
"""
