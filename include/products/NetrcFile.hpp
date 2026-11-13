#pragma once
/**
 * @file NetrcFile.hpp
 * @brief Where the user's netrc file is, what it says, and the entry of one host.
 *
 * A login that must not sit in a tracked config file lives in the user's netrc file.
 * This header is the one owner of that file for the whole program: @c DataDownloader asks
 * it where the file is and hands the path to libcurl (the CDDIS downloads), and
 * @c resolveNtripCredentials reads the entry of a caster host itself (the NTRIP streams).
 *
 * File location, first match wins:
 *   1. the configured path (@c data_download.netrc_path) when it exists.  A configured
 *      path that does not exist is reported and the search goes on;
 *   2. @c ~/.netrc (Unix convention);
 *   3. @c ~/_netrc (Windows convention).
 * The home directory is @c HOME, falling back to @c USERPROFILE.
 *
 * File syntax, the part of the classic netrc format that libcurl also reads:
 * @code
 *   machine products.igs-ip.net login alice password "pass word"
 *   machine urs.earthdata.nasa.gov
 *       login bob
 *       password secret
 *   # a comment
 * @endcode
 * Tokens are separated by blanks or line ends and may be spread over several lines.  A
 * token in double quotes may hold blanks; a backslash inside it escapes the next
 * character (n, r and t stand for the line feed, carriage return and tab).  A @c # at the
 * start of a token begins a comment that runs to the end of the line.  @c account values
 * and @c macdef bodies are skipped.  The @c default entry is never returned: it would
 * hand one login to every host.
 */

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief The user's home directory: @c HOME, else @c USERPROFILE.
 * @return The directory, or an empty path when neither variable is set.
 */
std::filesystem::path homeDirectory();

/**
 * @brief Outcome of the search for the netrc file (see the file banner for the order).
 *
 * Carries enough for a caller to word its own log line: which place the file was found
 * in, whether a configured path was skipped, and the two auto-discovery candidates.
 */
struct NetrcLocation {
    /// The place the file was found in.
    enum class Origin {
        None,             ///< No file exists in any place searched.
        Configured,       ///< The configured path.
        DotNetrc,         ///< @c ~/.netrc
        UnderscoreNetrc   ///< @c ~/_netrc
    };

    /// Which place the file was found in; @c None when no searched place holds one.
    Origin                origin{ Origin::None };
    std::filesystem::path path;                 ///< The file to read; empty when @c origin is None.
    bool                  configured_missing{ false };  ///< A configured path was given but does not exist.
    bool                  no_home{ false };     ///< Auto-discovery was needed but there is no home directory.
    std::filesystem::path dot_netrc;            ///< The first auto-discovery candidate (empty with no home).
    std::filesystem::path underscore_netrc;     ///< The second auto-discovery candidate (empty with no home).
};

/**
 * @brief Find the netrc file.
 * @param configured Path from the config (@c data_download.netrc_path), or empty.
 * @param home       Home directory for auto-discovery (empty: no auto-discovery).
 * @return Where the file is, or @c Origin::None.
 */
NetrcLocation locateNetrcFile(const std::filesystem::path& configured,
                              const std::filesystem::path& home);

/**
 * @brief Find the netrc file, taking the home directory from homeDirectory().
 *
 * Overload of the two-argument form. Uses @ref GnssProcessor::homeDirectory "homeDirectory()".
 *
 * @param configured Path from the config (@c data_download.netrc_path), or empty.
 * @return Where the file is, or @c Origin::None.
 */
NetrcLocation locateNetrcFile(const std::filesystem::path& configured = {});

/// One @c machine entry of a netrc file.
struct NetrcEntry {
    std::string machine;    ///< Host name the entry is for.
    std::string login;      ///< User name (empty when the entry has none).
    std::string password;   ///< Password (empty when the entry has none).
};

/**
 * @brief The @c machine entries of netrc text, in file order.
 *
 * Never throws and accepts any input; text that is not netrc syntax gives no entries.
 * The @c default entry is dropped.
 *
 * @param text File contents (UTF-8 or ANSI, without a byte-order mark).
 * @return The entries.
 */
std::vector<NetrcEntry> parseNetrc(const std::string& text);

/**
 * @brief The first usable entry for a host.
 *
 * An entry is usable when it has a login and a password.  Host names compare without
 * regard to case; logins compare exactly.
 *
 * @param entries Entries from @ref GnssProcessor::parseNetrc "parseNetrc".
 * @param machine Host name to look up.
 * @param login   When not empty, only an entry with exactly this login matches (the
 *                config names the account, the file supplies the secret).
 * @return The entry, or @c nullptr when there is none.  Points into @p entries.
 */
const NetrcEntry* findNetrcEntry(const std::vector<NetrcEntry>& entries,
                                 const std::string& machine,
                                 const std::string& login = {});

/// Why a netrc file gave no entries.
enum class NetrcReadStatus {
    Ok,           ///< The file was read and parsed (it may hold no entries).
    Unreadable,   ///< The file could not be opened or read, or is larger than @c kNetrcMaxBytes.
    NotText       ///< The file is UTF-16 or holds NUL bytes: not a text file libcurl can read either.
};

/// The result of reading one netrc file.
struct NetrcContents {
    /// Outcome of the read; @c Ok means the file was read and parsed.
    NetrcReadStatus        status{ NetrcReadStatus::Unreadable };
    std::vector<NetrcEntry> entries;   ///< Empty unless @c status is Ok.
};

/// Largest netrc file that is read, in bytes; a real one is a few hundred.
inline constexpr std::size_t kNetrcMaxBytes = 1u << 20;

/**
 * @brief Read and parse a netrc file.
 *
 * A UTF-8 byte-order mark is skipped.  A UTF-16 file (what Windows PowerShell 5 writes
 * for a redirect) is reported as @c NotText instead of being misread as an empty file.
 *
 * @param path The file.
 * @return Its status and entries.
 */
NetrcContents readNetrcFile(const std::filesystem::path& path);

} // namespace GnssProcessor
