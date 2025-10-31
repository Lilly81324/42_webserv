/* --- ServerPipeline.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "ServerPipeline.h"
#include "ServerConfig.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Router.h"
#include "RouteResolver.h"
#include "StaticHandler.h"
#include "ProxyHandler.h"
#include "CgiHandler.h"
#include "PutPatchHandler.h"
#include "RequestContext.h"
#include "ResponseFactory.h"
#include "UploadHandler.h"
#include "RateLimiter.h"
#include <sys/stat.h>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "Server.h"

static RateLimiter &globalLimiter()
{
	static RateLimiter rl;
	return rl;
}

#include <poll.h>

void ctxTrackPollFds(ClientConnection *client, std::vector<int> &tracked)
{
	std::vector<pollfd> pfd = client->getServer()->getLoop().getPfds();
	std::vector<pollfd>::const_iterator end = pfd.end();
	for (std::vector<pollfd>::const_iterator it = pfd.begin(); it != end; ++it)
	{
		if (it->fd > 0)
			tracked.push_back(it->fd);
	}
}

bool ServerPipeline::processRequest(const ServerConfig &cfg,
									int vs_indx,
									HttpRequest &req,
									HttpResponse &res,
									RouteDecision &decision,
									CGIStreamer *cgi_streamer,
									ClientConnection *client)
{
	// ---- Build RequestContext from inputs ----
	RequestContext ctx;
	ctx.cfg = &cfg;
	ctx.vs_index = vs_indx;
	ctx.vs = 0;
	ctxTrackPollFds(client, ctx.pollFds);

	if (ctx.vs_index >= 0 && ctx.vs_index < (int)cfg.servers().size())
		ctx.vs = &cfg.servers()[ctx.vs_index];
	if (!ctx.vs)
	{
		res = ResponseFactory::makeText(500, "No virtual server resolved\n",
										"Internal Server Error", /*close*/ true);
		return true;
	}

	// Copy routing decision details
	ctx.loc = decision.loc;
	ctx.effective_root = decision.effective_root;
	ctx.rel_path = decision.rel_path;
	ctx.cgi_ext = decision.cgi_ext;
	ctx.matched_prefix = decision.matched_prefix;
	ctx.upstream_name = decision.upstream_name; // may be pool NAME or concrete "host:port"

	// Body/temp defaults
	ctx.temp_file_used = false;
	ctx.temp_filename = std::string();

	// Proxy timeout defaults (overridden by per-location values if available)
	ctx.proxy_connect_timeout_ms = 5000;
	ctx.proxy_io_idle_timeout_ms = 15000;

	if (ctx.loc)
	{
		if (ctx.loc->proxy_connect_timeout_ms > 0)
			ctx.proxy_connect_timeout_ms = ctx.loc->proxy_connect_timeout_ms;
		if (ctx.loc->proxy_io_idle_timeout_ms > 0)
			ctx.proxy_io_idle_timeout_ms = ctx.loc->proxy_io_idle_timeout_ms;
	}

	// Hook CGI and the owning client connection
	ctx.cgi_streamer = cgi_streamer;
	ctx.client = client; // REQUIRED for ProxyHandler to call beginProxyTunnel()

	// ---- Rate limiting (before picking handler / heavy work) ----
	if (ctx.loc && ctx.loc->rate_limit.enabled)
	{
		RateBucketCfg cfg2;
		// convert requests/minute -> requests/second
		cfg2.rps = (ctx.loc->rate_limit.requests_per_minute > 0)
					   ? (static_cast<double>(ctx.loc->rate_limit.requests_per_minute) / 60.0)
					   : 0.0;
		cfg2.burst = (ctx.loc->rate_limit.burst > 0)
						 ? static_cast<double>(ctx.loc->rate_limit.burst)
						 : 0.0;
		cfg2.max_entries = 1024; // simple default store cap

		RateLimiter &rl = globalLimiter();
		rl.setConfig(cfg2);

		unsigned long long now_ms =
			static_cast<unsigned long long>(std::time(NULL)) * 1000ULL;

		RateDecision d = rl.allow(client->getIp(), now_ms);
		rl.maybeCleanup(now_ms);

		if (!d.allowed)
		{
			HttpResponse tooMany = ResponseFactory::makeText(
				429, "Too Many Requests\n", "Too Many Requests", true);

			char ra[32];
			std::snprintf(ra, sizeof ra, "%d", d.retry_after_seconds);
			tooMany.headers.set("Retry-After", std::string(ra));

			// Expose rate limit metadata
			char lim[32];
			std::snprintf(lim, sizeof lim, "%d", d.limit_rpm);
			tooMany.headers.set("X-RateLimit-Limit", std::string(lim));
			tooMany.headers.set("X-RateLimit-Remaining", "0");

			res = tooMany;
			return true; // short-circuit, no handler
		}
	}

	// ---- proxy target resolution (pool → host:port) BEFORE selecting handler ----
	//
	// If this route is a proxy and upstream_name looks like a pool NAME
	// (i.e., doesn't contain ':'), resolve it to a concrete host:port using
	// RouteResolver (round-robin among healthy nodes).
	//
	if (decision.kind == RouteDecision::HK_PROXY && ctx.loc && ctx.loc->is_proxy)
	{
		// detect "host:port" vs plain pool name
		if (ctx.upstream_name.find(':') == std::string::npos)
		{
			std::string host;
			int port = 0;
			if (!RouteResolver::resolveProxyTarget(*ctx.vs, ctx.upstream_name, host, port))
			{
				res = ResponseFactory::makeText(502, "Bad Gateway\n", "Bad Gateway", /*close*/ true);
				return true;
			}
			// overwrite with concrete target so ProxyHandler just connects
			std::ostringstream hp;
			hp << host << ":" << port;
			ctx.upstream_name = hp.str();
		}
	}

	// ---- Pick the handler once ----
	Handler *h = 0;
	switch (decision.kind)
	{
	case RouteDecision::HK_STATIC:
		h = new StaticHandler();
		break;
	case RouteDecision::HK_CGI:
		h = new CgiHandler();
		break;
	case RouteDecision::HK_PROXY:
		h = new ProxyHandler();
		break;
	case RouteDecision::HK_PUTPATCH:
		h = new PutPatchHandler();
		break;
	case RouteDecision::HK_UPLOAD:
		h = new UploadHandler();
		break;

	case RouteDecision::HK_ERROR:
	{
		const int code = decision.status ? decision.status : 500;
		res = ResponseFactory::makeText(code, "", /*reason*/ "", /*close*/ true);
		return true;
	}
	default:
		res = ResponseFactory::makeText(500, "Unhandled route kind\n",
										"Internal Server Error", /*close*/ true);
		return true;
	}

	// ---- Execute handler ----
	const bool done = h->handle(req, res, ctx);
	delete h;
	return done;
}
