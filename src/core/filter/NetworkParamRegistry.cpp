/**
 * @file NetworkParamRegistry.cpp
 * @brief The network parameter registry (see NetworkParamRegistry.hpp; card PRM).
 */
#include "filter/NetworkParamRegistry.hpp"

#include "app/NetworkRunContext.hpp"     // NetworkKnobs
#include "config/GnssConfig.hpp"
#include "filter/NetworkResponses.hpp"   // the declared response sites (card PRM phase 3a)
#include "filter/NetworkRunTypes.hpp"    // kRxCbMaxGapS

#include <fmt/format.h>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace GnssProcessor {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// The route of a border key: the dense information block when C1 is armed, else the
/// diagonal D2 prior.
ParamRoute borderRoute(const NetworkKnobs& k) {
    return k.fullcov_on ? ParamRoute::DenseBorder : ParamRoute::DiagonalBorder;
}

/// The route of a carried local column that is not in the whitened block: a conditional
/// coupling row with the border when card 1 is armed, else a diagonal station prior.
ParamRoute localRowRoute(const NetworkKnobs& k) {
    return k.carry_cpl_cond ? ParamRoute::ConditionalRow : ParamRoute::DiagonalStationPrior;
}

std::string num(double v) {
    return std::isfinite(v) ? fmt::format("{:.3g}", v) : std::string("-");
}

std::string jnum(double v) {
    return std::isfinite(v) ? fmt::format("{:.17g}", v) : std::string("null");
}

} // namespace

NetworkParamKind paramKindOf(SharedParamKind kind) {
    switch (kind) {
        case SharedParamKind::Clock:     return NetworkParamKind::SatClock;
        case SharedParamKind::PhaseBias: return NetworkParamKind::SatPhaseBias;
        case SharedParamKind::CodeBias:  return NetworkParamKind::SatCodeBias;
        case SharedParamKind::VtecSh:    return NetworkParamKind::VtecSh;
    }
    return NetworkParamKind::SatClock;
}

const char* paramKindName(NetworkParamKind kind) {
    switch (kind) {
        case NetworkParamKind::SatClock:     return "sat_clock";
        case NetworkParamKind::SatPhaseBias: return "sat_phase_bias";
        case NetworkParamKind::SatCodeBias:  return "sat_code_bias";
        case NetworkParamKind::VtecSh:       return "vtec_sh";
        case NetworkParamKind::Ambiguity:    return "ambiguity";
        case NetworkParamKind::Zwd:          return "zwd";
        case NetworkParamKind::SlantIono:    return "slant_iono";
        case NetworkParamKind::RxClock:      return "rx_clock";
        case NetworkParamKind::RxPhaseBias:  return "rx_phase_bias";
        case NetworkParamKind::RxCodeBias:   return "rx_code_bias";
        case NetworkParamKind::PhaseColour:  return "phase_colour";
        case NetworkParamKind::RxPosition:   return "rx_position";
    }
    return "?";
}

const char* paramQRuleName(ParamQRule rule) {
    switch (rule) {
        case ParamQRule::None:                return "none";
        case ParamQRule::RandomWalk:          return "rw";
        case ParamQRule::RandomWalkPerDegree: return "rw_per_degree";
        case ParamQRule::AnchorElapsed:       return "anchor_elapsed";
        case ParamQRule::RandomWalkPerEpoch:  return "rw_per_epoch";
        case ParamQRule::GaussMarkov:         return "gauss_markov";
    }
    return "?";
}

const char* paramRouteName(ParamRoute route) {
    switch (route) {
        case ParamRoute::NotCarried:           return "not_carried";
        case ParamRoute::DenseBorder:          return "dense_border";
        case ParamRoute::DiagonalBorder:       return "diagonal_border";
        case ParamRoute::LocalBlock:           return "local_block";
        case ParamRoute::ConditionalRow:       return "conditional_row";
        case ParamRoute::DiagonalStationPrior: return "diagonal_station_prior";
        case ParamRoute::PriorRow:             return "prior_row";
    }
    return "?";
}

NetworkParamRegistry NetworkParamRegistry::build(const NetworkKnobs& k, const GnssConfig& config) {
    NetworkParamRegistry r;
    auto set = [&](NetworkParamKind kind, ParamSpec s) {
        s.kind = kind;
        s.name = paramKindName(kind);
        r.specs_[static_cast<std::size_t>(kind)] = std::move(s);
    };

    // --- Border kinds: always carried (D2 / C1), q from the knob snapshot. ---
    {
        ParamSpec s;
        s.estimated = s.carried = true;
        s.q = k.q_clock;
        s.q_rule = k.cta_on ? ParamQRule::AnchorElapsed : ParamQRule::RandomWalk;
        if (k.cta_on) s.q_anchored = k.cta_q;
        s.var_floor_m2 = k.cta_on ? k.cta_var_floor : 0.0;
        s.birth_var_m2 = kNaN;
        s.gap_horizon_s = kNaN;   // a border kind: a departed column is READMIT's park, not a prune
        s.route = borderRoute(k);
        s.source = "filter.process_noise.sat_clock_q";
        if (k.cta_on)
            s.source += "; anchored at network.clock_anchor_q, floor network.clock_anchor_var_floor_m2";
        if (k.clk_model_on)
            s.source += "; network.clock_model: the two-state oscillator carries the clock, "
                        "not this fold";
        set(NetworkParamKind::SatClock, std::move(s));
    }
    {
        ParamSpec s;
        s.estimated = s.carried = true;
        s.q = k.q_pb;
        s.q_rule = ParamQRule::RandomWalk;
        s.var_floor_m2 = k.pb_var_floor;
        s.birth_var_m2 = kNaN;
        s.gap_horizon_s = kNaN;
        s.route = borderRoute(k);
        s.source = "filter.process_noise.sat_phase_bias_q; floor network.shared_carry_var_floor_pb_m2";
        set(NetworkParamKind::SatPhaseBias, std::move(s));
    }
    {
        ParamSpec s;
        s.estimated = s.carried = config.estimateNetworkSatCodeBias();
        s.q = k.q_cb;
        s.q_rule = s.carried ? ParamQRule::RandomWalk : ParamQRule::None;
        s.var_floor_m2 = k.cb_var_floor;
        s.birth_var_m2 = kNaN;
        s.gap_horizon_s = kNaN;
        s.route = s.carried ? borderRoute(k) : ParamRoute::NotCarried;
        s.source = "filter.process_noise.sat_code_bias_q; floor network.shared_carry_var_floor_cb_m2; "
                   "network.estimate_sat_code_bias";
        set(NetworkParamKind::SatCodeBias, std::move(s));
    }
    {
        ParamSpec s;
        s.estimated = s.carried = config.estimateNetworkIonoSh();
        s.q = config.getGlobalVtecShConfig().q_per_degree;
        s.q_units = "TECU^2/s";
        s.q_rule = s.carried ? ParamQRule::RandomWalkPerDegree : ParamQRule::None;
        s.var_floor_m2 = 0.0;
        s.birth_var_m2 = kNaN;
        s.gap_horizon_s = kNaN;
        s.route = s.carried ? ParamRoute::DiagonalBorder : ParamRoute::NotCarried;
        s.source = "estimation.global_vtec_sh.q_per_degree; network.estimate_iono_sh";
        set(NetworkParamKind::VtecSh, std::move(s));
    }

    // --- Station-local kinds. ---
    {
        ParamSpec s;
        s.estimated = true;
        s.carried = k.pl_on && !k.carry_invariant;
        s.q = k.pl_q_amb;
        s.q_rule = s.carried ? ParamQRule::RandomWalk : ParamQRule::None;
        s.var_floor_m2 = k.pl_var_floor;
        s.birth_var_m2 = k.amb_birth_var_floor;
        // Card PRM phase 3b: the gap prune's horizon (and the leveling vehicle's inject
        // skip's), the knob snapshot's double whether or not the kind is carried.
        s.gap_horizon_s = k.pl_max_gap_s;
        if (!s.carried)
            s.route = ParamRoute::NotCarried;
        else if (k.pl_local_block && k.carry_cpl_cond && !k.carry_cpl_joint)
            s.route = ParamRoute::LocalBlock;
        else
            s.route = localRowRoute(k);
        s.source = "network.phase_leveling_q_amb; floor network.phase_leveling_var_floor_m2; "
                   "birth network.amb_float_birth_var_m2; gap network.phase_leveling_max_gap_s";
        if (k.carry_invariant)
            s.source += "; network.carry_invariant: the invariant DD carry, not this route";
        set(NetworkParamKind::Ambiguity, std::move(s));
    }
    {
        ParamSpec s;
        s.estimated = true;
        s.carried = k.zwd_on;
        s.q = k.zwd_q;
        s.q_rule = s.carried ? ParamQRule::RandomWalk : ParamQRule::None;
        s.var_floor_m2 = 0.0;
        s.birth_var_m2 = kNaN;
        s.gap_horizon_s = k.pl_max_gap_s;   // the ZWD carry rides the leveling carry's horizon
        s.route = !s.carried ? ParamRoute::NotCarried
                : k.zwd_in_block ? ParamRoute::LocalBlock : localRowRoute(k);
        s.source = "network.zwd_carry_q; network.carry_zwd, network.carry_zwd_in_block; "
                   "gap network.phase_leveling_max_gap_s";
        set(NetworkParamKind::Zwd, std::move(s));
    }
    {
        // The two slant-ionosphere carries are exclusive (the loader refuses both); the
        // horizon declared here is the one carry in force, and both prunes read it.
        if (k.iono_blk && k.iono_on)
            throw std::logic_error("NetworkParamRegistry: carry_iono and carry_iono_in_block "
                                   "are both armed; the loader refuses this configuration");
        ParamSpec s;
        s.estimated = true;
        s.carried = k.iono_blk || k.iono_on;
        if (k.iono_blk) {
            s.q = k.iono_blk_q_m2;
            s.q_units = "m^2/epoch";
            s.q_rule = ParamQRule::RandomWalkPerEpoch;
            s.var_floor_m2 = 0.0;
            s.gap_horizon_s = k.iono_blk_max_gap_s;
            s.route = ParamRoute::LocalBlock;
            s.source = "network.carry_iono_in_block_q_m2 (L1 slant delay); "
                       "gap network.carry_iono_in_block_max_gap_s";
        } else if (k.iono_on) {
            s.q = k.iono_q;
            s.q_rule = ParamQRule::RandomWalk;
            s.var_floor_m2 = k.iono_var_floor;
            s.gap_horizon_s = k.iono_max_gap_s;
            s.route = ParamRoute::DiagonalStationPrior;
            s.source = "network.iono_carry_q; floor network.iono_carry_var_floor; "
                       "gap network.iono_carry_max_gap_s";
        } else {
            s.q = 0.0;
            s.q_rule = ParamQRule::None;
            s.var_floor_m2 = 0.0;
            s.gap_horizon_s = kNaN;
            s.route = ParamRoute::NotCarried;
            s.source = "network.carry_iono, network.carry_iono_in_block (both off)";
        }
        s.birth_var_m2 = kNaN;
        set(NetworkParamKind::SlantIono, std::move(s));
    }
    {
        ParamSpec s;
        s.estimated = true;
        s.carried = k.rxclk_blk;
        s.q = k.rxclk_q_m2;
        s.q_units = "m^2/epoch";
        s.q_rule = s.carried ? ParamQRule::RandomWalkPerEpoch : ParamQRule::None;
        s.var_floor_m2 = 0.0;
        s.birth_var_m2 = kNaN;
        s.gap_horizon_s = k.rxclk_max_gap_s;
        s.route = s.carried ? ParamRoute::LocalBlock : ParamRoute::NotCarried;
        s.source = "network.carry_rx_clock_q_m2; network.carry_rx_clock_in_block; "
                   "gap network.carry_rx_clock_max_gap_s";
        set(NetworkParamKind::RxClock, std::move(s));
    }
    {
        ParamSpec s;
        s.estimated = k.phase_datum_eq34;
        s.carried = k.phase_datum_eq34 && !k.carry_invariant && k.eq34_rpb_q > 0.0;
        s.q = k.eq34_rpb_q;
        s.q_rule = s.carried ? ParamQRule::RandomWalk : ParamQRule::None;
        s.var_floor_m2 = 0.0;
        s.birth_var_m2 = kNaN;
        s.gap_horizon_s = k.pl_max_gap_s;   // the RPB carry rides the leveling carry's horizon
        s.route = s.carried ? ParamRoute::PriorRow : ParamRoute::NotCarried;
        s.source = "network.phase_datum_eq34_rpb_q_m2s; network.phase_datum_eq34; "
                   "gap network.phase_leveling_max_gap_s";
        set(NetworkParamKind::RxPhaseBias, std::move(s));
    }
    {
        ParamSpec s;
        s.estimated = true;
        s.carried = k.rx_cb_on;
        s.q = k.rx_cb_q;
        s.q_rule = s.carried ? ParamQRule::RandomWalk : ParamQRule::None;
        s.var_floor_m2 = 0.0;
        s.birth_var_m2 = kNaN;
        s.gap_horizon_s = kRxCbMaxGapS;   // a constant (SPEC_rx_cb_carry section 3), not a knob
        s.route = !s.carried ? ParamRoute::NotCarried
                : k.rxcb_blk ? ParamRoute::LocalBlock : ParamRoute::DiagonalStationPrior;
        s.source = "network.rx_cb_carry_q; network.carry_rx_code_bias, "
                   "network.carry_rx_code_bias_in_block; gap kRxCbMaxGapS (fixed)";
        set(NetworkParamKind::RxCodeBias, std::move(s));
    }
    {
        // Card LGM: the registry's first new kind.  q is the stationary variance at the
        // zenith; the elevation law and the time constant complete the model.
        ParamSpec s;
        s.estimated = s.carried = k.pc_on;
        s.q = k.pc_sigma_zen * k.pc_sigma_zen;
        s.q_units = "m^2";
        s.q_rule = k.pc_on ? ParamQRule::GaussMarkov : ParamQRule::None;
        s.tau_s = k.pc_on ? k.pc_tau_s : kNaN;
        s.var_floor_m2 = 0.0;
        s.birth_var_m2 = k.pc_on ? s.q : kNaN;
        s.gap_horizon_s = k.pc_max_gap_s;
        s.route = k.pc_on ? ParamRoute::LocalBlock : ParamRoute::NotCarried;
        s.source = "network.phase_colour_sigma_zen_m, network.phase_colour_tau_s; sigma(e) = "
                   "sigma_zen / sin(max(e, network.phase_colour_elev_floor_deg)); "
                   "network.phase_colour_state; gap network.phase_colour_max_gap_s";
        set(NetworkParamKind::PhaseColour, std::move(s));
    }
    {
        // Card NOM N1: the station coordinates are columns of every station's state
        // manager that networkSplitStation_ keeps out of the local block (the network holds
        // each station at its roster coordinate; the A6 linearity guard throws if one
        // reaches A_r).  Declared here so the parameter inventory's Pinned instances have
        // a kind; nothing about the solve is read from this entry.
        ParamSpec s;
        s.estimated = s.carried = false;
        s.q = 0.0;
        s.q_rule = ParamQRule::None;
        s.var_floor_m2 = 0.0;
        s.birth_var_m2 = kNaN;
        s.gap_horizon_s = kNaN;
        s.route = ParamRoute::NotCarried;
        s.source = "held at the roster coordinate (networkSplitStation_; network.station_marker_to_arp "
                   "reduces it to the antenna reference point)";
        set(NetworkParamKind::RxPosition, std::move(s));
    }

    // Card PRM phase 3a: the responses armed per kind and event.  A declared response site
    // (filter/NetworkResponses.hpp) whose own gate holds on this snapshot contributes its
    // policy to its kind's event; an armed datum mechanism of card PRM phase 2 contributes
    // Stransform to the handover of every kind whose carried value it moves (a border kind
    // through prev_carry, a station-local kind through its value site, the owed value sites
    // included under network.carry_rebase_complete: card PRM phase 3b).
    for (int i = 0; i < kResponseSites; ++i) {
        const auto site = static_cast<ResponseSite>(i);
        if (!responseSiteArmed(site, k, config)) continue;
        const ResponseSiteSpec& rs = responseSiteSpec(site);
        r.specs_[static_cast<std::size_t>(rs.kind)].on[static_cast<std::size_t>(rs.event)] |=
            responsePolicyBit(rs.policy);
    }
    const auto onHandover = [&](NetworkParamKind kind) -> std::uint32_t& {
        return r.specs_[static_cast<std::size_t>(kind)]
            .on[static_cast<std::size_t>(ParamEvent::Handover)];
    };
    for (int m = 0; m < kCarryMechanisms; ++m) {
        const auto mech = static_cast<CarryMechanism>(m);
        if (!carryMechanismArmed(mech, k, config)) continue;
        const std::uint32_t border = carryMechanismBorderKinds(mech, k);
        for (int b = 0; b < 4; ++b)
            if (border & (std::uint32_t{1} << b))
                onHandover(paramKindOf(static_cast<SharedParamKind>(b))) |=
                    responsePolicyBit(ResponsePolicy::Stransform);
        for (int c = 0; c < kCarrySites; ++c) {
            NetworkParamKind lk;
            if (carryMechanismMoves(mech, static_cast<CarrySite>(c), k.rebase_complete)
                && carrySiteLocalKind(static_cast<CarrySite>(c), lk))
                onHandover(lk) |= responsePolicyBit(ResponsePolicy::Stransform);
        }
    }
    return r;
}

static std::string paramPolicyList(std::uint32_t mask, const char* sep) {
    std::string out;
    for (std::size_t p = 0; p < kResponsePolicies; ++p)
        if (mask & (std::uint32_t{1} << p))
            out += fmt::format("{}{}", out.empty() ? "" : sep,
                               responsePolicyName(static_cast<ResponsePolicy>(p)));
    return out;
}

std::vector<std::string> NetworkParamRegistry::responseTable() const {
    std::vector<std::string> out;
    for (const auto& s : specs_) {
        std::string line = fmt::format("{:<15}", s.name);
        bool any = false;
        for (std::size_t e = 0; e < kParamEvents; ++e) {
            if (!s.on[e]) continue;
            line += fmt::format("{}{}: {}", any ? "; " : " ",
                                paramEventName(static_cast<ParamEvent>(e)),
                                paramPolicyList(s.on[e], "+"));
            any = true;
        }
        if (!any) line += " none";
        out.push_back(std::move(line));
    }
    return out;
}

double NetworkParamRegistry::q(SharedParamKind kind) const {
    return spec(paramKindOf(kind)).q;
}

std::vector<std::string> NetworkParamRegistry::modelTable() const {
    std::vector<std::string> out;
    out.push_back(fmt::format("{:<15} {:>3} {:>3} {:>10} {:<9} {:<14} {:>10} {:>6} {:>9} {:>9} {:>7} {:<22} {}",
                              "kind", "est", "car", "q", "units", "q_rule", "q_anchored",
                              "tau_s", "floor_m2", "birth_m2", "gap_s", "route", "source"));
    for (const auto& s : specs_) {
        out.push_back(fmt::format("{:<15} {:>3} {:>3} {:>10} {:<9} {:<14} {:>10} {:>6} {:>9} {:>9} {:>7} {:<22} {}",
                                  s.name, s.estimated ? "yes" : "no", s.carried ? "yes" : "no",
                                  num(s.q), s.q_units, paramQRuleName(s.q_rule),
                                  num(s.q_anchored), num(s.tau_s), num(s.var_floor_m2),
                                  num(s.birth_var_m2), num(s.gap_horizon_s),
                                  paramRouteName(s.route), s.source));
    }
    return out;
}

std::string NetworkParamRegistry::modelJson() const {
    auto esc = [](const std::string& v) {
        std::string o;
        o.reserve(v.size() + 4);
        for (char c : v) {
            if (c == '\\' || c == '"') o.push_back('\\');
            o.push_back(c);
        }
        return o;
    };
    std::string j = "{\"record\":\"param_model\",\"kinds\":[";
    for (std::size_t i = 0; i < specs_.size(); ++i) {
        const auto& s = specs_[i];
        j += fmt::format("{}{{\"kind\":\"{}\",\"estimated\":{},\"carried\":{},\"q\":{},"
                         "\"q_units\":\"{}\",\"q_rule\":\"{}\",\"q_anchored\":{},\"tau_s\":{},"
                         "\"var_floor_m2\":{},\"birth_var_m2\":{},\"gap_horizon_s\":{},"
                         "\"route\":\"{}\",\"source\":\"{}\",\"on\":{{",
                         i ? "," : "", s.name, s.estimated ? "true" : "false",
                         s.carried ? "true" : "false", jnum(s.q), s.q_units,
                         paramQRuleName(s.q_rule), jnum(s.q_anchored), jnum(s.tau_s),
                         jnum(s.var_floor_m2), jnum(s.birth_var_m2), jnum(s.gap_horizon_s),
                         paramRouteName(s.route), esc(s.source));
        // Card PRM phase 3a: the armed response policies per event.
        for (std::size_t e = 0; e < kParamEvents; ++e) {
            std::string list;
            for (std::size_t p = 0; p < kResponsePolicies; ++p)
                if (s.on[e] & (std::uint32_t{1} << p))
                    list += fmt::format("{}\"{}\"", list.empty() ? "" : ",",
                                        responsePolicyName(static_cast<ResponsePolicy>(p)));
            j += fmt::format("{}\"{}\":[{}]", e ? "," : "",
                             paramEventName(static_cast<ParamEvent>(e)), list);
        }
        j += "}}";
    }
    j += "]}";
    return j;
}

} // namespace GnssProcessor
