/**
 * @file NetrcFile.cpp
 * @brief Netrc file location, parsing and per-host lookup (see NetrcFile.hpp).
 */

#include "products/NetrcFile.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace GnssProcessor {

namespace {

namespace fs = std::filesystem;

/// True for the characters that separate netrc tokens.
bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// @p s in lower case (ASCII only; the length is unchanged).
std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

/// True when the existing path @p p is a file or a directory entry that can be opened; false on any error.
bool pathExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && !ec;
}

/// Token reader over the text of a netrc file.
class NetrcTokens {
public:
    explicit NetrcTokens(const std::string& text) : text_(text) {}

    /// Read the next token into @p token; false at the end of the text.  A token in double
    /// quotes may hold blanks, and its quotes are removed.
    bool next(std::string& token) {
        token.clear();
        for (;;) {
            while (pos_ < text_.size() && isBlank(text_[pos_])) ++pos_;
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] != '#') break;
            while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;   // comment to the end of the line
        }
        if (text_[pos_] == '"') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] != '"') {
                char c = text_[pos_++];
                if (c == '\\' && pos_ < text_.size()) {
                    const char escaped = text_[pos_++];
                    c = escaped == 'n' ? '\n' : escaped == 'r' ? '\r' : escaped == 't' ? '\t' : escaped;
                }
                token.push_back(c);
            }
            if (pos_ < text_.size()) ++pos_;   // the closing quote
            return true;
        }
        while (pos_ < text_.size() && !isBlank(text_[pos_])) token.push_back(text_[pos_++]);
        return true;
    }

    /// Skip a macro body: the rest of the @c macdef line and every line up to the next blank one.
    void skipMacroBody() {
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
        while (pos_ < text_.size()) {
            ++pos_;   // the line feed that ends the previous line
            bool blank = true;
            while (pos_ < text_.size() && text_[pos_] != '\n') {
                if (!isBlank(text_[pos_])) blank = false;
                ++pos_;
            }
            if (blank) return;
        }
    }

private:
    const std::string& text_;
    std::size_t        pos_{ 0 };
};

} // namespace

// ---------------------------------------------------------------------------
// Location
// ---------------------------------------------------------------------------

fs::path homeDirectory() {
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
    if (const char* u = std::getenv("USERPROFILE"); u && *u) return fs::path(u);
    return {};
}

NetrcLocation locateNetrcFile(const fs::path& configured, const fs::path& home) {
    NetrcLocation found;

    if (!configured.empty()) {
        if (pathExists(configured)) {
            found.origin = NetrcLocation::Origin::Configured;
            found.path   = configured;
            return found;
        }
        found.configured_missing = true;
    }

    // The Unix name is tried first because it is the common setup on either OS; the
    // Windows name is the fallback.
    if (home.empty()) {
        found.no_home = true;
        return found;
    }
    found.dot_netrc        = home / ".netrc";
    found.underscore_netrc = home / "_netrc";
    if (pathExists(found.dot_netrc)) {
        found.origin = NetrcLocation::Origin::DotNetrc;
        found.path   = found.dot_netrc;
    } else if (pathExists(found.underscore_netrc)) {
        found.origin = NetrcLocation::Origin::UnderscoreNetrc;
        found.path   = found.underscore_netrc;
    }
    return found;
}

NetrcLocation locateNetrcFile(const fs::path& configured) {
    return locateNetrcFile(configured, homeDirectory());
}

// ---------------------------------------------------------------------------
// Parsing and lookup
// ---------------------------------------------------------------------------

std::vector<NetrcEntry> parseNetrc(const std::string& text) {
    std::vector<NetrcEntry> entries;
    NetrcTokens tokens(text);

    NetrcEntry current;
    bool       open = false;   // false before the first machine and inside a default entry
    const auto close = [&] {
        if (open) entries.push_back(std::move(current));
        current = NetrcEntry{};
        open    = false;
    };

    std::string token, value;
    while (tokens.next(token)) {
        const std::string key = lowerAscii(token);
        if (key == "default") {
            close();
            continue;
        }
        if (key == "macdef") {
            tokens.next(value);          // the macro name
            tokens.skipMacroBody();
            continue;
        }
        if (key != "machine" && key != "login" && key != "password" && key != "account") continue;
        if (!tokens.next(value)) break;  // a keyword at the end of the text has no value

        if (key == "machine") {
            close();
            current.machine = value;
            open            = true;
        } else if (open && key == "login") {
            current.login = value;
        } else if (open && key == "password") {
            current.password = value;
        }
    }
    close();
    return entries;
}

const NetrcEntry* findNetrcEntry(const std::vector<NetrcEntry>& entries,
                                 const std::string& machine,
                                 const std::string& login) {
    const std::string wanted = lowerAscii(machine);
    for (const NetrcEntry& e : entries) {
        if (lowerAscii(e.machine) != wanted) continue;
        if (e.login.empty() || e.password.empty()) continue;
        if (!login.empty() && e.login != login) continue;
        return &e;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// File
// ---------------------------------------------------------------------------

NetrcContents readNetrcFile(const fs::path& path) {
    NetrcContents out;

    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > kNetrcMaxBytes) return out;   // Unreadable

    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // A UTF-8 byte-order mark is skipped; UTF-16 text (byte-order mark, or NUL bytes between
    // the ASCII characters) is not a netrc file libcurl can read, so it is reported.
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    const bool utf16_bom = text.size() >= 2
        && ((static_cast<unsigned char>(text[0]) == 0xFF && static_cast<unsigned char>(text[1]) == 0xFE)
         || (static_cast<unsigned char>(text[0]) == 0xFE && static_cast<unsigned char>(text[1]) == 0xFF));
    if (utf16_bom || text.find('\0') != std::string::npos) {
        out.status = NetrcReadStatus::NotText;
        return out;
    }

    out.entries = parseNetrc(text);
    out.status  = NetrcReadStatus::Ok;
    return out;
}

} // namespace GnssProcessor
