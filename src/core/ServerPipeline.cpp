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
#include <sys/stat.h>
#include <sstream>

// C++98: helper to uppercase a method cheaply
static std::string upcase(const std::string &s)
{
	std::string r(s);
	for (size_t i = 0; i < r.size(); ++i)
		if (r[i] >= 'a' && r[i] <= 'z')
			r[i] = r[i] - 'a' + 'A';
	return r;
}

bool ServerPipeline::processRequest(const ServerConfig &cfg, int vs_indx, HttpRequest &req, HttpResponse &res , RouteDecision &decision)
{
	RequestContext ctx;
	ctx.cfg = &cfg;
	ctx.vs_index = vs_indx;
	ctx.vs = NULL;
	if (ctx.vs == NULL && ctx.vs_index >= 0 && ctx.vs_index < (int)cfg.servers().size())
		ctx.vs = &cfg.servers()[ctx.vs_index];

	if (ctx.vs == NULL)
	{
		// setError(rep, 500, "No virtual server resolved");
		return false;
	}

	// Pick the handler
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
	case RouteDecision::HK_ERROR:
		return false;
		break;
	default:
		// setError(res, dec.status, toReason(dec.status));
		return true;
	}

	const bool done = h->handle(req, res, ctx); // true => response complete
	delete h;
	return done;
}


PipelineAction ServerPipeline::policyGate(const ServerConfig &cfg, int vs_index, HttpRequest &req, RouteDecision &decision, HttpResponse &res)
{

	Router::makeDecisionForVS(cfg, vs_index, req.getMethod(), req.getPath(), decision);
	if (decision.kind == RouteDecision::HK_ERROR)
	{
		res = ResponseFactory::makeError(500, "Internal Server Error");
		return PIPE_ERR_SENT;
	}

	// 1) HTTP/1.1 Host requirement (do it here so ClientConnection stays lean)
	const Headers &H = req.getHeaders();
	if (!H.keyExists(HDR_HOST))
	{
		res = ResponseFactory::makeError(400, "Bad Request", /*close*/ true);
		return PIPE_ERR_SENT;
	}

	// 2) Method policy (if your Router surfaces 405 via decision.status, prefer that)
	if (decision.status == 405)
	{
		res = ResponseFactory::makeError(405, "Method Not Allowed", /*close*/ true);
		// Optional: if you have a prebuilt Allow header string:
		// res.headers.set("Allow", decision.allow_header);
		return PIPE_ERR_SENT;
	}

	// 3) Body policy (early, cheap)
	const bool hasCL = H.keyExists(HDR_CONTENT_LENGTH);
	const bool hasTE = H.keyExists(HDR_TRANSFER_ENCODING);

	bool chunked = false;
	if (hasTE)
	{
		std::string v = H.get(HDR_TRANSFER_ENCODING);
		for (size_t i = 0; i < v.size(); ++i)
			if (v[i] >= 'A' && v[i] <= 'Z')
				v[i] += 32;
		chunked = (v.find("chunked") != std::string::npos);
	}

	if (hasCL && chunked)
	{
		res = ResponseFactory::makeError(400, "Bad Request", /*close*/ true);
		return PIPE_ERR_SENT;
	}

	const std::string m = upcase(req.getMethod());
	const bool needs_body = (m == "POST" || m == "PUT" || m == "PATCH");

	if (needs_body && !hasCL && !chunked)
	{
		res = ResponseFactory::makeError(411, "Length Required", /*close*/ true);
		return PIPE_ERR_SENT;
	}

	if (hasCL)
	{
		// parse CL strictly
		size_t cl = 0;
		const std::string s = H.get(HDR_CONTENT_LENGTH);
		if (s.empty())
		{
			res = ResponseFactory::makeError(400, "Bad Request", /*close*/ true);
			return PIPE_ERR_SENT;
		}
		for (size_t i = 0; i < s.size(); ++i)
		{
			const char c = s[i];
			if (c < '0' || c > '9')
			{
				res = ResponseFactory::makeError(400, "Bad Request", /*close*/ true);
				return PIPE_ERR_SENT;
			}
			cl = cl * 10 + (c - '0');
		}

		// Prefer location limit, else server limit (your fields)
		const size_t locLimit = (decision.loc) ? decision.loc->write_conf.max_body_bytes : 0;
		const size_t srvLimit = (decision.vs) ? decision.vs->client_max_body_size : 0;
		const size_t limit = (locLimit > 0) ? locLimit : srvLimit;

		if (limit > 0 && cl > limit)
		{
			res = ResponseFactory::makeError(413, "Payload Too Large", /*close*/ true);
			return PIPE_ERR_SENT; // do NOT read body
		}
	}

	// 4) Decide the next step for the connection
	if (!needs_body || (!hasCL && !chunked))
	{
		return PIPE_HANDLE_NOW; // e.g., GET/HEAD or POST without body
	}
	return PIPE_READ_BODY; // stream the body next
}
