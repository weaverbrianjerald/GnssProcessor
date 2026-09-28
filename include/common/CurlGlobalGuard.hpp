#pragma once
/**
 * @file CurlGlobalGuard.hpp
 * @brief Process-wide, exactly-once libcurl global initialisation (RTB-2(a)).
 *
 * libcurl requires @c curl_global_init to run once per process, before any
 * @c curl_easy_init, and documents both it and @c curl_global_cleanup as not
 * thread-safe.  Two subsystems create curl handles: @c DataDownloader (product
 * downloads) and @c NtripClient (streaming NTRIP transport, one receive thread per
 * client).  If global init and cleanup were owned by DataDownloader's constructor
 * and destructor alone, two failures would follow:
 *   1. an NTRIP-only real-time session (no DataDownloader constructed) would never
 *      call @c curl_global_init and would depend on libcurl's lazy auto-init from
 *      @c curl_easy_init, which libcurl documents as thread-unsafe, and that is the
 *      multi-threaded receive-thread case;
 *   2. destroying a DataDownloader would run @c curl_global_cleanup while NTRIP
 *      receive threads may still be inside @c curl_easy_perform, which is undefined
 *      behaviour per the libcurl documentation.
 *
 * @c ensureCurlGlobalInit() addresses both: every curl-using subsystem calls it
 * before its first handle creation, and a @c std::call_once guard runs the global
 * init once regardless of construction order or thread.
 *
 * There is no matching cleanup call, by design: @c curl_global_cleanup is safe only
 * after every curl-using thread in the process has stopped, which no single
 * subsystem can know.  The libcurl documentation permits skipping it at process exit
 * ("the operating system reclaims the memory"); the resources live for the lifetime
 * of the process.
 */

namespace GnssProcessor {

/**
 * @brief Run @c curl_global_init(CURL_GLOBAL_DEFAULT) exactly once per process.
 *
 * Thread-safe and idempotent (@c std::call_once).  Call before the first
 * @c curl_easy_init on any code path; the current callers are @c DataDownloader's
 * constructor and @c NtripClient::start().  Never paired with
 * @c curl_global_cleanup; the file-level Doxygen gives the reason.
 */
void ensureCurlGlobalInit();

} // namespace GnssProcessor
