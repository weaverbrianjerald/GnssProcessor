/**
 * @file ConsumerSatTrace.cpp
 * @brief Card CST: the pure CSV formatter for @ref GnssProcessor::ConsumerSatRow.
 *
 * Deliberately separate from both the engine (which fills the row) and
 * NetworkRunTraces (which owns the stream): the schema is then a kernel that a
 * unit test can pin without constructing a RealTimeEngine or touching a file.
 */

#include "realtime/ConsumerSatTrace.hpp"

#include <fmt/format.h>

namespace GnssProcessor {

namespace {
/// A metre quantity at 6 dp, or an EMPTY field when the consumer had none.
/// Blank is never "0" — an absent phase bias and a zero phase bias are
/// different wires and the instrument exists to tell them apart.
inline std::string optM(bool have, double v) {
    return have ? fmt::format("{:.6f}", v) : std::string();
}
/// An integer counter, or an EMPTY field when that channel carries none.
inline std::string optI(bool have, int v) {
    return have ? fmt::format("{}", v) : std::string();
}
/// A variance at 6 significant digits (they span ~1e-8 .. 1e4 m² over an arc).
inline std::string optVar(bool have, double v) {
    return have ? fmt::format("{:.6g}", v) : std::string();
}
} // namespace

std::string formatConsumerSatRow(int week, double tow, const ConsumerSatRow& r) {
    return fmt::format(
        "{},{:.3f},{},{},{},{:.3f},{},"          // week,tow,sat,sig,code_sig,elev,n_sig
        "{},{},{},"                              // clk_m,cb_m,pb_m
        "{},{},{},"                              // clk_disc,cb_disc,pb_disc
        "{},{},{},"                              // ev_pb,ev_cb,ev_clk
        "{},{},"                                 // innov_code_m,innov_phase_m
        "{},{},"                                 // res_code_m,res_phase_m
        "{},{},{},{}\n",                         // amb_m,amb_var,iono_m,iono_var
        week, tow, r.sat, r.sig, r.code_sig, r.elev_deg, r.n_sig,
        optM(r.have_clk, r.clk_m), optM(r.have_cb, r.cb_m), optM(r.have_pb, r.pb_m),
        optI(r.have_clk_disc, r.clk_disc), optI(r.have_cb_disc, r.cb_disc),
        optI(r.have_pb_disc, r.pb_disc),
        r.ev_pb, r.ev_cb, r.ev_clk,
        optM(r.have_innov_code, r.innov_code_m), optM(r.have_innov_phase, r.innov_phase_m),
        optM(r.have_res_code, r.res_code_m), optM(r.have_res_phase, r.res_phase_m),
        optM(r.have_amb, r.amb_m), optVar(r.have_amb, r.amb_var),
        optM(r.have_iono, r.iono_m), optVar(r.have_iono, r.iono_var));
}

} // namespace GnssProcessor
