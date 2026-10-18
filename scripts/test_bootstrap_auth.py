#!/usr/bin/env python3
"""Tests of the Earthdata login in scripts/bootstrap_internal_data.py.

    python scripts/test_bootstrap_auth.py        (or: python -m unittest scripts/test_bootstrap_auth.py)

No network is used.  Three servers on the loopback interface stand in for CDDIS (which
redirects an unauthenticated request to the single sign-on), the Earthdata login (which asks
for Basic credentials and redirects back with a code and a session cookie) and an unrelated
host that also asks for Basic credentials.  The tests check the opener the script builds
(build_auth_opener) and the netrc handling around it, not the live service.
"""
import http.server
import os
import sys
import tempfile
import threading
import unittest
import urllib.parse
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bootstrap_internal_data as bs  # noqa: E402

USER, PASSWORD = "earthdata_user", "s3cret pass"
BODY = b"SP3 file body\n"


class _Servers:
    """The three loopback servers and what each one saw."""

    def __init__(self):
        self.seen = {"cddis": [], "urs": [], "other": []}      # (path, Authorization header or None)
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                role = self.server.role
                auth = self.headers.get("Authorization")
                outer.seen[role].append((self.path, auth))
                url = urllib.parse.urlsplit(self.path)
                query = urllib.parse.parse_qs(url.query)
                if role == "urs":
                    if auth != outer.expected_basic():
                        self._reply(401, headers={"WWW-Authenticate": 'Basic realm="Earthdata Login"'})
                    else:
                        back = query["back"][0] + "?code=granted"
                        self._reply(302, headers={"Location": back,
                                                  "Set-Cookie": "urs_session=ok; Path=/"})
                elif role == "cddis":
                    cookie = self.headers.get("Cookie", "")
                    if query.get("code") == ["granted"]:
                        self._reply(200, BODY, headers={"Set-Cookie": "cddis_session=ok; Path=/"})
                    elif "cddis_session=ok" in cookie:
                        self._reply(200, BODY)
                    else:
                        back = "http://127.0.0.1:%d%s" % (self.server.server_port, url.path)
                        sso = "http://127.0.0.1:%d/oauth/authorize?back=%s" % (
                            outer.ports["urs"], urllib.parse.quote(back, safe=""))
                        self._reply(302, headers={"Location": sso})
                else:                                            # an unrelated host that wants Basic
                    self._reply(401, headers={"WWW-Authenticate": 'Basic realm="other"'})

            def _reply(self, code, body=b"", headers=None):
                self.send_response(code)
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.ports = {}
        self._servers = []
        for role in ("cddis", "urs", "other"):
            srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            srv.role = role
            self.ports[role] = srv.server_port
            self._servers.append(srv)
            threading.Thread(target=srv.serve_forever, daemon=True).start()

    @staticmethod
    def expected_basic():
        import base64
        return "Basic " + base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode()

    def host(self, role):
        return "127.0.0.1:%d" % self.ports[role]

    def url(self, role, path="/archive/file.sp3"):
        return "http://%s%s" % (self.host(role), path)

    def close(self):
        for srv in self._servers:
            srv.shutdown()
            srv.server_close()


class EarthdataLoginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = _Servers()

    @classmethod
    def tearDownClass(cls):
        cls.srv.close()

    def setUp(self):
        for v in self.srv.seen.values():
            v.clear()
        self.tmp = tempfile.TemporaryDirectory()
        self.dest = Path(self.tmp.name) / "a" / "b" / "file.sp3"

    def tearDown(self):
        self.tmp.cleanup()

    def opener(self, user=USER, password=PASSWORD):
        return bs.build_auth_opener({self.srv.host("urs"): (user, password)}, scheme="http")

    def test_login_redirect_cookie_and_download(self):
        self.assertTrue(bs.download_file(self.srv.url("cddis"), self.dest, opener=self.opener()))
        self.assertEqual(self.dest.read_bytes(), BODY)
        # The credentials were offered to the login host after its 401, and nowhere else.
        urs = self.srv.seen["urs"]
        self.assertEqual([a is not None for _, a in urs], [False, True])
        self.assertEqual(urs[1][1], self.srv.expected_basic())
        self.assertTrue(all(a is None for _, a in self.srv.seen["cddis"]))
        self.assertEqual(len(self.srv.seen["cddis"]), 2)             # the first request, the return

    def test_session_cookie_spares_the_second_login(self):
        opener = self.opener()
        self.assertTrue(bs.download_file(self.srv.url("cddis", "/archive/a.sp3"), self.dest, opener=opener))
        n_urs = len(self.srv.seen["urs"])
        other = self.dest.with_name("second.sp3")
        self.assertTrue(bs.download_file(self.srv.url("cddis", "/archive/b.sp3"), other, opener=opener))
        self.assertEqual(len(self.srv.seen["urs"]), n_urs)           # served on the cookie alone
        self.assertEqual(other.read_bytes(), BODY)

    def test_wrong_password_fails_and_leaves_no_file(self):
        self.assertFalse(bs.download_file(self.srv.url("cddis"), self.dest,
                                          opener=self.opener(password="wrong")))
        self.assertFalse(self.dest.exists())
        # One try with the wrong credentials, not a loop.
        self.assertLessEqual(sum(a is not None for _, a in self.srv.seen["urs"]), 1)

    def test_no_credentials_fails_without_offering_any(self):
        empty = bs.build_auth_opener({}, scheme="http")
        self.assertFalse(bs.download_file(self.srv.url("cddis"), self.dest, opener=empty))
        self.assertFalse(self.dest.exists())
        self.assertTrue(all(a is None for _, a in self.srv.seen["urs"]))

    def test_credentials_are_not_offered_to_a_host_not_in_the_login_table(self):
        self.assertFalse(bs.download_file(self.srv.url("other"), self.dest, opener=self.opener()))
        self.assertTrue(self.srv.seen["other"])
        self.assertTrue(all(a is None for _, a in self.srv.seen["other"]))

    def test_a_cached_file_is_not_fetched(self):
        self.dest.parent.mkdir(parents=True)
        self.dest.write_bytes(b"x")
        self.assertTrue(bs.download_file(self.srv.url("cddis"), self.dest, opener=self.opener()))
        self.assertEqual(self.dest.read_bytes(), b"x")
        self.assertEqual(self.srv.seen["cddis"], [])

    def test_installed_opener_is_the_default(self):
        saved = bs._OPENER
        try:
            bs._OPENER = self.opener()
            self.assertTrue(bs.download_file(self.srv.url("cddis"), self.dest))
            self.assertEqual(self.dest.read_bytes(), BODY)
        finally:
            bs._OPENER = saved


class NetrcTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, text):
        path = self.home / name
        path.write_text(text)
        os.chmod(path, 0o600)
        return path

    def test_resolution_order(self):
        self.assertIsNone(bs.resolve_netrc_path(home=self.home))
        under = self.write("_netrc", "")
        self.assertEqual(bs.resolve_netrc_path(home=self.home), under)
        dot = self.write(".netrc", "")
        self.assertEqual(bs.resolve_netrc_path(home=self.home), dot)

    def test_explicit_path_wins_and_is_not_replaced_when_missing(self):
        self.write(".netrc", "")
        mine = self.write("mine", "")
        self.assertEqual(bs.resolve_netrc_path(mine, home=self.home), mine)
        self.assertIsNone(bs.resolve_netrc_path(self.home / "absent", home=self.home))

    def test_logins_come_from_the_named_hosts_only(self):
        path = self.write(".netrc", (
            f"machine {bs.EARTHDATA_HOST} login alice password pw-alice\n"
            "machine example.org login bob password pw-bob\n"
            "default login carol password pw-carol\n"))
        got = bs.netrc_logins(path, (bs.EARTHDATA_HOST, bs.CDDIS_HOST))
        self.assertEqual(got, {bs.EARTHDATA_HOST: ("alice", "pw-alice")})      # no default, no bob

    def test_both_hosts_when_both_are_filed(self):
        path = self.write(".netrc", (
            f"machine {bs.EARTHDATA_HOST} login alice password pw1\n"
            f"machine {bs.CDDIS_HOST} login alice password pw2\n"))
        got = bs.netrc_logins(path, (bs.EARTHDATA_HOST, bs.CDDIS_HOST))
        self.assertEqual(got, {bs.EARTHDATA_HOST: ("alice", "pw1"), bs.CDDIS_HOST: ("alice", "pw2")})

    def test_malformed_file_gives_no_logins(self):
        path = self.write(".netrc", "machine\n")
        self.assertEqual(bs.netrc_logins(path, (bs.EARTHDATA_HOST,)), {})

    def test_entry_without_a_password_is_not_a_login(self):
        path = self.write(".netrc", f"machine {bs.EARTHDATA_HOST} login alice\n")
        self.assertEqual(bs.netrc_logins(path, (bs.EARTHDATA_HOST,)), {})


if __name__ == "__main__":
    unittest.main()
