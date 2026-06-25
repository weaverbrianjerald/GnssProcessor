/**
 * @file ApplicationNetwork.cpp
 * @brief Implementation of Application::runNetwork (minimum-constraint
 *        S-system PPP-RTK NETWORK producer).
 *
 * Extracted from Application.cpp to keep the per-mode processing pipelines in
 * dedicated translation units (mirrors ApplicationBaseline.cpp /
 * ApplicationRealtime.cpp).  The network producer is a joint multi-station
 * estimator: per-station local normals reduced onto a shared satellite-
 * parameter block via an exact Schur complement, estimating the estimable
 * satellite clock + satellite phase/code bias + atmosphere under an explicitly
 * declared minimum-constraint S-basis, emitted in IGS-SSR convention for the
 * companion PPP_RTK_User consumer.
 *
 * See docs/ssystem_conformance_2026-06-15.md (§7) and the approved design plan
 * for the full architecture and the staged build-out (W1..W8).
 *
 * STATUS: W1 scaffold only.  Mode wiring (enum, JSON parse, dispatch,
 * DifferencingMode::None, forced uncombined) is live; the estimator, S-basis,
 * product emitter, and closed-loop harness land in subsequent slices.  This
 * entry point logs a clear "not yet implemented" error and returns non-zero so
 * a network config does not appear to succeed silently.
 */

#include "app/Application.hpp"

namespace GnssProcessor {

int Application::runNetwork() {
    auto log = log_manager_.getLogger("main_log");
    log->error(
        "PPP-RTK-Network (minimum-constraint S-system network producer) is not "
        "yet implemented — W1 mode scaffold only.  The joint multi-station "
        "estimator, explicit S-basis, product emitter, and closed-loop "
        "validation harness are staged in subsequent slices.");
    return 1;
}

} // namespace GnssProcessor
