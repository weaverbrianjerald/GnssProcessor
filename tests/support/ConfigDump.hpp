#pragma once
/**
 * @file ConfigDump.hpp
 * @brief Test support: the text dump of every member of a loaded configuration, one
 *        "name=value" line per member, sorted.
 *
 * The loader decides much more than it reads: it refuses, clamps, derives and forces values,
 * and the only complete statement of what a run will use is the loaded object itself.  This
 * dump prints that object, so that two loads (two files, two builds, the code before and after
 * a loader change) can be compared with a plain text diff.
 *
 * The member list is not written by hand.  tests/support/gen_config_dump.py reads the public
 * const getters of GnssConfig and the fields of the plain structs they return and writes
 * ConfigDumpGenerated.inc, which is included below; re-run the script after a header change.
 * Names are the getter's name, followed for a struct by the dotted field path
 * ("getNetworkConfig.ambiguity_resolution.enable"), and for a vector of structs by the element
 * index ("getNetworkConfig.stations[3].name", after "getNetworkConfig.stations.size").
 *
 * Values: a bool as true or false; an integer in decimal; a floating-point value with 17
 * significant digits (printf %.17g, so equal text means equal bits for finite values); a
 * string or path as its text, with backslash, newline and carriage return escaped; an enum as
 * its underlying integer; a vector, array or set as "[a,b]"; a map as "{k:v,k:v}"; a pair as
 * "(a,b)"; an empty optional as "none".  A credential (a password) is printed as "<set>" or
 * "<empty>", never as its text.  The types the script cannot print are listed in the comment
 * at the top of the generated file.
 */

#include "config/GnssConfig.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnss_test {
namespace config_dump_detail {

template <class T> struct IsVector : std::false_type {};
template <class T, class A> struct IsVector<std::vector<T, A>> : std::true_type {};
template <class T> struct IsStdArray : std::false_type {};
template <class T, std::size_t N> struct IsStdArray<std::array<T, N>> : std::true_type {};
template <class T> struct IsSet : std::false_type {};
template <class T, class C, class A> struct IsSet<std::set<T, C, A>> : std::true_type {};
template <class T> struct IsMap : std::false_type {};
template <class K, class V, class C, class A> struct IsMap<std::map<K, V, C, A>> : std::true_type {};
template <class T> struct IsPair : std::false_type {};
template <class A, class B> struct IsPair<std::pair<A, B>> : std::true_type {};
template <class T> struct IsOptional : std::false_type {};
template <class T> struct IsOptional<std::optional<T>> : std::true_type {};

/** @brief @p s with backslash, newline and carriage return escaped, so one value is one line. */
inline std::string escaped(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

/** @brief The dump text of one value (see the file comment for the format of each type). */
template <class T>
std::string dumpText(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_same_v<T, char>) {
        return escaped(std::string(1, v));
    } else if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        if constexpr (std::is_signed_v<U>)
            return std::to_string(static_cast<long long>(static_cast<U>(v)));
        else
            return std::to_string(static_cast<unsigned long long>(static_cast<U>(v)));
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (std::is_signed_v<T>)
            return std::to_string(static_cast<long long>(v));
        else
            return std::to_string(static_cast<unsigned long long>(v));
    } else if constexpr (std::is_floating_point_v<T>) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", static_cast<double>(v));
        return buf;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return escaped(v);
    } else if constexpr (std::is_same_v<T, std::filesystem::path>) {
        return escaped(v.string());
    } else if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, char*>) {
        return v ? escaped(std::string(v)) : std::string("(null)");
    } else if constexpr (IsOptional<T>::value) {
        return v ? dumpText(*v) : std::string("none");
    } else if constexpr (IsPair<T>::value) {
        return "(" + dumpText(v.first) + "," + dumpText(v.second) + ")";
    } else if constexpr (IsMap<T>::value) {
        std::string out = "{";
        bool first = true;
        for (const auto& kv : v) {
            if (!first) out += ",";
            first = false;
            out += dumpText(kv.first) + ":" + dumpText(kv.second);
        }
        return out + "}";
    } else if constexpr (IsVector<T>::value || IsStdArray<T>::value || IsSet<T>::value) {
        using E = typename T::value_type;
        std::string out = "[";
        bool first = true;
        for (const auto& e : v) {
            if (!first) out += ",";
            first = false;
            out += dumpText<E>(e);   // explicit E: the element of std::vector<bool> is a proxy
        }
        return out + "]";
    } else {
        return "<unprintable>";
    }
}

}  // namespace config_dump_detail

/**
 * @brief Every member of @p cfg as sorted "name=value" lines, each ended by a newline.
 *
 * Calls only const getters and reads only the structs they return; nothing in @p cfg changes.
 *
 * @param cfg A configuration after GnssConfig::loadFromJson.
 * @return The dump text (one line per member, in byte order of the line).
 */
inline std::string configMemberDump(const GnssProcessor::GnssConfig& cfg) {
    std::vector<std::string> lines;
    lines.reserve(2048);
    const auto add = [&lines](const std::string& name, const auto& value) {
        lines.push_back(name + "=" + config_dump_detail::dumpText(value));
    };
    const auto addSecret = [&lines](const std::string& name, const std::string& value) {
        lines.push_back(name + "=" + (value.empty() ? "<empty>" : "<set>"));
    };
#include "ConfigDumpGenerated.inc"
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const std::string& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

}  // namespace gnss_test
