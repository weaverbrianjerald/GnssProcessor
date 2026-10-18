/**
 * @file ObservationEquationCheck.cpp
 * @brief Counters, bitwise comparison, carrier frequency audit and report of the
 *        observation-equation check (see measurement/ObservationEquationCheck.hpp).
 */

#include "measurement/ObservationEquationCheck.hpp"

#include "common/GnssIdentity.hpp"
#include "common/PppUtils.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <fmt/format.h>

namespace GnssProcessor {

namespace {

/// The counters as atomics (the producer builds its stations' rows in parallel).
struct AtomicCounts {
    std::atomic<long long> rows{0};
    std::atomic<long long> usable_mismatch{0};
    std::atomic<long long> unmapped_identity{0};
    std::atomic<long long> jacobian_mismatch{0};
    std::atomic<long long> predicted_mismatch{0};
};

AtomicCounts& counters() {
    static AtomicCounts c;
    return c;
}

/// The differing audit lines, written once.
struct AuditStore {
    std::once_flag           once;
    std::vector<std::string> lines;
};

AuditStore& auditStore() {
    static AuditStore a;
    return a;
}

constexpr System kSystems[] = {System::GPS, System::Galileo, System::GLONASS,
                               System::BeiDou, System::QZSS, System::SBAS};

/// The frequency text a band digit is searched in ("L1C").
std::string signalText(char digit) { return std::string("L") + digit + "C"; }

} // namespace

void obsEquationCheckRecord(bool usable_new, bool usable_reference, ObsFault fault,
                            bool jacobian_equal, bool predicted_equal) {
    AtomicCounts& c = counters();
    c.rows.fetch_add(1, std::memory_order_relaxed);
    if (usable_new != usable_reference) {
        c.usable_mismatch.fetch_add(1, std::memory_order_relaxed);
        if (fault == ObsFault::UnmappedIdentity)
            c.unmapped_identity.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!usable_new) return;
    if (!jacobian_equal) c.jacobian_mismatch.fetch_add(1, std::memory_order_relaxed);
    if (!predicted_equal) c.predicted_mismatch.fetch_add(1, std::memory_order_relaxed);
}

ObsEquationCheckCounts obsEquationCheckCounts() {
    const AtomicCounts& c = counters();
    ObsEquationCheckCounts out;
    out.rows               = c.rows.load(std::memory_order_relaxed);
    out.usable_mismatch    = c.usable_mismatch.load(std::memory_order_relaxed);
    out.unmapped_identity  = c.unmapped_identity.load(std::memory_order_relaxed);
    out.jacobian_mismatch  = c.jacobian_mismatch.load(std::memory_order_relaxed);
    out.predicted_mismatch = c.predicted_mismatch.load(std::memory_order_relaxed);
    return out;
}

void obsEquationCheckReset() {
    AtomicCounts& c = counters();
    c.rows.store(0);
    c.usable_mismatch.store(0);
    c.unmapped_identity.store(0);
    c.jacobian_mismatch.store(0);
    c.predicted_mismatch.store(0);
}

bool sameBits(double a, double b) {
    std::uint64_t ua = 0, ub = 0;
    std::memcpy(&ua, &a, sizeof ua);
    std::memcpy(&ub, &b, sizeof ub);
    return ua == ub;
}

bool sameBits(const Eigen::VectorXd& a, const Eigen::VectorXd& b) {
    if (a.size() != b.size()) return false;
    return a.size() == 0
        || std::memcmp(a.data(), b.data(), sizeof(double) * static_cast<std::size_t>(a.size())) == 0;
}

std::vector<ObsFrequencyRow> obsFrequencyAudit(LogManager& log) {
    std::vector<ObsFrequencyRow> rows;
    PppUtils slot0(log);
    for (const System sys : kSystems) {
        const ConstellationTraits& t = traitsOf(sys);
        for (std::size_t i = 0; i < t.n_bands; ++i) {
            ObsFrequencyRow row;
            row.sys         = sys;
            row.digit       = t.bands[i].digit;
            row.identity_hz = nominalCarrierFrequencyHz(t.bands[i].band);
            const std::string text = signalText(row.digit);
            if (sys != System::GLONASS) {
                row.ppp_utils_hz = slot0.getFrequency(text, sys, std::string(1, t.letter) + "01");
            } else {
                // The helper reads the channel of the satellite name, so each slot gets its
                // own helper with "R01" on that slot.
                const auto atSlot = [&](int k) {
                    PppUtils helper(log);
                    helper.setGlonassSlots({{"R01", k}});
                    return helper.getFrequency(text, sys, "R01");
                };
                const double f0 = atSlot(0);
                for (int k = -7; k <= 6; ++k)
                    row.slot_spread_hz = std::max(row.slot_spread_hz, std::abs(atSlot(k) - f0));
                row.ppp_utils_hz = f0;
            }
            rows.push_back(row);
        }
    }
    return rows;
}

std::vector<std::string> obsFrequencyDifferences(const std::vector<ObsFrequencyRow>& rows) {
    std::vector<std::string> lines;
    for (const ObsFrequencyRow& r : rows) {
        const bool differs = std::abs(r.ppp_utils_hz - r.identity_hz) > 1e-3
                          || r.slot_spread_hz > 1e-3;
        if (!differs) continue;
        lines.push_back(fmt::format(
            "frequency {} band {}: PppUtils {:.3f} Hz, identity {:.3f} Hz{}",
            systemName(r.sys), r.digit, r.ppp_utils_hz, r.identity_hz,
            r.slot_spread_hz > 1e-3
                ? fmt::format(" (PppUtils varies over FDMA slots by up to {:.1f} Hz; the row builder"
                              " takes each satellite's slot, the ambiguity seed and the identity"
                              " layer slot 0, the other seeds the default channel table)",
                              r.slot_spread_hz)
                : std::string()));
    }
    return lines;
}

void obsEquationCheckNoteFrequencies(LogManager& log) {
    AuditStore& store = auditStore();
    std::call_once(store.once, [&] { store.lines = obsFrequencyDifferences(obsFrequencyAudit(log)); });
}

std::vector<std::string> obsEquationCheckReport() {
    const ObsEquationCheckCounts c = obsEquationCheckCounts();
    std::vector<std::string> lines;
    if (c.rows == 0) return lines;
    lines.push_back(fmt::format(
        "obs_equation_check: {} rows compared, {} usable mismatches ({} unmapped identities), "
        "{} Jacobian mismatches, {} predicted mismatches",
        c.rows, c.usable_mismatch, c.unmapped_identity, c.jacobian_mismatch,
        c.predicted_mismatch));
    AuditStore& store = auditStore();
    for (const std::string& line : store.lines) lines.push_back("obs_equation_check: " + line);
    return lines;
}

} // namespace GnssProcessor
