#include "HttpPreconditions.h"
#include "HttpRequest.h"
#include "Headers.h"
#include "HEADER_ENTRIES.h"

#include <ctime>
#include <string>
#include <vector>

// ---- tiny helpers ---------------------------------------------------

static std::string trim_ws(const std::string& s) {
    std::string::size_type a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
    return s.substr(a, b - a);
}

static void split_commas(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    for (std::string::size_type i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == ',') {
            out.push_back(trim_ws(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(trim_ws(cur));
}

// RFC 1123 date → time_t (UTC). Example: "Wed, 03 Sep 2025 15:56:30 GMT"
static bool parse_http_date_rfc1123(const std::string& s, std::time_t& out) {
    // Prepare a zeroed tm without memset (memset is forbidden).
    struct tm tmv;
    tmv.tm_sec   = 0;
    tmv.tm_min   = 0;
    tmv.tm_hour  = 0;
    tmv.tm_mday  = 0;
    tmv.tm_mon   = 0;
    tmv.tm_year  = 0;
    tmv.tm_wday  = 0;
    tmv.tm_yday  = 0;
    tmv.tm_isdst = 0;

#if defined(_XOPEN_SOURCE) || defined(_GNU_SOURCE) || defined(__linux__)
    // POSIX strptime is widely available on Linux.
    const char* p = ::strptime(s.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tmv);
    if (!p || *p != '\0') return false;

    // Use timegm (UTC) when available.
    #if defined(_GNU_SOURCE) || defined(__USE_MISC) || defined(__linux__)
        time_t t = ::timegm(&tmv);
        if (t == (time_t)-1) return false;
        out = t;
        return true;
    #else
        // Fallback: mktime (local time). Correct if server TZ is UTC.
        tmv.tm_isdst = -1;
        time_t t = ::mktime(&tmv);
        if (t == (time_t)-1) return false;
        out = t;
        return true;
    #endif
#else
    (void)tmv; (void)out;
    return false;
#endif
}

// ---- public API -----------------------------------------------------

bool HttpPreconditions::isNotModified(const HttpRequest& req,
                                      const std::string& etag,
                                      std::time_t mtime)
{
    const Headers& h = req.getHeaders();

    // 1) If-None-Match (strong precedence)
    const std::string inm = h.get(HDR_IF_NONE_MATCH);
    if (!inm.empty()) {
        if (inm == "*") {
            return true; // any representation matches
        }
        std::vector<std::string> etags;
        split_commas(inm, etags);
        for (std::vector<std::string>::size_type i = 0; i < etags.size(); ++i) {
            const std::string candidate = trim_ws(etags[i]);
            if (candidate == etag)
                return true;
        }
        // If-None-Match present but no match -> treat as not satisfied; fall through.
    }

    // 2) If-Modified-Since
    const std::string ims = h.get(HDR_IF_MODIFIED_SINCE);
    if (!ims.empty()) {
        std::time_t since = 0;
        if (parse_http_date_rfc1123(ims, since)) {
            if (mtime <= since)
                return true; // not modified since IMS
        }
    }

    return false;
}
