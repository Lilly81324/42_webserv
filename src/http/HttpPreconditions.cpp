#include "HttpPreconditions.h"
#include "HttpRequest.h"
#include "Headers.h"
#include "HEADER_ENTRIES.h"

#include <ctime>
#include <string>
#include <vector>

// ---- tiny helpers ---------------------------------------------------

/**
 * @brief Returns given string stripped at front and end of spaces and tabs
 * @param s String to strip
 * @returns the result of stripping
 */
static std::string trim_ws(const std::string& s) {
    std::string::size_type a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
    return s.substr(a, b - a);
}

/**
 * Splits string into substrings, based on commas
 * Removes spaces and tabs before and after commas
 */
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

/**
 * @brief Checks the If-None-Match Header
 * @param inm Value of the Header Field for If-None-Match
 * @param etag Current ETag
 * @returns true, if inm is "*" and etag is empty
 * @returns true, if Given ETag matches one of the entries in inm
 * @returns false otherwise
 */
bool HttpPreconditions::checkIfNoneMatch(const std::string &inm, const std::string &etag)
{
	if (inm.empty())
		return (false);
	if (inm == "*")
		return (etag.empty());
	std::vector<std::string> etags;
	split_commas(inm, etags);
	for (std::vector<std::string>::size_type i = 0; i < etags.size(); ++i)
	{
		if (ETagUtil::weakComp(trim_ws(etags[i]), etag))
			return true;
	}
	return (false);
}

/**
 * @brief Checks if file was modified or is not matching the etag
 * @returns true if Header field IF-None-Match is "*" AND etag is empty
 * @returns true if given ETag HDR_IF_NONE_MATCH Header field is true
 * @returns true if the resource has NOT been modified according to headers
 */
bool HttpPreconditions::isNotModified(const HttpRequest& req,
                                      const std::string& etag,
                                      std::time_t mtime)
{
    const Headers& h = req.getHeaders();
	const std::string & inm = h.get(HDR_IF_NONE_MATCH);
	// 1) If-None-Match
	if (!inm.empty())
		return (HttpPreconditions::checkIfNoneMatch(inm, etag));
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

/**
 * @brief Checks if the specified ETag matches the one in the request Header
 * @note Should only handle strong ETags (no W/"blablabla")
 * @param req HttpRequest which holds Header fields to check
 * @param etag String that should be a newly created ETag for the target file
 * @returns true if no HDR_IF_MATCH Header field exists
 * @returns true if field is set to "*" and givenEtag is valid
 * @returns true if givenEtag matches one of the fields specified ETags
 * @returns false otherwise
 * 
 * Use Case:
 * Mainly for PUT, PATCH and DELETE
 * Interpretation:
 * A false value means that the Etag is not matching, but should be
 * this means there is an error, the status code should be set to 412,
 * and the request should not be processed further
 * A true return means that the request should be handled as usual
 * For further information: RFC 9110 §13.1.2.
 */
bool HttpPreconditions::checkIfMatch(const HttpRequest &req, const std::string &givenEtag)
{
	std::vector<std::string> etagArray;
	const Headers &h = req.getHeaders();
	const std::string &storedEtags = h.get(HDR_IF_MATCH);

	// If no Key for checking Matching Etags
	if (!h.keyExists(HDR_IF_MATCH))
		return (true);
	
	// Any Matches
	if (storedEtags == "*")
	{
		// If Resource is accesible, stop running (Is this also a 412? Shouldnt this be handled later?)
		if (givenEtag.empty())
			return (false);
		return (true);
	}

	// Go through all etags stored in the Header
	split_commas(storedEtags, etagArray);
	for (std::vector<std::string>::const_iterator it = etagArray.begin(); it != etagArray.end(); it++)
	{
		// If one matches the one we search -> match
		if (ETagUtil::strongComp(*it, givenEtag))
			return (true);
	}
	// No match found -> Given ETag is invalid/out-of-date
	return (false);
}
