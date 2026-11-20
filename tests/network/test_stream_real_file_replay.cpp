/**
 * @file test_stream_real_file_replay.cpp
 * @brief Whole product files written by real network runs, replayed end to end the way every
 *        file user replays them.
 *
 * Every user replay of the network's correction stream reads @c products.txt with
 * @c NetworkProductReader::read and pushes each epoch into its own store through
 * @c NetworkProductReader::replay and a replay emitter built from the file's S line.  The
 * synthetic suites (StreamCounterRoundTrip, StreamLegendTokens, StreamFlagRoundTrip) cover
 * single epochs; this suite runs the same path over whole days of real files and checks four
 * properties on each file:
 *
 *  - **P1, counter disagreements by class.**  The replay emitter recomputes the three
 *    discontinuity counters from the file's values and marks.  Where the file carries its
 *    own counters (@c network.serialize_disc_counters), each value the recompute gets wrong
 *    is recorded and attributed to the producer setting the replay emitter lacks: a second
 *    emitter fed the same epochs adds, one at a time and in a fixed order, the settings the
 *    network run applied at setup and that the S line does not carry (the unsettled steps
 *    with their unheld form, the step-class declaration, the phase-family member table, the
 *    continuity guard; read from the run's @c arm.json beside the file).  A value is of the
 *    class of the first setting whose addition makes it agree; a value no setting explains
 *    is of class "other" (on the real files these are the chain's declarations at a
 *    re-host, which no S token or setting carries; the case requires every one of them to
 *    be a step of the file, never of the replay).  Each emitter takes the file's counters
 *    after every epoch, so each epoch is judged on its own steps.
 *  - **P2, text round trip.**  After each epoch's replay, @c NetworkProductWriter::write
 *    re-writes the parsed epoch with the replay emitter as the counter source; every data
 *    line (E, C, B, P, R, U, G, I) must equal the original.
 *  - **P3, the tail reader equals the batch reader.**  The file is appended to a temporary
 *    file in fixed chunk sizes that cut lines and epochs, @c NetworkProductTail::pollInto
 *    runs after each chunk, and the store after each poll must equal the batch store after
 *    the same number of epochs.
 *  - **P4, a store hash list.**  A text dump of the store after each epoch (clock, URA,
 *    code biases and counters, phase biases with flags, counters, sigmas and hold
 *    generation, declared level steps, generation id) is hashed; the list is the identity
 *    reference for any change of the reader or the emitter.
 *
 * The suite is gated: it reads a ';'-separated list of product files from the environment
 * variable @c GNSS_STREAM_FILES and skips with a message when it is unset.  Evidence (the P1
 * CSV and summary, the P4 hash list) goes to @c GNSS_STREAM_OUT when set, else to the test's
 * temporary folder.  A file of 50 MB takes tens of seconds per property in a Release build
 * of gnss_tests and much longer in Debug.
 */

#include <gtest/gtest.h>

#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include "../support/TestTempDir.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// Inputs and outputs
// ---------------------------------------------------------------------------------------------

/// The product files named by GNSS_STREAM_FILES (';'-separated); empty when unset.
std::vector<fs::path> streamFiles() {
    std::vector<fs::path> out;
    const char* v = std::getenv("GNSS_STREAM_FILES");
    if (!v) return out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ';'))
        if (!item.empty()) out.emplace_back(item);
    return out;
}

/// Where evidence is written: GNSS_STREAM_OUT, else this test's temporary folder.
fs::path outDir() {
    if (const char* v = std::getenv("GNSS_STREAM_OUT")) {
        fs::create_directories(v);
        return fs::path(v);
    }
    return gnss_test::tempDir();
}

/// A file's label: the name of the run folder that holds it.
std::string labelOf(const fs::path& p) { return p.parent_path().filename().string(); }

const char* const kSkipMessage =
    "GNSS_STREAM_FILES is unset: set it to a ';'-separated list of product files "
    "(products.txt of network runs) to run the real-file replay properties.";

/// FNV-1a, 64 bit: a stable hash for the store dumps (std::hash is not stable across builds).
std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

/// The data lines of a product file grouped by epoch: tag in E, C, B, P, R, U, G, I.
std::vector<std::vector<std::string>> dataLinesByEpoch(const fs::path& p) {
    std::vector<std::vector<std::string>> out;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2 || line[1] != ',') continue;
        const char tag = line[0];
        if (tag == 'E') { out.emplace_back(); }
        if (std::string("ECBPRUGI").find(tag) == std::string::npos) continue;
        if (out.empty()) continue;
        out.back().push_back(line);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// The producer settings a replay emitter lacks, read from the run's arm.json
// ---------------------------------------------------------------------------------------------

/// The emitter settings the network run applies at setup (ApplicationNetworkSetup) and that
/// the S line does not carry.  Defaults are the loader's.
struct SetterRules {
    bool                     have_arm{false};
    std::vector<std::string> unsettled_pairs;     ///< network.ring_fix.pairs when ring_fix.enable and unsettled_steps.
    bool                     unheld{false};       ///< network.ring_fix.unsettled_steps_unheld.
    std::vector<std::string> step_class_pairs;    ///< network.disc_step_class_pairs.
    double                   step_class_tol{0.15};///< network.disc_step_class_tol_cyc.
    std::map<std::pair<char, std::string>, std::string> members;   ///< network.phase_families (+ codes).
    double                   guard_cyc{0.0};      ///< network.ring_fix.continuity_guard_cyc.
    int                      guard_hold{20};      ///< network.ring_fix.continuity_guard_hold.
    int                      guard_min_run{4};    ///< network.ring_fix.continuity_guard_min_run.
};

SetterRules readSetterRules(const fs::path& product_file) {
    SetterRules r;
    const fs::path arm = product_file.parent_path() / "arm.json";
    std::ifstream in(arm);
    if (!in) return r;
    nlohmann::json j;
    try { in >> j; } catch (...) { return r; }
    r.have_arm = true;
    if (!j.contains("network")) return r;
    const auto& n = j["network"];
    auto strings = [](const nlohmann::json& a) {
        std::vector<std::string> v;
        if (a.is_array()) for (const auto& x : a) v.push_back(x.get<std::string>());
        return v;
    };
    if (n.contains("disc_step_class_pairs")) r.step_class_pairs = strings(n["disc_step_class_pairs"]);
    if (n.contains("disc_step_class_tol_cyc")) r.step_class_tol = n["disc_step_class_tol_cyc"].get<double>();
    const bool codes = n.value("phase_family_codes", false);
    if (n.contains("phase_families"))
        for (const std::string& f : strings(n["phase_families"])) {
            // "E:L1X>L1C": member L1X repeats identity L1C (and C1X repeats C1C with the codes form).
            const auto colon = f.find(':'), gt = f.find('>');
            if (colon == std::string::npos || gt == std::string::npos) continue;
            const char c = f[0];
            const std::string member = f.substr(colon + 1, gt - colon - 1), identity = f.substr(gt + 1);
            r.members[{c, member}] = identity;
            if (codes) r.members[{c, "C" + member.substr(1)}] = "C" + identity.substr(1);
        }
    if (n.contains("ring_fix")) {
        const auto& rf = n["ring_fix"];
        if (rf.value("enable", false) && rf.value("unsettled_steps", false)) {
            r.unsettled_pairs = strings(rf.value("pairs", nlohmann::json::array()));
            r.unheld          = rf.value("unsettled_steps_unheld", false);
            r.guard_cyc       = rf.value("continuity_guard_cyc", 0.0);
            r.guard_hold      = rf.value("continuity_guard_hold", 20);
            r.guard_min_run   = rf.value("continuity_guard_min_run", 4);
        }
    }
    return r;
}

/// One replay emitter of the attribution, with the settings of its layer.
struct Layer {
    std::string           name;      ///< The class a value takes when this layer first makes it agree.
    NetworkProductEmitter emitter;
    SsrCorrectionStore    store;     ///< Discarded; emitToStore needs one.
};

/// Build the cumulative layers: the user's replay emitter, then each setting added in order.
std::vector<std::unique_ptr<Layer>> buildLayers(const SbasisContract& contract, const SetterRules& r) {
    std::vector<std::unique_ptr<Layer>> v;
    auto add = [&](const std::string& name, int level) {
        auto L = std::make_unique<Layer>();
        L->name    = name;
        L->emitter = NetworkProductReader::makeReplayEmitter(contract);
        if (level >= 1 && !r.unsettled_pairs.empty()) {
            L->emitter.setUnsettledSteps(r.unsettled_pairs);
            if (r.unheld) L->emitter.setUnsettledStepsUnheld(true);
        }
        if (level >= 2 && !r.step_class_pairs.empty())
            L->emitter.setStepClassTest(r.step_class_pairs, r.step_class_tol);
        if (level >= 3 && !r.members.empty()) L->emitter.setPhaseFamilyMembers(r.members);
        if (level >= 4 && r.guard_cyc > 0.0 && !r.unsettled_pairs.empty())
            L->emitter.setContinuityGuard(r.guard_cyc, r.guard_hold, r.guard_min_run);
        v.push_back(std::move(L));
    };
    add("user", 0);
    if (!r.unsettled_pairs.empty())  add("unsettled", 1);
    if (!r.step_class_pairs.empty()) add("step_class", 2);
    if (!r.members.empty())          add("member", 3);
    if (r.guard_cyc > 0.0)           add("guard", 4);
    return v;
}

/// The values one emitter got wrong at one epoch: "channel|key" -> (file, recompute).
using Disagreements = std::map<std::string, std::pair<int, int>>;

Disagreements compareCounters(const NetworkProductRecord& rec, const NetworkProductEmitter& em) {
    Disagreements d;
    const auto& sd = rec.serialized_disc;
    auto split = [](const std::string& key, std::string& sat, std::string& sig) {
        const auto bar = key.find('|');
        sat = key.substr(0, bar);
        sig = bar == std::string::npos ? std::string() : key.substr(bar + 1);
    };
    for (const auto& [sat, v] : sd.clock) {
        const int r = em.clockDiscontinuityCounter(sat);
        if (r != v) d["clock|" + sat] = { v, r };
    }
    for (const auto& [key, v] : sd.code) {
        std::string sat, sig; split(key, sat, sig);
        const int r = em.codeBiasDiscontinuityCounter(sat, sig);
        if (r != v) d["code|" + key] = { v, r };
    }
    for (const auto& [key, v] : sd.phase) {
        std::string sat, sig; split(key, sat, sig);
        const int r = em.discontinuityCounter(sat, sig);
        if (r != v) d["phase|" + key] = { v, r };
    }
    return d;
}

/// Feed one epoch to an emitter the way NetworkProductReader::replay does, without the
/// counter installation (the caller installs after comparing).
void emitEpoch(const NetworkProductRecord& rec, NetworkProductEmitter& em, SsrCorrectionStore& st) {
    if (rec.datum_changed) em.markDatumChange(rec.datum_consts);
    if (!rec.retro_sats.empty()) em.markSatDiscontinuity(rec.retro_sats);
    em.emitToStore(rec.epoch, st);
}

// ---------------------------------------------------------------------------------------------
// The store dump (P3, P4)
// ---------------------------------------------------------------------------------------------

/// Every satellite and signal the replay has pushed so far, for the dump's walk.
struct Universe {
    std::set<std::string>                        sats;
    std::map<std::string, std::set<std::string>> code, phase;
    void add(const NetworkSolutionEpoch& e) {
        for (const auto& s : e.sats) {
            if (s.sat.empty()) continue;
            sats.insert(s.sat);
            for (const auto& [sig, b] : s.code_bias_m) { (void)b; code[s.sat].insert(sig); }
            for (const auto& [sig, b] : s.phase_bias)  { (void)b; phase[s.sat].insert(sig); }
        }
    }
};

/// A deterministic text dump of what a user can read from @p st at @p t.  Ages are not
/// applied (max age 1e12 s), so the dump shows the records, not the age rules.
std::string dumpStore(const SsrCorrectionStore& st, const Universe& u, const GnssTime& t) {
    constexpr double kAnyAge = 1e12;
    std::string o;
    o.reserve(u.sats.size() * 256);
    if (const auto g = st.getGenerationId()) o += fmt::format("gid {}\n", *g);
    for (const std::string& sat : u.sats) {
        o += "K " + sat + "\n";
        if (const auto oc = st.getOrbitClock(sat, t, kAnyAge))
            o += fmt::format("c {} {} {} {}:{} {} {} {}\n", oc->c0, oc->c1, oc->c2, oc->epoch_time.week,
                             oc->epoch_time.tow, oc->iode, oc->clock_iode, oc->orbit_iod_ssr);
        if (const auto cd = st.getClockDiscontinuity(sat)) o += fmt::format("cd {}\n", *cd);
        if (const auto ura = st.getUra(sat, t, kAnyAge)) o += fmt::format("u {}\n", *ura);
        if (const auto it = u.code.find(sat); it != u.code.end())
            for (const std::string& sig : it->second) {
                if (!st.hasCodeBias(sat, sig)) continue;
                const auto d = st.getCodeBiasDiscontinuity(sat, sig);
                o += fmt::format("b {} {} {}\n", sig, st.getCodeBias(sat, sig), d ? *d : -1);
            }
        if (const auto it = u.phase.find(sat); it != u.phase.end())
            for (const std::string& sig : it->second)
                if (const auto pb = st.getPhaseBias(sat, sig))
                    o += fmt::format("p {} {} {} {} {} {} {} {}\n", sig, pb->bias_m, pb->signal_integer ? 1 : 0,
                                     pb->wide_lane_integer, pb->discontinuity_counter, pb->sigma_m,
                                     pb->combined_sigma_m, pb->hold_generation);
        if (const auto pt = st.getPhaseBiasTime(sat)) o += fmt::format("pt {}:{}\n", pt->week, pt->tow);
        if (const auto lv = st.getDeclaredLevelSteps(sat)) {
            o += fmt::format("lv {} {}", lv->clock_m, lv->n);
            for (const auto& [sig, x] : lv->bias_m) o += fmt::format(" {}={}", sig, x);
            o += "\n";
        }
    }
    return o;
}

/// The batch replay of one file as a user runs it: hash and tow per epoch.
struct BatchResult {
    bool                       ok{false};
    std::size_t                epochs{0};
    std::vector<std::uint64_t> hashes;
    std::vector<double>        tows;
    bool                       has_counters{false};
};

/// Run (once per process and file) the user's replay: read, then replay every epoch into a
/// fresh store with the replay emitter of the file's contract.
const BatchResult& batchReplay(const fs::path& p) {
    static std::map<std::string, BatchResult> cache;
    const std::string key = p.string();
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    BatchResult r;
    SbasisContract contract;
    bool ok = false;
    const std::vector<NetworkProductRecord> recs = NetworkProductReader::read(key, &contract, &ok);
    r.ok = ok && !recs.empty();
    SsrCorrectionStore    store;
    NetworkProductEmitter emitter = NetworkProductReader::makeReplayEmitter(contract);
    Universe u;
    for (const NetworkProductRecord& rec : recs) {
        NetworkProductReader::replay(rec, store, emitter);
        u.add(rec.epoch);
        r.hashes.push_back(fnv1a(dumpStore(store, u, rec.epoch.epoch_time)));
        r.tows.push_back(rec.epoch.epoch_time.tow);
        r.has_counters = r.has_counters || rec.serialized_disc.any();
    }
    r.epochs = recs.size();
    return cache.emplace(key, std::move(r)).first->second;
}

// ---------------------------------------------------------------------------------------------
// P1: counter disagreements of a file replay, by class
// ---------------------------------------------------------------------------------------------

TEST(StreamRealFileReplay, CounterDisagreementsAreAttributedToTheProducerSettings) {
    const auto files = streamFiles();
    if (files.empty()) GTEST_SKIP() << kSkipMessage;
    const fs::path out = outDir();
    for (const fs::path& p : files) {
        SCOPED_TRACE(p.string());
        const std::string label = labelOf(p);
        SbasisContract contract;
        bool ok = false;
        const std::vector<NetworkProductRecord> recs = NetworkProductReader::read(p.string(), &contract, &ok);
        ASSERT_TRUE(ok) << "the file does not parse cleanly";
        ASSERT_FALSE(recs.empty());
        const SetterRules rules = readSetterRules(p);
        std::vector<std::unique_ptr<Layer>> layers = buildLayers(contract, rules);

        std::ofstream csv(out / (label + "_p1_disagreements.csv"));
        csv << "file,epoch,tow,channel,key,file_value,replay_value,direction,class,signal_integer,wide_lane_integer\n";
        std::size_t epochs_with = 0, values = 0, epochs_with_counters = 0;
        std::map<std::string, std::size_t> by_class, by_channel, by_class_dir;
        // The emitter with every setting: the values it still gets wrong, by direction.  The layers are cumulative,
        // so only the last one is a statement about the producer's rules; an intermediate layer can disagree where
        // the user layer agreed (for example the unheld steps of a member signal before the member table is added).
        std::map<std::string, std::size_t> residual;
        for (std::size_t i = 0; i < recs.size(); ++i) {
            const NetworkProductRecord& rec = recs[i];
            std::vector<Disagreements> d(layers.size());
            for (std::size_t L = 0; L < layers.size(); ++L) {
                emitEpoch(rec, layers[L]->emitter, layers[L]->store);
                d[L] = compareCounters(rec, layers[L]->emitter);
                // Install the file's counters so the next epoch is judged on its own steps.
                layers[L]->emitter.adoptDiscontinuityCounters(rec.serialized_disc.phase, rec.serialized_disc.code,
                                                              rec.serialized_disc.clock);
            }
            if (rec.serialized_disc.any()) ++epochs_with_counters;
            for (const auto& [k, v] : d.back())
                ++residual[std::string(v.first > v.second ? "file_ahead" : "replay_ahead")
                           + (d[0].count(k) != 0 ? "" : ",new")];
            if (d[0].empty()) continue;
            ++epochs_with;
            for (const auto& [k, v] : d[0]) {
                ++values;
                std::string cls = "other";
                for (std::size_t L = 1; L < layers.size(); ++L)
                    if (d[L].count(k) == 0) { cls = layers[L]->name; break; }
                const std::string channel = k.substr(0, k.find('|'));
                const std::string key     = k.substr(k.find('|') + 1);
                const char* dir = v.first > v.second ? "file_ahead" : "replay_ahead";
                ++by_class[cls];
                ++by_channel[channel];
                ++by_class_dir[cls + ":" + dir];
                int si = -1, wli = -1;
                if (channel == "phase") {
                    const std::string sat = key.substr(0, key.find('|')), sig = key.substr(key.find('|') + 1);
                    for (const auto& s : rec.epoch.sats)
                        if (s.sat == sat)
                            if (const auto pb = s.phase_bias.find(sig); pb != s.phase_bias.end()) {
                                si  = pb->second.signal_integer ? 1 : 0;
                                wli = pb->second.wide_lane_integer;
                            }
                }
                csv << fmt::format("{},{},{},{},{},{},{},{},{},{},{}\n", label, i, rec.epoch.epoch_time.tow, channel,
                                   key, v.first, v.second, dir, cls, si, wli);
            }
        }
        std::string summary = fmt::format(
            "{}: epochs {}, with counters {}, with a disagreement {}, values {}; layers", label, recs.size(),
            epochs_with_counters, epochs_with, values);
        for (const auto& L : layers) summary += " " + L->name;
        summary += "; by channel";
        for (const auto& [c, n] : by_channel) summary += fmt::format(" {}={}", c, n);
        summary += "; by class";
        for (const auto& [c, n] : by_class) summary += fmt::format(" {}={}", c, n);
        summary += "; by class and direction";
        for (const auto& [c, n] : by_class_dir) summary += fmt::format(" {}={}", c, n);
        summary += "; left with every setting";
        for (const auto& [c, n] : residual) summary += fmt::format(" {}={}", c, n);
        if (residual.empty()) summary += " none";
        std::ofstream(out / (label + "_p1_summary.txt")) << summary << "\n";
        std::cout << "[P1] " << summary << std::endl;
        if (epochs_with_counters == 0) continue;   // nothing to compare: the file carries no counters
        // A value no setting explains is a step the file declares and the replay cannot: the chain's declarations
        // at a re-host travel in no S token and no setting (on the files of 2026-11-19 every such value sits on an
        // epoch where network_events.csv records a chain re-host of that satellite).  The replay never steps where
        // the file did not without a setting that explains it.
        EXPECT_EQ(by_class_dir["other:replay_ahead"], 0u)
            << label << ": " << by_class_dir["other:replay_ahead"]
            << " counter value(s) where the replay stepped, the file did not and no producer setting explains it";
    }
}

// ---------------------------------------------------------------------------------------------
// P2: the parsed epochs re-write to the same data lines
// ---------------------------------------------------------------------------------------------

TEST(StreamRealFileReplay, ParsedEpochsRewriteToTheSameDataLines) {
    const auto files = streamFiles();
    if (files.empty()) GTEST_SKIP() << kSkipMessage;
    for (const fs::path& p : files) {
        SCOPED_TRACE(p.string());
        const std::string label = labelOf(p);
        SbasisContract contract;
        bool ok = false;
        const std::vector<NetworkProductRecord> recs = NetworkProductReader::read(p.string(), &contract, &ok);
        ASSERT_TRUE(ok);
        bool has_counters = false;
        for (const auto& rec : recs) has_counters = has_counters || rec.serialized_disc.any();
        const fs::path rewritten = gnss_test::tempPath(label + "_rewrite.txt");
        {
            NetworkProductWriter writer(rewritten.string());
            SsrCorrectionStore    store;
            NetworkProductEmitter emitter = NetworkProductReader::makeReplayEmitter(contract);
            for (const NetworkProductRecord& rec : recs) {
                NetworkProductReader::replay(rec, store, emitter);
                writer.write(rec.epoch, rec.datum_changed, rec.datum_consts, rec.retro_sats,
                             has_counters ? &emitter : nullptr);
            }
        }
        const auto a = dataLinesByEpoch(p);
        const auto b = dataLinesByEpoch(rewritten);
        ASSERT_EQ(a.size(), b.size()) << label << ": epoch count differs after the re-write";
        std::size_t differing = 0;
        std::string first;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i] == b[i]) continue;
            ++differing;
            if (first.empty()) {
                for (std::size_t k = 0; k < std::max(a[i].size(), b[i].size()); ++k) {
                    const std::string x = k < a[i].size() ? a[i][k] : "<none>";
                    const std::string y = k < b[i].size() ? b[i][k] : "<none>";
                    if (x != y) { first = fmt::format("epoch {}: file '{}' rewrite '{}'", i, x, y); break; }
                }
            }
        }
        std::cout << fmt::format("[P2] {}: epochs {}, epochs whose data lines differ {}{}{}", label, a.size(),
                                 differing, first.empty() ? "" : "; first: ", first)
                  << std::endl;
        EXPECT_EQ(differing, 0u) << label << ": " << first;
    }
}

// ---------------------------------------------------------------------------------------------
// P3: the tail reader delivers the batch reader's store at every poll
// ---------------------------------------------------------------------------------------------

TEST(StreamRealFileReplay, TheTailReaderMatchesTheBatchReplayAtEveryPoll) {
    const auto files = streamFiles();
    if (files.empty()) GTEST_SKIP() << kSkipMessage;
    // Chunk sizes cycle through these byte counts, so polls fall mid-line, mid-epoch and on
    // whole runs of epochs.
    constexpr std::array<std::size_t, 6> kChunks{ 1, 13, 997, 65521, 524287, 1048573 };
    for (const fs::path& p : files) {
        SCOPED_TRACE(p.string());
        const std::string label = labelOf(p);
        const BatchResult& batch = batchReplay(p);
        ASSERT_TRUE(batch.ok);
        std::string bytes;
        {
            std::ifstream in(p, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const fs::path grown = gnss_test::tempPath(label + "_grown.txt");
        { std::ofstream(grown, std::ios::binary | std::ios::trunc); }
        NetworkProductTail tail(grown.string(), /*start_at_end=*/false);
        SsrCorrectionStore store;
        Universe u;
        std::size_t pos = 0, polls = 0, mismatched = 0, delivered = 0, ci = 0;
        std::string first;
        while (pos < bytes.size()) {
            const std::size_t n = std::min(kChunks[ci++ % kChunks.size()], bytes.size() - pos);
            {
                std::ofstream app(grown, std::ios::binary | std::ios::app);
                app.write(bytes.data() + pos, static_cast<std::streamsize>(n));
            }
            pos += n;
            const std::vector<NetworkProductRecord> got = tail.pollInto(store);
            ++polls;
            for (const auto& rec : got) {
                u.add(rec.epoch);
                if (delivered < batch.tows.size() && rec.epoch.epoch_time.tow != batch.tows[delivered] && first.empty())
                    first = fmt::format("record {} tow {} against batch {}", delivered, rec.epoch.epoch_time.tow,
                                        batch.tows[delivered]);
                ++delivered;
            }
            if (got.empty() || delivered == 0) continue;
            const std::uint64_t h = fnv1a(dumpStore(store, u, got.back().epoch.epoch_time));
            if (delivered > batch.hashes.size() || h != batch.hashes[delivered - 1]) {
                ++mismatched;
                if (first.empty()) first = fmt::format("store after {} epochs differs", delivered);
            }
        }
        // The tail never flushes the last epoch (no later E line arrives); every earlier one is delivered.
        std::cout << fmt::format("[P3] {}: polls {}, epochs delivered {} of {}, polls whose store differs {}{}{}",
                                 label, polls, delivered, batch.epochs, mismatched, first.empty() ? "" : "; first: ",
                                 first)
                  << std::endl;
        EXPECT_EQ(delivered + 1, batch.epochs);
        EXPECT_EQ(mismatched, 0u) << first;
        EXPECT_TRUE(first.empty()) << first;
    }
}

// ---------------------------------------------------------------------------------------------
// P4: the store hash list of the user's replay
// ---------------------------------------------------------------------------------------------

TEST(StreamRealFileReplay, TheStoreHashListIsWritten) {
    const auto files = streamFiles();
    if (files.empty()) GTEST_SKIP() << kSkipMessage;
    const fs::path out = outDir();
    for (const fs::path& p : files) {
        SCOPED_TRACE(p.string());
        const std::string label = labelOf(p);
        const BatchResult& batch = batchReplay(p);
        ASSERT_TRUE(batch.ok);
        std::ofstream list(out / (label + "_p4_store_hashes.txt"));
        std::uint64_t whole = 1469598103934665603ULL;
        for (std::size_t i = 0; i < batch.hashes.size(); ++i) {
            list << fmt::format("{} {} {:016x}\n", i, batch.tows[i], batch.hashes[i]);
            whole = fnv1a(fmt::format("{:016x}{:016x}", whole, batch.hashes[i]));
        }
        std::cout << fmt::format("[P4] {}: epochs {}, counters in file {}, list digest {:016x}", label,
                                 batch.epochs, batch.has_counters ? "yes" : "no", whole)
                  << std::endl;
        EXPECT_EQ(batch.hashes.size(), batch.epochs);
    }
}

}  // namespace
}  // namespace GnssProcessor
