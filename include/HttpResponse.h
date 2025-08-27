/* --- HttpResponse.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef HTTPRESPONSE_H
#define HTTPRESPONSE_H

#include <string>
#include <vector>
#include <errno.h>
#include "Headers.h"
#include "CookieJar.h"

using namespace std;

/**
 * @brief Class that represents a full Http Response
 * ---------------------------------------------------
 */
class HttpResponse
{
public:
	string http_version;
	string session_id;
	size_t bodyLength;
	Headers headers;
	CookieJar cookies;
	vector<char> body;
	int status;
	std::string reason;
	int exit_code;
	HttpResponse() : http_version("HTTP/1.1"),
					 session_id(""),
					 bodyLength(0),
					 status(200),
					 reason("OK")
	{
	}
	~HttpResponse(){};
};

/**
 * DANGER
 * PLACEHOLDER
 * USES THE ostream.write external function
 * which isnt allowed
 * DELETE LATER
 * ...or keep it for debugging
 */
std::ostream &operator<<(std::ostream &out, const HttpResponse &target);

#endif // HTTPRESPONSE_H
