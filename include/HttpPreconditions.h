#ifndef HTTP_PRECONDITIONS_H
#define HTTP_PRECONDITIONS_H

#include "ETagUtil.h"
#include <string>
#include <ctime>

class HttpRequest;

/**
 * Minimal HTTP preconditions helper (ETag + Last-Modified)
 *
 * isNotModified(req, etag, mtime) returns true when the client’s cached copy
 * is still valid so the server should send 304 Not Modified.
 *
 * Implements:
 *  - If-None-Match: exact match against a list of ETags (or "*")
 *  - If-Modified-Since: RFC1123 date; true if mtime <= IMS
 *
 * Notes:
 *  - If-None-Match takes precedence over If-Modified-Since.
 *  - Weak ETags (W/...) are compared verbatim (no special handling).
 */
namespace HttpPreconditions {
	bool checkIfNoneMatch(const std::string &inm, const std::string &etag);
    bool isNotModified(const HttpRequest& req,
                       const std::string& etag,
                       std::time_t mtime);
	bool	checkIfMatch(const HttpRequest &req, const std::string &etag);
}

#endif // HTTP_PRECONDITIONS_H
