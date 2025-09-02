#if !defined(REGUESTGUARDS_H)
#define REGUESTGUARDS_H

#include <cstddef>
#include <string>
#include "Headers.h"

class Server;
// class FlatHeaders;

struct Preflight
{

	bool ok;
	int reject_status;
	std::string reject_reason;
	bool needs_body;
	std::size_t max_body_bytes;
	Preflight() : ok(true), reject_status(0), needs_body(false), max_body_bytes(0) {}
};

class RequestGuards
{
public:
	static Preflight preflight(const ServerConfig &cfg,
							   int vs_idx,
							   const std::string &method,
							   const std::string &uri,
							   const Headers &hdrs,
							   RouteDecision *out_route);

	static Preflight preflight(const ServerConfig &cfg,
							   int vs_idx,
							   const std::string &method,
							   const std::string &uri,
							   const Headers &hdrs)
	{
		return preflight(cfg, vs_idx, method, uri, hdrs, 0);
	}
};

#endif //  REGUESTGUARDS_H
