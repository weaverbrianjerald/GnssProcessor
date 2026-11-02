/**
 * @file NtripCredentials.cpp
 * @brief NTRIP login resolution: config password, else netrc entry of the host (see NtripCredentials.hpp).
 */

#include "realtime/NtripCredentials.hpp"

#include "products/NetrcFile.hpp"

namespace GnssProcessor {

NtripCredentials resolveNtripCredentials(const std::string& host,
                                         const std::string& username,
                                         const std::string& password,
                                         const std::filesystem::path& netrc_path,
                                         const std::filesystem::path& home) {
    NtripCredentials out;
    out.username = username;
    out.password = password;
    if (!password.empty()) return out;   // the config's own login, no file read

    const NetrcLocation where = locateNetrcFile(netrc_path, home);
    if (where.origin == NetrcLocation::Origin::None) {
        out.origin = NtripCredentialOrigin::NoNetrcFile;
        return out;
    }
    out.netrc_file = where.path;

    const NetrcContents file = readNetrcFile(where.path);
    if (file.status != NetrcReadStatus::Ok) {
        out.origin = NtripCredentialOrigin::UnreadableNetrcFile;
        return out;
    }
    const NetrcEntry* entry = findNetrcEntry(file.entries, host, username);
    if (entry == nullptr) {
        out.origin = NtripCredentialOrigin::NoNetrcEntry;
        return out;
    }
    out.username = entry->login;
    out.password = entry->password;
    out.origin   = NtripCredentialOrigin::Netrc;
    return out;
}

NtripCredentials resolveNtripCredentials(const std::string& host,
                                         const std::string& username,
                                         const std::string& password,
                                         const std::filesystem::path& netrc_path) {
    return resolveNtripCredentials(host, username, password, netrc_path, homeDirectory());
}

std::string describeNtripCredentials(const NtripCredentials& credentials) {
    const std::string file = credentials.netrc_file.string();
    switch (credentials.origin) {
        case NtripCredentialOrigin::Config:
            return {};
        case NtripCredentialOrigin::Netrc:
            return "login and password from the netrc file " + file;
        case NtripCredentialOrigin::NoNetrcFile:
            return "no password in the config and no netrc file found "
                   "(data_download.netrc_path, ~/.netrc, ~/_netrc)";
        case NtripCredentialOrigin::UnreadableNetrcFile:
            return "no password in the config and the netrc file " + file
                   + " cannot be read as text (UTF-8 or ANSI expected)";
        case NtripCredentialOrigin::NoNetrcEntry:
            return "no password in the config and no entry with a login and password "
                   "for this host in the netrc file " + file;
    }
    return {};
}

} // namespace GnssProcessor
