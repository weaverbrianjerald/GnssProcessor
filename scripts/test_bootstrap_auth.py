#!/usr/bin/env python3
"""Tests of the Earthdata login and the download check in scripts/bootstrap_internal_data.py.

    python scripts/test_bootstrap_auth.py        (or: python -m unittest scripts/test_bootstrap_auth.py)

No network is used.  Four servers on the loopback interface stand in for CDDIS (which
redirects an unauthenticated request to the single sign-on), the Earthdata login (which
answers a request without the right Basic credentials with its HTML login page and status
200, as the live service does, and redirects back with a code and a session cookie when the
credentials are right), an unrelated host that asks for Basic credentials, and a plain file
server.  The tests check the opener the script builds (build_auth_opener), the netrc handling
around it, and the content check of download_file (an HTML page, or a .gz that is not gzip
data, is refused and never kept), not the live service.
"""
import base64
import contextlib
import gzip
import http.server
import io
import os
import sys
import tempfile
import threading
import unittest
import unittest.mock
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bootstrap_internal_data as bs  # noqa: E402

USER, PASSWORD = "earthdata_user", "s3cret pass"
BODY = b"SP3 file body\n"
LOGIN_PAGE = (b'<!DOCTYPE html>\n<html lang="en">\n<head>\n  <meta charset="utf-8">\n'
              b'  <title>Earthdata Login</title>\n</head>\n<body>\n'
              b'  <form action="/login" method="post"><input name="username"></form>\n'
              b'</body>\n</html>\n')
GZ_BODY = gzip.compress(b"orbit product\n" * 50)

_saved_no_proxy = None


def setUpModule():
    """Keep an environment proxy from catching the loopback requests."""
    global _saved_no_proxy
    _saved_no_proxy = {k: os.environ.get(k) for k in ("NO_PROXY", "no_proxy")}
    os.environ["NO_PROXY"] = os.environ["no_proxy"] = "127.0.0.1"


def tearDownModule():
    for key, value in _saved_no_proxy.items():
        if value is None:
            os.environ.pop(key, None)
        else:
            os.environ[key] = value


class _Servers:
    """The four loopback servers and what each one saw."""

    def __init__(self):
        self.seen = {"cddis": [], "urs": [], "other": [], "plain": []}   # (path, Authorization or None)
        #: path -> (status, body, content type) for the archive and the plain server.
        self.content = {}
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
                        # The live login answers with its login page, not with a 401.
                        self._reply(200, LOGIN_PAGE, "text/html; charset=utf-8")
                    else:
                        back = query["back"][0] + "?code=granted"
                        self._reply(302, headers={"Location": back,
                                                  "Set-Cookie": "urs_session=ok; Path=/"})
                elif role == "cddis":
                    cookie = self.headers.get("Cookie", "")
                    if url.path.startswith("/offsite/"):
                        back = outer.url("other", "/landing")
                        self._to_login(back)
                    elif query.get("code") == ["granted"]:
                        self._serve(url.path, headers={"Set-Cookie": "cddis_session=ok; Path=/"})
                    elif "cddis_session=ok" in cookie:
                        self._serve(url.path)
                    else:
                        self._to_login("http://127.0.0.1:%d%s" % (self.server.server_port, url.path))
                elif role == "plain":
                    self._serve(url.path)
                elif url.path == "/landing":                     # an unrelated host that wants Basic
                    self._reply(200, BODY)
                else:
                    self._reply(401, headers={"WWW-Authenticate": 'Basic realm="other"'})

            def _to_login(self, back):
                sso = "http://127.0.0.1:%d/oauth/authorize?back=%s" % (
                    outer.ports["urs"], urllib.parse.quote(back, safe=""))
                self._reply(302, headers={"Location": sso})

            def _serve(self, path, headers=None):
                status, body, ctype = outer.content.get(path, (200, BODY, "application/octet-stream"))
                self._reply(status, body, ctype, headers)

            def _reply(self, code, body=b"", ctype=None, headers=None):
                self.send_response(code)
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                if ctype is not None:
                    self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.ports = {}
        self._servers = []
        for role in ("cddis", "urs", "other", "plain"):
            srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            srv.role = role
            self.ports[role] = srv.server_port
            self._servers.append(srv)
            threading.Thread(target=srv.serve_forever, daemon=True).start()

    @staticmethod
    def expected_basic():
        return "Basic " + base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode()

    def host(self, role):
        return "127.0.0.1:%d" % self.ports[role]

    def url(self, role, path="/archive/file.sp3"):
        return "http://%s%s" % (self.host(role), path)

    def close(self):
        for srv in self._servers:
            srv.shutdown()
            srv.server_close()


class _ServerTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = _Servers()

    @classmethod
    def tearDownClass(cls):
        cls.srv.close()

    def setUp(self):
        for v in self.srv.seen.values():
            v.clear()
        self.srv.content.clear()
        self.tmp = tempfile.TemporaryDirectory()
        self.dest = Path(self.tmp.name) / "a" / "b" / "file.sp3"

    def tearDown(self):
        self.tmp.cleanup()

    def opener(self, user=USER, password=PASSWORD):
        return bs.build_auth_opener({self.srv.host("urs"): (user, password)}, scheme="http")


class EarthdataLoginTest(_ServerTestCase):
    def test_credentials_are_sent_up_front_then_redirect_cookie_and_download(self):
        self.assertTrue(bs.download_file(self.srv.url("cddis"), self.dest, opener=self.opener()))
        self.assertEqual(self.dest.read_bytes(), BODY)
        # One request to the login host, and it carried the credentials: the login page never
        # asked for them with a 401, so a handler that waits for one would have sent none.
        urs = self.srv.seen["urs"]
        self.assertEqual(len(urs), 1)
        self.assertEqual(urs[0][1], self.srv.expected_basic())
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

    def test_a_gzip_product_through_the_login(self):
        dest = self.dest.with_name("orb.SP3.gz")
        self.srv.content["/archive/orb.SP3.gz"] = (200, GZ_BODY, "application/x-gzip")
        self.assertTrue(bs.download_file(self.srv.url("cddis", "/archive/orb.SP3.gz"), dest,
                                         opener=self.opener()))
        self.assertEqual(dest.read_bytes(), GZ_BODY)

    def test_wrong_password_gets_the_login_page_which_is_refused_and_not_saved(self):
        # The loopback archive stands for CDDIS, so the message carries the netrc hint.
        with unittest.mock.patch.object(bs, "CDDIS_HOST", self.srv.host("cddis")), \
                self.assertRaises(bs.DownloadContentError) as ctx:
            bs.download_file(self.srv.url("cddis"), self.dest, opener=self.opener(password="wrong"))
        self.assertFalse(self.dest.exists())
        message = str(ctx.exception)
        self.assertIn("HTML page", message)
        self.assertIn('"Earthdata Login"', message)
        self.assertIn(f"Check the {bs.EARTHDATA_HOST} entry of the netrc file", message)
        self.assertIn("/oauth/authorize", message)                   # where the answer came from
        self.assertNotIn("wrong", message)
        self.assertNotIn(PASSWORD, message)
        # One try with the wrong credentials, not a loop.
        self.assertEqual(sum(a is not None for _, a in self.srv.seen["urs"]), 1)

    def test_no_credentials_is_refused_without_offering_any(self):
        empty = bs.build_auth_opener({}, scheme="http")
        with self.assertRaises(bs.DownloadContentError):
            bs.download_file(self.srv.url("cddis"), self.dest, opener=empty)
        self.assertFalse(self.dest.exists())
        self.assertEqual(len(self.srv.seen["urs"]), 1)
        self.assertTrue(all(a is None for _, a in self.srv.seen["urs"]))

    def test_credentials_are_not_offered_to_a_host_not_in_the_login_table(self):
        self.assertFalse(bs.download_file(self.srv.url("other"), self.dest, opener=self.opener()))
        self.assertTrue(self.srv.seen["other"])
        self.assertTrue(all(a is None for _, a in self.srv.seen["other"]))

    def test_credentials_are_not_carried_along_a_redirect_to_another_host(self):
        # cddis sends the client to the login host, which (credentials accepted) sends it on to
        # the unrelated host: that host must see no Authorization header.
        self.assertTrue(bs.download_file(self.srv.url("cddis", "/offsite/x"), self.dest,
                                         opener=self.opener()))
        self.assertEqual(self.dest.read_bytes(), BODY)
        self.assertEqual([a for _, a in self.srv.seen["urs"]], [self.srv.expected_basic()])
        self.assertTrue(self.srv.seen["other"])
        self.assertTrue(all(a is None for _, a in self.srv.seen["other"]))

    def test_a_plain_http_request_gets_no_credentials_when_the_table_is_for_https(self):
        opener = bs.build_auth_opener({self.srv.host("urs"): (USER, PASSWORD)})     # scheme https
        with self.assertRaises(bs.DownloadContentError):
            bs.download_file(self.srv.url("cddis"), self.dest, opener=opener)
        self.assertTrue(self.srv.seen["urs"])
        self.assertTrue(all(a is None for _, a in self.srv.seen["urs"]))

    def test_installed_opener_is_the_default(self):
        saved = bs._OPENER
        try:
            bs._OPENER = self.opener()
            self.assertTrue(bs.download_file(self.srv.url("cddis"), self.dest))
            self.assertEqual(self.dest.read_bytes(), BODY)
        finally:
            bs._OPENER = saved


class DownloadContentCheckTest(_ServerTestCase):
    """What download_file keeps: a refused response leaves nothing behind."""

    def setUp(self):
        super().setUp()
        self.plain = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def fetch(self, path, dest=None):
        return bs.download_file(self.srv.url("plain", path), dest or self.dest, opener=self.plain)

    def test_html_in_place_of_a_gz_is_refused(self):
        dest = self.dest.with_name("orb.SP3.gz")
        self.srv.content["/x/orb.SP3.gz"] = (200, LOGIN_PAGE, "text/html; charset=utf-8")
        with self.assertRaises(bs.DownloadContentError) as ctx:
            self.fetch("/x/orb.SP3.gz?token=abc123", dest)
        self.assertFalse(dest.exists())
        self.assertIn("HTML page", str(ctx.exception))
        self.assertIn("/x/orb.SP3.gz", str(ctx.exception))
        self.assertNotIn("abc123", str(ctx.exception))               # the query is not echoed
        self.assertNotIn("netrc", str(ctx.exception))                # no Earthdata hint for another host

    def test_html_in_place_of_a_plain_file_is_refused(self):
        dest = self.dest.with_name("igs20.atx")
        self.srv.content["/static/igs20.atx"] = (200, LOGIN_PAGE, "text/html")
        with self.assertRaises(bs.DownloadContentError):
            self.fetch("/static/igs20.atx", dest)
        self.assertFalse(dest.exists())

    def test_html_is_refused_whatever_its_content_type_says(self):
        self.srv.content["/x/file.sp3"] = (200, LOGIN_PAGE, "application/octet-stream")
        with self.assertRaises(bs.DownloadContentError):
            self.fetch("/x/file.sp3")
        self.assertFalse(self.dest.exists())

    def test_a_gz_without_the_gzip_magic_is_refused(self):
        dest = self.dest.with_name("orb.SP3.gz")
        self.srv.content["/x/orb.SP3.gz"] = (200, BODY, "application/x-gzip")
        with self.assertRaises(bs.DownloadContentError) as ctx:
            self.fetch("/x/orb.SP3.gz", dest)
        self.assertFalse(dest.exists())
        self.assertIn("not gzip data", str(ctx.exception))
        self.assertIn("53 50 33 20", str(ctx.exception))             # "SP3 " in hex

    def test_an_empty_gz_is_refused(self):
        dest = self.dest.with_name("orb.SP3.gz")
        self.srv.content["/x/orb.SP3.gz"] = (200, b"", "application/x-gzip")
        with self.assertRaises(bs.DownloadContentError) as ctx:
            self.fetch("/x/orb.SP3.gz", dest)
        self.assertFalse(dest.exists())
        self.assertIn("empty", str(ctx.exception))

    def test_a_large_gzip_file_is_kept_whole(self):
        body = gzip.compress(os.urandom(300_000))                    # incompressible: well over the sniffed bytes
        self.assertGreater(len(body), 10 * bs._SNIFF_BYTES)
        dest = self.dest.with_name("big.clk.gz")
        self.srv.content["/x/big.clk.gz"] = (200, body, "application/x-gzip")
        self.assertTrue(self.fetch("/x/big.clk.gz", dest))
        self.assertEqual(dest.read_bytes(), body)

    def test_text_and_binary_products_are_kept(self):
        atx = b"          1.4            M                                      ANTEX VERSION / SYST\n"
        nc = b"CDF\x01" + bytes(range(256)) * 20
        for name, body, ctype in (("igs20.atx", atx, "text/plain"),
                                  ("EOT20_load.nc", nc, "application/x-netcdf"),
                                  ("served_as_html.atx", atx, "text/html")):   # wrong type, body not HTML
            with self.subTest(name=name):
                dest = self.dest.with_name(name)
                self.srv.content["/s/" + name] = (200, body, ctype)
                self.assertTrue(self.fetch("/s/" + name, dest))
                self.assertEqual(dest.read_bytes(), body)

    def test_a_refusal_removes_an_empty_file_left_at_the_destination(self):
        self.dest.parent.mkdir(parents=True)
        self.dest.write_bytes(b"")                                   # size 0 does not count as cached
        self.srv.content["/x/file.sp3"] = (200, LOGIN_PAGE, "text/html")
        with self.assertRaises(bs.DownloadContentError):
            self.fetch("/x/file.sp3")
        self.assertFalse(self.dest.exists())

    def test_an_http_error_still_returns_false(self):
        self.srv.content["/x/file.sp3"] = (404, LOGIN_PAGE, "text/html")
        self.assertFalse(self.fetch("/x/file.sp3"))
        self.assertFalse(self.dest.exists())

    def test_a_cached_good_file_is_not_fetched(self):
        dest = self.dest.with_name("orb.SP3.gz")
        dest.parent.mkdir(parents=True)
        dest.write_bytes(GZ_BODY)
        self.assertTrue(self.fetch("/x/orb.SP3.gz", dest))
        self.assertEqual(self.srv.seen["plain"], [])

    def test_a_cached_login_page_is_discarded_and_fetched_again(self):
        dest = self.dest.with_name("orb.SP3.gz")
        dest.parent.mkdir(parents=True)
        dest.write_bytes(LOGIN_PAGE)                                 # left by a run before the check existed
        self.srv.content["/x/orb.SP3.gz"] = (200, GZ_BODY, "application/x-gzip")
        with contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertTrue(self.fetch("/x/orb.SP3.gz", dest))
        self.assertEqual(dest.read_bytes(), GZ_BODY)
        self.assertEqual(len(self.srv.seen["plain"]), 1)
        self.assertIn("discard", err.getvalue())

    def test_a_cached_login_page_stays_refused_when_the_server_sends_it_again(self):
        dest = self.dest.with_name("orb.SP3.gz")
        dest.parent.mkdir(parents=True)
        dest.write_bytes(LOGIN_PAGE)
        self.srv.content["/x/orb.SP3.gz"] = (200, LOGIN_PAGE, "text/html")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(bs.DownloadContentError):
            self.fetch("/x/orb.SP3.gz", dest)
        self.assertFalse(dest.exists())


class MainTest(unittest.TestCase):
    """main() stops with a message and a nonzero status on a refused download."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        (self.repo / "CMakeLists.txt").write_text("")
        patcher = unittest.mock.patch.object(bs, "install_cddis_auth")
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self.tmp.cleanup)

    def run_main(self, fetch_all):
        out, err = io.StringIO(), io.StringIO()
        with unittest.mock.patch.object(bs, "fetch_all", fetch_all), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = bs.main(["--repo", str(self.repo)])
        return status, out.getvalue(), err.getvalue()

    def test_a_refused_download_stops_the_run(self):
        def refuse(*args, **kwargs):
            raise bs.DownloadContentError("refused https://example.org/x.gz: the response is an HTML page")
        status, out, err = self.run_main(refuse)
        self.assertEqual(status, 1)
        self.assertIn("refused https://example.org/x.gz", err)
        self.assertNotIn("Done.", out)

    def test_a_clean_run_reports_what_still_needs_supplying(self):
        status, out, err = self.run_main(lambda *a, **k: (["NARY 2023-321 -> data/obs/"], []))
        self.assertEqual(status, 0)
        self.assertIn("NARY 2023-321", err)
        self.assertIn("Done.", out)


class LooksLikeHtmlTest(unittest.TestCase):
    def test_html_pages(self):
        for head in (LOGIN_PAGE,
                     b"<!doctype HTML>\n<html>",
                     b"\xef\xbb\xbf  \r\n<HTML><BODY>x",
                     b"<?xml version='1.0'?>\n<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.0//EN'>\n<html>",
                     b"<head><title>Sign in</title></head>"):
            with self.subTest(head=head[:30]):
                self.assertTrue(bs.looks_like_html(head))

    def test_an_html_media_type_with_a_markup_body(self):
        self.assertTrue(bs.looks_like_html(b"<p>no tag from the marker list</p>", "text/html; charset=utf-8"))
        self.assertFalse(bs.looks_like_html(b"<p>no tag from the marker list</p>", "text/plain"))

    def test_product_files_are_not_html(self):
        for head in (b"          1.4            M                  ANTEX VERSION / SYST\n",
                     b"#dP2024  5  8  0  0  0.00000000     288 ORBIT IGS20 HLM  COD\n",
                     b"     3.04           OBSERVATION DATA    M                   RINEX VERSION / TYPE\n",
                     b"CRINEX VERS   / TYPE\n",
                     b"%ZTD grid 5x5\n",
                     b"CDF\x01\x00\x00\x00\x00",
                     b"\x89HDF\r\n\x1a\n",
                     GZ_BODY[:200],
                     b""):
            with self.subTest(head=head[:20]):
                self.assertFalse(bs.looks_like_html(head, "text/html"))   # not even on a wrong Content-Type

    def test_a_marker_not_at_the_start_does_not_count(self):
        self.assertFalse(bs.looks_like_html(b"COMMENT  see <html> in the manual\n"))


class RefusalReasonTest(unittest.TestCase):
    def test_none_for_an_acceptable_body(self):
        self.assertIsNone(bs.refusal_reason("orb.SP3.gz", GZ_BODY[:100]))
        self.assertIsNone(bs.refusal_reason("ORB.SP3.GZ", GZ_BODY[:100]))
        self.assertIsNone(bs.refusal_reason("igs20.atx", b"          1.4   M\n"))
        self.assertIsNone(bs.refusal_reason("EOT20_load.nc", b"CDF\x01"))

    def test_html_names_its_title(self):
        reason = bs.refusal_reason("orb.SP3.gz", LOGIN_PAGE, "text/html")
        self.assertIn("HTML page", reason)
        self.assertIn('"Earthdata Login"', reason)

    def test_html_without_a_title(self):
        self.assertEqual(bs.refusal_reason("a.atx", b"<html><body>x</body></html>"),
                         "the response is an HTML page")

    def test_a_gz_name_needs_the_magic_but_other_names_do_not(self):
        self.assertIn("not gzip data", bs.refusal_reason("a.clk.gz", b"%CLK\n"))
        self.assertIsNone(bs.refusal_reason("a.clk", b"%CLK\n"))

    def test_the_title_is_cut_to_one_short_line(self):
        head = b"<html><title>" + b"x\n  y " * 100 + b"</title></html>"
        reason = bs.refusal_reason("a.atx", head)
        self.assertNotIn("\n", reason)
        self.assertLess(len(reason), 140)


class PreemptiveBasicAuthTest(unittest.TestCase):
    """The handler on its own: which requests get the header, and that a redirect does not copy it."""

    HOST = bs.EARTHDATA_HOST

    def handler(self, scheme="https"):
        return bs.PreemptiveBasicAuth({self.HOST: (USER, PASSWORD)}, scheme)

    def header(self, url, scheme="https"):
        req = urllib.request.Request(url)
        handler = self.handler(scheme)
        process = handler.https_request if req.type == "https" else handler.http_request
        return process(req).get_header("Authorization")

    def test_the_value_is_basic_of_login_and_password(self):
        self.assertEqual(self.header(f"https://{self.HOST}/oauth/authorize"),
                         "Basic " + base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode())

    def test_non_ascii_credentials_are_utf8(self):
        handler = bs.PreemptiveBasicAuth({self.HOST: ("user", "päss")})
        req = handler.https_request(urllib.request.Request(f"https://{self.HOST}/"))
        self.assertEqual(base64.b64decode(req.get_header("Authorization")[6:]), "user:päss".encode("utf-8"))

    def test_the_host_match_is_exact_and_case_blind(self):
        self.assertIsNotNone(self.header(f"https://{self.HOST.upper()}/x"))
        self.assertIsNotNone(self.header(f"https://{self.HOST}:443/x"))                 # the default port
        for url in (f"https://{self.HOST}.evil.example/x",
                    f"https://evil{self.HOST}/x",
                    f"https://x.{self.HOST}/x",
                    f"https://{self.HOST}:8443/x",
                    f"https://{bs.CDDIS_HOST}/x",
                    f"https://files.igs.org/x"):
            with self.subTest(url=url):
                self.assertIsNone(self.header(url))

    def test_a_downgrade_to_http_gets_nothing(self):
        self.assertIsNone(self.header(f"http://{self.HOST}/x"))
        self.assertIsNotNone(self.header(f"http://{self.HOST}/x", scheme="http"))
        self.assertIsNone(self.header(f"https://{self.HOST}/x", scheme="http"))

    def test_an_authorization_already_present_is_left_alone(self):
        req = urllib.request.Request(f"https://{self.HOST}/x", headers={"Authorization": "Bearer t"})
        self.assertEqual(self.handler().https_request(req).get_header("Authorization"), "Bearer t")

    def test_the_header_is_not_copied_to_a_redirected_request(self):
        req = self.handler().https_request(urllib.request.Request(f"https://{self.HOST}/x"))
        self.assertIsNotNone(req.get_header("Authorization"))
        new = urllib.request.HTTPRedirectHandler().redirect_request(
            req, None, 302, "Found", {}, "https://elsewhere.example/y")
        self.assertIsNotNone(new)
        self.assertFalse(new.has_header("Authorization"))


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


class InstallCddisAuthTest(unittest.TestCase):
    """install_cddis_auth reads the Earthdata entry only, and offers it to that host only."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        saved = bs._OPENER
        bs._OPENER = None
        self.addCleanup(lambda: setattr(bs, "_OPENER", saved))

    def netrc(self, text):
        path = Path(self.tmp.name) / "netrc"
        path.write_text(text)
        os.chmod(path, 0o600)
        return path

    def install(self, path):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            bs.install_cddis_auth(path)
        return out.getvalue(), err.getvalue()

    def header_for(self, url):
        handler = next(h for h in bs._OPENER.handlers if isinstance(h, bs.PreemptiveBasicAuth))
        return handler.https_request(urllib.request.Request(url)).get_header("Authorization")

    def test_only_the_earthdata_host_is_offered_its_entry(self):
        path = self.netrc(f"machine {bs.EARTHDATA_HOST} login zq-login-1 password zq-secret-1\n"
                          f"machine {bs.CDDIS_HOST} login zq-login-2 password zq-secret-2\n")
        out, err = self.install(path)
        self.assertIsNotNone(bs._OPENER)
        self.assertEqual(self.header_for(f"https://{bs.EARTHDATA_HOST}/oauth/authorize"),
                         "Basic " + base64.b64encode(b"zq-login-1:zq-secret-1").decode())
        self.assertIsNone(self.header_for(f"https://{bs.CDDIS_HOST}/archive/x"))
        self.assertIn(bs.EARTHDATA_HOST, out)
        for secret in ("zq-login-1", "zq-secret-1", "zq-login-2", "zq-secret-2"):
            self.assertNotIn(secret, out + err)                                 # the credentials are never printed

    def test_a_cddis_entry_alone_installs_nothing_and_says_so(self):
        path = self.netrc(f"machine {bs.CDDIS_HOST} login zq-login-2 password zq-secret-2\n")
        out, err = self.install(path)
        self.assertIsNone(bs._OPENER)
        self.assertIn(f"no entry for {bs.EARTHDATA_HOST}", err)
        self.assertIn("not used", err)
        self.assertNotIn("zq-secret-2", out + err)

    def test_no_netrc_file_installs_nothing_and_says_so(self):
        out, err = self.install(Path(self.tmp.name) / "absent")
        self.assertIsNone(bs._OPENER)
        self.assertIn("no netrc file found", err)
        self.assertIn("refused", err)


if __name__ == "__main__":
    unittest.main()
