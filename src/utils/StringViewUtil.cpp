/* Copyright 2020 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "BaseUtil.h"

// utilities for string view
namespace sv {

bool StartsWith(std::string_view s, std::string_view prefix) {
    auto plen = prefix.length();
    auto slen = s.length();
    if (plen > slen) {
        return false;
    }
    return str::EqN(s.data(), prefix.data(), plen);
}

bool StartsWith(std::string_view s, const char* prefix) {
    auto p = std::string_view(prefix);
    return StartsWith(s, p);
}

std::string_view NormalizeNewlines(std::string_view s) {
    str::Str tmp(s);
    tmp.Replace("\r\n", "\n");
    tmp.Replace("\r", "\n");
    return tmp.StealAsView();
}

Vec<std::string_view> Split(std::string_view sv, char split, size_t max) {
    Vec<std::string_view> res;
    const char* s = sv.data();
    const char* end = s + sv.size();
    if (max > 0) {
        // we want to stop at max - 1 because we also add
        max--;
    }
    const char* prev = s;
    while (s < end) {
        char c = *s;
        if (c == split) {
            size_t size = s - prev;
            res.push_back({prev, size});
            prev = s + 1;
            if (max != 0 && max == res.size()) {
                break;
            }
        }
        ++s;
    }
    // add the rest if non-empty
    size_t size = end - prev;
    if (size > 0) {
        res.push_back({prev, size});
    }
    return res;
}

std::string_view TrimSpace(std::string_view str) {
    const char* s = str.data();
    const char* end = s + str.size();
    while (s < end && str::IsWs(*s)) {
        ++s;
    }
    while (end > s) {
        char c = end[-1];
        if (!str::IsWs(c)) {
            break;
        }
        --end;
    }
    size_t size = end - s;
    return {s, size};
}

// update sv to skip first n characters
size_t SkipN(std::string_view& sv, size_t n) {
    CrashIf(n > sv.size());
    const char* s = sv.data() + n;
    size_t newSize = sv.size() - n;
    sv = {s, newSize};
    return n;
}

// updat svn to skip until end
size_t SkipTo(std::string_view& sv, const char* end) {
    const char* s = sv.data();
    CrashIf(end < s);
    size_t toSkip = end - s;
    CrashIf(toSkip > sv.size());
    SkipN(sv, toSkip);
    return toSkip;
}

// returns a substring of sv until delim or end of string
// updates sv to reflect the rest of the string
// meant of iterative calls so updates 'sv' in place
// return { nullptr, 0 } to indicate finished iteration
std::string_view ParseUntil(std::string_view& sv, char delim) {
    const char* s = sv.data();
    const char* e = s + sv.size();
    const char* start = s;
    if (s == e) {
        return {nullptr, 0};
    }
    while (s < e) {
        if (*s == delim) {
            break;
        }
        s++;
    }
    // skip one past delim
    size_t n = SkipTo(sv, s);
    // skip delim
    if (s < e) {
        SkipN(sv, 1);
    }
    return {start, n};
}

std::string_view ParseUntilBack(std::string_view& sv, char delim) {
    const char* start = sv.data();
    const char* end = start + sv.size();
    if (start == end) {
        return {nullptr, 0};
    }
    const char* s = end - 1;
    while (s >= start) {
        if (*s == delim) {
            break;
        }
        s--;
    }
    size_t size = (size_t)(end - s - 1);
    std::string_view el = {s + 1, size};
    size_t newSize = sv.size() - size;
    if (newSize > 0) {
        // eat delim
        newSize--;
    }
    sv = {start, newSize};
    return el;
}

// sv is "key: value"
// returns value if key is <key>
std::string_view ParseKV(std::string_view sv, const char* key) {
    auto parts = sv::Split(sv, ':', 2);
    if (parts.size() == 1) {
        return {};
    }
    auto k = parts[0];
    if (!str::EqI(k, key)) {
        return {};
    }
    auto v = parts[1];
    v = sv::TrimSpace(v);
    return v;
}

// skips all c chars in the beginning of sv
// returns number of chars skipped
// TODO: rename trimLeft?
size_t SkipChars(std::string_view& sv, char c) {
    const char* s = sv.data();
    const char* e = s + sv.size();
    while (s < e) {
        if (*s != c) {
            break;
        }
        s++;
    }
    return SkipTo(sv, s);
}

bool NeedsQuoting(char c) {
    switch (c) {
        case '"':
        case '\\':
        case '\n':
        case '\r':
        case '\t':
        case '\b':
        case '\f':
            return true;
    }
    return false;
}

static char quoteChar(char c) {
    switch (c) {
        case '"':
        case '\\':
            return c;
        case '\n':
            return 'n';
        case '\r':
            return 'r';
        case '\t':
            return 't';
        case '\b':
            return 'b';
        case '\f':
            return 'f';
    }
    CrashIf(true);
    return c;
}

static std::tuple<char, bool> unquoteChar(char c) {
    switch (c) {
        case '"':
        case '\\':
            return {c, true};
        case 'n':
            return {'\n', true};
        case 'r':
            return {'\r', true};
        case 't':
            return {'\t', true};
        case 'b':
            return {'\b', true};
        case 'f':
            return {'\f', true};
    }
    return {'\0', false};
}

void AppendQuotedString(std::string_view sv, str::Str& out) {
    out.AppendChar('"');
    const char* s = sv.data();
    const char* end = s + sv.size();
    while (s < end) {
        auto c = *s;
        if (NeedsQuoting(c)) {
            out.AppendChar('\\');
            c = quoteChar(c);
        }
        out.AppendChar(c);
        s++;
    }
    out.AppendChar('"');
}

// if <line> starts with '"' it's quoted value that should end with '"'
// otherwise it's unquoted value that ends with ' '
// returns false if starts with '"' but doesn't end with '"'
// sets <out> to parsed value
// updates <line> to consume parsed characters
bool ParseQuotedString(std::string_view& line, str::Str& out) {
    if (line.size() == 0) {
        // empty value is ok
        return true;
    }
    const char* s = line.data();
    const char* end = s + line.size();
    char c = *s;
    if (c != '"') {
        // unqoted
        std::string_view v = sv::ParseUntil(line, ' ');
        out.AppendView(v);
        return true;
    }
    s++;
    while (s < end) {
        c = *s;
        if (c == '"') {
            s++;
            SkipTo(line, s);
            return true;
        }
        if (c != '\\') {
            out.AppendChar(c);
            s++;
            continue;
        }
        // possibly escaping
        s++;
        if (s >= end) {
            return false;
        }
        c = *s;
        auto [c2, ok] = unquoteChar(c);
        if (ok) {
            out.AppendChar(c2);
        } else {
            out.AppendChar('\\');
            out.AppendChar(c);
        }
        s++;
    }
    // started with '"' but didn't end with it
    return false;
}

} // namespace sv