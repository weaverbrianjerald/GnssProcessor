/**
 * @file NetworkProductIO.cpp
 * @brief Implementation of the PPP-RTK network product file writer / reader.
 */

#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "math/TimeUtils.hpp"                // IGS long-name spans on the GPS time scale

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>   // tailLog()'s never-null fallback

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>

namespace GnssProcessor {

namespace {

/// Split @p line on ',' into raw fields (no trimming; RINEX obs codes and sat
/// IDs never contain commas, so this is lossless for this format).
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

/**
 * @brief Line-at-a-time epoch assembler shared by the batch reader and the tail.
 *
 * Holds the per-line parse state (current epoch under assembly, sat index, pending
 * S line, sticky ok flag) so that @c NetworkProductReader::read and the incremental
 * @c NetworkProductTail path (RTA-6) run one parser and cannot diverge.  An epoch is
 * completed (returned) when the next E line flushes it; @c finish() flushes the
 * trailing epoch at the end of a batch read.  A live tail never calls @c finish(),
 * because the trailing epoch may still be growing.
 */
struct LineAssembler {
    /// 3-arg read() semantics: a short/malformed S line flags @c ok only when the
    /// caller asked for the contract (the 2-arg overload treats it as a valid,
    /// ignored line).  The tail always tracks the contract.
    bool track_contract{false};
    /// Collect every H heartbeat in file order when the caller asks (4-arg read).
    /// A fully withheld epoch's H completes no product record, so the per-record
    /// @c epoch_status field alone would miss the states H is written for.  Off by
    /// default: the tail and the 2/3-arg reads carry no vector.  (H-rider,
    /// edge-queue §1.6 item 3b.)
    bool track_statuses{false};
    std::vector<NetworkProducerStatus> statuses;

    NetworkProductRecord               cur;
    std::map<std::string, std::size_t> sat_index;   ///< sat -> index into cur.epoch.sats
    bool                               have_cur{false};
    /// The most recent S line not yet attached to an epoch.  An S line (header or
    /// mid-file re-broadcast) applies to the epochs that follow it, so it is stamped
    /// into the next E line's record as contract_refresh (RTA-5).
    SbasisContract pending_s;
    bool           pending_s_valid{false};
    /// The most recent H heartbeat not yet attached to an epoch (RTA-10).  The
    /// producer writes an emitted epoch's H immediately before its E line, so a
    /// pending H attaches to the next E record only when their week/tow match
    /// exactly.  Both lines serialize the tow with the same shortest-round-trip
    /// formatting, so the parsed doubles compare equal.  The standalone H of a
    /// fully withheld epoch therefore never attaches to a later epoch.
    NetworkProducerStatus pending_h;
    bool                  pending_h_valid{false};
    /// The declared signal roster block under assembly (E2).
    /// A declaration replaces the previous one wholesale, so the producer writes
    /// every row together and the first @c N record after any non-@c N line
    /// starts a fresh block (@ref decl_block_open).  When the block closes, the
    /// declaration becomes the reader's current one and is stamped on the next E
    /// record (@ref decl_fresh), in the same way as the S line (RTA-5).
    NetworkSignalDeclaration decl_block;
    bool                     decl_block_open{false};
    NetworkSignalDeclaration declaration;     ///< reader-current declaration (present=false until one is seen).
    bool                     decl_fresh{false};///< a block closed and has not been stamped yet.

    /// Close an open @c N block: the accumulated rows become the current
    /// declaration.  Called at the top of every non-@c N tag.
    void closeDeclBlock() {
        if (!decl_block_open) return;
        decl_block_open = false;
        declaration     = decl_block;
        decl_fresh      = true;
    }
    /// Latest H heartbeat consumed, attached or standalone; returned by the tail's
    /// lastStatus() (RTA-10).
    std::optional<NetworkProducerStatus> latest_status;
    /// Reader-current contract state: the last S line consumed (present=false
    /// until one is).  The batch reader copies this into contract_out at EOF.
    SbasisContract contract;
    /// Sticky parse-status flag with read()'s *ok semantics: false after any
    /// malformed line or unknown tag; parsing continues past it.
    bool ok{true};

    /// Find-or-create the per-satellite solution within the current epoch.  The
    /// returned reference is used immediately by the caller before any further
    /// push_back, so reallocation cannot invalidate it across uses.
    NetworkSatSolution& satSolution(const std::string& sat) {
        const auto it = sat_index.find(sat);
        if (it != sat_index.end()) return cur.epoch.sats[it->second];
        sat_index[sat] = cur.epoch.sats.size();
        NetworkSatSolution s;
        s.sat = sat;
        cur.epoch.sats.push_back(std::move(s));
        return cur.epoch.sats.back();
    }

    /// Consume one raw line (with or without a trailing '\r'; never a '\n').
    /// Returns the epoch that was under assembly when this line completes it (an
    /// E, H or F line flushes its predecessor); nullopt otherwise.
    std::optional<NetworkProductRecord> consume(const std::string& raw) {
        std::optional<NetworkProductRecord> completed;
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF
        if (line.empty() || line[0] == '#') return completed;

        const std::vector<std::string> f = splitCsv(line);
        if (f.empty() || f[0].empty()) return completed;
        const std::string& tag = f[0];

        // An N block is contiguous, so any other tag ends it.
        if (tag != "N") closeDeclBlock();

        try {
            if (tag == "E") {
                if (f.size() < 6) { ok = false; return completed; }
                if (have_cur) completed = std::move(cur);
                cur = NetworkProductRecord{};
                sat_index.clear();
                have_cur = false;
                cur.epoch.epoch_time.week = std::stoi(f[1]);
                cur.epoch.epoch_time.tow  = std::stod(f[2]);
                cur.epoch.iod_ssr         = std::stoi(f[3]);
                cur.epoch.provider_id     = std::stoi(f[4]);
                cur.epoch.solution_id     = std::stoi(f[5]);
                cur.datum_changed         = (f.size() > 6) && (std::stoi(f[6]) != 0);
                if (f.size() > 7)   // optional affected-constellation set, field 8 (A2)
                    for (char ch : f[7]) cur.datum_consts.insert(ch);
                if (pending_s_valid) {   // the S line preceding this epoch (RTA-5)
                    cur.contract_refresh = pending_s;
                    pending_s_valid      = false;
                }
                if (pending_h_valid) {   // the H heartbeat preceding this epoch (RTA-10)
                    // Attach only on an exact week/tow match, i.e. the emitted epoch's
                    // own heartbeat.  A withheld epoch's standalone H (different time)
                    // is dropped here; it already updated latest_status when consumed.
                    if (pending_h.time.week == cur.epoch.epoch_time.week &&
                        pending_h.time.tow == cur.epoch.epoch_time.tow)
                        cur.epoch_status = pending_h;
                    pending_h_valid = false;
                }
                // The declaration block that preceded this epoch.  It is stamped on
                // the record whose epoch it describes, as contract_refresh is, so a
                // consumer applies it at its position in the stream rather than at
                // end of file.
                if (decl_fresh) {
                    cur.declaration_refresh = declaration;
                    decl_fresh              = false;
                }
                have_cur = true;
            } else if (tag == "N") {   // declared signal roster
                if (!decl_block_open) {
                    decl_block      = NetworkSignalDeclaration{};
                    decl_block_open = true;
                }
                if (!signalDeclarationParseRecord(f, decl_block)) ok = false;
            } else if (tag == "C") {
                if (!have_cur || f.size() < 3) { ok = false; return completed; }
                NetworkSatSolution& s = satSolution(f[1]);
                s.has_clock  = true;
                s.clock_c0_m = std::stod(f[2]);
                // Optional positional fields: c1 (field 4, B3b), then the post-emit
                // clock disc counter (field 5, RTA-4).  Self-describing key=value
                // tokens (iode=, oiod=, oepoch=, ...) follow them (RTF-2).  A
                // field containing '=' is a key=value token, never a positional
                // number, so the positional parse stops at the first '=' field.  A
                // file without key=value tokens parses as c1 plus clock disc only.
                std::size_t k = 3;
                if (k < f.size() && f[k].find('=') == std::string::npos)
                    s.clock_c1_m_s = std::stod(f[k++]);                    // drift c1 (B3b)
                if (k < f.size() && f[k].find('=') == std::string::npos)   // clock disc (RTA-4)
                    cur.serialized_disc.clock[f[1]] = std::stoi(f[k++]);
                for (; k < f.size(); ++k) {   // brdc_rts anchor and orbit echo
                    if (f[k].rfind("iode=", 0) == 0) {
                        s.clock_iode = std::stoi(f[k].substr(5));
                    } else if (f[k].rfind("oiod=", 0) == 0) {
                        s.orbit_iod_ssr = std::stoi(f[k].substr(5));
                    } else if (f[k].rfind("oepoch=", 0) == 0) {
                        const std::string v = f[k].substr(7);
                        const auto colon = v.find(':');
                        if (colon != std::string::npos) {
                            s.orbit_epoch.week = std::stoi(v.substr(0, colon));
                            s.orbit_epoch.tow  = std::stod(v.substr(colon + 1));
                        }
                    } else if (f[k].rfind("sigcc=", 0) == 0) {   // combined code-correction σ (M4)
                        s.sigcc_m = std::stod(f[k].substr(6));
                    } else if (f[k].rfind("rho=", 0) == 0) {     // code/phase correction correlation (M4)
                        s.rho = std::stod(f[k].substr(4));
                    } else if (f[k].rfind("lvl=", 0) == 0) {     // declared level step
                        s.level_step_m = std::stod(f[k].substr(4));
                    }
                    // Unknown key=value tokens are ignored (forward-compatible).
                }
            } else if (tag == "B") {
                if (!have_cur || f.size() < 4) { ok = false; return completed; }
                satSolution(f[1]).code_bias_m[f[2]] = std::stod(f[3]);
                // Fields >= 5, in any order: the positional post-emit code disc counter
                // (RTA-4) and the `isc=` key=value token (RF-0f).  A field containing
                // '=' is never parsed as the counter, as with the C record's iode=/oiod=
                // tokens; otherwise `isc=obs` would reach std::stoi and throw on a file
                // whose producer did not serialize disc counters.
                for (std::size_t k = 4; k < f.size(); ++k) {
                    if (f[k].rfind("isc=", 0) == 0) {
                        // Only the value "obs" is recognized; any other value is ignored.
                        if (f[k].substr(4) == "obs")
                            satSolution(f[1]).isc_observed.insert(f[2]);
                    } else if (f[k].rfind("lvl=", 0) == 0) {   // R-A: declared follow step
                        satSolution(f[1]).cb_level_step_m[f[2]] = std::stod(f[k].substr(4));
                    } else if (f[k].find('=') == std::string::npos && !f[k].empty()) {
                        try {
                            cur.serialized_disc.code[f[1] + "|" + f[2]] = std::stoi(f[k]);
                        } catch (...) { /* malformed counter: ignored */ }
                    }
                }
            } else if (tag == "P") {
                if (!have_cur || f.size() < 6) { ok = false; return completed; }
                NetworkPhaseBias pb;
                pb.bias_m            = std::stod(f[3]);
                pb.signal_integer    = (std::stoi(f[4]) != 0);
                pb.wide_lane_integer = std::stoi(f[5]);
                // Fields >= 7, in any order: the positional post-emit phase disc counter
                // (a bare int, RTA-4) and key=value tokens such as the σ token
                // sigma=<val> (T3d).  Each field is dispatched by its shape, as in the
                // S line, so the two forms coexist.
                for (std::size_t k = 6; k < f.size(); ++k) {
                    if (f[k].rfind("sigma=", 0) == 0)
                        pb.sigma_m = std::stod(f[k].substr(6));
                    else if (f[k].rfind("sigc=", 0) == 0)   // combined phase-correction σ (M4)
                        pb.sigc_m = std::stod(f[k].substr(5));
                    else if (f[k].rfind("gen=", 0) == 0)    // hold generation
                        pb.hold_generation =
                            static_cast<std::uint32_t>(std::stoul(f[k].substr(4)));
                    else if (f[k].find('=') == std::string::npos)
                        cur.serialized_disc.phase[f[1] + "|" + f[2]] = std::stoi(f[k]);
                }
                satSolution(f[1]).phase_bias[f[2]] = pb;
            } else if (tag == "R") {   // retro-bump satellite set, per-sat disc (B1)
                if (!have_cur) { ok = false; return completed; }
                for (std::size_t k = 1; k < f.size(); ++k)
                    if (!f[k].empty()) cur.retro_sats.insert(f[k]);
            } else if (tag == "U") {   // per-sat URA quality channel (B1)
                if (!have_cur || f.size() < 3) { ok = false; return completed; }
                NetworkSatSolution& s = satSolution(f[1]);
                s.clock_ura_m = std::stod(f[2]);
                s.has_ura     = true;
            } else if (tag == "G") {   // per-epoch wall-clock time of generation (RT0-4)
                if (!have_cur || f.size() < 3) { ok = false; return completed; }
                cur.epoch.generation_time.week = std::stoi(f[1]);
                cur.epoch.generation_time.tow  = std::stod(f[2]);
            } else if (tag == "I") {   // per-epoch solution-generation id (RTA-3)
                if (!have_cur || f.size() < 2) { ok = false; return completed; }
                cur.epoch.generation_id = static_cast<uint32_t>(std::stoul(f[1]));
            } else if (tag == "H") {   // epoch-status heartbeat, positional (RTA-10)
                if (f.size() < 7) { ok = false; return completed; }
                // An H line always begins a new epoch unit: the producer writes it
                // immediately before an emitted epoch's E line, or standalone for a
                // fully withheld epoch.  It therefore completes the epoch under
                // assembly as the next E line would.  This bounds a live tail's
                // latency across withheld stretches: the withheld epoch's H both
                // flushes the previous emitted epoch and surfaces the status.
                if (have_cur) {
                    completed = std::move(cur);
                    cur       = NetworkProductRecord{};
                    sat_index.clear();
                    have_cur = false;
                }
                NetworkProducerStatus st;
                st.time.week        = std::stoi(f[1]);
                st.time.tow         = std::stod(f[2]);
                st.state            = NetworkProducerStatus::parseState(f[3]);
                st.sigma0           = std::stod(f[4]);
                st.n_sats_published = std::stoi(f[5]);
                st.withheld         = f[6];
                latest_status   = st;
                pending_h       = st;
                pending_h_valid = true;
                if (track_statuses) statuses.push_back(st);
            } else if (tag == "S") {   // S-basis contract: v2 header (B6) or re-broadcast (RTA-5)
                if (f.size() >= 11) {
                    SbasisContract c;
                    c.present = true;
                    c.decl.reference_receiver_id = f[1];
                    c.decl.bias_sign             = std::stod(f[2]);
                    c.decl.windup_yaw_model_id   = f[3];
                    c.decl.amb_datum       = static_cast<SbasisDeclaration::AmbDatum>(std::stoi(f[4]));
                    c.decl.isb_datum       = static_cast<SbasisDeclaration::IsbDatum>(std::stoi(f[5]));
                    c.decl.emit_code_bias  = (std::stoi(f[6]) != 0);
                    c.decl.code_bias_datum = static_cast<SbasisDeclaration::CodeBiasDatum>(std::stoi(f[7]));
                    c.disc_threshold_m     = std::stod(f[8]);
                    c.emit_clock_disc      = (std::stoi(f[9]) != 0);
                    c.apriori_product_id   = f[10];
                    // Trailing fields 12+: '<const>:<pivot_sat>' realized-pivot
                    // entries, plus key=value extensions such as 'gid=' / 'stale='
                    // (RTA-3, RTA-11) and 'frame=' / 'cepoch=' / 'atx=' / 'oref='
                    // (RTA-12).  Unknown shapes are ignored (forward-compatible).
                    for (std::size_t k = 11; k < f.size(); ++k) {
                        if (f[k].rfind("gid=", 0) == 0) {
                            c.generation_id = static_cast<uint32_t>(std::stoul(f[k].substr(4)));
                        } else if (f[k].rfind("stale=", 0) == 0) {
                            c.staleness_horizon_s = std::stod(f[k].substr(6));
                        } else if (f[k].rfind("cbjump=", 0) == 0) {
                            c.code_bias_jump_test = (std::stoi(f[k].substr(7)) != 0);
                        } else if (f[k].rfind("clkset=", 0) == 0) {
                            c.clock_disc_on_set_change = (std::stoi(f[k].substr(7)) != 0);
                        } else if (f[k].rfind("frame=", 0) == 0) {
                            c.frame_id = f[k].substr(6);
                        } else if (f[k].rfind("cepoch=", 0) == 0) {
                            c.coord_epoch = f[k].substr(7);
                        } else if (f[k].rfind("atx=", 0) == 0) {
                            c.atx_id = f[k].substr(4);
                        } else if (f[k].rfind("oref=", 0) == 0) {
                            c.orbit_ref = f[k].substr(5);
                        } else if (f[k].rfind("pdr=", 0) == 0) {
                            c.decl.eq34_datum_receiver = f[k].substr(4);   // diagnostic
                        } else if (f[k].rfind("agek=", 0) == 0) {
                            c.age_scale_k = std::stod(f[k].substr(5));      // age-scaling coeff (M4)
                        } else if (f[k].rfind("discfrac=", 0) == 0) {
                            c.disc_threshold_frac_cycles = std::stod(f[k].substr(9));  // per-λ jump gate (H3)
                        } else if (f[k].rfind("discscope=", 0) == 0) {
                            c.disc_mark_scope_channel = (f[k].substr(10) == "channel");  // disc mark scope
                        } else if (f[k].rfind("pbhold=", 0) == 0) {
                            const int ph = std::stoi(f[k].substr(7));
                            c.emit_pb_hold = (ph != 0);               // phase-bias hold
                            c.emit_pb_hold_phase_clock = (ph == 2);   // phase-clock mode
                            // pbhold=4 (the removed block member) still reads as a
                            // smoothed hold, so an older product file keeps its meaning.
                            c.emit_pb_hold_smoothed    = (ph == 3 || ph == 4);   // smoothed mode
                        } else if (f[k].rfind("hsig=", 0) == 0) {
                            c.h_sigma0_source = f[k].substr(5);   // sigma0 statistic id (H-rider)
                        } else if (f[k].rfind("cbpin=", 0) == 0) {
                            // Code-bias IF pin pair, "<const>:<s1>-<s2>" (§1.5).  Dispatched
                            // before the ':' pivot fallback below, because the value
                            // itself contains a ':'.
                            const std::string v = f[k].substr(6);
                            const auto colon = v.find(':');
                            if (colon != std::string::npos)
                                c.cb_pin_pair[v.substr(0, colon)] = v.substr(colon + 1);
                        } else if (f[k].rfind("gfpin=", 0) == 0) {
                            // Geometry-free datum pair, "<const>:<s1>-<s2>" (or the
                            // '+'-joined per-family form).  Dispatched before the ':' pivot
                            // fallback for the same reason as cbpin=.  The value after the
                            // first ':' is stored verbatim, so a future multi-family value
                            // survives a round-trip through a reader that predates it
                            // (Odijk rd 3b).
                            const std::string v = f[k].substr(6);
                            const auto colon = v.find(':');
                            if (colon != std::string::npos)
                                c.gf_pin_pair[v.substr(0, colon)] = v.substr(colon + 1);
                        } else if (f[k].rfind("sat_gf_datum=", 0) == 0) {
                            // The value the geometry-free datum was pinned at ("zero" |
                            // "broadcast_tgd"), stored verbatim so an unknown future mode
                            // survives a round-trip through this reader rather than being
                            // normalized away (RF-0d).  Dispatched before the ':' pivot
                            // fallback, like the two tokens above: a future value that
                            // contains a ':' would otherwise be read as a pivot entry.
                            c.sat_gf_datum = f[k].substr(13);
                        } else {
                            const auto colon = f[k].find(':');
                            if (colon != std::string::npos)
                                c.decl.clock_pivot_sat[f[k].substr(0, colon)] = f[k].substr(colon + 1);
                        }
                    }
                    // Repeated S lines (RTA-5): each receipt updates the reader's
                    // current contract state (the batch contract_out ends as the last
                    // one) and is also stamped on the record of the epoch it precedes
                    // (contract_refresh), so a consumer can re-run checkContract on
                    // every receipt at its position in the stream.
                    contract        = c;
                    pending_s       = c;
                    pending_s_valid = true;
                } else if (track_contract) {
                    ok = false;   // short/malformed S line: flag it, keep parsing epochs
                }
                // A short 'S' line without contract tracking (2-arg read) stays a
                // valid, ignored line.
            } else {
                ok = false;   // unknown tag: flag it, keep parsing
            }
        } catch (const std::exception&) {
            ok = false;       // malformed numeric field: flag it, skip the rest of the line
        }
        return completed;
    }

    /// Flush the trailing epoch (batch read at EOF).  A live tail never calls
    /// this: its trailing epoch may still be growing on disk.
    std::optional<NetworkProductRecord> finish() {
        std::optional<NetworkProductRecord> completed;
        closeDeclBlock();   // a trailing N block still becomes the reader's current one
        if (have_cur) {
            completed = std::move(cur);
            cur = NetworkProductRecord{};
            sat_index.clear();
            have_cur = false;
        }
        return completed;
    }
};

} // namespace

// ===========================================================================
// NetworkProductWriter
// ===========================================================================

NetworkProductWriter::NetworkProductWriter(const std::string& path)
    : path_(path) {
    // Create the parent directory if it does not exist, so a relative path like
    // "logs/network_products.txt" does not fail the open just because the dir is
    // absent (ofstream does not create directories).
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    out_.open(path, std::ios::out | std::ios::trunc);
    writeHeader_(nullptr);   // v1 header (byte-identical to the pre-B6 format)
}

NetworkProductWriter::NetworkProductWriter(const std::string& path, const SbasisContract& contract)
    : path_(path) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    out_.open(path, std::ios::out | std::ios::trunc);
    writeHeader_(&contract);  // v2 header carrying the S-basis contract (B6)
}

void NetworkProductWriter::writeSLine_(const SbasisContract& contract) {
    // S-basis contract line (B6).  It is a real 'S' record, not a '#' comment, so
    // the reader ingests it:
    //   S,ref_rx,bias_sign,windup_id,amb_datum,isb_datum,emit_cb,cb_datum,
    //     disc_thresh_m,emit_clock_disc,apriori_product_id[,<const>:<pivot_sat>...]
    //     [,gid=<generation_id>][,stale=<staleness_horizon_s>]
    //     [,frame=<frame_id>][,cepoch=<coord_epoch>][,atx=<atx_id>][,oref=<orbit_ref>]
    // The replay emitter's seven construction parameters all come from this line:
    // disc_thresh_m, emit_clock_disc, the optional stale= horizon (RTA-11), cbjump=,
    // clkset=, discfrac= and discscope=.  A consumer therefore replays
    // the counters with no out-of-band producer config
    // (NetworkProductReader::makeReplayEmitter).  Trailing fields 12+ are
    // distinguished by shape: a field with ':' is a realized pivot entry, and
    // 'gid=' / 'stale=' plus the RTA-12 identity keys 'frame=' / 'cepoch=' /
    // 'atx=' / 'oref=' are key=value extensions.  Each extension is written only
    // when armed or non-empty, so a run with them off writes a byte-identical v2
    // header; the reader ignores unknown shapes.  The v2 header and the mid-file
    // re-broadcast (RTA-5) both write the line through this function.
    out_ << "S," << contract.decl.reference_receiver_id
         << ',' << fmt::format("{}", contract.decl.bias_sign)
         << ',' << contract.decl.windup_yaw_model_id
         << ',' << static_cast<int>(contract.decl.amb_datum)
         << ',' << static_cast<int>(contract.decl.isb_datum)
         << ',' << (contract.decl.emit_code_bias ? 1 : 0)
         << ',' << static_cast<int>(contract.decl.code_bias_datum)
         << ',' << fmt::format("{}", contract.disc_threshold_m)
         << ',' << (contract.emit_clock_disc ? 1 : 0)
         << ',' << contract.apriori_product_id;
    for (const auto& [cst, sat] : contract.decl.clock_pivot_sat)
        out_ << ',' << cst << ':' << sat;
    if (contract.generation_id != 0)          // only when armed, for byte-identity (RTA-3)
        out_ << ",gid=" << contract.generation_id;
    if (contract.staleness_horizon_s > 0.0)   // only when armed, for byte-identity (RTA-11)
        out_ << ",stale=" << fmt::format("{}", contract.staleness_horizon_s);
    if (contract.code_bias_jump_test)         // only when armed (byte-identity)
        out_ << ",cbjump=1";
    if (contract.clock_disc_on_set_change)    // only when armed (byte-identity)
        out_ << ",clkset=1";
    // Identity fields (RTA-12), each written only when non-empty, so a producer
    // that declares none of them writes the S line without them, byte-for-byte.
    if (!contract.frame_id.empty())
        out_ << ",frame=" << contract.frame_id;
    if (!contract.coord_epoch.empty())
        out_ << ",cepoch=" << contract.coord_epoch;
    if (!contract.atx_id.empty())
        out_ << ",atx=" << contract.atx_id;
    if (!contract.orbit_ref.empty())
        out_ << ",oref=" << contract.orbit_ref;
    // Diagnostic: the datum receiver r1 (SingleReceiverChain).  Written only
    // when non-empty, so a non-eq34 producer's S line is byte-identical without it.
    if (!contract.decl.eq34_datum_receiver.empty())
        out_ << ",pdr=" << contract.decl.eq34_datum_receiver;
    // Age-scaling coefficient for the combined-correction variance (M4, contract v2).
    // Written only when non-zero, so a producer that does not advertise age scaling
    // writes the S line without it, byte-for-byte.  It is a quality hint, not a
    // replay-emitter parameter, so makeReplayEmitter / checkContract ignore it.
    if (contract.age_scale_k != 0.0)
        out_ << ",agek=" << fmt::format("{}", contract.age_scale_k);
    // Per-λ fractional-cycle discontinuity threshold (H3).  Written only when > 0,
    // so a flat-metre producer's S line is byte-identical without it.  Unlike agek=,
    // this is a replay-emitter parameter: makeReplayEmitter applies it and
    // checkContract compares it, since a per-λ vs flat mismatch mis-counts
    // discontinuities.
    if (contract.disc_threshold_frac_cycles > 0.0)
        out_ << ",discfrac=" << fmt::format("{}", contract.disc_threshold_frac_cycles);
    // The statistic the H heartbeat's sigma0 field carries (H-rider).  Written only
    // when non-empty, so a producer on the RTA-10 default writes the S line
    // byte-for-byte.  It is an interpretation hint (a consumer thresholding σ̂₀ needs
    // to know which population it reads); makeReplayEmitter / checkContract ignore it.
    if (!contract.h_sigma0_source.empty())
        out_ << ",hsig=" << contract.h_sigma0_source;
    // The code-bias IF pin pair per constellation: the signal pair in which the
    // emitted OSB stream is internally consistent ("cbpin=G:C1C-C2L").
    // selectIfPinPair pins the lexically first pair, which is not the
    // RTKLIB-convention (C1W,C2W) pair consumers typically form; a consumer that
    // mixes frames (one signal inside the pinned pair and one outside, e.g. C1C
    // with C2W) pays the §1.5 mixed-pair penalty (day-129 frame-guards report).
    // Written only when non-empty, so a producer that does not declare it writes
    // the S line byte-for-byte.  It is an interpretation hint, not a replay-emitter
    // parameter: makeReplayEmitter ignores it, and checkContract compares it only
    // for constellations the consumer declares (clock_pivot_sat semantics).
    for (const auto& [cst, pair] : contract.cb_pin_pair)
        out_ << ",cbpin=" << cst << ':' << pair;
    // The geometry-free datum pair (the retired network.code_bias_gf_datum, Odijk rd
    // 3b; the producer no longer fills it, so it is written only for a contract read
    // from such a product).  Its presence is the payload: it states that the emitted
    // code biases are the slant-posture estimable function (pinned pair ≡ datum,
    // content only from j >= 3, each satellite's GF bias absorbed by the ionosphere per
    // Table 8) rather than the single-layer one, on an otherwise identical record
    // layout.  Written only when non-empty, so a single-layer producer writes the S
    // line byte-for-byte.  It is an interpretation hint, not a replay-emitter
    // parameter: makeReplayEmitter ignores it, and checkContract compares it only for
    // constellations the consumer declares.
    for (const auto& [cst, pair] : contract.gf_pin_pair)
        out_ << ",gfpin=" << cst << ':' << pair;
    // The value that geometry-free datum was pinned at (network.sat_gf_datum, RF-0d).
    // It is a separate token because gfpin= carries only presence and a signal pair:
    // the "zero" and "broadcast_tgd" frames differ by c*T_GD on every emitted code
    // bias of the satellite, with the opposite shift in the ionosphere the consumer
    // estimates for itself.  Written only when non-empty, so a producer without it
    // writes the S line byte-for-byte.  A reader that does not know the token skips
    // it, so the addition is forward-compatible.
    if (!contract.sat_gf_datum.empty())
        out_ << ",sat_gf_datum=" << contract.sat_gf_datum;
    // A per-satellite discontinuity mark bumped only the channels the marking site
    // reported (network.disc_mark_scope).  A replay-emitter parameter in the
    // same sense as discfrac=, with one limit: the R record carries no channel set,
    // so a replay re-marks with the full satellite scope regardless.  The token lets
    // checkContract report that divergence.  Written only when armed, so a producer
    // without it writes the S line byte-for-byte.
    if (contract.disc_mark_scope_channel)
        out_ << ",discscope=channel";
    // The emitted phase bias is held at the first value of its generation
    // (network.emit_pb_hold), so a P record's bias_m is constant over a
    // generation, not a per-epoch estimate.  This governs how the whole phase-bias
    // channel is read, so it is declared on the contract line and not only on the
    // per-record gen= token.  A reader that sees pbhold=1 knows a phase_disc bump
    // without a gen= change carries no value step; a gen= token without pbhold=1
    // means the file's contract and payload disagree, which checkContract reports.
    // Written only when armed, so a producer without it writes the S line
    // byte-for-byte.
    // pbhold=2: the hold keeps the phase-visible clock instead, so a held
    // bias_m moves with the satellite's common-removed clock within a generation.
    // pbhold=3: the smoothed hold, a held bias_m follows the clock clone.
    // (pbhold=4, the removed block member, is no longer written.)
    if (contract.emit_pb_hold)
        out_ << (contract.emit_pb_hold_smoothed ? ",pbhold=3"
                 : contract.emit_pb_hold_phase_clock ? ",pbhold=2" : ",pbhold=1");
    out_ << '\n';
}

void NetworkProductWriter::writeContract(const SbasisContract& contract) {
    if (!out_) return;
    writeSLine_(contract);   // mid-file re-broadcast in the header grammar (RTA-5)
}

void NetworkProductWriter::writeEpochStatus(const NetworkProducerStatus& status) {
    if (!out_) return;
    // Heartbeat (RTA-10): H,week,tow,status,sigma0,n_sats_published,withheld.  The
    // tow uses the same shortest-round-trip formatting as the E line, so the reader's
    // pending-H week/tow match (LineAssembler) compares exactly equal doubles for an
    // emitted epoch's own heartbeat.  sigma0 is written with 4 decimals per the RTA-10
    // grammar (0 when unavailable); an empty withheld list is written as "-".  The
    // record is declared in the '#' header legend like every record (handoff-§7
    // schema fix), is never fed to replay, and has purely positional fields.
    out_ << "H," << status.time.week << ',' << fmt::format("{}", status.time.tow)
         << ',' << NetworkProducerStatus::stateName(status.state)
         << ',' << fmt::format("{:.4f}", status.sigma0)
         << ',' << status.n_sats_published
         << ',' << (status.withheld.empty() ? "-" : status.withheld) << '\n';
}

void NetworkProductWriter::writeHeader_(const SbasisContract* contract) {
    if (!out_) return;
    if (contract) {
        out_ << "# gnss_network_product v2\n";
        writeSLine_(*contract);
    } else {
        out_ << "# gnss_network_product v1\n";
    }
    // The legend below is grammar-complete: it declares every record tag and every
    // optional field the writer can emit.  '[]' marks fields present only under
    // their producer toggle; 'key=' tokens are self-describing.  The legend is not
    // frozen to the base format: a legend that omits an optional field leads a
    // schema-faithful consumer to drop that field (the phase-bias σ in sigma= was
    // dropped this way).  v1 and v2 headers share the legend verbatim and differ
    // only in the banner and the S line.  Extending the wire format requires
    // extending the legend; HeaderLegendDeclaresEveryEmittedField in
    // test_network_product_io.cpp enforces it.  Legend lines are '#' comments: the
    // reader skips them and replay is unaffected.  A cross-build SHA-256 of whole
    // product files also covers the legend; to compare only the data stream, strip
    // the '^#' lines first.  (Schema fix, DOY-129 handoff §7.)
    out_ << "# fields in [] are optional (present only when the producing toggle is armed);\n"
            "# 'key=value' tokens are self-describing; unknown keys are ignorable.\n"
            "# E,week,tow,iod_ssr,provider_id,solution_id,datum_changed[,datum_consts]\n"
            "# C,sat,clock_c0_m[,clock_c1_m_s[,clock_disc]][,iode=<n>[,oiod=<n>][,oepoch=<week>:<tow>]][,sigcc=<m>[,rho=<r>]][,lvl=<m>]\n"
            "# B,sat,signal,code_bias_m[,code_disc][,isc=obs][,lvl=<m>]\n"
            "# P,sat,signal,bias_m,signal_integer,wide_lane_integer[,phase_disc][,sigma=<m>][,sigc=<m>][,gen=<n>]\n"
            "# G,week,tow                    (RT0-4 wall-clock time-of-generation)\n"
            "# I,generation_id               (RTA-3 solution-generation id)\n"
            "# R,retro_sat[,retro_sat...]    (B1 per-satellite discontinuity bump)\n"
            "# U,sat,clock_ura_m             (B1 per-satellite URA quality, 1-sigma m)\n"
            "# H,week,tow,status,sigma0,n_sats_published,withheld   (RTA-10 epoch-status heartbeat)\n"
            "# S,ref_rx,bias_sign,windup_id,amb_datum,isb_datum,emit_cb,cb_datum,disc_thresh_m,"
            "emit_clock_disc,apriori_id[,<const>:<pivot_sat>...][,gid=<n>][,stale=<s>][,cbjump=1]"
            "[,clkset=1][,frame=<id>][,cepoch=<e>][,atx=<f>][,oref=<CoM|APC>][,pdr=<rx>][,agek=<k>]"
            "[,discfrac=<c>][,hsig=<src>][,cbpin=<const>:<s1>-<s2>...]"
            "[,gfpin=<const>:<s1>-<s2>...][,sat_gf_datum=<mode>]"
            "[,discscope=channel][,pbhold=1]"
            "   (B6 S-basis contract)\n"
            "# N,constellation,band_digit[,ph=<sig>[+<sig>...]][,co=<sig>[+<sig>...]]"
            "[,comp=<sig>[+<sig>...]][,dph=<sig>][,dco=<sig>]"
            "   (declared signal roster; a contiguous N block"
            " REPLACES the previous declaration wholesale)\n";
}

std::size_t NetworkProductWriter::writeSignalDeclaration(
        const NetworkSignalDeclaration& decl) {
    if (!out_ || !decl.present) return 0;
    const std::vector<std::string> lines = signalDeclarationToWire(decl);
    for (const std::string& l : lines) out_ << l << '\n';
    return lines.size();
}

void NetworkProductWriter::write(const NetworkSolutionEpoch& sol, bool datum_changed,
                                 const std::set<char>& datum_consts,
                                 const std::set<std::string>& retro_sats,
                                 const NetworkProductEmitter* disc_source) {
    if (!out_) return;

    out_ << "E," << sol.epoch_time.week << ',' << fmt::format("{}", sol.epoch_time.tow)
         << ',' << sol.iod_ssr << ',' << sol.provider_id << ',' << sol.solution_id
         << ',' << (datum_changed ? 1 : 0);
    // The affected-constellation set is appended as field 8 only when non-empty, so
    // a run with per-constellation scoping off writes the 7-field E line
    // byte-for-byte.  Replay of a file lacking field 8 falls back to a global bump
    // (A2).
    if (!datum_consts.empty())
        out_ << ',' << std::string(datum_consts.begin(), datum_consts.end());
    out_ << '\n';
    // Optional per-epoch wall-clock time of generation as a separate G record
    // ("G,week,tow"), written only when the producer stamped it
    // (generation_time.week != 0), so a run with network.emit_generation_time off is
    // byte-identical.  A consumer measures latency as its ingest wall-clock minus
    // this generation time (RT0-4).
    if (sol.generation_time.week != 0)
        out_ << "G," << sol.generation_time.week << ','
             << fmt::format("{}", sol.generation_time.tow) << '\n';
    // Optional solution-generation id as a separate I record ("I,<id>"), written
    // only when the producer stamped it (network.emit_generation_id), so a run with
    // it off is byte-identical.  Serializing it per epoch, as the G record is, keeps
    // replay deterministic (the id is read back from the file itself) and lets a
    // tail reader recover it from any epoch block (RTA-3).
    if (sol.generation_id != 0)
        out_ << "I," << sol.generation_id << '\n';
    // The retro-bump satellite set as a separate R record (comma-separated sat ids,
    // "R,G13,E09"; parsed by splitCsv), written only when non-empty, so a run with
    // retro-bump off is byte-identical (B1).
    if (!retro_sats.empty()) {
        out_ << "R";
        for (const std::string& sat : retro_sats) out_ << ',' << sat;
        out_ << '\n';
    }

    // With disc_source non-null, each C/B/P record gains a trailing field with the
    // emitter's current counter (RTA-4).  The driver calls write() after
    // emitter.emitToStore(sol, store), so these are the post-emit counters for this
    // epoch, byte-identical to what the store records received.  The C line then
    // always carries the c1 drift field (0 when absent), so the clock counter sits
    // at fixed field 5 and cannot be misparsed as a drift.
    for (const auto& s : sol.sats) {
        if (s.has_clock) {
            out_ << "C," << s.sat << ',' << fmt::format("{}", s.clock_c0_m);
            if (disc_source) {
                out_ << ',' << fmt::format("{}", s.clock_c1_m_s)
                     << ',' << disc_source->clockDiscontinuityCounter(s.sat);
            } else if (s.clock_c1_m_s != 0.0) {  // optional drift field 4, only when clock_model emits it (B3b)
                out_ << ',' << fmt::format("{}", s.clock_c1_m_s);
            }
            // brdc_rts clock ephemeris-anchor and orbit-echo fields as trailing
            // key=value tokens (RTF-2).  They are self-describing and
            // position-independent, because the c1/clock_disc positional fields above
            // vary in count and orbit_iod_ssr/orbit_epoch are independently optional.
            // Written only when the producer stamped a broadcast IODE (clock_iode >= 0,
            // i.e. brdc_rts mode); sp3 mode leaves clock_iode = -1, so nothing is
            // appended and the C line is byte-identical.  A field containing '=' is a
            // key=value token, never a positional number, so the reader's positional
            // c1/clock_disc parse stops at the first such field.
            if (s.clock_iode >= 0) {
                out_ << ",iode=" << s.clock_iode;
                if (s.orbit_iod_ssr >= 0) out_ << ",oiod=" << s.orbit_iod_ssr;
                if (s.orbit_epoch.week != 0)
                    out_ << ",oepoch=" << s.orbit_epoch.week << ':'
                         << fmt::format("{}", s.orbit_epoch.tow);
            }
            // Optional per-sat combined code-correction σ and code/phase correlation as
            // key=value tokens (M4, contract v2).  A field with '=' ends the reader's
            // positional c1/clock_disc parse, so they coexist with the disc counter as
            // iode= does.  sigcc_m is nonzero only when network.emit_combined_sigma is
            // armed, so a run with it off emits nothing and is byte-identical.  rho is
            // written only alongside sigcc: it correlates the combined code with the
            // combined phase and has no meaning without a combined code σ.
            if (s.sigcc_m > 0.0 && std::isfinite(s.sigcc_m)) {
                out_ << ",sigcc=" << fmt::format("{}", s.sigcc_m);
                if (std::isfinite(s.rho))
                    out_ << ",rho=" << fmt::format("{}", s.rho);
            }
            // The satellite's level step since its previous emission: the declaration
            // the phase-bias jump test needs to reproduce the producer's counters on a
            // replay.  Written only when nonzero and finite, so an unarmed
            // wire is byte-identical.
            if (s.level_step_m != 0.0 && std::isfinite(s.level_step_m))
                out_ << ",lvl=" << fmt::format("{}", s.level_step_m);
            out_ << '\n';
        }
        for (const auto& [signal, bias_m] : s.code_bias_m) {
            out_ << "B," << s.sat << ',' << signal << ',' << fmt::format("{}", bias_m);
            if (disc_source)
                out_ << ',' << disc_source->codeBiasDiscontinuityCounter(s.sat, signal);
            // A separate per-record token, not an overload of an existing one (RF-0f).
            // It states that this code bias is `b(pinned band-2 signal) + ISC^s` with
            // the ISC estimated: some station tracked both signals of this satellite,
            // so the difference is data, not an assertion.  The producer emits only
            // such companion records; a pinned ISC is an assertion of zero that no
            // value distinguishes from an estimate, so it is withheld instead of
            // flagged.  Absent unless network.sat_isc_state is armed, so a run with it
            // off writes byte-identical records.
            if (s.isc_observed.count(signal)) out_ << ",isc=obs";
            // Return plan R-A: the free column's follow step since the
            // satellite's previous emission, the declaration the code-bias jump test
            // needs to reproduce the producer's counters on a replay.  Written ONLY
            // when nonzero, so an unarmed wire is byte-identical.
            {
                const auto lv = s.cb_level_step_m.find(signal);
                if (lv != s.cb_level_step_m.end() && lv->second != 0.0
                    && std::isfinite(lv->second))
                    out_ << ",lvl=" << fmt::format("{}", lv->second);
            }
            out_ << '\n';
        }
        for (const auto& [signal, pb] : s.phase_bias) {
            out_ << "P," << s.sat << ',' << signal << ',' << fmt::format("{}", pb.bias_m)
                 << ',' << (pb.signal_integer ? 1 : 0) << ',' << pb.wide_lane_integer;
            if (disc_source)
                out_ << ',' << disc_source->discontinuityCounter(s.sat, signal);
            // Optional trailing σ as a key=value token, so it never collides with the
            // positional disc field (T3d, B1b).  pb.sigma_m is nonzero only when
            // emit_phase_bias_sigma is armed, so a run with it off emits no token and
            // is byte-identical.  Same pattern as the C line's iode= tail.
            if (pb.sigma_m > 0.0 && std::isfinite(pb.sigma_m))
                out_ << ",sigma=" << fmt::format("{}", pb.sigma_m);
            // Optional combined phase-correction σ as a key=value token beside sigma=
            // (M4, contract v2).  pb.sigc_m is nonzero only when
            // network.emit_combined_sigma is armed, so a run with it off emits no token
            // and is byte-identical.  Distinct from sigma= (the phase-bias term alone):
            // sigc is the σ of clock and phase bias combined.
            if (pb.sigc_m > 0.0 && std::isfinite(pb.sigc_m))
                out_ << ",sigc=" << fmt::format("{}", pb.sigc_m);
            // The hold generation as a self-describing key=value token beside sigma= /
            // sigc=.  pb.hold_generation is 0 unless network.emit_pb_hold
            // held this key, so a run with it off emits no token and is byte-identical.
            // A change of gen= is the declaration a file consumer refloats on; the
            // token's presence states that bias_m is constant for the life of that
            // generation, so a phase_disc bump on the same record carries no value step.
            if (pb.hold_generation != 0)
                out_ << ",gen=" << pb.hold_generation;
            out_ << '\n';
        }
        if (s.has_ura)   // per-sat URA; U lines only when network.emit_ura is on (B1)
            out_ << "U," << s.sat << ',' << fmt::format("{}", s.clock_ura_m) << '\n';
    }
}

// ===========================================================================
// NetworkProductReader
// ===========================================================================

std::vector<NetworkProductRecord> NetworkProductReader::read(const std::string& path, bool* ok) {
    return read(path, nullptr, ok);   // delegate with no contract wanted (B6)
}

std::vector<NetworkProductRecord> NetworkProductReader::read(const std::string& path,
                                                             SbasisContract* contract_out,
                                                             bool* ok,
                                                             std::vector<NetworkProducerStatus>* statuses_out) {
    std::vector<NetworkProductRecord> out;
    if (contract_out) *contract_out = SbasisContract{};   // present=false until an S line is seen
    if (statuses_out) statuses_out->clear();
    std::ifstream in(path);
    if (!in) { if (ok) *ok = false; return out; }

    // The per-tag parse semantics live in the shared LineAssembler (one code
    // path with the RTA-6 NetworkProductTail); this loop only feeds it lines
    // and flushes the trailing epoch at EOF.
    LineAssembler assembler;
    assembler.track_contract = (contract_out != nullptr);
    assembler.track_statuses = (statuses_out != nullptr);

    std::string line;
    while (std::getline(in, line))
        if (auto rec = assembler.consume(line)) out.push_back(std::move(*rec));
    if (auto rec = assembler.finish()) out.push_back(std::move(*rec));

    if (contract_out && assembler.contract.present) *contract_out = assembler.contract;
    if (ok) *ok = assembler.ok;
    if (statuses_out) *statuses_out = std::move(assembler.statuses);
    return out;
}

void NetworkProductReader::replay(const NetworkProductRecord& rec,
                                  SsrCorrectionStore& store,
                                  NetworkProductEmitter& emitter) {
    // Discontinuity counters (phase and code) are serialized only under
    // network.serialize_disc_counters (RTA-4, handled below).  The emitter
    // recomputes them deterministically from inputs reproduced here: (1) the
    // per-epoch solution itself (phase-bias jumps; for code, the per-(const,signal)
    // satellite set and per-satellite distinct-band composition of code_bias_m,
    // which is the producer's gauge structure), and (2) the serialized datum_changed
    // flag, re-applied via markDatumChange() to force the same clock-pivot bump, plus
    // the R record's retro-bump below.  Replaying the emitted epoch sequence in order
    // therefore reproduces the live store byte-for-byte, including every
    // discontinuity counter.
    // The datum change is re-applied scoped to the serialized affected-constellation
    // set (E-line field 8).  An empty set (files without field 8, scoping-off runs)
    // forces a global bump (A2).
    // The producer's declared signal roster reaches the consumer store here, before
    // the epoch's records, so a consumer reading this epoch already holds the
    // declaration that describes it.  A wire without an N
    // block never calls this, so the store's declaration stays absent and every
    // consumer gating on presence is unchanged.
    if (rec.declaration_refresh) store.setDeclaredRoster(*rec.declaration_refresh);
    if (rec.datum_changed) emitter.markDatumChange(rec.datum_consts);
    // Re-apply the per-satellite retro-bump (R record) so the consumer-visible
    // discontinuity counters reproduce byte-for-byte on replay (B1).
    // The R record carries satellite ids and no channel set, so the default
    // kDiscChanAll is the only reconstruction available here: a replay of a
    // channel-scoped producer re-marks more channels than the producer did.  The
    // scope travels in the S contract (discscope=) so checkContract reports it;
    // closing the gap needs an R-record grammar extension (open item).
    if (!rec.retro_sats.empty()) emitter.markSatDiscontinuity(rec.retro_sats);
    emitter.emitToStore(rec.epoch, store);

    // When the record carries serialized post-emit counter values
    // (network.serialize_disc_counters, RTA-4), the deterministic recompute above
    // serves as a self-check.  For a full-file replay the two agree.  A
    // disagreement is expected for a reader that joined mid-file (every key
    // recomputes from a first sighting = 0), which is the case the wire values
    // exist for: warn, adopt the file's values into the emitter, and re-emit the
    // epoch so the reconstructed store records carry them.  The re-emit is
    // idempotent: the baselines and satellite-set trackers already equal this
    // epoch's content and the first emit consumed every forced mark, so no counter
    // steps; only the pushed records are re-stamped with the adopted counters.
    const auto& sd = rec.serialized_disc;
    if (sd.any()) {
        auto splitKey = [](const std::string& key, std::string& sat, std::string& sig) {
            const auto bar = key.find('|');
            sat = key.substr(0, bar == std::string::npos ? key.size() : bar);
            sig = bar == std::string::npos ? std::string() : key.substr(bar + 1);
        };
        std::size_t n_mismatch = 0;
        std::string first_mismatch;
        auto miss = [&](const std::string& what, const std::string& key, int filed, int recomputed) {
            if (n_mismatch++ == 0)
                first_mismatch = fmt::format("{} {}: file {} vs replay {}", what, key, filed, recomputed);
        };
        for (const auto& [sat, v] : sd.clock)
            if (emitter.clockDiscontinuityCounter(sat) != v)
                miss("clock", sat, v, emitter.clockDiscontinuityCounter(sat));
        for (const auto& [key, v] : sd.code) {
            std::string sat, sig; splitKey(key, sat, sig);
            if (emitter.codeBiasDiscontinuityCounter(sat, sig) != v)
                miss("code", key, v, emitter.codeBiasDiscontinuityCounter(sat, sig));
        }
        for (const auto& [key, v] : sd.phase) {
            std::string sat, sig; splitKey(key, sat, sig);
            if (emitter.discontinuityCounter(sat, sig) != v)
                miss("phase", key, v, emitter.discontinuityCounter(sat, sig));
        }
        if (n_mismatch > 0) {
            auto lg = spdlog::get("app");
            if (!lg) lg = spdlog::default_logger();
            if (lg)
                lg->warn("NetworkProductReader::replay (RTA-4): {} serialized disc-counter "
                         "value(s) disagree with the replay recompute at epoch tow {:.1f} "
                         "(first: {}) — adopting the file's post-emit values (expected for a "
                         "mid-file / tail join; a mismatch on a FULL-file replay indicates a "
                         "producer/replay divergence).",
                         n_mismatch, rec.epoch.epoch_time.tow, first_mismatch);
            emitter.adoptDiscontinuityCounters(sd.phase, sd.code, sd.clock);
            emitter.emitToStore(rec.epoch, store);   // re-stamp store records (idempotent)
        }
    }
}

std::string NetworkProductReader::csvSanitize(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c == ',') c = ';';
    return o;
}

bool NetworkProductReader::checkContract(const SbasisContract& file,
                                         const SbasisContract& expected,
                                         std::vector<std::string>* mismatches) {
    bool ok = true;
    auto note = [&](const std::string& m) { ok = false; if (mismatches) mismatches->push_back(m); };
    if (!file.present)
        note("product carries no S-basis contract (v1 / pre-B6 file) — cannot verify compatibility");
    if (file.decl.reference_receiver_id != expected.decl.reference_receiver_id)
        note(fmt::format("reference_receiver_id: file '{}' vs expected '{}'",
                         file.decl.reference_receiver_id, expected.decl.reference_receiver_id));
    if (file.decl.bias_sign != expected.decl.bias_sign)
        note(fmt::format("bias_sign: file {} vs expected {}",
                         file.decl.bias_sign, expected.decl.bias_sign));
    if (file.decl.windup_yaw_model_id != expected.decl.windup_yaw_model_id)
        note(fmt::format("windup_yaw_model_id: file '{}' vs expected '{}'",
                         file.decl.windup_yaw_model_id, expected.decl.windup_yaw_model_id));
    if (file.decl.amb_datum != expected.decl.amb_datum)       note("amb_datum differs");
    if (file.decl.isb_datum != expected.decl.isb_datum)       note("isb_datum differs");
    if (file.decl.emit_code_bias != expected.decl.emit_code_bias)
        note(fmt::format("emit_code_bias: file {} vs expected {}",
                         file.decl.emit_code_bias, expected.decl.emit_code_bias));
    if (file.decl.code_bias_datum != expected.decl.code_bias_datum) note("code_bias_datum differs");
    // Clock pivots are checked per constellation, only for the constellations the
    // consumer declares an expectation for (as with apriori_product_id below).  A
    // consumer typically pins one pivot via config while the file carries every
    // realized pivot, and the extra file entries are not a mismatch.
    for (const auto& [cst, sat] : expected.decl.clock_pivot_sat) {
        const auto fit = file.decl.clock_pivot_sat.find(cst);
        if (fit == file.decl.clock_pivot_sat.end())
            note(fmt::format("clock_pivot_sat[{}]: file has none vs expected '{}'", cst, sat));
        else if (fit->second != sat)
            note(fmt::format("clock_pivot_sat[{}]: file '{}' vs expected '{}'",
                             cst, fit->second, sat));
    }
    // The code-bias IF pin pair (§1.5), checked per constellation, only for the
    // constellations the consumer declares an expectation for (clock_pivot_sat
    // semantics).  A mismatch means the consumer's chosen signal pair is not in the
    // product's internally consistent frame: its receiver code-bias state then
    // absorbs a between-channel differential against the moving satellite OSBs,
    // the measured mixed-pair regime, which is worse than either consistent pair.
    for (const auto& [cst, pair] : expected.cb_pin_pair) {
        const auto fit = file.cb_pin_pair.find(cst);
        if (fit == file.cb_pin_pair.end())
            note(fmt::format("cb_pin_pair[{}]: file declares none vs expected '{}'", cst, pair));
        else if (fit->second != pair)
            note(fmt::format("cb_pin_pair[{}]: file '{}' vs expected '{}'",
                             cst, fit->second, pair));
    }
    // The geometry-free datum pair (Odijk rd 3b), with the same per-constellation,
    // declared-expectations-only semantics.  A mismatch here is more severe than the
    // cbpin one: "file declares none" means the consumer expects the slant-posture
    // estimable function (pinned pair ≡ datum, GF lump in the ionosphere) but the
    // file is single-layer, so the two disagree about what the B records are, not
    // only about which frame they are consistent in.
    for (const auto& [cst, pair] : expected.gf_pin_pair) {
        const auto fit = file.gf_pin_pair.find(cst);
        if (fit == file.gf_pin_pair.end())
            note(fmt::format("gf_pin_pair[{}]: file declares none (single-layer code-bias "
                             "datum) vs expected '{}' (geometry-free datum)", cst, pair));
        else if (fit->second != pair)
            note(fmt::format("gf_pin_pair[{}]: file '{}' vs expected '{}'",
                             cst, fit->second, pair));
    }
    // Replay-emitter construction parameters: a divergence in one of them means a
    // consumer replaying with its own emitter config would reproduce different
    // discontinuity counters from the producer's live store.  Six of the eight are
    // compared below; code_bias_jump_test and clock_disc_on_set_change are not.
    if (file.disc_threshold_m != expected.disc_threshold_m)
        note(fmt::format("disc_threshold_m: file {} vs expected {}",
                         file.disc_threshold_m, expected.disc_threshold_m));
    // The per-λ fractional-cycle threshold (H3): a per-λ vs flat mismatch, or a
    // differing fraction, makes the consumer's replay emitter mis-count
    // discontinuities.
    if (file.disc_threshold_frac_cycles != expected.disc_threshold_frac_cycles)
        note(fmt::format("disc_threshold_frac_cycles: file {} vs expected {}",
                         file.disc_threshold_frac_cycles, expected.disc_threshold_frac_cycles));
    // The per-channel mark scope.  A mismatch means the producer declared
    // a different channel population from the one the replay reconstructs.  Even a
    // match does not make the replay exact, because the R record carries no channel
    // set (see the field's doc); the check makes the scope visible to a consumer.
    if (file.disc_mark_scope_channel != expected.disc_mark_scope_channel)
        note(fmt::format("disc_mark_scope_channel: file {} vs expected {}",
                         file.disc_mark_scope_channel, expected.disc_mark_scope_channel));
    // The emit-side phase-bias hold.  This is not a replay-emitter
    // parameter but a statement about how the P records are read: when held, a
    // bias_m is constant over a generation and a phase_disc bump carries no value
    // step, so a consumer that assumes the unheld semantics either ignores a real
    // refloat signal or infers a step that is not there.  Reported in both
    // directions.
    if (file.emit_pb_hold != expected.emit_pb_hold)
        note(fmt::format("emit_pb_hold: file {} vs expected {}",
                         file.emit_pb_hold, expected.emit_pb_hold));
    // The phase-clock mode changes what a held bias_m is (the phase-visible
    // clock is the constant), so a disagreement is reported like the hold itself.
    if (file.emit_pb_hold_phase_clock != expected.emit_pb_hold_phase_clock)
        note(fmt::format("emit_pb_hold_phase_clock: file {} vs expected {}",
                         file.emit_pb_hold_phase_clock, expected.emit_pb_hold_phase_clock));
    if (file.emit_pb_hold_smoothed != expected.emit_pb_hold_smoothed)
        note(fmt::format("emit_pb_hold_smoothed: file {} vs expected {}",
                         file.emit_pb_hold_smoothed, expected.emit_pb_hold_smoothed));
    if (file.emit_clock_disc != expected.emit_clock_disc)
        note(fmt::format("emit_clock_disc: file {} vs expected {}",
                         file.emit_clock_disc, expected.emit_clock_disc));
    // The staleness horizon is also a replay-emitter construction parameter (RTA-11):
    // a divergence means replayed counters would differ from the producer's live
    // store.
    if (file.staleness_horizon_s != expected.staleness_horizon_s)
        note(fmt::format("staleness_horizon_s: file {} vs expected {}",
                         file.staleness_horizon_s, expected.staleness_horizon_s));
    // apriori_product_id only checked when the consumer declares an expectation.
    if (!expected.apriori_product_id.empty() &&
        file.apriori_product_id != expected.apriori_product_id)
        note(fmt::format("apriori_product_id: file '{}' vs expected '{}'",
                         file.apriori_product_id, expected.apriori_product_id));
    // Identity fields (RTA-12), each checked only when the consumer declares a
    // non-empty expectation (the apriori_product_id semantics).  A consumer that
    // declares none never mismatches on them, whatever the file carries.
    if (!expected.frame_id.empty() && file.frame_id != expected.frame_id)
        note(fmt::format("frame_id: file '{}' vs expected '{}'",
                         file.frame_id, expected.frame_id));
    if (!expected.coord_epoch.empty() && file.coord_epoch != expected.coord_epoch)
        note(fmt::format("coord_epoch: file '{}' vs expected '{}'",
                         file.coord_epoch, expected.coord_epoch));
    if (!expected.atx_id.empty() && file.atx_id != expected.atx_id)
        note(fmt::format("atx_id: file '{}' vs expected '{}'",
                         file.atx_id, expected.atx_id));
    if (!expected.orbit_ref.empty() && file.orbit_ref != expected.orbit_ref)
        note(fmt::format("orbit_ref: file '{}' vs expected '{}'",
                         file.orbit_ref, expected.orbit_ref));
    // Generation id (RTA-3), checked only when the consumer declares an expectation,
    // as with apriori_product_id.  A consumer typically does not pin one, but a
    // per-receipt check across re-broadcasts (RTA-5) pins the run's single id.
    if (expected.generation_id != 0 && file.generation_id != expected.generation_id)
        note(fmt::format("generation_id: file {} vs expected {}",
                         file.generation_id, expected.generation_id));
    return ok;
}

NetworkProductEmitter NetworkProductReader::makeReplayEmitter(const SbasisContract& contract) {
    // With present=false (v1 file, no S line) the SbasisContract defaults apply: the
    // emitter's default threshold, no clock-disc channel, no staleness horizon, and
    // no optional code-jump / clock-set channels, which is the replay behavior of a
    // file without a contract (B6).
    return NetworkProductEmitter(contract.disc_threshold_m, contract.emit_clock_disc,
                                 contract.staleness_horizon_s,
                                 contract.code_bias_jump_test,
                                 contract.clock_disc_on_set_change,
                                 contract.disc_threshold_frac_cycles,
                                 contract.disc_mark_scope_channel);   // disc mark scope
}

bool NetworkProductReader::admitsWire(const SbasisContract& file, bool allow_undeclared,
                                      std::string* why) {
    if (file.present) {
        if (why) why->clear();
        return true;
    }
    if (why) {
        const std::string cause =
            "the wire carries no S-basis contract line: it was written before the line was mad"
            "e part of every wire (network.serialize_sbasis_contract was off).  Without "
            "it the replay builds the DEFAULT emitter (the flat default phase-bias threshold, "
            "no clock-discontinuity channel, no staleness horizon, no code-jump or set-change "
            "channel), so every clock re-reference the producer declared is silent to the "
            "replay and it diverges from the in-process consumer.";
        *why = allow_undeclared
            ? cause + "  network.replay_allow_undeclared_wire is set: replaying it anyway, as "
                      "a legacy leg graded on purpose."
            : cause + "  Re-produce the wire with a current build, or set "
                      "network.replay_allow_undeclared_wire = true to grade a legacy leg on "
                      "purpose.";
    }
    return allow_undeclared;
}

// ===========================================================================
// NetworkProductTail (RTA-6)
// ===========================================================================

namespace {
/// Logger used by the tail (matches replay()'s fallback chain).
///
/// Never returns null: the three call sites below dereference the result
/// directly (`tailLog()->warn(...)`), and `spdlog::default_logger()` can be
/// null, because `spdlog::drop_all()` clears the registry's default and
/// LogManager calls it on both re-initialize (LogManager::initialize) and
/// shutdown (LogManager::shutdown).  The logger is resolved per call rather than cached in a
/// function-local static, the pattern that faulted in `navLog()` in
/// RinexNavParser.cpp.
std::shared_ptr<spdlog::logger> tailLog() {
    if (auto lg = spdlog::get("app")) return lg;
    if (auto def = spdlog::default_logger()) return def;
    // Registry has no default (post-drop_all).  Unregistered console fallback,
    // so a tail diagnostic is never silently lost and never faults.
    static const std::shared_ptr<spdlog::logger> fallback =
        std::make_shared<spdlog::logger>(
            "network_product_tail",
            std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    return fallback;
}
} // namespace

/// File-follow and parse state.  All parse semantics live in the shared
/// LineAssembler; this struct only manages the byte stream (partial-line
/// buffering, mid-file join, truncation/rotation) and the owned replay emitter.
/// The file is opened on each poll (open + seek is cheap at the throttled poll
/// cadence).  No handle is held between polls, so the tail never blocks a
/// producer-side delete or rotation on Windows (an open ifstream lacks
/// FILE_SHARE_DELETE).
struct NetworkProductTail::Impl {
    std::string    path;
    std::uintmax_t offset{0};          ///< Bytes consumed from the file so far.
    std::string    partial;            ///< Buffered partial trailing line (no '\n' yet).
    bool           skip_to_newline{false};  ///< Mid-file join landed mid-line: drop bytes until the first '\n'.
    LineAssembler  assembler;
    /// Owned replay emitter (carries cross-epoch counter state).  Rebuilt from
    /// the contract (makeReplayEmitter) when the first S line arrives before any
    /// epoch has been replayed; a mid-file joiner keeps the default and relies
    /// on RTA-4 counter adoption.
    NetworkProductEmitter emitter;
    bool           emitter_from_contract{false};
    bool           warned_default_emitter{false};
    std::size_t    delivered{0};       ///< Epochs replayed via pollInto since (re)open.

    /// Reset everything after a truncation/rotation: reopen from the start with
    /// fresh parse and emitter state (the new file is a new producer run).
    void reset() {
        offset = 0;
        partial.clear();
        skip_to_newline = false;
        assembler = LineAssembler{};
        assembler.track_contract = true;
        emitter = NetworkProductEmitter{};
        emitter_from_contract  = false;
        warned_default_emitter = false;
        delivered = 0;
    }
};

NetworkProductTail::NetworkProductTail(const std::string& path, bool start_at_end)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    impl_->assembler.track_contract = true;
    if (start_at_end) {
        // Mid-file join: skip everything already in the file.  The join point may
        // fall mid-line (the producer could be mid-write), so peek at the byte
        // before it.  If that byte is '\n', the join is on a line boundary and the
        // next appended line is consumed whole; otherwise bytes are dropped up to
        // the first newline, so the tail of the torn line is never parsed.
        std::error_code ec;
        const auto sz = std::filesystem::file_size(path, ec);
        if (!ec && sz > 0) {
            impl_->offset = sz;
            std::ifstream probe(path, std::ios::binary);
            if (probe) {
                probe.seekg(static_cast<std::streamoff>(sz - 1));
                char last = '\0';
                probe.read(&last, 1);
                impl_->skip_to_newline = (probe.gcount() == 1) ? (last != '\n') : true;
            } else {
                impl_->skip_to_newline = true;
            }
            tailLog()->info("NetworkProductTail: joined '{}' mid-file at byte {} — the "
                            "contract arrives with the next S line (arm "
                            "network.contract_rebroadcast_s on the producer) and "
                            "discontinuity counters adopt from the wire when "
                            "network.serialize_disc_counters is armed.",
                            path, sz);
        }
    }
}

NetworkProductTail::~NetworkProductTail() = default;

const std::string&    NetworkProductTail::path() const     { return impl_->path; }
const SbasisContract& NetworkProductTail::contract() const { return impl_->assembler.contract; }
bool                  NetworkProductTail::healthy() const  { return impl_->assembler.ok; }
std::size_t           NetworkProductTail::epochsDelivered() const { return impl_->delivered; }
std::optional<NetworkProducerStatus> NetworkProductTail::lastStatus() const {
    return impl_->assembler.latest_status;   // reset with the assembler on rotation (RTA-10)
}

std::vector<NetworkProductRecord> NetworkProductTail::poll() {
    std::vector<NetworkProductRecord> out;
    Impl& im = *impl_;

    std::error_code ec;
    const auto sz = std::filesystem::file_size(im.path, ec);
    if (ec) return out;   // file absent (not created yet, or mid-rotation): keep waiting

    if (sz < im.offset) {
        // Truncation / rotation: the producer restarted and truncated the file
        // (NetworkProductWriter opens with std::ios::trunc).  Reopen from the
        // start with fresh parse/emitter state; the new run's header (and RTA-3
        // generation id) re-establishes the contract.
        tailLog()->warn("NetworkProductTail: '{}' shrank ({} -> {} bytes) — producer "
                        "restart/rotation; reopening from the start with fresh state.",
                        im.path, im.offset, sz);
        im.reset();
    }
    if (sz <= im.offset) return out;   // nothing new appended

    // Open per poll; no handle is held between polls (see Impl).  Consume the
    // newly appended byte range [offset, sz); reading only up to the stat'ed
    // size keeps one poll bounded even against a fast producer.
    std::ifstream in(im.path, std::ios::binary);
    if (!in) return out;
    in.seekg(static_cast<std::streamoff>(im.offset));
    std::uintmax_t avail = sz - im.offset;
    char buf[4096];
    while (avail > 0) {
        const std::streamsize want =
            static_cast<std::streamsize>(std::min<std::uintmax_t>(sizeof buf, avail));
        in.read(buf, want);
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        std::size_t begin = 0;
        if (im.skip_to_newline) {
            // Mid-file join landed mid-line: discard through the first newline.
            const char* nl = static_cast<const char*>(
                std::memchr(buf, '\n', static_cast<std::size_t>(n)));
            if (!nl) { im.offset += static_cast<std::uintmax_t>(n); avail -= static_cast<std::uintmax_t>(n); continue; }
            begin = static_cast<std::size_t>(nl - buf) + 1;
            im.skip_to_newline = false;
        }
        im.partial.append(buf + begin, static_cast<std::size_t>(n) - begin);
        im.offset += static_cast<std::uintmax_t>(n);
        avail     -= static_cast<std::uintmax_t>(n);
    }

    // Hand every complete line to the shared assembler; the partial trailing
    // line stays buffered until its newline arrives on a later poll.
    std::size_t nl_pos;
    while ((nl_pos = im.partial.find('\n')) != std::string::npos) {
        const std::string line = im.partial.substr(0, nl_pos);
        im.partial.erase(0, nl_pos + 1);
        if (auto rec = im.assembler.consume(line)) out.push_back(std::move(*rec));
        // First S contract: adopt the producer's replay-emitter construction, but
        // only while no epoch has been replayed yet, because the emitter carries
        // cross-epoch baselines that a rebuild would discard.
        if (!im.emitter_from_contract && im.assembler.contract.present) {
            if (im.delivered == 0) {
                im.emitter = NetworkProductReader::makeReplayEmitter(im.assembler.contract);
                im.emitter_from_contract = true;
            } else if (!im.warned_default_emitter) {
                im.warned_default_emitter = true;
                tailLog()->warn("NetworkProductTail: S contract arrived AFTER {} replayed "
                                "epoch(s) (mid-file join) — keeping the default replay "
                                "emitter; discontinuity counters stay correct only via "
                                "the RTA-4 serialized values (network.serialize_disc_counters).",
                                im.delivered);
            }
        }
    }
    return out;
}

std::vector<NetworkProductRecord> NetworkProductTail::pollInto(SsrCorrectionStore& store) {
    std::vector<NetworkProductRecord> recs = poll();
    for (const NetworkProductRecord& rec : recs) {
        // Shared replay path: markDatumChange, retro-bump and emitToStore, plus the
        // serialized-counter adoption that serves a mid-file join (RTA-4).
        NetworkProductReader::replay(rec, store, impl_->emitter);
        ++impl_->delivered;
    }
    // Propagate the producer's advertised combined-σ age-scaling coefficient (the
    // S-line agek= token) into the consumer's store so the measurement model can
    // age-inflate the combined variance (M4, contract v2).  replay() does not read
    // the contract, so this is the only channel that carries agek to the consumer.
    // It is a store-wide scalar (default 0 = no inflation), refreshed from the
    // current contract on each poll (cheap, idempotent), and set only once a
    // contract has been seen.
    if (impl_->assembler.contract.present)
        store.setCombinedSigmaAgeCoeff(impl_->assembler.contract.age_scale_k);
    return recs;
}

// ===========================================================================
// A-priori identity of the run's own product files
// ===========================================================================

namespace {
/// [start, end) of a product's span in continuous GPS seconds, read from its IGS long
/// name (e.g. COD0MGXFIN_20241290000_01D_05M_ORB.SP3: start 2024 DOY 129 00:00 on the
/// GPS time scale, span one day).  False when the name does not follow the convention.
bool productLongNameSpan(const std::string& name, double& start_s, double& end_s) {
    static const std::regex re(R"(_(\d{4})(\d{3})(\d{2})(\d{2})_(\d{2})([MHDWY])_)");
    std::smatch m;
    if (!std::regex_search(name, m, re)) return false;
    const int year = std::stoi(m[1].str());
    const int doy  = std::stoi(m[2].str());
    const int hh   = std::stoi(m[3].str());
    const int mm   = std::stoi(m[4].str());
    const int len  = std::stoi(m[5].str());
    const char unit = m[6].str()[0];
    int month = 0, day = 0;
    if (len <= 0 || hh > 23 || mm > 59 || !TimeUtils::dayOfYearToDate(year, doy, month, day))
        return false;
    const double unit_s = unit == 'M' ? 60.0
                        : unit == 'H' ? 3600.0
                        : unit == 'D' ? 86400.0
                        : unit == 'W' ? 604800.0
                                      : 365.0 * 86400.0;   // 'Y'
    start_s = TimeUtils::continuousGpsSeconds(
        TimeUtils::gregorianGpsToGpsTime(year, month, day, hh, mm, 0.0));
    end_s = start_s + static_cast<double>(len) * unit_s;
    return true;
}
}  // namespace

std::string aprioriIdentityForWindow(const std::vector<std::filesystem::path>& product_paths,
                                     double t0_gps_s, double t1_gps_s, bool* named_all) {
    if (named_all) *named_all = false;
    // The files of one kind (the extension getProductPath matches) in force during the
    // window; every file of the kind when one name does not parse or none meets it.
    auto inForce = [&](const std::string& ext) {
        std::vector<std::string> all, in_force;
        bool parsed_all = true;
        for (const auto& p : product_paths) {
            if (p.extension().string() != ext) continue;
            const std::string name = p.filename().string();
            all.push_back(name);
            double s = 0.0, e = 0.0;
            if (!productLongNameSpan(name, s, e)) { parsed_all = false; continue; }
            if (s < t1_gps_s && e > t0_gps_s) in_force.push_back(name);
        }
        if (!all.empty() && (!parsed_all || in_force.empty())) {
            if (named_all) *named_all = true;
            return all;
        }
        return in_force;
    };
    const std::vector<std::string> sp3 = inForce(".SP3");
    const std::vector<std::string> clk = inForce(".CLK");
    if (sp3.empty() && clk.empty()) return {};
    auto join = [](const std::vector<std::string>& v) {
        if (v.empty()) return std::string("?");
        std::string s;
        for (const auto& x : v) s += (s.empty() ? "" : "+") + x;
        return s;
    };
    return join(sp3) + "+" + join(clk);
}

} // namespace GnssProcessor
