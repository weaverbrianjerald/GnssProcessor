/**
 * @file CodeBiasGap.cpp
 * @brief Edges, windows and arithmetic of the consumer's code-bias gap response (CodeBiasGap.hpp).
 */

#include "realtime/CodeBiasGap.hpp"

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

namespace {
std::string gapKey(const std::string& sat, const std::string& signal) { return sat + "|" + signal; }
}  // namespace

void CodeBiasGap::configure(const Config& c) {
    cfg_ = c;
    if (cfg_.revalidate_ep < 1) cfg_.revalidate_ep = 20;
    if (cfg_.window_ep < 1) cfg_.window_ep = 120;
}

void CodeBiasGap::observe(const std::string& sat, const std::string& signal, bool has_bias) {
    const std::string key = gapKey(sat, signal);
    auto it = open_.find(key);
    if (!has_bias) {
        if (it == open_.end()) {
            open_[key] = 1;
            ++ctr_.opened;
            events_.push_back({ EventKind::Open, sat, signal, 0.0, 0 });
            // A window still running for this satellite is superseded by the new gap: its
            // verdict would be read on code that is about to lose its bias.
            if (windows_.erase(sat)) {
                ++ctr_.expired;
                events_.push_back({ EventKind::Expired, sat, "", 0.0, 0 });
            }
        } else {
            ++it->second;
        }
        ++ctr_.gap_epochs;
        return;
    }
    if (it == open_.end()) return;
    const int lasted = it->second;
    open_.erase(it);
    ++ctr_.closed;
    events_.push_back({ EventKind::Close, sat, signal, 0.0, lasted });
    // The window opens when the satellite's last open signal closes.
    if (!satInGap(sat)) {
        if (windows_.count(sat)) {                 // a window of an earlier gap still running
            ++ctr_.expired;
            events_.push_back({ EventKind::Expired, sat, "", 0.0, 0 });
        }
        windows_[sat] = Window{};
        ++ctr_.windows;
    }
}

bool CodeBiasGap::inGap(const std::string& sat, const std::string& signal) const {
    return open_.count(gapKey(sat, signal)) > 0;
}

bool CodeBiasGap::satInGap(const std::string& sat) const {
    const std::string prefix = sat + "|";
    auto it = open_.lower_bound(prefix);
    return it != open_.end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> CodeBiasGap::excludedRowKeys() const {
    std::vector<std::string> keys;
    if (cfg_.mode != Mode::Full) return keys;
    for (const auto& [key, n] : open_) {
        (void)n;
        const auto bar = key.find('|');
        keys.push_back(key.substr(0, bar) + "_" + key.substr(bar + 1));
    }
    return keys;
}

bool CodeBiasGap::barred(const std::string& sat) const {
    if (cfg_.mode != Mode::Full) return false;
    if (satInGap(sat)) return true;
    const auto it = windows_.find(sat);
    return it != windows_.end() && it->second.age < cfg_.revalidate_ep;
}

std::vector<std::string> CodeBiasGap::windowSatellites() const {
    std::vector<std::string> sats;
    sats.reserve(windows_.size());
    for (const auto& [sat, w] : windows_) { (void)w; sats.push_back(sat); }
    return sats;
}

std::optional<CodeBiasGap::EventKind> CodeBiasGap::sample(const std::string& sat,
                                                          std::optional<double> offset_cyc) {
    auto it = windows_.find(sat);
    if (it == windows_.end()) return std::nullopt;
    Window& w = it->second;
    ++w.age;
    if (offset_cyc) {
        ++w.run;
        w.sum += *offset_cyc;
        ++ctr_.tested_epochs;
        if (w.run >= cfg_.revalidate_ep) {
            const double mean = w.sum / w.run;
            const int n = w.run;
            const bool release = std::abs(mean) > kReleaseCycles;
            windows_.erase(it);
            if (release) {
                ++ctr_.released;
                events_.push_back({ EventKind::Released, sat, "", mean, n });
                windows_[sat] = Window{};   // the re-floated satellite is barred and tested again
                ++ctr_.windows;
                return EventKind::Released;
            }
            ++ctr_.validated;
            events_.push_back({ EventKind::Validated, sat, "", mean, n });
            return EventKind::Validated;
        }
    } else {
        w.run = 0;
        w.sum = 0.0;
    }
    // A run already under way finishes past the window's end; a window with no run ends.
    if (w.age >= cfg_.window_ep && w.run == 0) {
        windows_.erase(it);
        ++ctr_.expired;
        events_.push_back({ EventKind::Expired, sat, "", 0.0, 0 });
        return EventKind::Expired;
    }
    return std::nullopt;
}

bool CodeBiasGap::closureHolds() const {
    return ctr_.validated + ctr_.released + ctr_.expired + windows_.size() == ctr_.windows;
}

std::map<std::string, double> CodeBiasGap::medianRemoved(const std::map<std::string, double>& d) {
    std::map<char, std::vector<double>> by_sys;
    for (const auto& [sat, v] : d)
        if (!sat.empty()) by_sys[sat[0]].push_back(v);
    std::map<char, double> median;
    for (auto& [sys, v] : by_sys) {
        if (v.size() < 4) continue;
        std::sort(v.begin(), v.end());
        const std::size_t n = v.size();
        median[sys] = (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    }
    std::map<std::string, double> out;
    for (const auto& [sat, v] : d) {
        if (sat.empty()) continue;
        const auto m = median.find(sat[0]);
        if (m != median.end()) out[sat] = v - m->second;
    }
    return out;
}

double CodeBiasGap::correctedMelbourneWubbena(double f1, double f2, double phi1_m, double phi2_m,
                                              double p1_m, double p2_m, double pb1_m, double pb2_m,
                                              double cb1_m, double cb2_m) {
    constexpr double kC = 299792458.0;
    const double l1 = kC / f1;
    const double l2 = kC / f2;
    const double lw = kC / (f1 - f2);
    const double phase_wl = (phi1_m - pb1_m) / l1 - (phi2_m - pb2_m) / l2;
    const double code_nl = (f1 * (p1_m - cb1_m) + f2 * (p2_m - cb2_m)) / (f1 + f2);
    return phase_wl - code_nl / lw;
}

CodeBiasGap::Mode codeBiasGapModeFromString(const std::string& s) {
    if (s == "revalidate") return CodeBiasGap::Mode::Revalidate;
    if (s == "full") return CodeBiasGap::Mode::Full;
    return CodeBiasGap::Mode::Off;
}

const char* codeBiasGapModeName(CodeBiasGap::Mode m) {
    switch (m) {
    case CodeBiasGap::Mode::Off:        return "off";
    case CodeBiasGap::Mode::Revalidate: return "revalidate";
    case CodeBiasGap::Mode::Full:       return "full";
    }
    return "off";
}

const char* codeBiasGapEventName(CodeBiasGap::EventKind k) {
    switch (k) {
    case CodeBiasGap::EventKind::Open:      return "cbgap_open";
    case CodeBiasGap::EventKind::Close:     return "cbgap_close";
    case CodeBiasGap::EventKind::Validated: return "cbgap_validated";
    case CodeBiasGap::EventKind::Released:  return "cbgap_released";
    case CodeBiasGap::EventKind::Expired:   return "cbgap_expired";
    }
    return "cbgap";
}

}  // namespace GnssProcessor
