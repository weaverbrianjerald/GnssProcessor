#pragma once
/**
 * @file CurlGlobalGuard.hpp
 * @brief Process-wide, exactly-once libcurl global initialisation (RTB-2(a)).
 *
 * libcurl requires @c curl_global_init to run EXACTLY ONCE per process,
 * before any @c curl_easy_init, and documents both it and
 * @c curl_global_cleanup as NOT thread-safe.  Two independent subsystems in
 * this codebase create curl handles — @c DataDownloader (product downloads)
 * and @c NtripClient (streaming NTRIP transport, one receive thread per
 * client) — historically with init/cleanup living only in DataDownloader's
 * constructor/destructor.  That had two failure modes:
 *   1. an NTRIP-only real-time session (no DataDownloader constructed)
 *      never ran @c curl_global_init at all, relying on libcurl's lazy
 *      auto-init from @c curl_easy_init — which is documented as
 *      thread-UNSAFE precisely in the multi-threaded receive-thread case;
 *   2. destroying a DataDownloader ran @c curl_global_cleanup while NTRIP
 *      receive threads were still inside @c curl_easy_perform — undefined
 *      behaviour per the libcurl documentation.
 *
 * @c ensureCurlGlobalInit() is the single fix: every curl-using subsystem
 * calls it before its first handle creation; a @c std::call_once guard runs
 * the global init exactly once regardless of construction order or thread.
 *
 * There is deliberately NO matching cleanup call: @c curl_global_cleanup is
 * only safe once every curl-using thread in the process has stopped, which
 * no single subsystem can know.  Skipping it at process exit is explicitly
 * sanctioned by the libcurl documentation ("the operating system reclaims
 * the memory") — the resources are process-lifetime by design.
 */

namespace GnssProcessor {

/**
 * @brief Run @c curl_global_init(CURL_GLOBAL_DEFAULT) exactly once per process.
 *
 * Thread-safe and idempotent (@c std::call_once).  Call before the first
 * @c curl_easy_init on any code path — currently @c DataDownloader's
 * constructor and @c NtripClient::start().  Never paired with
 * @c curl_global_cleanup; see the file Doxygen for why.
 */
void ensureCurlGlobalInit();

} // namespace GnssProcessor
