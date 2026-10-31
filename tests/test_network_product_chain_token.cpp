/**
 * @file test_network_product_chain_token.cpp
 * @brief The chain component of a satellite phase bias on the wire: the reader takes the
 *        P record's @c cmp= token, the writer does not emit it, and the consumer's wire
 *        context folds it per satellite.
 *
 * A truth wire carries @c cmp=1 on every P record; the producer writes none yet, so the
 * reader must take the token where it is and read its absence as 0.
 */

#include <gtest/gtest.h>

#include "network/solve/Sbasis.hpp"          // SbasisDeclaration::AmbDatum
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "realtime/ArObserve.hpp"
#include "support/TestTempDir.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

std::string tempFile(const char* name) { return (gnss_test::tempDir() / name).string(); }

std::string readAllBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const NetworkPhaseBias* findBias(const NetworkProductRecord& r, const std::string& sat,
                                 const std::string& signal) {
    for (const NetworkSatSolution& s : r.epoch.sats)
        if (s.sat == sat) {
            const auto it = s.phase_bias.find(signal);
            return it == s.phase_bias.end() ? nullptr : &it->second;
        }
    return nullptr;
}

// A wire in the shape of the truth wires of the integer-recovering datum: amb_datum 2 on the
// S line, and a component and a hold generation on the P records, in either order.
const char* kTruthShapedWire =
    "# gnss_network_product v2\n"
    "S,S30000SIM,1,nominal-wu93,2,0,1,0,0.05,1,SYNTH,G:G01,cbjump=1\n"
    "E,2313,261030.0000116825,0,0,0,0\n"
    "C,G05,0.0000000000\n"
    "B,G05,C1C,-1.6012510000\n"
    "P,G05,L1C,0.1945380000,1,1,0,gen=1,cmp=1\n"
    "P,G05,L2W,0.3285760000,1,1,0,gen=1,cmp=1\n"
    "C,G06,0.0000000000\n"
    "P,G06,L1C,-0.1868820000,1,1,0,cmp=2,gen=3\n"
    "P,G06,L2W,-0.3230050000,1,1,0\n"
    "E,2313,261060.0000116825,0,0,0,0\n"
    "C,G05,0.0000000000\n"
    "P,G05,L1C,0.1945380000,1,1,0,gen=1,cmp=7\n";

TEST(NetworkProductChainTokenTest, ReaderTakesTheComponentTokenWhereItIsAndReadsItsAbsenceAsZero) {
    const std::string path = tempFile("chain_token_wire.txt");
    { std::ofstream f(path, std::ios::binary); f << kTruthShapedWire; }
    SbasisContract contract;
    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &contract, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 2u);
    ASSERT_TRUE(contract.present);
    EXPECT_EQ(static_cast<int>(contract.decl.amb_datum), kWireAmbDatumChain);

    const NetworkPhaseBias* g05_l1 = findBias(recs[0], "G05", "L1C");
    const NetworkPhaseBias* g05_l2 = findBias(recs[0], "G05", "L2W");
    const NetworkPhaseBias* g06_l1 = findBias(recs[0], "G06", "L1C");
    const NetworkPhaseBias* g06_l2 = findBias(recs[0], "G06", "L2W");
    ASSERT_TRUE(g05_l1 && g05_l2 && g06_l1 && g06_l2);
    EXPECT_EQ(g05_l1->chain_component, 1u);
    EXPECT_EQ(g05_l2->chain_component, 1u);
    EXPECT_EQ(g06_l1->chain_component, 2u);             // before the generation token
    EXPECT_EQ(g06_l1->hold_generation, 3u);
    EXPECT_EQ(g05_l1->hold_generation, 1u);              // after... and the other order
    EXPECT_EQ(g06_l2->chain_component, 0u);              // no token: absent
    EXPECT_EQ(g06_l2->hold_generation, 0u);

    // The values around the token are the ones the line carries.
    EXPECT_NEAR(g05_l1->bias_m, 0.194538, 1e-9);
    EXPECT_TRUE(g05_l1->signal_integer);
    EXPECT_EQ(g05_l1->wide_lane_integer, 1);

    const NetworkPhaseBias* later = findBias(recs[1], "G05", "L1C");
    ASSERT_TRUE(later);
    EXPECT_EQ(later->chain_component, 7u);               // the second epoch is its own record
}

TEST(NetworkProductChainTokenTest, AWireWithoutTheTokenReadsEveryComponentAsAbsent) {
    const std::string path = tempFile("chain_token_none.txt");
    {
        std::ofstream f(path, std::ios::binary);
        f << "# gnss_network_product v2\n"
             "E,2313,261030.0,0,0,0,0\n"
             "C,G05,0.0\n"
             "P,G05,L1C,0.1,1,1,0,gen=2\n"
             "P,G05,L2W,0.2,1,1,0\n";
    }
    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    for (const char* sig : { "L1C", "L2W" }) {
        const NetworkPhaseBias* pb = findBias(recs[0], "G05", sig);
        ASSERT_TRUE(pb);
        EXPECT_EQ(pb->chain_component, 0u);
    }
}

TEST(NetworkProductChainTokenTest, TheWriterNeverEmitsTheComponent) {
    // Two epochs, identical but for the component the second carries on its phase biases: the
    // files are byte-identical, so the token is read-only on this side of the wire.
    auto epoch = [](std::uint32_t component) {
        NetworkSolutionEpoch sol;
        sol.epoch_time = GnssTime{ 2313, 261030.0 };
        sol.iod_ssr = 3;
        NetworkSatSolution g;
        g.sat = "G05";
        g.has_clock = true;
        g.clock_c0_m = 12.5;
        NetworkPhaseBias pb{ 0.25, true, 1 };
        pb.hold_generation = 4;
        pb.chain_component = component;
        g.phase_bias["L1C"] = pb;
        g.phase_bias["L2W"] = pb;
        sol.sats.push_back(g);
        return sol;
    };
    const std::string a = tempFile("chain_token_w0.txt"), b = tempFile("chain_token_w7.txt");
    {
        NetworkProductWriter w(a);
        ASSERT_TRUE(w.good());
        w.write(epoch(0));
    }
    {
        NetworkProductWriter w(b);
        ASSERT_TRUE(w.good());
        w.write(epoch(7));
    }
    const std::string bytes_a = readAllBytes(a), bytes_b = readAllBytes(b);
    EXPECT_EQ(bytes_a, bytes_b);
    EXPECT_EQ(bytes_b.find("cmp="), std::string::npos);
    EXPECT_NE(bytes_b.find("gen=4"), std::string::npos) << "the generation token is still written";

    // A round trip through the reader drops the component (it was never on the wire).
    bool ok = false;
    const auto recs = NetworkProductReader::read(b, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    const NetworkPhaseBias* pb = findBias(recs[0], "G05", "L1C");
    ASSERT_TRUE(pb);
    EXPECT_EQ(pb->chain_component, 0u);
    EXPECT_EQ(pb->hold_generation, 4u);
}

TEST(NetworkProductChainTokenTest, TheWireContextFoldsComponentsPerSatellite) {
    auto sat = [](const std::string& name,
                  const std::vector<std::pair<std::string, std::uint32_t>>& comps) {
        NetworkSatSolution s;
        s.sat = name;
        s.has_clock = true;
        for (const auto& [sig, c] : comps) {
            NetworkPhaseBias pb{ 0.1, true, 1 };
            pb.chain_component = c;
            s.phase_bias[sig] = pb;
        }
        return s;
    };
    WireChainContext ctx;
    ctx.amb_datum = kWireAmbDatumChain;

    NetworkSolutionEpoch e1;
    e1.sats.push_back(sat("G05", { {"L1C", 1}, {"L2W", 1} }));
    e1.sats.push_back(sat("G06", { {"L1C", 2}, {"L2W", 0} }));       // a record without a token adds nothing
    absorbWireChain(ctx, e1);
    EXPECT_EQ(ctx.cmp.size(), 3u);
    EXPECT_EQ(ctx.cmp.at("G05|L1C"), 1u);
    EXPECT_EQ(ctx.cmp.at("G05|L2W"), 1u);
    EXPECT_EQ(ctx.cmp.at("G06|L1C"), 2u);
    EXPECT_EQ(ctx.cmp.count("G06|L2W"), 0u);

    // The next epoch replaces a satellite's entries wholesale: G05 moves component and loses a
    // band; G06 is not in the epoch and keeps what it had.
    NetworkSolutionEpoch e2;
    e2.sats.push_back(sat("G05", { {"L1C", 3} }));
    absorbWireChain(ctx, e2);
    EXPECT_EQ(ctx.cmp.at("G05|L1C"), 3u);
    EXPECT_EQ(ctx.cmp.count("G05|L2W"), 0u);
    EXPECT_EQ(ctx.cmp.at("G06|L1C"), 2u);

    // A satellite that appears with a clock only has no phase biases to replace.
    NetworkSolutionEpoch e3;
    NetworkSatSolution clock_only;
    clock_only.sat = "G06";
    clock_only.has_clock = true;
    e3.sats.push_back(clock_only);
    absorbWireChain(ctx, e3);
    EXPECT_EQ(ctx.cmp.at("G06|L1C"), 2u);

    // A satellite whose biases come back without a token loses its entries.
    NetworkSolutionEpoch e4;
    e4.sats.push_back(sat("G06", { {"L1C", 0} }));
    absorbWireChain(ctx, e4);
    EXPECT_EQ(ctx.cmp.count("G06|L1C"), 0u);
    EXPECT_EQ(ctx.amb_datum, kWireAmbDatumChain) << "the datum is not the epochs' to change";
}

TEST(NetworkProductChainTokenTest, TheChainDatumConstantIsTheWireEnumerator) {
    EXPECT_EQ(kWireAmbDatumChain,
              static_cast<int>(SbasisDeclaration::AmbDatum::SingleReceiverChain));
}

} // namespace
