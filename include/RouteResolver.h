/* --- RouteResolver.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef ROUTERESOLVER_H
#define ROUTERESOLVER_H

#include "VirtualServer.h"
#include "ServerConfig.h"
#include <string>
#include <vector>

struct RouteDecision
{
	enum HandlerKind
	{
		HK_STATIC,
		HK_CGI,
		HK_PROXY,
		HK_RETURN,
		HK_PUTPATCH,
		HK_ERROR
	};
	HandlerKind kind;
	int status; // for HK_ERROR
	const VirtualServer *vs;
	const Location *loc;
	std::string cgi_ext;
	std::string upstream_name;

	// Policy and helper fields surfaced from Location
	std::vector<std::string> try_files;
	RateLimitConfig rate_limit;
	std::vector<std::string> allow_list;
	std::vector<std::string> deny_list;
	 // for HK_RETURN: target (url or text)
	std::string return_target;
	// Routing helpers
	std::string matched_prefix;
	std::string rel_path;
	std::string effective_root;

	RouteDecision() : kind(HK_ERROR), status(500), vs(0), loc(0), cgi_ext(), upstream_name(), try_files(), rate_limit(), allow_list(), deny_list(), return_target(), matched_prefix(), rel_path(), effective_root() {}
};

class RouteResolver
{
public:
	static const Location *matchLocation(const VirtualServer &vs, const std::string &path);

	// eturning the matched prefix (useful to compute rel_path)
	static const Location *matchLocation(const VirtualServer &vs, const std::string &path, std::string &matched_prefix);

private:
	RouteResolver();
	RouteResolver(const RouteResolver &);
	RouteResolver &operator=(const RouteResolver &);
};

#endif // ROUTERESOLVER_H
