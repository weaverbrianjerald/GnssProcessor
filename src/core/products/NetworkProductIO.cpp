/**
 * @file NetworkProductIO.cpp
 * @brief Implementation of the PPP-RTK network product file writer / reader.
 */

#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>

namespace GnssProcessor {

namespace {

/// Split @p line on ',' into raw fields (no trimming; RINEX obs codes and sat
/// IDs never contain commas, so this is lossless for our format).
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
 * Holds exactly the per-line parse state the original @c NetworkProductReader::read
 * loop kept in locals (current epoch under assembly, sat index, pending RTA-5 S
 * line, sticky ok flag) so the batch path and the incremental
 * @c NetworkProductTail path (RTA-6) run ONE parser and can never diverge.  The
 * per-tag semantics are the read() loop body verbatim: an epoch is COMPLETED
 * (returned) when the NEXT E line flushes it; @c finish() flushes the trailing
 * epoch at end of a batch read (a live tail never calls it — the trailing epoch
 * may still be growing).
 */
struct LineAssembler {
    /// 3-arg read() semantics: a short/malformed S line flags @c ok only when the
    /// caller asked for the contract (the 2-arg overload treats it as a valid,
    /// ignored line).  The tail always tracks the contract.
    bool track_contract{false};

    NetworkProductRecord               cur;
    std::map<std::string, std::size_t> sat_index;   ///< sat -> index into cur.epoch.sats
    bool                               have_cur{false};
    /// RTA-5: the most recent S line not yet attached to an epoch.  An S line
    /// (header or mid-file re-broadcast) applies to the epochs that FOLLOW it, so
    /// it is stamped into the NEXT E line's record as contract_refresh.
    SbasisContract pending_s;
    bool           pending_s_valid{false};
    /// RTA-10: the most recent H heartbeat not yet attached to an epoch.  The
    /// producer writes an emitted epoch's H immediately before its E line, so a
    /// pending H attaches to the NEXT E record ONLY when their week/tow match
    /// exactly (both sides serialize the tow with the same shortest-round-trip
    /// formatting, so the parsed doubles compare equal); the standalone H of a
    /// fully WITHHELD epoch therefore never mis-attaches to a later epoch.
    NetworkProducerStatus pending_h;
    bool                  pending_h_valid{false};
    /// RTA-10: latest H heartbeat consumed (attached or standalone) — the tail's
    /// lastStatus() surface.
    std::optional<NetworkProducerStatus> latest_status;
    /// Reader-current contract state: the LAST S line consumed (present=false
    /// until one is).  The batch reader copies this into contract_out at EOF.
    SbasisContract contract;
    /// Sticky parse-health flag — read()'s *ok semantics (false on any malformed
    /// line / unknown tag; parsing continues past it).
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
    /// Returns the previously assembling epoch when this line COMPLETES it (an
    /// E line flushing its predecessor); nullopt otherwise.
    std::optional<NetworkProductRecord> consume(const std::string& raw) {
        std::optional<NetworkProductRecord> completed;
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF
        if (line.empty() || line[0] == '#') return completed;

        const std::vector<std::string> f = splitCsv(line);
        if (f.empty() || f[0].empty()) return completed;
        const std::string& tag = f[0];

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
                if (f.size() > 7)   // A2: optional affected-constellation set (field 8)
                    for (char ch : f[7]) cur.datum_consts.insert(ch);
                if (pending_s_valid) {   // RTA-5: the S line preceding this epoch
                    cur.contract_refresh = pending_s;
                    pending_s_valid      = false;
                }
                if (pending_h_valid) {   // RTA-10: the H heartbeat preceding this epoch
                    // Attach only on an exact week/tow match — the emitted epoch's own
                    // heartbeat.  A withheld epoch's standalone H (different time) is
                    // dropped here; it already updated latest_status when consumed.
                    if (pending_h.time.week == cur.epoch.epoch_time.week &&
                        pending_h.time.tow == cur.epoch.epoch_time.tow)
                        cur.epoch_status = pending_h;
                    pending_h_valid = false;
                }
                have_cur = true;
            } else if (tag == "C") {
                if (!have_cur || f.size() < 3) { ok = false; return completed; }
                NetworkSatSolution& s = satSolution(f[1]);
                s.has_clock  = true;
                s.clock_c0_m = std::stod(f[2]);
                // Positional optional fields: c1 (field 4, B3b) then the RTA-4
                // post-emit clock disc counter (field 5).  Card 4 (RTF-2) appends
                // self-describing key=value tokens (iode=/oiod=/oepoch=) AFTER
                // these; a field containing '=' is a key=value token, never a
                // positional number, so the positional parse stops at the first '='
                // field.  For every pre-Card-4 file no field has '=', so this is
                // byte-for-byte the old two-line c1/clock-disc parse.
                std::size_t k = 3;
                if (k < f.size() && f[k].find('=') == std::string::npos)
                    s.clock_c1_m_s = std::stod(f[k++]);                    // B3b drift
                if (k < f.size() && f[k].find('=') == std::string::npos)   // RTA-4 clock disc
                    cur.serialized_disc.clock[f[1]] = std::stoi(f[k++]);
                for (; k < f.size(); ++k) {   // Card 4 brdc_rts anchor / orbit echo
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
                    } else if (f[k].rfind("sigcc=", 0) == 0) {   // M4: combined code-correction σ
                        s.sigcc_m = std::stod(f[k].substr(6));
                    } else if (f[k].rfind("rho=", 0) == 0) {     // M4: code/phase correction correlation
                        s.rho = std::stod(f[k].substr(4));
                    }
                    // Unknown key=value tokens are ignored (forward-compatible).
                }
            } else if (tag == "B") {
                if (!have_cur || f.size() < 4) { ok = false; return completed; }
                satSolution(f[1]).code_bias_m[f[2]] = std::stod(f[3]);
                if (f.size() > 4)   // RTA-4: optional post-emit code disc counter (field 5)
                    cur.serialized_disc.code[f[1] + "|" + f[2]] = std::stoi(f[4]);
            } else if (tag == "P") {
                if (!have_cur || f.size() < 6) { ok = false; return completed; }
                NetworkPhaseBias pb;
                pb.bias_m            = std::stod(f[3]);
                pb.signal_integer    = (std::stoi(f[4]) != 0);
                pb.wide_lane_integer = std::stoi(f[5]);
                // Fields >=6 (any order): the RTA-4 positional post-emit phase disc counter (a
                // bare int) and/or the T3d σ token (sigma=<val>).  Scan + dispatch by shape so
                // the two coexist (mirrors the S-line key=value dispatch).
                for (std::size_t k = 6; k < f.size(); ++k) {
                    if (f[k].rfind("sigma=", 0) == 0)
                        pb.sigma_m = std::stod(f[k].substr(6));
                    else if (f[k].rfind("sigc=", 0) == 0)   // M4: combined phase-correction σ
                        pb.sigc_m = std::stod(f[k].substr(5));
                    else if (f[k].find('=') == std::string::npos)
                        cur.serialized_disc.phase[f[1] + "|" + f[2]] = std::stoi(f[k]);
                }
                satSolution(f[1]).phase_bias[f[2]] = pb;
            } else if (tag == "R") {   // B1: retro-bump satellite set (per-sat disc)
                if (!have_cur) { ok = false; return completed; }
                for (std::size_t k = 1; k < f.size(); ++k)
                    if (!f[k].empty()) cur.retro_sats.insert(f[k]);
            } else if (tag == "U") {   // B1: per-sat URA quality channel
                if (!have_cur || f.size() < 3) { ok = false; return completed; }
                NetworkSatSolution& s = satSolution(f[1]);
                s.clock_ura_m = std::stod(f[2]);
                s.has_ura     = true;
            } else if (tag == "G") {   // RT0-4: per-epoch wall-clock time-of-generation
                if (!have_cur || f.size() < 3) { ok = false; return completed; }
                cur.epoch.generation_time.week = std::stoi(f[1]);
                cur.epoch.generation_time.tow  = std::stod(f[2]);
            } else if (tag == "I") {   // RTA-3: per-epoch solution-generation id
                if (!have_cur || f.size() < 2) { ok = false; return completed; }
                cur.epoch.generation_id = static_cast<uint32_t>(std::stoul(f[1]));
            } else if (tag == "H") {   // RTA-10: epoch-status heartbeat (positional)
                if (f.size() < 7) { ok = false; return completed; }
                // An H line always BEGINS a new epoch unit — the producer writes it
                // immediately before an emitted epoch's E line, or standalone for a
                // fully withheld epoch — so it completes the epoch under assembly
                // exactly like the next E line would.  This is what bounds a live
                // tail's latency across withheld stretches: the withheld epoch's H
                // both flushes the previous emitted epoch and surfaces the status.
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
            } else if (tag == "S") {   // B6: S-basis contract (v2 header / RTA-5 re-broadcast)
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
                    // entries, plus the RTA-3/RTA-11 'gid='/'stale=' and RTA-12
                    // 'frame='/'cepoch='/'atx='/'oref=' key=value extensions.
                    // Unknown shapes are ignored (forward-compatible).
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
                            c.decl.eq34_datum_receiver = f[k].substr(4);   // Card 3 diagnostic
                        } else if (f[k].rfind("agek=", 0) == 0) {
                            c.age_scale_k = std::stod(f[k].substr(5));      // M4: age-scaling coeff
                        } else if (f[k].rfind("discfrac=", 0) == 0) {
                            c.disc_threshold_frac_cycles = std::stod(f[k].substr(9));  // H3: per-λ jump gate
                        } else {
                            const auto colon = f[k].find(':');
                            if (colon != std::string::npos)
                                c.decl.clock_pivot_sat[f[k].substr(0, colon)] = f[k].substr(colon + 1);
                        }
                    }
                    // Repeated S lines (RTA-5): each receipt updates the reader's
                    // CURRENT contract state — the batch contract_out ends as the
                    // LAST one — and is also stamped on the record of the epoch it
                    // precedes (contract_refresh), so a consumer can re-run
                    // checkContract on every receipt at the right position.
                    contract        = c;
                    pending_s       = c;
                    pending_s_valid = true;
                } else if (track_contract) {
                    ok = false;   // short/malformed S line — flag it, keep parsing epochs
                }
                // A short 'S' line without contract tracking (2-arg read) stays a
                // valid, ignored line.
            } else {
                ok = false;   // unknown tag — flag but keep going
            }
        } catch (const std::exception&) {
            ok = false;       // malformed numeric field — flag, skip line
        }
        return completed;
    }

    /// Flush the trailing epoch (batch read at EOF).  A live tail never calls
    /// this: its trailing epoch may still be growing on disk.
    std::optional<NetworkProductRecord> finish() {
        std::optional<NetworkProductRecord> completed;
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
    writeHeader_(&contract);  // B6: v2 header carrying the S-basis contract
}

void NetworkProductWriter::writeSLine_(const SbasisContract& contract) {
    // B6 S-basis contract line (machine-parseable, real 'S' record — NOT a '#'
    // comment, so the reader ingests it):
    //   S,ref_rx,bias_sign,windup_id,amb_datum,isb_datum,emit_cb,cb_datum,
    //     disc_thresh_m,emit_clock_disc,apriori_product_id[,<const>:<pivot_sat>...]
    //     [,gid=<generation_id>][,stale=<staleness_horizon_s>]
    //     [,frame=<frame_id>][,cepoch=<coord_epoch>][,atx=<atx_id>][,oref=<orbit_ref>]
    // disc_thresh_m + emit_clock_disc (+ the optional stale= horizon, RTA-11)
    // fully determine the emitter construction, so a consumer replays the
    // counters with no out-of-band producer config
    // (NetworkProductReader::makeReplayEmitter).  Trailing fields 12+ are
    // discriminated by shape: ':' = a realized pivot entry, 'gid='/'stale='
    // plus the RTA-12 identity keys 'frame='/'cepoch='/'atx='/'oref=' =
    // key=value extensions (each written ONLY when armed/non-empty, keeping an
    // OFF run's v2 header byte-identical); unknown shapes are ignored by the
    // reader.  Shared verbatim by the v2 header and the RTA-5 mid-file
    // re-broadcast.
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
    if (contract.generation_id != 0)          // RTA-3: only when armed (byte-identity)
        out_ << ",gid=" << contract.generation_id;
    if (contract.staleness_horizon_s > 0.0)   // RTA-11: only when armed (byte-identity)
        out_ << ",stale=" << fmt::format("{}", contract.staleness_horizon_s);
    if (contract.code_bias_jump_test)         // only when armed (byte-identity)
        out_ << ",cbjump=1";
    if (contract.clock_disc_on_set_change)    // only when armed (byte-identity)
        out_ << ",clkset=1";
    // RTA-12 identity fields: each ONLY when non-empty, so a producer that
    // declares none of them writes the pre-RTA-12 S line byte-for-byte.
    if (!contract.frame_id.empty())
        out_ << ",frame=" << contract.frame_id;
    if (!contract.coord_epoch.empty())
        out_ << ",cepoch=" << contract.coord_epoch;
    if (!contract.atx_id.empty())
        out_ << ",atx=" << contract.atx_id;
    if (!contract.orbit_ref.empty())
        out_ << ",oref=" << contract.orbit_ref;
    // Card 3 (SingleReceiverChain) diagnostic: the datum receiver r1.  ONLY when
    // non-empty, so a non-eq34 producer writes the S line byte-for-byte.
    if (!contract.decl.eq34_datum_receiver.empty())
        out_ << ",pdr=" << contract.decl.eq34_datum_receiver;
    // M4 (contract v2): age-scaling coefficient for the combined-correction variance.
    // ONLY when non-zero, so a producer that does not advertise age scaling writes the
    // pre-M4 S line byte-for-byte.  A quality hint (not a replay-emitter param), so
    // makeReplayEmitter / checkContract ignore it.
    if (contract.age_scale_k != 0.0)
        out_ << ",agek=" << fmt::format("{}", contract.age_scale_k);
    // H3: per-λ fractional-cycle discontinuity threshold.  ONLY when > 0, so a
    // legacy (flat-metre) producer writes the S line byte-for-byte.  Unlike agek=
    // this IS a replay-emitter parameter — makeReplayEmitter applies it and
    // checkContract compares it (a per-λ vs flat mismatch mis-counts discontinuities).
    if (contract.disc_threshold_frac_cycles > 0.0)
        out_ << ",discfrac=" << fmt::format("{}", contract.disc_threshold_frac_cycles);
    out_ << '\n';
}

void NetworkProductWriter::writeContract(const SbasisContract& contract) {
    if (!out_) return;
    writeSLine_(contract);   // RTA-5: mid-file re-broadcast, header grammar verbatim
}

void NetworkProductWriter::writeEpochStatus(const NetworkProducerStatus& status) {
    if (!out_) return;
    // RTA-10 heartbeat: H,week,tow,status,sigma0,n_sats_published,withheld.  The tow
    // uses the SAME shortest-round-trip formatting as the E line, so the reader's
    // pending-H week/tow attach-match (LineAssembler) compares exactly equal doubles
    // for an emitted epoch's own heartbeat.  sigma0 fixed at 4 decimals per the RTA-10
    // grammar (0 when unavailable).  Never legended in the '#' header (frozen bytes)
    // and never fed to replay — purely positional, like the S contract line.
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
    // Legend kept verbatim to the pre-B1/A2/B3b format so a v1 file is byte-identical
    // to the pre-change writer.  The optional trailing fields (E-line datum_consts,
    // C-line clock_c1_m_s) and the U/R records are position-parsed by the reader (f.size()
    // guards) and appear only when their producer toggles emit them.
    out_ << "# E,week,tow,iod_ssr,provider_id,solution_id,datum_changed\n"
            "# C,sat,clock_c0_m\n"
            "# B,sat,signal,code_bias_m\n"
            "# P,sat,signal,bias_m,signal_integer,wide_lane_integer\n";
    // R2b: the optional R/U records (and the S contract grammar, documented above the S line)
    // are legended ONLY in the v2 header — the v1 legend above is frozen for byte-identity
    // (a prior review reverted a v1 legend edit).  These are '#' comments, skipped by the
    // reader, so they never affect parsing or replay.
    if (contract)
        out_ << "# R,retro_sat[,retro_sat...]   (B1 per-satellite discontinuity bump)\n"
                "# U,sat,clock_ura_m             (B1 per-satellite URA quality, 1-sigma m)\n"
                "# G,week,tow                    (RT0-4 wall-clock time-of-generation)\n"
                "# S,ref_rx,...,pivots           (B6 S-basis contract; field grammar above)\n";
}

void NetworkProductWriter::write(const NetworkSolutionEpoch& sol, bool datum_changed,
                                 const std::set<char>& datum_consts,
                                 const std::set<std::string>& retro_sats,
                                 const NetworkProductEmitter* disc_source) {
    if (!out_) return;

    out_ << "E," << sol.epoch_time.week << ',' << fmt::format("{}", sol.epoch_time.tow)
         << ',' << sol.iod_ssr << ',' << sol.provider_id << ',' << sol.solution_id
         << ',' << (datum_changed ? 1 : 0);
    // A2: append the affected-constellation set as field 8 ONLY when non-empty, so
    // a run with per-constellation scoping off writes the pre-A2 7-field E line
    // byte-for-byte.  Replay of a file lacking field 8 falls back to a global bump.
    if (!datum_consts.empty())
        out_ << ',' << std::string(datum_consts.begin(), datum_consts.end());
    out_ << '\n';
    // RT0-4: optional per-epoch wall-clock TIME-OF-GENERATION as a separate G record
    // ("G,week,tow"), written ONLY when the producer stamped it (generation_time.week
    // != 0), so a run with network.emit_generation_time off is byte-identical.  A
    // consumer measures latency as (its ingest wall-clock − this generation time).
    if (sol.generation_time.week != 0)
        out_ << "G," << sol.generation_time.week << ','
             << fmt::format("{}", sol.generation_time.tow) << '\n';
    // RTA-3: optional solution-generation id as a separate I record ("I,<id>"),
    // written ONLY when the producer stamped it (network.emit_generation_id), so
    // an OFF run is byte-identical.  Serializing it per epoch (following the G-
    // record precedent) keeps replay deterministic — the id comes back out of the
    // file itself — and lets a tail reader recover it from any epoch block.
    if (sol.generation_id != 0)
        out_ << "I," << sol.generation_id << '\n';
    // B1: the retro-bump satellite set as a separate R record (COMMA-separated sat
    // ids, "R,G13,E09"; parsed by splitCsv), written ONLY when non-empty so a
    // retro-bump-off run is byte-identical.
    if (!retro_sats.empty()) {
        out_ << "R";
        for (const std::string& sat : retro_sats) out_ << ',' << sat;
        out_ << '\n';
    }

    // RTA-4 (disc_source non-null): each C/B/P record gains a trailing field with
    // the CURRENT counter from the emitter.  Because the driver calls write()
    // AFTER emitter.emitToStore(sol, store), these are the POST-EMIT counters for
    // THIS epoch — byte-identical to what the store records received.  The C line
    // then always carries the C1 drift field (0 when absent) so the clock counter
    // sits at fixed field 5 and can never be misparsed as a drift.
    for (const auto& s : sol.sats) {
        if (s.has_clock) {
            out_ << "C," << s.sat << ',' << fmt::format("{}", s.clock_c0_m);
            if (disc_source) {
                out_ << ',' << fmt::format("{}", s.clock_c1_m_s)
                     << ',' << disc_source->clockDiscontinuityCounter(s.sat);
            } else if (s.clock_c1_m_s != 0.0) {  // B3b: optional drift field 4 (only when clock_model emits it)
                out_ << ',' << fmt::format("{}", s.clock_c1_m_s);
            }
            // Card 4 (RTF-2): brdc_rts clock ephemeris-anchor + orbit-echo fields as
            // trailing key=value tokens.  Position-independent — the c1/clock_disc
            // positional fields above vary in count, and orbit_iod_ssr/orbit_epoch
            // are themselves independently optional — and self-describing.  Written
            // ONLY when the producer stamped a broadcast IODE (clock_iode >= 0, i.e.
            // brdc_rts mode); sp3 mode leaves clock_iode = -1 → nothing appended →
            // the C line is byte-identical.  A field containing '=' is a key=value
            // token, never a positional number, so the reader's positional
            // c1/clock_disc parse stops at the first such field.
            if (s.clock_iode >= 0) {
                out_ << ",iode=" << s.clock_iode;
                if (s.orbit_iod_ssr >= 0) out_ << ",oiod=" << s.orbit_iod_ssr;
                if (s.orbit_epoch.week != 0)
                    out_ << ",oepoch=" << s.orbit_epoch.week << ':'
                         << fmt::format("{}", s.orbit_epoch.tow);
            }
            // M4 (contract v2): optional per-sat COMBINED code-correction σ + code/phase
            // correlation as key=value tokens (self-describing, position-independent — a
            // field with '=' ends the reader's positional c1/clock_disc parse, so they
            // coexist with the disc counter exactly as iode= does).  Nonzero only when
            // network.emit_combined_sigma is armed ⇒ OFF run emits nothing ⇒
            // byte-identical.  rho is written ONLY alongside sigcc (it correlates the
            // combined code with the combined phase, meaningless without a combined code σ).
            if (s.sigcc_m > 0.0 && std::isfinite(s.sigcc_m)) {
                out_ << ",sigcc=" << fmt::format("{}", s.sigcc_m);
                if (std::isfinite(s.rho))
                    out_ << ",rho=" << fmt::format("{}", s.rho);
            }
            out_ << '\n';
        }
        for (const auto& [signal, bias_m] : s.code_bias_m) {
            out_ << "B," << s.sat << ',' << signal << ',' << fmt::format("{}", bias_m);
            if (disc_source)
                out_ << ',' << disc_source->codeBiasDiscontinuityCounter(s.sat, signal);
            out_ << '\n';
        }
        for (const auto& [signal, pb] : s.phase_bias) {
            out_ << "P," << s.sat << ',' << signal << ',' << fmt::format("{}", pb.bias_m)
                 << ',' << (pb.signal_integer ? 1 : 0) << ',' << pb.wide_lane_integer;
            if (disc_source)
                out_ << ',' << disc_source->discontinuityCounter(s.sat, signal);
            // T3d (B1b): optional trailing σ as a key=value token so it never collides with the
            // positional disc field.  pb.sigma_m is nonzero only when emit_phase_bias_sigma is
            // armed ⇒ an OFF run emits no token ⇒ byte-identical.  Mirrors the C-line iode= tail.
            if (pb.sigma_m > 0.0 && std::isfinite(pb.sigma_m))
                out_ << ",sigma=" << fmt::format("{}", pb.sigma_m);
            // M4 (contract v2): optional COMBINED phase-correction σ as a key=value
            // token beside sigma=.  pb.sigc_m is nonzero only when
            // network.emit_combined_sigma is armed ⇒ an OFF run emits no token ⇒
            // byte-identical.  Distinct from sigma= (the phase-bias term alone):
            // sigc = σ of clock + phase bias combined.
            if (pb.sigc_m > 0.0 && std::isfinite(pb.sigc_m))
                out_ << ",sigc=" << fmt::format("{}", pb.sigc_m);
            out_ << '\n';
        }
        if (s.has_ura)   // B1: per-sat URA (emitted only when network.emit_ura on → no U lines otherwise)
            out_ << "U," << s.sat << ',' << fmt::format("{}", s.clock_ura_m) << '\n';
    }
}

// ===========================================================================
// NetworkProductReader
// ===========================================================================

std::vector<NetworkProductRecord> NetworkProductReader::read(const std::string& path, bool* ok) {
    return read(path, nullptr, ok);   // delegate (B6): no contract wanted
}

std::vector<NetworkProductRecord> NetworkProductReader::read(const std::string& path,
                                                             SbasisContract* contract_out,
                                                             bool* ok) {
    std::vector<NetworkProductRecord> out;
    if (contract_out) *contract_out = SbasisContract{};   // present=false until an S line is seen
    std::ifstream in(path);
    if (!in) { if (ok) *ok = false; return out; }

    // The per-tag parse semantics live in the shared LineAssembler (one code
    // path with the RTA-6 NetworkProductTail); this loop only feeds it lines
    // and flushes the trailing epoch at EOF.
    LineAssembler assembler;
    assembler.track_contract = (contract_out != nullptr);

    std::string line;
    while (std::getline(in, line))
        if (auto rec = assembler.consume(line)) out.push_back(std::move(*rec));
    if (auto rec = assembler.finish()) out.push_back(std::move(*rec));

    if (contract_out && assembler.contract.present) *contract_out = assembler.contract;
    if (ok) *ok = assembler.ok;
    return out;
}

void NetworkProductReader::replay(const NetworkProductRecord& rec,
                                  SsrCorrectionStore& store,
                                  NetworkProductEmitter& emitter) {
    // Discontinuity counters are NOT serialized — for phase OR for code.  The
    // emitter recomputes them deterministically from two inputs, both of which
    // are reproduced here: (1) the per-epoch solution itself (phase-bias jumps;
    // and, for code, the per-(const,signal) satellite-set + per-satellite
    // distinct-band composition of code_bias_m, which is exactly the producer's
    // gauge structure), and (2) the serialized datum_changed flag, re-applied
    // via markDatumChange() to force the same clock-pivot bump.  Replaying the
    // emitted-only epoch sequence in order therefore reproduces the live store
    // byte-for-byte, including every discontinuity counter.
    // A2: re-apply the datum change SCOPED to the serialized affected-constellation
    // set (E-line field 8).  An empty set (pre-A2 files / scoping-off runs) forces a
    // global bump, exactly reproducing the pre-A2 behavior.
    if (rec.datum_changed) emitter.markDatumChange(rec.datum_consts);
    // B1: re-apply the per-satellite retro-bump (R record) so the consumer-visible
    // discontinuity counters reproduce byte-for-byte on replay.
    if (!rec.retro_sats.empty()) emitter.markSatDiscontinuity(rec.retro_sats);
    emitter.emitToStore(rec.epoch, store);

    // RTA-4: when the record carries serialized POST-EMIT counter values
    // (network.serialize_disc_counters), the deterministic recompute above is kept
    // as a self-check.  For a full-file replay the two must agree; a disagreement
    // is EXPECTED for a reader that joined mid-file (every key recomputes from a
    // first sighting = 0) and is exactly what the wire values are for — warn,
    // ADOPT the file's values into the emitter, and re-emit the epoch so the
    // reconstructed store records carry them.  The re-emit is idempotent: the
    // baselines / satellite-set trackers already equal this epoch's content and
    // every forced mark was consumed by the first emit, so no counter steps —
    // only the pushed records are re-stamped with the adopted counters.
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
    // Clock pivots: checked per-constellation, only for the constellations the
    // consumer DECLARES an expectation for (mirrors apriori_product_id below) —
    // a consumer typically pins one pivot via config while the file carries every
    // realized pivot, and the extra file entries are not a mismatch.
    for (const auto& [cst, sat] : expected.decl.clock_pivot_sat) {
        const auto fit = file.decl.clock_pivot_sat.find(cst);
        if (fit == file.decl.clock_pivot_sat.end())
            note(fmt::format("clock_pivot_sat[{}]: file has none vs expected '{}'", cst, sat));
        else if (fit->second != sat)
            note(fmt::format("clock_pivot_sat[{}]: file '{}' vs expected '{}'",
                             cst, fit->second, sat));
    }
    // The two replay-emitter construction parameters: a divergence here means a
    // consumer replaying with its own emitter config would reproduce DIFFERENT
    // discontinuity counters than the producer's live store.
    if (file.disc_threshold_m != expected.disc_threshold_m)
        note(fmt::format("disc_threshold_m: file {} vs expected {}",
                         file.disc_threshold_m, expected.disc_threshold_m));
    // H3: the per-λ fractional-cycle threshold — a per-λ-vs-flat (or differing frac)
    // mismatch makes the consumer's replay emitter mis-count discontinuities.
    if (file.disc_threshold_frac_cycles != expected.disc_threshold_frac_cycles)
        note(fmt::format("disc_threshold_frac_cycles: file {} vs expected {}",
                         file.disc_threshold_frac_cycles, expected.disc_threshold_frac_cycles));
    if (file.emit_clock_disc != expected.emit_clock_disc)
        note(fmt::format("emit_clock_disc: file {} vs expected {}",
                         file.emit_clock_disc, expected.emit_clock_disc));
    // RTA-11: the third replay-emitter construction parameter — a divergence means
    // replayed counters would differ from the producer's live store.
    if (file.staleness_horizon_s != expected.staleness_horizon_s)
        note(fmt::format("staleness_horizon_s: file {} vs expected {}",
                         file.staleness_horizon_s, expected.staleness_horizon_s));
    // apriori_product_id only checked when the consumer declares an expectation.
    if (!expected.apriori_product_id.empty() &&
        file.apriori_product_id != expected.apriori_product_id)
        note(fmt::format("apriori_product_id: file '{}' vs expected '{}'",
                         file.apriori_product_id, expected.apriori_product_id));
    // RTA-12 identity fields: each checked ONLY when the consumer declares a
    // non-empty expectation (the apriori_product_id semantics) — a v1/pre-RTA-12
    // file or an undeclared consumer never mismatches on them.
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
    // RTA-3: generation id, only when the consumer declares an expectation
    // (mirrors apriori_product_id — a consumer typically does not pin one, but a
    // per-receipt check across RTA-5 re-broadcasts pins the run's single id).
    if (expected.generation_id != 0 && file.generation_id != expected.generation_id)
        note(fmt::format("generation_id: file {} vs expected {}",
                         file.generation_id, expected.generation_id));
    return ok;
}

NetworkProductEmitter NetworkProductReader::makeReplayEmitter(const SbasisContract& contract) {
    // present=false (v1 / pre-B6 file): the defaults reproduce the pre-B6 replay
    // behavior exactly (the emitter's default threshold, no clock-disc channel,
    // no staleness horizon, no optional code-jump / clock-set channels).
    return NetworkProductEmitter(contract.disc_threshold_m, contract.emit_clock_disc,
                                 contract.staleness_horizon_s,
                                 contract.code_bias_jump_test,
                                 contract.clock_disc_on_set_change,
                                 contract.disc_threshold_frac_cycles);
}

// ===========================================================================
// NetworkProductTail (RTA-6)
// ===========================================================================

namespace {
/// Logger used by the tail (matches replay()'s fallback chain).
std::shared_ptr<spdlog::logger> tailLog() {
    auto lg = spdlog::get("app");
    return lg ? lg : spdlog::default_logger();
}
} // namespace

/// File-follow + parse state.  All parse semantics live in the shared
/// LineAssembler; this only manages the byte stream (partial-line buffering,
/// mid-file join, truncation/rotation) and the owned replay emitter.  The file
/// is opened PER POLL (open + seek is cheap at the throttled poll cadence): no
/// handle is held between polls, so the tail never blocks a producer-side
/// delete/rotation on Windows (an open ifstream lacks FILE_SHARE_DELETE).
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
        // Mid-file join: skip everything already in the file.  We may land
        // mid-line (the producer could be mid-write); peek at the byte before
        // the join point — if it is '\n' we are on a line boundary and the next
        // appended line is consumed whole, otherwise drop bytes until the first
        // newline (the torn line's tail must not be parsed).
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
    return impl_->assembler.latest_status;   // RTA-10: reset with the assembler on rotation
}

std::vector<NetworkProductRecord> NetworkProductTail::poll() {
    std::vector<NetworkProductRecord> out;
    Impl& im = *impl_;

    std::error_code ec;
    const auto sz = std::filesystem::file_size(im.path, ec);
    if (ec) return out;   // file absent (not created yet / mid-rotation) — keep waiting

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

    // Open per poll (no handle held between polls — see Impl).  Consume the
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

    // Hand every COMPLETE line to the shared assembler; the partial trailing
    // line stays buffered until its newline arrives on a later poll.
    std::size_t nl_pos;
    while ((nl_pos = im.partial.find('\n')) != std::string::npos) {
        const std::string line = im.partial.substr(0, nl_pos);
        im.partial.erase(0, nl_pos + 1);
        if (auto rec = im.assembler.consume(line)) out.push_back(std::move(*rec));
        // First S contract: adopt the producer's replay-emitter construction —
        // but only while no epoch has been replayed yet (the emitter carries
        // cross-epoch baselines that a rebuild would discard).
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
        // Shared replay path — markDatumChange / retro-bump / emitToStore plus
        // the RTA-4 serialized-counter adoption (the mid-file-join mechanism).
        NetworkProductReader::replay(rec, store, impl_->emitter);
        ++impl_->delivered;
    }
    // M4 (contract v2): propagate the producer's advertised combined-σ age-scaling
    // coefficient (the S-line agek= token) into the consumer's store so the
    // measurement model can age-inflate the combined variance.  The S line does not
    // feed replay, so this is the only channel that carries agek to the consumer; it
    // is a store-wide scalar (default 0 = no inflation), refreshed from the current
    // contract each poll (cheap, idempotent).  Only when a contract has been seen.
    if (impl_->assembler.contract.present)
        store.setCombinedSigmaAgeCoeff(impl_->assembler.contract.age_scale_k);
    return recs;
}

} // namespace GnssProcessor
