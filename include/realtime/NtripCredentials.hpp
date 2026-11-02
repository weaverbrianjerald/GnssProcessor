#pragma once
/**
 * @file NtripCredentials.hpp
 * @brief The login an NTRIP stream presents to its caster: the config's password, else
 *        the netrc entry of the caster host.
 *
 * Config files are committed, so they carry no password.  Each NTRIP stream (the primary
 * observation stream, the SSR corrections stream, the broadcast-ephemeris stream and the
 * per-station lanes of the network front end) resolves its login in this order:
 *   1. The config carries a password: the config's login and password are used as
 *      written.  No file is read, so a config that still holds a password behaves as it
 *      always did.
 *   2. Otherwise the netrc entry of the caster host (@c NetrcFile.hpp for the file
 *      location and syntax).  When the config names a login, only the entry with that
 *      login matches; with no login in the config, the entry supplies both.
 *   3. Otherwise the config's login, if any, with an empty password (an anonymous
 *      stream when the config has no login), the behaviour before this lookup existed.
 * The netrc is read on every connection attempt, so a password changed in the file takes
 * effect at the next reconnect without a restart.
 */

#include <filesystem>
#include <string>

namespace GnssProcessor {

/// Where the credentials of one NTRIP stream came from.
enum class NtripCredentialOrigin {
    Config,               ///< The config carries a password; login and password are used as written.
    Netrc,                ///< No password in the config; login and password come from the netrc entry of the host.
    NoNetrcFile,          ///< No password in the config and no netrc file exists.
    UnreadableNetrcFile,  ///< No password in the config; the netrc file cannot be read or is not text.
    NoNetrcEntry          ///< No password in the config; the netrc file has no usable entry for the host (and login).
};

/// The login to present to a caster, and where it came from.
struct NtripCredentials {
    std::string           username;                  ///< Empty: connect without authentication.
    std::string           password;                  ///< Empty with a non-empty username: authenticate with an empty password.
    NtripCredentialOrigin origin{ NtripCredentialOrigin::Config };
    std::filesystem::path netrc_file;                ///< The file that was read; empty when none was.
};

/**
 * @brief Resolve the login of one NTRIP stream (order in the file banner).
 *
 * Reads the netrc file only when @p password is empty.  Never throws.
 *
 * @param host       Caster host name, the key of the netrc entry.
 * @param username   Login from the config (may be empty).
 * @param password   Password from the config (may be empty).
 * @param netrc_path Configured netrc file (@c data_download.netrc_path), or empty.
 * @param home       Home directory for the default netrc locations.
 * @return The login to use and its origin.
 */
NtripCredentials resolveNtripCredentials(const std::string& host,
                                         const std::string& username,
                                         const std::string& password,
                                         const std::filesystem::path& netrc_path,
                                         const std::filesystem::path& home);

/// @overload Uses the user's home directory (@c HOME, else @c USERPROFILE).
NtripCredentials resolveNtripCredentials(const std::string& host,
                                         const std::string& username,
                                         const std::string& password,
                                         const std::filesystem::path& netrc_path = {});

/**
 * @brief One phrase for the log that says where the login came from.
 *
 * Names the netrc file but never the login or the password.
 *
 * @param credentials Result of @ref resolveNtripCredentials.
 * @return The phrase, or an empty string when the config's own login was used as written.
 */
std::string describeNtripCredentials(const NtripCredentials& credentials);

} // namespace GnssProcessor
