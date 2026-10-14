/**
 * @file test_gnss_identity_user.cpp
 * @brief The user-side helpers of common/GnssIdentity.hpp (the section
 *        "User-side helpers").
 *
 * Each helper replaces an idiom written by hand at user-side call sites and claims to equal
 * it for every input, since the migration of those sites claims identical output.  Each
 * helper is therefore tested twice: against a frozen copy of the idiom as the sites write it,
 * over every string of up to four characters drawn from the characters the idiom reads (so
 * the empty string, a separator first or last and two separators are all covered), and then
 * value by value on the cases its documentation names.  Where two sites wrote one idea two
 * ways, both idioms are frozen here and the inputs on which they differ are pinned.
 */

#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

    using namespace GnssProcessor;

    /**
     * @brief Every string of length 0 to @p max_len over @p alphabet.
     * @param alphabet The characters to draw from.
     * @param max_len  The longest string generated.
     * @return The strings, shortest first.
     */
    std::vector<std::string> allStrings(const std::string& alphabet, std::size_t max_len) {
        std::vector<std::string> out{std::string()};
        std::size_t begin = 0;
        for (std::size_t len = 1; len <= max_len; ++len) {
            const std::size_t end = out.size();
            for (std::size_t i = begin; i < end; ++i)
                for (char c : alphabet) out.push_back(out[i] + c);
            begin = end;
        }
        return out;
    }

    /// Signal-label corpus: every string of up to four characters over a label's characters
    /// and the two other pair separators ('+' of the DD row label, '/' of the aliased code).
    const std::vector<std::string>& labelCorpus() {
        static const std::vector<std::string> k = [] {
            std::vector<std::string> v = allStrings("L1_+/", 4);
            for (const char* s : {"L1C", "C1C", "L1C_L2W", "C1C_C2W", "L1C_L2W_L5Q", "L1C__L2W",
                                  "_L2W", "L1C_", "L1C+L2W", "L2W/L2C", "G01_L1C_L2W"})
                v.emplace_back(s);
            return v;
        }();
        return k;
    }

    // -----------------------------------------------------------------------
    // Frozen idioms (as the user-side sites write them at ff7766a)
    // -----------------------------------------------------------------------

    /// The predicate, e.g. KalmanPostFitQc.cpp "if (sig.find('_') != std::string::npos)".
    bool idiomIsIfLabel(const std::string& label) { return label.find('_') != std::string::npos; }

    /// The strict split of IfPhaseSlipDetector::splitIfLabel and MwSlipDetector::splitIfLabel
    /// (MeasurementModelRow.cpp's ifCombHasFullBia and ifCombHasFullRxAtx write the same).
    bool idiomSplitIfLabel(const std::string& label, std::string& sig1, std::string& sig2) {
        sig1.clear();
        sig2.clear();
        const size_t pos = label.find('_');
        if (pos == std::string::npos || pos == 0 || pos + 1 >= label.size()) return false;
        sig1 = label.substr(0, pos);
        sig2 = label.substr(pos + 1);
        return (!sig1.empty() && !sig2.empty());
    }

    /// The strict predicate of MeasurementModel::phaseBiasCoveredForAr.
    bool idiomStrictIsIf(const std::string& signal) {
        const size_t us = signal.find('_');
        return (us != std::string::npos && us > 0 && us + 1 < signal.size());
    }

    /// Row-id corpus: every string of up to four characters over a satellite id's characters
    /// and the pair separator, plus named ids.
    const std::vector<std::string>& rowIdCorpus() {
        static const std::vector<std::string> k = [] {
            std::vector<std::string> v = allStrings("G1-", 4);
            for (const char* s : {"G01", "G01-G05", "-G05", "G01-", "G01-G05-G07", "G01--G05",
                                  "G 5-G03", "E11-E02", "C01-C05"})
                v.emplace_back(s);
            return v;
        }();
        return k;
    }

    /// kalman::splitBssdSatId's body before it forwarded to SatPairId::parse (KalmanRowIdentity.hpp).
    std::pair<std::string, std::string> idiomSplitBssdSatId(const std::string& sat_id) {
        const size_t dash = sat_id.find('-');
        if (dash != std::string::npos && dash > 0 && dash + 1 < sat_id.size())
            return { sat_id.substr(0, dash), sat_id.substr(dash + 1) };
        return { sat_id, "" };
    }

    /// The trace writer's primary (TraceLogger.cpp, three sites).
    std::string idiomPrimaryBeforeDash(const std::string& sat_full) {
        const auto dash = sat_full.find('-');
        return (dash == std::string::npos) ? sat_full : sat_full.substr(0, dash);
    }

    /// The baseline DD statistics' pivot (ApplicationBaseline.cpp): the pivot when the
    /// condition holds, std::nullopt where the site skips the row.
    std::optional<std::string> idiomPivotAfterDash(const std::string& sid) {
        const auto dash = sid.find('-');
        if (dash != std::string::npos && dash + 1 < sid.size()) return sid.substr(dash + 1);
        return std::nullopt;
    }

    /// The real-time engine's difference-row skip (RealTimeEngineEstimate.cpp,
    /// RealTimeEngineSsrDiag.cpp).
    bool idiomHasDash(const std::string& sat) { return sat.find('-') != std::string::npos; }

} // namespace

// ===========================================================================
// The ionosphere-free pair label
// ===========================================================================

TEST(GnssIdentityUserTest, IonoFreePairLabelPredicateEqualsTheFindIdiom) {
    for (const std::string& s : labelCorpus())
        EXPECT_EQ(isIonoFreePairLabel(s), idiomIsIfLabel(s)) << '"' << s << '"';
}

TEST(GnssIdentityUserTest, IonoFreePairLabelPredicateValues) {
    static_assert(isIonoFreePairLabel("L1C_L2W"), "constexpr: a pair label");
    static_assert(!isIonoFreePairLabel("L1C"), "constexpr: a single signal");
    EXPECT_FALSE(isIonoFreePairLabel(""));
    EXPECT_FALSE(isIonoFreePairLabel("L1C"));
    EXPECT_TRUE(isIonoFreePairLabel("L1C_L2W"));
    EXPECT_TRUE(isIonoFreePairLabel("L1C_L2W_L5Q"));   // two separators
    EXPECT_TRUE(isIonoFreePairLabel("_"));
    EXPECT_TRUE(isIonoFreePairLabel("_L2W"));
    EXPECT_TRUE(isIonoFreePairLabel("L1C_"));
    EXPECT_FALSE(isIonoFreePairLabel("L1C+L2W"));      // the DD row's IF label
    EXPECT_FALSE(isIonoFreePairLabel("L2W/L2C"));      // an aliased DD code
}

TEST(GnssIdentityUserTest, IonoFreePairLabelParseEqualsTheSplitIdiom) {
    for (const std::string& s : labelCorpus()) {
        std::string sig1, sig2;
        const bool ok = idiomSplitIfLabel(s, sig1, sig2);
        const std::optional<IonoFreePairLabel> p = IonoFreePairLabel::parse(s);
        ASSERT_EQ(p.has_value(), ok) << '"' << s << '"';
        EXPECT_EQ(p.has_value(), idiomStrictIsIf(s)) << '"' << s << '"';
        if (p) {
            EXPECT_EQ(p->first, sig1) << '"' << s << '"';
            EXPECT_EQ(p->second, sig2) << '"' << s << '"';
        }
    }
}

TEST(GnssIdentityUserTest, IonoFreePairLabelParseValues) {
    const auto p = IonoFreePairLabel::parse("L1C_L2W");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->first, "L1C");
    EXPECT_EQ(p->second, "L2W");

    const auto two = IonoFreePairLabel::parse("L1C_L2W_L5Q");   // two separators
    ASSERT_TRUE(two.has_value());
    EXPECT_EQ(two->first, "L1C");
    EXPECT_EQ(two->second, "L2W_L5Q");

    const auto dbl = IonoFreePairLabel::parse("L1C__L2W");
    ASSERT_TRUE(dbl.has_value());
    EXPECT_EQ(dbl->first, "L1C");
    EXPECT_EQ(dbl->second, "_L2W");

    for (const char* refused : {"", "L1C", "_", "__", "_L2W", "L1C_", "L1C+L2W"})
        EXPECT_FALSE(IonoFreePairLabel::parse(refused).has_value()) << '"' << refused << '"';
}

TEST(GnssIdentityUserTest, IonoFreePairLabelPredicateAndParseDifferOnlyAtTheEnds) {
    // A label the strict parse accepts always holds a '_' ...
    for (const std::string& s : labelCorpus())
        if (IonoFreePairLabel::parse(s)) EXPECT_TRUE(isIonoFreePairLabel(s)) << '"' << s << '"';
    // ... and the two readings part only where the first '_' is first or last.
    for (const char* s : {"_", "__", "_L2W", "L1C_", "_L1C_L2W"}) {
        EXPECT_TRUE(isIonoFreePairLabel(s)) << s;
        EXPECT_FALSE(IonoFreePairLabel::parse(s).has_value()) << s;
    }
}

// ===========================================================================
// The between-satellite difference row id
// ===========================================================================

TEST(GnssIdentityUserTest, SatPairIdParseEqualsSplitBssdSatId) {
    for (const std::string& s : rowIdCorpus()) {
        const auto [prim, ref] = idiomSplitBssdSatId(s);
        const SatPairId p = SatPairId::parse(s);
        EXPECT_EQ(p.primary, prim) << '"' << s << '"';
        EXPECT_EQ(p.reference, ref) << '"' << s << '"';
        const size_t dash = s.find('-');
        EXPECT_EQ(p.isPair(), dash != std::string::npos && dash > 0 && dash + 1 < s.size())
            << '"' << s << '"';
    }
}

TEST(GnssIdentityUserTest, SatPairIdParseValues) {
    const auto [prim, ref] = SatPairId::parse("G01-G05");   // structured bindings, as the sites read it
    EXPECT_EQ(prim, "G01");
    EXPECT_EQ(ref, "G05");
    EXPECT_TRUE(SatPairId::parse("G01-G05").isPair());

    struct Case { const char* id; const char* primary; const char* reference; };
    for (const Case& c : {Case{"G01", "G01", ""},
                          Case{"", "", ""},
                          Case{"-G05", "-G05", ""},           // '-' first: not a pair
                          Case{"G01-", "G01-", ""},           // '-' last: not a pair
                          Case{"-", "-", ""},
                          Case{"G01-G05-G07", "G01", "G05-G07"},
                          Case{"G01--G05", "G01", "-G05"}}) {
        const SatPairId p = SatPairId::parse(c.id);
        EXPECT_EQ(p.primary, c.primary) << c.id;
        EXPECT_EQ(p.reference, c.reference) << c.id;
        EXPECT_EQ(p.isPair(), !std::string(c.reference).empty()) << c.id;
    }
}

TEST(GnssIdentityUserTest, SatPairIdSplitAtFirstDashEqualsTheLooseIdioms) {
    for (const std::string& s : rowIdCorpus()) {
        const SatPairId p = SatPairId::splitAtFirstDash(s);
        EXPECT_EQ(p.primary, idiomPrimaryBeforeDash(s)) << '"' << s << '"';
        const std::optional<std::string> pivot = idiomPivotAfterDash(s);
        EXPECT_EQ(!p.reference.empty(), pivot.has_value()) << '"' << s << '"';
        if (pivot) EXPECT_EQ(p.reference, *pivot) << '"' << s << '"';
    }
}

TEST(GnssIdentityUserTest, SatPairIdTheTwoReadingsDifferOnlyAtTheEnds) {
    for (const std::string& s : rowIdCorpus()) {
        const size_t dash = s.find('-');
        const bool at_an_end = dash != std::string::npos && (dash == 0 || dash + 1 == s.size());
        if (at_an_end) continue;
        const SatPairId strict = SatPairId::parse(s);
        const SatPairId loose  = SatPairId::splitAtFirstDash(s);
        EXPECT_EQ(loose.primary, strict.primary) << '"' << s << '"';
        EXPECT_EQ(loose.reference, strict.reference) << '"' << s << '"';
    }
    struct Case { const char* id; const char* primary; const char* reference; };
    for (const Case& c : {Case{"-G05", "", "G05"}, Case{"G01-", "G01", ""}, Case{"-", "", ""},
                          Case{"G01", "G01", ""}, Case{"", "", ""}}) {
        const SatPairId p = SatPairId::splitAtFirstDash(c.id);
        EXPECT_EQ(p.primary, c.primary) << c.id;
        EXPECT_EQ(p.reference, c.reference) << c.id;
    }
}

TEST(GnssIdentityUserTest, HasSatPairSeparatorEqualsTheFindIdiom) {
    static_assert(hasSatPairSeparator("G01-G05"), "constexpr: a pair row");
    static_assert(!hasSatPairSeparator("G01"), "constexpr: an undifferenced row");
    for (const std::string& s : rowIdCorpus())
        EXPECT_EQ(hasSatPairSeparator(s), idiomHasDash(s)) << '"' << s << '"';
    // Looser than parse(...).isPair(): true where the '-' is first or last.
    for (const char* s : {"-G05", "G01-", "-"}) {
        EXPECT_TRUE(hasSatPairSeparator(s)) << s;
        EXPECT_FALSE(SatPairId::parse(s).isPair()) << s;
    }
    EXPECT_FALSE(hasSatPairSeparator(""));
}

TEST(GnssIdentityUserTest, SatPairIdStrEqualsTheBuilderJoin) {
    const std::vector<std::string> members = allStrings("G1-", 2);
    for (const std::string& a : members)
        for (const std::string& b : members) {
            const SatPairId p{a, b};
            EXPECT_EQ(p.str(), a + "-" + b) << '"' << a << "\" \"" << b << '"';
            if (!a.empty() && !b.empty() && a.find('-') == std::string::npos) {
                const SatPairId back = SatPairId::parse(p.str());
                EXPECT_EQ(back.primary, a) << '"' << a << "\" \"" << b << '"';
                EXPECT_EQ(back.reference, b) << '"' << a << "\" \"" << b << '"';
            }
        }
    EXPECT_EQ((SatPairId{"G01", "G05"}.str()), "G01-G05");
    EXPECT_EQ((SatPairId{"G01", ""}.str()), "G01-");      // joined whatever the members hold
}
