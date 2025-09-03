/* --- ResponseFactory.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/27/2025
------------------------------------------ */

#include "ResponseFactory.h"

#include <ctime>
#include <sstream>
#include <map>

// --- tiny helpers ---

static const std::string& reasonFor(int code) {
	struct StatusPair { int code; const char* reason; };
	static const StatusPair status_array[] = {
		{200,"OK"}, {204,"No Content"},
		{301,"Moved Permanently"}, {302,"Found"}, {303,"See Other"}, {307,"Temporary Redirect"}, {308,"Permanent Redirect"},
		{400,"Bad Request"}, {401,"Unauthorized"}, {403,"Forbidden"}, {404,"Not Found"},
		{405,"Method Not Allowed"}, {411,"Length Required"}, {413,"Payload Too Large"},
		{417,"Expectation Failed"}, {421,"Misdirected Request"}, {431,"Request Header Fields Too Large"},
		{500,"Internal Server Error"}, {501,"Not Implemented"}, {502,"Bad Gateway"},
		{503,"Service Unavailable"}, {505,"HTTP Version Not Supported"}
	};
	static std::map<int, std::string> k;
	if (k.empty()) {
		for (size_t i = 0; i < sizeof(status_array)/sizeof(status_array[0]); ++i)
			k[status_array[i].code] = status_array[i].reason;
	}
	static const std::string unk = "Unknown";
	std::map<int,std::string>::const_iterator it = k.find(code);
	return (it != k.end()) ? it->second : unk;
}

// RFC 7231 / RFC 9110 
static std::string rfc1123_now_gmt()
{
	char buf[64];
	std::time_t t = std::time(0);
	std::tm g;
#if defined(_WIN32)
	gmtime_s(&g, &t);
#else
	g = *std::gmtime(&t);
#endif
	std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &g);
	return std::string(buf);
}

static void addCommonHeaders(HttpResponse &r, bool close)
{
	r.headers.set("Date", rfc1123_now_gmt());
	r.headers.set("Server", "webserv/1.0");
	r.headers.set("Connection", close ? "close" : "keep-alive");
}

// --- builders ---

HttpResponse ResponseFactory::makeError(int code,const std::string & reasons,bool close, const std::string& bodyText)
{
	HttpResponse r;
	r.status = code;
	r.reason = reasons.empty() ? reasonFor(code) : reasons;

		std::string text;
		if (bodyText.empty()) {
			std::ostringstream oss;
			oss << code << " " << r.reason << "\n";
			text = oss.str();
		} else {
			text = bodyText;
		}

	// Body (plain text)
	r.body.assign(text.begin(), text.end());
	r.headers.set("Content-Type", "text/plain; charset=utf-8");

	addCommonHeaders(r, close);
	return r;
}

HttpResponse ResponseFactory::makeText(int code,const std::string& text,const std::string & reasons,bool close)
{
	HttpResponse r;
	r.status = code;
	r.reason = reasons.empty() ? reasonFor(code) : reasons;

	r.body.assign(text.begin(), text.end());
	r.headers.set("Content-Type", "text/plain; charset=utf-8");

	addCommonHeaders(r, close);
	return r;
}
