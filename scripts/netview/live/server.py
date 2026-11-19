"""The live monitor's local server (Python standard library only, bound to 127.0.0.1).

    GET /           the live page (style, script and colour maps inlined)
    GET /meta       the run's description: receivers, satellites, families, field lists, colour ranges, status
    GET /stream     server-sent events: 'snapshot' (the rolling window, sent on connect and after a jump) and then
                    one 'record' event per epoch as it is released; a comment every 15 s keeps it open
    GET /snapshot   the rolling window once, as JSON (the page's still view, ?still, for checks and screenshots)
    GET /control    ?speed=epochs per second &pause=1|0 &jump=epoch: changes the release; answers the status
    GET /archive    the built dashboard page the replay comes from (when it comes from one), for going deeper

Two sources: a finished run, whose epochs are released at a chosen rate so the page sees what a live engine would
send (telemetry.ReplaySource), and a running engine's live record, whose epochs are released as the engine writes
them (follow.FollowSource; the rate then only sets how fast the window catches up after a jump back).  A source's
lists that grow (satellites, event words) travel in each record's "add" and in each snapshot's "lists".
"""
from __future__ import annotations

import collections
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


def _dumps(obj) -> str:
    return json.dumps(obj, separators=(',', ':'), allow_nan=False)


class Replay:
    """The replay: a clock that releases one epoch's record at a time into a rolling window."""

    def __init__(self, source, window: int = 240, speed: float = 1.0, start: int = 0):
        self.src = source
        self.window = max(10, int(window))
        self.speed = max(0.05, float(speed))
        self.paused = False
        self.stopped = False
        self.cond = threading.Condition()
        self.buf: collections.deque = collections.deque(maxlen=self.window)
        self.gen = 0                                        # +1 at each jump: streams send a fresh snapshot
        self.e = 0
        self._fill(max(0, min(source.n_ep - 1, int(start))))

    def _fill(self, e: int):
        """The window before epoch e (caller holds the lock or is the constructor)."""
        self.buf.clear()
        for k in range(max(0, e - self.window), e):
            self.buf.append((k, _dumps(self.src.record(k))))
        self.e = e

    def status(self) -> dict:
        return {'e': self.e, 'n_ep': self.src.n_ep, 'speed': self.speed, 'paused': self.paused, 'gen': self.gen,
                'step_s': self.src.step_s, 'window': self.window, 'mode': self.src.mode}

    def snapshot(self) -> str:
        """The window as one JSON message (the caller holds the lock): status, the source's growing lists, records."""
        lists = self.src.lists()
        return ('{"status":' + _dumps(self.status()) + (',"lists":' + _dumps(lists) if lists else '') +
                ',"records":[' + ','.join(s for _k, s in self.buf) + ']}')

    def control(self, q: dict) -> dict:
        with self.cond:
            if 'speed' in q:
                self.speed = min(200.0, max(0.05, float(q['speed'])))
            if 'pause' in q:
                self.paused = q['pause'] in ('1', 'true', 'yes')
            if 'jump' in q:
                self._fill(max(0, min(self.src.n_ep - 1, int(float(q['jump'])))))
                self.gen += 1
            self.cond.notify_all()
            return self.status()

    def run(self):
        """The clock (its own thread): one record per 1/speed seconds, catching up if a record took longer."""
        due = time.monotonic()
        while not self.stopped:
            if self.e >= self.src.n_ep:
                self.src.poll()                             # a running engine's new lines (a finished run: none)
            with self.cond:
                waiting = self.paused or self.e >= self.src.n_ep
                if not waiting:
                    rec = _dumps(self.src.record(self.e))
                    self.buf.append((self.e, rec))
                    self.e += 1
                    self.cond.notify_all()
                speed = self.speed
            if waiting:
                time.sleep(0.2)
                due = time.monotonic()
                continue
            due += 1.0 / speed
            lag = due - time.monotonic()
            if lag > 0:
                time.sleep(lag)
            elif lag < -2.0:                                # far behind (a slow record or a speed change): resync
                due = time.monotonic()


def make_handler(replay: Replay, page_html: str, archive: Path | None):
    class Handler(BaseHTTPRequestHandler):
        server_version = 'netview-live'

        def log_message(self, fmt, *args):                 # quiet: one line per request would flood the console
            pass

        def _send(self, code: int, body: bytes, ctype: str):
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):  # noqa: N802 (the standard library's name)
            u = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            try:
                if u.path == '/':
                    self._send(200, page_html.encode('utf-8'), 'text/html; charset=utf-8')
                elif u.path == '/meta':
                    meta = replay.src.meta(replay.window)
                    self._send(200, _dumps(dict(meta, status=replay.status())).encode('utf-8'), 'application/json')
                elif u.path == '/control':
                    self._send(200, _dumps(replay.control(q)).encode('utf-8'), 'application/json')
                elif u.path == '/archive' and archive is not None:
                    self._send(200, archive.read_bytes(), 'text/html; charset=utf-8')
                elif u.path == '/stream':
                    self._stream()
                elif u.path == '/snapshot':
                    with replay.cond:
                        body = replay.snapshot()
                    self._send(200, body.encode('utf-8'), 'application/json')
                elif u.path == '/favicon.ico':
                    self._send(204, b'', 'image/x-icon')
                else:
                    self._send(404, b'not found', 'text/plain')
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

        def _stream(self):
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Connection', 'keep-alive')
            self.end_headers()
            gen, last = None, -1
            while not replay.stopped:
                with replay.cond:
                    if gen != replay.gen:
                        gen = replay.gen
                        last = replay.buf[-1][0] if replay.buf else replay.e - 1
                        out = 'event: snapshot\ndata: ' + replay.snapshot() + '\n\n'
                    else:
                        if not replay.buf or replay.buf[-1][0] <= last:
                            replay.cond.wait(timeout=15.0)
                        if gen != replay.gen:
                            continue
                        new = [(k, s) for k, s in replay.buf if k > last]
                        if new:
                            last = new[-1][0]
                            out = ''.join('event: record\ndata: ' + s + '\n\n' for _k, s in new)
                        else:
                            out = ': keep\n\n'
                self.wfile.write(out.encode('utf-8'))
                self.wfile.flush()

    return Handler


def serve(source, page_html: str, port: int = 8770, window: int = 240, speed: float = 1.0, start: int = 0,
          archive: Path | None = None, log=print):
    """Start the replay and the server; returns (server, replay, thread) with the server already listening."""
    source.poll()
    replay = Replay(source, window=window, speed=speed, start=start)
    source.meta(replay.window)                              # computed once now (a finished run's ranges)
    server = ThreadingHTTPServer(('127.0.0.1', port), make_handler(replay, page_html, archive))
    server.daemon_threads = True
    clock = threading.Thread(target=replay.run, name='replay', daemon=True)
    clock.start()
    return server, replay, clock
