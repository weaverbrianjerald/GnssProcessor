/**
 * @file CurlGlobalGuard.cpp
 * @brief Process-wide, exactly-once libcurl global initialisation (RTB-2(a)).
 */

#include "common/CurlGlobalGuard.hpp"
#include <curl/curl.h>
#include <mutex>

namespace GnssProcessor {

void ensureCurlGlobalInit() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // namespace GnssProcessor
