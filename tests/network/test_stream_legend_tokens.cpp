/**
 * @file test_stream_legend_tokens.cpp
 * @brief The header legend of the correction-stream file against what the writer emits and
 *        what the parser reads.
 *
 * The writer prints a '#' legend in the header of every product file: one line per record
 * tag, naming each positional field and each key=value token it can write.  A reader that
 * follows the legend drops whatever the legend does not name, so the legend is a declaration
 * of the stream and must hold in both directions: every token the writer emits is declared,
 * and every declared token is one the parser acts on.
 *
 * The token set is taken from the writer itself, never from a list in this file: a stream is
 * written through the real writer with every optional record, field and contract token
 * armed (three epochs, one per phase-bias hold mode on the S line), and the tokens are read
 * off its data lines.  A further case proves that this stream emits every declared token, so
 * the two sets are compared in full.  "The parser acts on a token" is measured, not assumed:
 * the file is parsed with and without the token and the parsed records, contract and
 * heartbeats are compared field by field.
 *
 * The legend is a declaration, so the cases assert what it declares, never under which
 * configuration a token appears.  Three tokens that the user engine has no reader for (the
 * wide-lane integer indicator, the G record's generation time and the isc=obs mark) and one
 * the parser reads but the writer never emits (cmp=) are checked on their own.
 */

#include <gtest/gtest.h>

#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SignalDeclaration.hpp"
#include "products/SsrCorrectionStore.hpp"

#include "../support/TestTempDir.hpp"

#include <cstddef>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

const GnssTime kT0{2313, 259230.5};
constexpr double kDt = 30.0;

/// The S contract with every key=value extension armed.  @p hold_mode selects the phase-bias
/// hold written as pbhold=: 1 the plain hold, 2 the phase-clock hold, 3 the smoothed hold.
SbasisContract armedContract(int hold_mode) {
    SbasisContract c;
    c.present                     = true;
    c.decl.reference_receiver_id  = "REF100XXX";
    c.decl.clock_pivot_sat["G"]   = "G05";
    c.decl.eq34_datum_receiver    = "REF100XXX";
    c.decl.emit_code_bias         = true;
    c.apriori_product_id          = "TESTPRODUCT";
    c.disc_threshold_m            = 0.05;
    c.disc_threshold_frac_cycles  = 0.25;
    c.emit_clock_disc             = true;
    c.staleness_horizon_s         = 60.0;
    c.code_bias_jump_test         = true;
    c.clock_disc_on_set_change    = true;
    c.generation_id               = 7;
    c.frame_id                    = "IGS20";
    c.coord_epoch                 = "2024.5";
    c.atx_id                      = "igs20.atx";
    c.orbit_ref                   = "CoM";
    c.age_scale_k                 = 1e-6;
    c.disc_phase_visible_min_sats = 4;
    c.h_sigma0_source             = "pc";
    c.cb_pin_pair["G"]            = "C1C-C2W";
    c.gf_pin_pair["G"]            = "C1C-C2W";
    c.sat_gf_datum                = "broadcast_tgd";
    c.disc_mark_scope_channel     = true;
    c.emit_pb_hold                = true;
    c.emit_pb_hold_phase_clock    = (hold_mode == 2);
    c.emit_pb_hold_smoothed       = (hold_mode == 3);
    c.windup_mode                 = kWindUpModeAccumulated;
    return c;
}

/// One epoch with every optional per-record field armed: G05 carries a clock with drift,
/// broadcast anchor, orbit echo, combined code sigma, correlation, level step and URA, three
/// code biases (one marked isc=obs, one with a follow step) and two phase biases with sigma,
/// combined sigma and hold generation; G12 carries the base fields only.
NetworkSolutionEpoch armedEpoch(const GnssTime& t, int k) {
    NetworkSolutionEpoch sol;
    sol.epoch_time      = t;
    sol.generation_time = GnssTime{t.week, t.tow + 12.5};
    sol.generation_id   = 7;
    sol.iod_ssr         = 3 + k;
    sol.provider_id     = 42;
    sol.solution_id     = 1;

    NetworkSatSolution a;
    a.sat          = "G05";
    a.has_clock    = true;
    a.clock_c0_m   = 12.5 + 0.001 * k;
    a.clock_c1_m_s = -0.0025;
    a.clock_iode    = 42;
    a.orbit_iod_ssr = 7;
    a.orbit_epoch   = GnssTime{t.week, t.tow - 30.0};
    a.sigcc_m      = 0.31;
    a.rho          = -0.5;
    a.level_step_m = 0.125;
    a.has_ura      = true;
    a.clock_ura_m  = 0.037;
    a.code_bias_m["C1C"] = 0.3125;
    a.code_bias_m["C2W"] = -1.0625;
    a.code_bias_m["C2L"] = 0.5;
    a.isc_observed.insert("C2L");
    a.cb_level_step_m["C1C"] = -0.0625;
    NetworkPhaseBias l1;
    l1.bias_m            = 0.0421;
    l1.signal_integer    = true;
    l1.wide_lane_integer = 2;
    l1.sigma_m           = 0.012;
    l1.sigc_m            = 0.0026;
    l1.hold_generation   = 3;
    NetworkPhaseBias l2  = l1;
    l2.bias_m            = -0.137;
    l2.wide_lane_integer = 1;
    a.phase_bias["L1C"] = l1;
    a.phase_bias["L2W"] = l2;
    sol.sats.push_back(a);

    NetworkSatSolution b;
    b.sat        = "G12";
    b.has_clock  = true;
    b.clock_c0_m = -8.75;
    b.code_bias_m["C1C"] = 0.27;
    NetworkPhaseBias p;
    p.bias_m            = 0.11;
    p.signal_integer    = true;
    p.wide_lane_integer = 1;
    b.phase_bias["L1C"] = p;
    sol.sats.push_back(b);
    return sol;
}

/// A declared roster with every N token filled.
NetworkSignalDeclaration armedDeclaration() {
    NetworkSignalDeclaration d;
    d.present = true;
    SignalDeclarationBand& row = d.bands["GPS"]['1'];
    row.phase       = {"L1C"};
    row.code        = {"C1C"};
    row.companion   = {"C1W"};
    row.datum_phase = "L1C";
    row.datum_code  = "C1C";
    return d;
}

/**
 * @brief Writes the armed stream and returns its path.
 *
 * Three epochs.  The first follows the header S line (plain hold) and an N block; the second
 * and third each follow a re-broadcast S line (phase-clock hold, smoothed hold).  Every epoch
 * follows its H heartbeat.  The first two declare a datum change of GPS and a retro mark of
 * G05 (E field 8, R record); the third declares neither, so its E line has seven fields.
 * The counters come from an emitter built from the header contract and fed the same epochs,
 * as the producer does.
 */
std::string writeArmedStream() {
    const std::string path = gnss_test::tempPath("armed.txt").string();
    NetworkProductEmitter emit = NetworkProductReader::makeReplayEmitter(armedContract(1));
    SsrCorrectionStore    store;
    NetworkProductWriter  w(path, armedContract(1));
    EXPECT_TRUE(w.good());
    GnssTime t = kT0;
    for (int k = 0; k < 3; ++k) {
        if (k == 0) EXPECT_EQ(w.writeSignalDeclaration(armedDeclaration()), 1u);
        else        w.writeContract(armedContract(k + 1));
        NetworkProducerStatus st;
        st.time             = t;
        st.state            = NetworkProducerStatus::State::Ok;
        st.sigma0           = 1.23;
        st.n_sats_published = 2;
        st.withheld         = "-";
        w.writeEpochStatus(st);

        const NetworkSolutionEpoch  sol   = armedEpoch(t, k);
        const bool                  dc    = (k < 2);
        const std::set<char>        cons  = dc ? std::set<char>{'G'} : std::set<char>{};
        const std::set<std::string> retro = dc ? std::set<std::string>{"G05"} : std::set<std::string>{};
        if (dc) {
            emit.markDatumChange(cons);
            emit.markSatDiscontinuity(retro);
        }
        emit.emitToStore(sol, store);
        w.write(sol, dc, cons, retro, &emit);
        t.tow += kDt;
    }
    return path;
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    std::string   line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

void writeLines(const std::string& path, const std::vector<std::string>& lines) {
    std::ofstream out(path, std::ios::trunc);
    for (const std::string& l : lines) out << l << '\n';
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string::npos) { out.push_back(line.substr(start)); break; }
        out.push_back(line.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

std::string joinCsv(const std::vector<std::string>& f) {
    std::string out;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (i) out += ',';
        out += f[i];
    }
    return out;
}

bool isData(const std::string& line) { return !line.empty() && line[0] != '#'; }

/// The key of a key=value field, or empty for a positional field.
std::string keyOf(const std::string& field) {
    const auto eq = field.find('=');
    return eq == std::string::npos ? std::string() : field.substr(0, eq);
}

/// One record tag's declared grammar.
struct TagGrammar {
    std::size_t                        max_positional{0};  ///< Positional fields, the tag included.
    bool                               variadic{false};    ///< The line ends in "...".
    std::map<std::string, std::string> keys;               ///< Declared key -> declared value text.
};

/**
 * @brief The legend's per-tag grammar.
 *
 * A legend line is "# <TAG>,...".  A trailing "(...)" note is dropped, brackets and ellipses
 * are removed, and each comma field is either positional or a key=value declaration.
 */
std::map<std::string, TagGrammar> parseLegend(const std::vector<std::string>& lines) {
    std::map<std::string, TagGrammar> legend;
    for (const std::string& line : lines) {
        if (line.size() < 4 || line[0] != '#' || line[3] != ',') continue;
        std::string body  = line.substr(2);
        const auto  paren = body.find('(');
        if (paren != std::string::npos) body = body.substr(0, paren);
        while (!body.empty() && body.back() == ' ') body.pop_back();
        TagGrammar g;
        g.variadic = body.find("...") != std::string::npos;
        std::string flat;
        for (char ch : body)
            if (ch != '[' && ch != ']') flat += ch;
        std::size_t e;
        while ((e = flat.find("...")) != std::string::npos) flat.erase(e, 3);
        for (const std::string& tok : splitCsv(flat)) {
            const auto eq = tok.find('=');
            if (eq == std::string::npos) ++g.max_positional;
            else                         g.keys[tok.substr(0, eq)] = tok.substr(eq + 1);
        }
        legend[std::string(1, body[0])] = g;
    }
    return legend;
}

/**
 * @brief Whether an emitted value is one the legend declares for its key.
 *
 * A declared "<name>" or "<a>:<b>" is a placeholder and admits any value; "<a|b|c>" is an
 * enumeration; a declaration without angle brackets is the one literal value admitted.
 */
bool valueDeclared(const std::string& declared, const std::string& value) {
    if (declared.size() >= 2 && declared.front() == '<' && declared.back() == '>') {
        const std::string inner = declared.substr(1, declared.size() - 2);
        if (inner.find('<') != std::string::npos || inner.find('|') == std::string::npos)
            return true;
        std::size_t start = 0;
        while (true) {
            const std::size_t bar = inner.find('|', start);
            if (inner.substr(start, bar - start) == value) return true;
            if (bar == std::string::npos) return false;
            start = bar + 1;
        }
    }
    return declared == value;
}

// --- the parse, written down field by field ------------------------------------------------

void putTime(std::ostream& os, const char* name, const GnssTime& t) {
    os << name << '=' << t.week << '/' << t.tow << ' ';
}
template <class Range>
void putList(std::ostream& os, const char* name, const Range& r) {
    os << name << '[';
    for (const auto& v : r) os << v << ';';
    os << "] ";
}
template <class Map>
void putMap(std::ostream& os, const char* name, const Map& m) {
    os << name << '{';
    for (const auto& kv : m) os << kv.first << '=' << kv.second << ';';
    os << "} ";
}

void putContract(std::ostream& os, const SbasisContract& c) {
    os << "S present=" << c.present << " ref=" << c.decl.reference_receiver_id
       << " sign=" << c.decl.bias_sign << " wu=" << c.decl.windup_yaw_model_id
       << " amb=" << static_cast<int>(c.decl.amb_datum)
       << " isb=" << static_cast<int>(c.decl.isb_datum)
       << " ecb=" << c.decl.emit_code_bias
       << " cbd=" << static_cast<int>(c.decl.code_bias_datum)
       << " anchor=" << c.decl.broadcast_anchor_sigma_m
       << " pdr=" << c.decl.eq34_datum_receiver << " apriori=" << c.apriori_product_id
       << " thr=" << c.disc_threshold_m << " frac=" << c.disc_threshold_frac_cycles
       << " scope=" << c.disc_mark_scope_channel << " pv=" << c.disc_phase_visible_min_sats
       << " hold=" << c.emit_pb_hold << c.emit_pb_hold_phase_clock << c.emit_pb_hold_smoothed
       << " clkdisc=" << c.emit_clock_disc << " stale=" << c.staleness_horizon_s
       << " agek=" << c.age_scale_k << " cbjump=" << c.code_bias_jump_test
       << " clkset=" << c.clock_disc_on_set_change << " hsig=" << c.h_sigma0_source
       << " gid=" << c.generation_id << " frame=" << c.frame_id << " cepoch=" << c.coord_epoch
       << " atx=" << c.atx_id << " oref=" << c.orbit_ref << " gfd=" << c.sat_gf_datum
       << " wumode=" << c.windup_mode << ' ';
    putMap(os, "pivots", c.decl.clock_pivot_sat);
    putMap(os, "cbpin", c.cb_pin_pair);
    putMap(os, "gfpin", c.gf_pin_pair);
    os << '\n';
}

void putStatus(std::ostream& os, const NetworkProducerStatus& st) {
    os << "H ";
    putTime(os, "t", st.time);
    os << "state=" << static_cast<int>(st.state) << " sigma0=" << st.sigma0
       << " n=" << st.n_sats_published << " withheld=" << st.withheld << '\n';
}

void putDeclaration(std::ostream& os, const NetworkSignalDeclaration& d) {
    os << "N present=" << d.present << ' ';
    for (const auto& [cons, per_band] : d.bands)
        for (const auto& [band, row] : per_band) {
            os << cons << '/' << band << ' ';
            putList(os, "ph", row.phase);
            putList(os, "co", row.code);
            putList(os, "comp", row.companion);
            os << "dph=" << row.datum_phase << " dco=" << row.datum_code << ' ';
        }
    os << '\n';
}

void putRecord(std::ostream& os, const NetworkProductRecord& r) {
    const NetworkSolutionEpoch& e = r.epoch;
    os << "E ";
    putTime(os, "t", e.epoch_time);
    putTime(os, "gen_t", e.generation_time);
    os << "gid=" << e.generation_id << " iod=" << e.iod_ssr << " prov=" << e.provider_id
       << " sol=" << e.solution_id << " dc=" << r.datum_changed << ' ';
    putList(os, "consts", r.datum_consts);
    putList(os, "retro", r.retro_sats);
    putMap(os, "disc_clk", r.serialized_disc.clock);
    putMap(os, "disc_cb", r.serialized_disc.code);
    putMap(os, "disc_pb", r.serialized_disc.phase);
    os << '\n';
    for (const NetworkSatSolution& s : e.sats) {
        os << "  sat=" << s.sat << " clk=" << s.has_clock << ' ' << s.clock_c0_m << ' '
           << s.clock_c1_m_s << " ura=" << s.has_ura << ' ' << s.clock_ura_m
           << " sigcc=" << s.sigcc_m << " rho=" << s.rho << " lvl=" << s.level_step_m
           << " iode=" << s.clock_iode << " oiod=" << s.orbit_iod_ssr << ' ';
        putTime(os, "oepoch", s.orbit_epoch);
        putMap(os, "cb", s.code_bias_m);
        putMap(os, "cb_lvl", s.cb_level_step_m);
        putList(os, "isc", s.isc_observed);
        for (const auto& [sig, pb] : s.phase_bias)
            os << "pb:" << sig << '=' << pb.bias_m << '/' << pb.signal_integer << '/'
               << pb.wide_lane_integer << '/' << pb.sigma_m << '/' << pb.sigc_m << '/'
               << pb.hold_generation << '/' << pb.chain_component << ' ';
        os << '\n';
    }
    if (r.contract_refresh.present) putContract(os, r.contract_refresh);
    if (r.epoch_status) putStatus(os, *r.epoch_status);
    if (r.declaration_refresh) putDeclaration(os, *r.declaration_refresh);
}

/// Everything the parser returns for @p lines (records, final contract, heartbeats, the ok
/// flag), written out field by field so two parses compare as two strings.
std::string parsed(const std::vector<std::string>& lines) {
    const std::string path = gnss_test::tempPath("variant.txt").string();
    writeLines(path, lines);
    SbasisContract                     contract;
    bool                               ok = false;
    std::vector<NetworkProducerStatus> statuses;
    const auto recs = NetworkProductReader::read(path, &contract, &ok, &statuses);
    std::ostringstream os;
    os << std::setprecision(17);
    os << "ok=" << ok << " records=" << recs.size() << '\n';
    putContract(os, contract);
    for (const auto& st : statuses) putStatus(os, st);
    for (const auto& r : recs) putRecord(os, r);
    return os.str();
}

/// @p lines with field @p j of line @p i removed.
std::vector<std::string> withoutField(std::vector<std::string> lines, std::size_t i, std::size_t j) {
    std::vector<std::string> f = splitCsv(lines[i]);
    f.erase(f.begin() + static_cast<std::ptrdiff_t>(j));
    lines[i] = joinCsv(f);
    return lines;
}

/// Index of the first data line of tag @p tag carrying key @p key, or lines.size().
std::size_t firstLineWithKey(const std::vector<std::string>& lines, const std::string& tag,
                             const std::string& key, std::size_t* field) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!isData(lines[i])) continue;
        const std::vector<std::string> f = splitCsv(lines[i]);
        if (f[0] != tag) continue;
        for (std::size_t j = 1; j < f.size(); ++j)
            if (keyOf(f[j]) == key) { *field = j; return i; }
    }
    return lines.size();
}

/// Index of the first or last data line of tag @p tag, or lines.size().
std::size_t lineOfTag(const std::vector<std::string>& lines, const std::string& tag, bool last) {
    std::size_t found = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!isData(lines[i]) || splitCsv(lines[i])[0] != tag) continue;
        found = i;
        if (!last) break;
    }
    return found;
}

// ---------------------------------------------------------------------------------------------

// Every data line the writer emitted fits its tag's legend line: the tag is declared, every
// key is declared on that tag, and the positional fields do not outnumber the declared ones.
TEST(StreamLegendTokens, EveryEmittedTokenIsDeclared) {
    const std::vector<std::string> lines  = readLines(writeArmedStream());
    const auto                     legend = parseLegend(lines);
    ASSERT_FALSE(legend.empty());

    std::size_t n_data = 0;
    for (const std::string& dl : lines) {
        if (!isData(dl)) continue;
        ++n_data;
        const std::vector<std::string> f = splitCsv(dl);
        const auto it = legend.find(f[0]);
        ASSERT_NE(it, legend.end()) << "record tag not in the legend: " << dl;
        std::size_t n_pos = 0;
        for (const std::string& fld : f) {
            const std::string key = keyOf(fld);
            if (key.empty()) { ++n_pos; continue; }
            EXPECT_EQ(it->second.keys.count(key), 1u)
                << "key '" << key << "' not declared on tag " << f[0] << ": " << dl;
        }
        if (!it->second.variadic)
            EXPECT_LE(n_pos, it->second.max_positional) << "more positional fields than declared: " << dl;
    }
    EXPECT_GT(n_data, 0u);
}

// Every value the writer emitted for a key is one the legend admits: a literal declaration
// (pbhold=1, isc=obs, discscope=channel) admits that value only, an enumeration its members.
TEST(StreamLegendTokens, EveryEmittedValueIsDeclared) {
    const std::vector<std::string> lines  = readLines(writeArmedStream());
    const auto                     legend = parseLegend(lines);

    std::set<std::string> undeclared;
    for (const std::string& dl : lines) {
        if (!isData(dl)) continue;
        const std::vector<std::string> f = splitCsv(dl);
        const auto it = legend.find(f[0]);
        if (it == legend.end()) continue;   // reported by EveryEmittedTokenIsDeclared
        for (const std::string& fld : f) {
            const std::string key = keyOf(fld);
            if (key.empty()) continue;
            const auto d = it->second.keys.find(key);
            if (d == it->second.keys.end()) continue;   // reported by EveryEmittedTokenIsDeclared
            if (valueDeclared(d->second, fld.substr(key.size() + 1))) continue;
            undeclared.insert(f[0] + " " + fld + " (declared " + key + "=" + d->second + ")");
        }
    }
    for (const std::string& u : undeclared) ADD_FAILURE() << "value not declared: " << u;
}

// The armed stream emits every tag and every key the legend declares, so the token set taken
// from the writer covers the legend and the other cases compare the two sets in full.
TEST(StreamLegendTokens, TheArmedStreamEmitsEveryDeclaredToken) {
    const std::vector<std::string> lines  = readLines(writeArmedStream());
    const auto                     legend = parseLegend(lines);

    std::map<std::string, std::set<std::string>> emitted;   // tag -> keys
    for (const std::string& dl : lines) {
        if (!isData(dl)) continue;
        const std::vector<std::string> f = splitCsv(dl);
        auto& keys = emitted[f[0]];
        for (const std::string& fld : f)
            if (!keyOf(fld).empty()) keys.insert(keyOf(fld));
    }
    for (const char* tag : {"E", "C", "B", "P", "G", "I", "R", "U", "H", "S", "N"})
        EXPECT_EQ(legend.count(tag), 1u) << "the legend no longer declares tag " << tag;
    for (const auto& [tag, g] : legend) {
        ASSERT_EQ(emitted.count(tag), 1u) << "the armed stream emits no " << tag << " record";
        for (const auto& [key, value] : g.keys)
            EXPECT_EQ(emitted.at(tag).count(key), 1u)
                << "the armed stream emits no " << key << "= on tag " << tag;
    }
}

// Every declared key changes what the parser returns when it is removed from the line that
// carries it: the parser acts on the token rather than skipping it as unknown.  The whole
// armed stream parses cleanly first.
TEST(StreamLegendTokens, EveryDeclaredTokenChangesTheParse) {
    const std::vector<std::string> lines  = readLines(writeArmedStream());
    const auto                     legend = parseLegend(lines);
    const std::string              base   = parsed(lines);
    ASSERT_NE(base.find("ok=1 records=3"), std::string::npos) << base.substr(0, 200);

    for (const auto& [tag, g] : legend)
        for (const auto& [key, value] : g.keys) {
            std::size_t       j = 0;
            const std::size_t i = firstLineWithKey(lines, tag, key, &j);
            if (i == lines.size()) continue;   // reported by TheArmedStreamEmitsEveryDeclaredToken
            EXPECT_NE(parsed(withoutField(lines, i, j)), base)
                << "the parser ignores the declared token " << key << "= on tag " << tag
                << " (line: " << lines[i] << ")";
        }
}

// The legend says unknown keys are ignorable.  An unknown key=value token appended to the
// last line of each record tag leaves the parse unchanged.
TEST(StreamLegendTokens, AnUnknownKeyIsIgnoredOnEveryRecord) {
    const std::vector<std::string> lines  = readLines(writeArmedStream());
    const auto                     legend = parseLegend(lines);
    const std::string              base   = parsed(lines);

    std::set<std::string> misread;
    for (const auto& [tag, g] : legend) {
        const std::size_t i = lineOfTag(lines, tag, /*last=*/true);
        ASSERT_LT(i, lines.size()) << "no " << tag << " record";
        std::vector<std::string> v = lines;
        v[i] += ",zz_unknown=1";
        if (parsed(v) != base) misread.insert(tag);
    }
    // The last E line is the seven-field one: its optional eighth field is absent, so the
    // appended token lands where the constellation set would be.
    for (const std::string& tag : misread) ADD_FAILURE() << "an unknown key changes the parse of tag " << tag;
}

// Three tokens the user engine has no reader for still change the parse: the wide-lane
// integer indicator (P field 6, which also reaches the store's phase-bias entry), the G
// record's generation time and the isc=obs mark of a code bias.
TEST(StreamLegendTokens, TokensWithoutAUserReaderStillParse) {
    const std::string              path  = writeArmedStream();
    const std::vector<std::string> lines = readLines(path);
    const std::string              base  = parsed(lines);

    // P field 6, the wide-lane integer indicator: edited in place.
    const std::size_t ip = lineOfTag(lines, "P", /*last=*/false);
    ASSERT_LT(ip, lines.size());
    {
        std::vector<std::string> f = splitCsv(lines[ip]);
        ASSERT_GE(f.size(), 6u);
        f[5] = (f[5] == "3") ? "0" : "3";
        std::vector<std::string> v = lines;
        v[ip] = joinCsv(f);
        EXPECT_NE(parsed(v), base) << "the parser ignores the wide-lane integer indicator";
    }

    // The G record: removed whole.
    const std::size_t ig = lineOfTag(lines, "G", /*last=*/false);
    ASSERT_LT(ig, lines.size());
    {
        std::vector<std::string> v = lines;
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(ig));
        EXPECT_NE(parsed(v), base) << "the parser ignores the G record";
    }

    // isc=obs on the B record.
    std::size_t       j  = 0;
    const std::size_t ib = firstLineWithKey(lines, "B", "isc", &j);
    ASSERT_LT(ib, lines.size()) << "the armed stream emits no isc=obs";
    EXPECT_NE(parsed(withoutField(lines, ib, j)), base) << "the parser ignores isc=obs";

    // The indicator travels on into the store a user reads, value for value.
    SbasisContract contract;
    bool           ok = false;
    const auto     recs = NetworkProductReader::read(path, &contract, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 3u);
    NetworkProductEmitter replay_emitter = NetworkProductReader::makeReplayEmitter(contract);
    SsrCorrectionStore    store;
    for (const auto& r : recs) NetworkProductReader::replay(r, store, replay_emitter);
    const auto l1 = store.getPhaseBias("G05", "L1C");
    const auto l2 = store.getPhaseBias("G05", "L2W");
    ASSERT_TRUE(l1.has_value());
    ASSERT_TRUE(l2.has_value());
    EXPECT_EQ(l1->wide_lane_integer, 2);
    EXPECT_EQ(l2->wide_lane_integer, 1);
}

// cmp= (the chain component of a phase bias) is read by the parser but never emitted by the
// writer: an appended cmp= changes the parse, and no data line of the armed stream carries it.
TEST(StreamLegendTokens, ChainComponentIsReadButNeverWritten) {
    const std::vector<std::string> lines = readLines(writeArmedStream());
    const std::string              base  = parsed(lines);

    for (const std::string& dl : lines) {
        if (!isData(dl)) continue;
        for (const std::string& fld : splitCsv(dl)) EXPECT_NE(keyOf(fld), "cmp") << dl;
    }
    const std::size_t ip = lineOfTag(lines, "P", /*last=*/false);
    ASSERT_LT(ip, lines.size());
    std::vector<std::string> v = lines;
    v[ip] += ",cmp=4";
    EXPECT_NE(parsed(v), base) << "the parser ignores cmp=";
}

}  // namespace
}  // namespace GnssProcessor
