/**
 * @file test_path_resolver_bias_file.cpp
 * @brief `files.use_bias_file`: PathResolver returns the observation day's `.bia` / `.bsx` file by
 *        default and none with the key false; a bias file named in `files.product_list_path`
 *        together with false is refused by the loader.
 *
 * Each case builds a small internal data tree in the test's temp directory
 * (`<root>/2024-129/products/` with an OSB `.BIA`, a `.BSX`, an orbit and a clock file, and
 * `<root>/static/atx/`), loads a configuration pointing at it and runs PathResolver::resolve.
 *
 * Covered:
 *   - the default (key absent) returns the `.BIA` and the `.BSX` with the other products;
 *   - false returns neither and every other product unchanged, in the same order;
 *   - PathResolver::isBiasFile: `.bia` and `.bsx` in any case, nothing else;
 *   - a `.BIA` named in `files.product_list_path` with the key false is refused; the same list
 *     with the key at its default loads, and a list without a bias file loads with the key false.
 * Not covered here: the production mode's removal from the downloader's list (it needs a
 * download); the code is the same isBiasFile test applied to that list.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/PathResolver.hpp"
#include "support/TestTempDir.hpp"   // per-process, per-test scratch directory

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

/// An internal data tree for 2024-129 with two bias files, an orbit, a clock and an antenna file.
fs::path makeDataTree() {
    const fs::path root = gnss_test::tempDir() / "data";
    const fs::path products = root / "2024-129" / "products";
    fs::create_directories(products);
    fs::create_directories(root / "static" / "atx");
    for (const fs::path& f : { products / "COD0MGXFIN_20241290000_01D_01D_OSB.BIA",
                               products / "COD0MGXFIN_20241290000_01D_01D_OSB.BSX",
                               products / "COD0MGXFIN_20241290000_01D_05M_ORB.SP3",
                               products / "COD0MGXFIN_20241290000_01D_30S_CLK.CLK",
                               root / "static" / "atx" / "igs20.atx" }) {
        std::ofstream(f) << "placeholder\n";
    }
    return root;
}

/// Load a configuration on @p root; @p files_extra is added to the files object (starting with a
/// comma).  Returns loadFromJson's result.
bool loadOn(const fs::path& root, const std::string& files_extra, GnssConfig& cfg, LogManager& log) {
    const fs::path path = gnss_test::tempDir() / "config.json";
    std::string root_text = root.generic_string();
    {
        std::ofstream out(path);
        out << R"({
            "data_source": "internal",
            "files": {"internal_data_root": ")" << root_text << R"(")" << files_extra << R"(},
            "data_download": {"enabled": false, "year": 2024, "doy": 129},
            "processing": {"mode": "ppp"}
        })";
    }
    return cfg.loadFromJson(path, log);
}

/// The products PathResolver returns for @p cfg, by file name.
std::vector<std::string> resolvedNames(const GnssConfig& cfg, LogManager& log) {
    PathResolver resolver(cfg, log);
    PathResolver::ResolvedPaths resolved;
    EXPECT_TRUE(resolver.resolve(resolved));
    std::vector<std::string> names;
    for (const fs::path& p : resolved.product_paths) names.push_back(p.filename().string());
    return names;
}

bool hasName(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace

TEST(PathResolverBiasFile, TheDefaultReturnsTheBiaAndTheBsxWithTheOtherProducts) {
    const fs::path root = makeDataTree();
    LogManager log;
    GnssConfig cfg;
    ASSERT_TRUE(loadOn(root, "", cfg, log));
    EXPECT_TRUE(cfg.useBiasFile());
    const std::vector<std::string> names = resolvedNames(cfg, log);
    EXPECT_TRUE(hasName(names, "COD0MGXFIN_20241290000_01D_01D_OSB.BIA"));
    EXPECT_TRUE(hasName(names, "COD0MGXFIN_20241290000_01D_01D_OSB.BSX"));
    EXPECT_TRUE(hasName(names, "COD0MGXFIN_20241290000_01D_05M_ORB.SP3"));
    EXPECT_TRUE(hasName(names, "COD0MGXFIN_20241290000_01D_30S_CLK.CLK"));
    EXPECT_TRUE(hasName(names, "igs20.atx"));
}

TEST(PathResolverBiasFile, FalseReturnsNoBiasFileAndEveryOtherProductInTheSameOrder) {
    const fs::path root = makeDataTree();
    LogManager log;

    GnssConfig with_file;
    ASSERT_TRUE(loadOn(root, "", with_file, log));
    std::vector<std::string> expected = resolvedNames(with_file, log);
    expected.erase(std::remove_if(expected.begin(), expected.end(),
                                  [](const std::string& n) { return PathResolver::isBiasFile(n); }),
                   expected.end());

    GnssConfig without_file;
    ASSERT_TRUE(loadOn(root, R"(, "use_bias_file": false)", without_file, log));
    EXPECT_FALSE(without_file.useBiasFile());
    const std::vector<std::string> names = resolvedNames(without_file, log);
    for (const std::string& n : names) EXPECT_FALSE(PathResolver::isBiasFile(n)) << n;
    EXPECT_EQ(names, expected);
    EXPECT_EQ(names.size(), 3u);   // orbit, clock, antenna
}

TEST(PathResolverBiasFile, IsBiasFileTakesBiaAndBsxInAnyCaseAndNothingElse) {
    EXPECT_TRUE(PathResolver::isBiasFile("COD0MGXFIN_20241290000_01D_01D_OSB.BIA"));
    EXPECT_TRUE(PathResolver::isBiasFile("x.bia"));
    EXPECT_TRUE(PathResolver::isBiasFile("x.Bsx"));
    EXPECT_TRUE(PathResolver::isBiasFile("dir/x.BSX"));
    EXPECT_FALSE(PathResolver::isBiasFile("x.SP3"));
    EXPECT_FALSE(PathResolver::isBiasFile("x.bias"));
    EXPECT_FALSE(PathResolver::isBiasFile("x.BIA.gz"));
    EXPECT_FALSE(PathResolver::isBiasFile("bia"));
}

TEST(PathResolverBiasFile, ABiasFileInTheProductListWithTheKeyFalseIsRefused) {
    const fs::path root = makeDataTree();
    const fs::path products = root / "2024-129" / "products";
    const fs::path with_bias = gnss_test::tempDir() / "products_with_bias.txt";
    const fs::path without_bias = gnss_test::tempDir() / "products_without_bias.txt";
    std::ofstream(with_bias) << (products / "COD0MGXFIN_20241290000_01D_05M_ORB.SP3").generic_string() << "\n"
                             << (products / "COD0MGXFIN_20241290000_01D_01D_OSB.BIA").generic_string() << "\n";
    std::ofstream(without_bias) << (products / "COD0MGXFIN_20241290000_01D_05M_ORB.SP3").generic_string() << "\n";
    const auto list = [](const fs::path& p) {
        return std::string(R"(, "product_list_path": ")") + p.generic_string() + "\"";
    };
    LogManager log;

    GnssConfig refused;
    EXPECT_FALSE(loadOn(root, list(with_bias) + R"(, "use_bias_file": false)", refused, log));

    GnssConfig default_key;
    EXPECT_TRUE(loadOn(root, list(with_bias), default_key, log));

    GnssConfig no_bias_in_list;
    EXPECT_TRUE(loadOn(root, list(without_bias) + R"(, "use_bias_file": false)", no_bias_in_list, log));
}
