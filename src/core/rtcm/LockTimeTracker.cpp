/**
 * @file LockTimeTracker.cpp
 * @brief MSM lock-time → loss-of-lock (LLI 0x1) history (RTB-3).
 *
 * See the header for the detection rule (RTKLIB @c lossoflock equivalent)
 * and the keying / threading contract.
 */

#include "rtcm/LockTimeTracker.hpp"

namespace GnssProcessor {

bool LockTimeTracker::update(const std::string& station, const std::string& sat,
                             const std::string& signal, double lock_ms) {
    std::string key;
    key.reserve(station.size() + sat.size() + signal.size() + 2);
    key.append(station).push_back('|');
    key.append(sat).push_back('|');
    key.append(signal);

    // A never-seen key reads as prev = 0 (RTKLIB's zero-initialised lock
    // array): a fresh signal already holding lock (> 0) does not flag; a
    // fresh signal at lock 0 does.
    double& prev = last_lock_ms_[key];   // value-initialised to 0.0 on insert
    const bool loss = (lock_ms < prev) || (lock_ms == 0.0 && prev == 0.0);
    prev = lock_ms;
    return loss;
}

} // namespace GnssProcessor
