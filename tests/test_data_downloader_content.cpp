/**
 * @file test_data_downloader_content.cpp
 * @brief DataDownloader content check: a login page or a non-gzip ".gz" is never a product.
 *
 * The Earthdata login answers an unauthenticated request with its HTML page at HTTP 200,
 * and zlib's gzread copies a file that is not gzip data through unchanged. Without a
 * content check the page was saved as the ".gz", reused as cached on every later run, and
 * written out as the ".SP3" or ".CLK" product. The cases below pin the rule
 * (DataDownloader::contentRefusal, the rule of refusal_reason in
 * scripts/bootstrap_internal_data.py) and its four uses: a fresh transfer, a cached
 * download, a gz input and a decompressed output already on disk. The transfers use
 * file:// addresses, so no network is needed; the positive transfer case proves the
 * libcurl build serves them.
 */

#include <gtest/gtest.h>

#include "logging/LogManager.hpp"
#include "products/DataDownloader.hpp"

#include <zlib.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

const std::string kLoginPage =
    "<!DOCTYPE html>\n<html><head><title>Earthdata Login</title></head>\n"
    "<body><form action=\"/login\"></form></body></html>\n";
const std::string kSp3Body = "#dP2024  5  8  0  0  0.00000000      96 ORBIT IGS20 FIT  COD\n";

/// A fresh scratch directory under the system temp dir, removed on destruction.
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path()
             / ("gnss_dl_content_" + std::to_string(counter.fetch_add(1)) + "_"
                + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void writeGzip(const fs::path& p, const std::string& bytes) {
    gzFile g = gzopen(p.string().c_str(), "wb");
    ASSERT_NE(g, nullptr);
    gzwrite(g, bytes.data(), static_cast<unsigned>(bytes.size()));
    gzclose(g);
}

/// file:// address of a local path (forward slashes; a drive letter keeps its colon).
std::string fileUrl(const fs::path& p) {
    std::string s = fs::absolute(p).generic_string();
    return (s.size() > 1 && s[1] == ':') ? "file:///" + s : "file://" + s;
}

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

TEST(DataDownloaderContent, LoginPageIsRefusedWithItsTitle) {
    const std::string reason = DataDownloader::contentRefusal("COD0MGXFIN.SP3.gz", kLoginPage);
    EXPECT_EQ(reason, "the response is an HTML page titled \"Earthdata Login\"");
    EXPECT_FALSE(DataDownloader::contentRefusal("igs20.atx", kLoginPage).empty());
}

TEST(DataDownloaderContent, HtmlNeedsALeadingAngleBracket) {
    // Leading blanks and a UTF-8 BOM are skipped; a marker anywhere after them counts.
    EXPECT_FALSE(DataDownloader::contentRefusal("a.SP3", "\xEF\xBB\xBF \r\n<html>").empty());
    // A body that does not start with '<' is never HTML, whatever its Content-Type.
    EXPECT_TRUE(DataDownloader::contentRefusal("a.SP3", kSp3Body, "text/html").empty());
    // '<' without a marker is HTML only on an HTML media type.
    EXPECT_TRUE(DataDownloader::contentRefusal("a.xml", "<?xml version=\"1.0\"?>").empty());
    EXPECT_FALSE(DataDownloader::contentRefusal("a.xml", "<p>sign in</p>",
                                                "Text/HTML; charset=utf-8").empty());
    EXPECT_FALSE(DataDownloader::contentRefusal("a.xml", "<p>x</p>",
                                                "application/xhtml+xml").empty());
}

TEST(DataDownloaderContent, GzNameNeedsTheGzipMagic) {
    EXPECT_TRUE(DataDownloader::contentRefusal("a.SP3.gz", std::string("\x1f\x8b\x08\x00", 4)).empty());
    EXPECT_EQ(DataDownloader::contentRefusal("a.SP3.GZ", "#dP2024"),
              "the file is a .gz but the response is not gzip data "
              "(it begins with bytes 23 64 50 32)");
    EXPECT_EQ(DataDownloader::contentRefusal("a.gz", ""),
              "the file is a .gz but the response is not gzip data (the body is empty)");
    // A plain product name is not held to the magic.
    EXPECT_TRUE(DataDownloader::contentRefusal("a.SP3", kSp3Body).empty());
}

// ---------------------------------------------------------------------------
// Its uses
// ---------------------------------------------------------------------------

TEST(DataDownloaderContent, TransferOfALoginPageFailsAndLeavesNothing) {
    ScratchDir dir;
    const fs::path page = dir.path / "login.html";
    const fs::path good = dir.path / "src.SP3.gz";
    writeFile(page, kLoginPage);
    writeGzip(good, kSp3Body);
    LogManager log;
    DataDownloader dl(log, dir.path / "no_netrc");

    // The positive case first: file:// transfers work in this libcurl build.
    const fs::path ok_dest = dir.path / "out" / "ok.SP3.gz";
    ASSERT_TRUE(dl.download(fileUrl(good), ok_dest));
    EXPECT_EQ(readFile(ok_dest), readFile(good));

    const fs::path gz_dest = dir.path / "out" / "COD0MGXFIN.SP3.gz";
    EXPECT_FALSE(dl.download(fileUrl(page), gz_dest));
    EXPECT_FALSE(fs::exists(gz_dest));

    const fs::path atx_dest = dir.path / "out" / "igs20.atx";
    EXPECT_FALSE(dl.download(fileUrl(page), atx_dest));
    EXPECT_FALSE(fs::exists(atx_dest));
}

TEST(DataDownloaderContent, CachedLoginPageIsDiscardedAndFetchedAgain) {
    ScratchDir dir;
    const fs::path good = dir.path / "src.SP3.gz";
    writeGzip(good, kSp3Body);
    const fs::path dest = dir.path / "out" / "COD0MGXFIN.SP3.gz";
    fs::create_directories(dest.parent_path());
    writeFile(dest, kLoginPage);
    LogManager log;
    DataDownloader dl(log, dir.path / "no_netrc");

    ASSERT_TRUE(dl.download(fileUrl(good), dest));
    EXPECT_EQ(readFile(dest), readFile(good));

    // A valid cached file is kept without a transfer (the address does not exist).
    EXPECT_TRUE(dl.download(fileUrl(dir.path / "missing.gz"), dest));
}

TEST(DataDownloaderContent, DecompressRefusesANonGzipInput) {
    ScratchDir dir;
    const fs::path gz = dir.path / "COD0MGXFIN.SP3.gz";
    const fs::path out = dir.path / "COD0MGXFIN.SP3";
    writeFile(gz, kLoginPage);
    LogManager log;
    DataDownloader dl(log);

    EXPECT_FALSE(dl.decompressGz(gz, out));
    EXPECT_FALSE(fs::exists(out));

    writeGzip(gz, kSp3Body);
    ASSERT_TRUE(dl.decompressGz(gz, out));
    EXPECT_EQ(readFile(out), kSp3Body);
}

TEST(DataDownloaderContent, DecompressedLoginPageIsReplaced) {
    ScratchDir dir;
    const fs::path gz = dir.path / "COD0MGXFIN.SP3.gz";
    const fs::path out = dir.path / "COD0MGXFIN.SP3";
    writeGzip(gz, kSp3Body);
    writeFile(out, kLoginPage);
    LogManager log;
    DataDownloader dl(log);

    ASSERT_TRUE(dl.decompressGz(gz, out));
    EXPECT_EQ(readFile(out), kSp3Body);
}

TEST(DataDownloaderContent, DownloadAndDecompressReplacesASavedLoginPage) {
    ScratchDir dir;
    const fs::path src = dir.path / "src" / "COD0MGXFIN.SP3.gz";
    fs::create_directories(src.parent_path());
    writeGzip(src, kSp3Body);
    const fs::path zip_dir = dir.path / "zip";
    const fs::path unzip_dir = dir.path / "unzip";
    fs::create_directories(unzip_dir);
    writeFile(unzip_dir / "COD0MGXFIN.SP3", kLoginPage);
    LogManager log;
    DataDownloader dl(log, dir.path / "no_netrc");

    const fs::path got = dl.downloadAndDecompress(fileUrl(src), zip_dir, unzip_dir);
    ASSERT_EQ(got, unzip_dir / "COD0MGXFIN.SP3");
    EXPECT_EQ(readFile(got), kSp3Body);
}

} // namespace
} // namespace GnssProcessor
