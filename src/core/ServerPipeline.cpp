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
#include <sys/stat.h>
#include <sstream>

bool ServerPipeline::processRequest(const ServerConfig &cfg, int vs_indx, HttpRequest &req, HttpResponse &res)
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

	RouteDecision decision;
	Router::makeDecisionForVS(cfg, vs_indx, req.getMethod(), req.getPath(), decision);

	if (decision.kind == RouteDecision::HK_ERROR)
		return false;
	// 405 Method Not Allowed (use decision.allowed_methods if you surface it; otherwise leave as-is)
	if (decision.status == 405)
	{ // your Router sets HK_ERROR/status for deny; if not, skip this
		res.status = 405;
		res.reason = "Method Not Allowed";
		// If you keep an Allow string somewhere, set it here:
		// res.headers.set("Allow", decision.allow_header);
		// Minimal body (optional)
		const std::string body = "Method Not Allowed";
		res.body.assign(body.begin(), body.end());
		return true;
	}
	const Headers &H = req.getHeaders();
	// Body policy
	const bool hasCL = H.keyExists(HDR_CONTENT_LENGTH);
	const bool hasTE = H.keyExists(HDR_TRANSFER_ENCODING);
	bool chunked = false;
	if (hasTE)
	{
		std::string v = H.get(HDR_TRANSFER_ENCODING);
		for (size_t i = 0; i < v.size(); ++i)
			if (v[i] >= 'A' && v[i] <= 'Z')
				v[i] = v[i] - 'A' + 'a';
		chunked = (v.find("chunked") != std::string::npos);
	}

	// Conflict: both CL and chunked
	if (hasCL && chunked)
	{
		res.status = 400;
		res.reason = "Bad Request";
		const std::string body = "Bad Request";
		res.body.assign(body.begin(), body.end());
		return true;
	}

	// Methods that require a body (adjust if you need)
	std::string m = req.getMethod();
	for (size_t i = 0; i < m.size(); ++i)
		if (m[i] >= 'a' && m[i] <= 'z')
			m[i] = m[i] - 'a' + 'A';
	const bool needs_body = (m == "POST" || m == "PUT" || m == "PATCH");

	// 411 Length Required
	if (needs_body && !hasCL && !chunked)
	{
		res.status = 411;
		res.reason = "Length Required";
		const std::string body = "Length Required";
		res.body.assign(body.begin(), body.end());
		return true;
	}

	// Early 413 on declared Content-Length
	size_t cl = 0;
	if (hasCL)
	{
		std::string s = H.get(HDR_CONTENT_LENGTH);
		// strict digits parse
		for (size_t i = 0; i < s.size(); ++i)
		{
			char c = s[i];
			if (c < '0' || c > '9')
			{
				res.status = 400;
				res.reason = "Bad Request";
				return true;
			}
			cl = cl * 10 + (c - '0');
		}
		// location limit else server limit
		size_t locLimit = (decision.loc) ? decision.loc->write_conf.max_body_bytes : 0;
		size_t srvLimit = (decision.vs) ? decision.vs->client_max_body_size : 0;
		size_t limit = (locLimit > 0) ? locLimit : srvLimit;

		if (limit > 0 && cl > limit)
		{
			res.status = 413;
			res.reason = "Payload Too Large";
			const std::string body = "Payload Too Large";
			res.body.assign(body.begin(), body.end());
			// You may also add: res.headers.set("Connection","close");
			return true; // early reject (don’t read body)
		}

		ctx.loc = decision.loc;
		ctx.cgi_ext = decision.cgi_ext;
		ctx.upstream_name = decision.upstream_name;
		if (decision.vs)
			ctx.vs = decision.vs; // Router may refine VS

		ctx.rel_path = decision.rel_path;
		ctx.effective_root = decision.effective_root;

		// HK_RETURN: immediate response
		if (decision.kind == RouteDecision::HK_RETURN)
		{
			// If redirect-like status, set Location header; otherwise put target in body
			if (!decision.return_target.empty())
			{
				if (decision.status >= 300 && decision.status < 400)
					ctx.cfg->servers()[vs_indx].locations; // no-op to avoid unused warning
			}
			// build a simple body containing the return_target (or empty)
			std::string body = decision.return_target;
			res.body.assign(body.begin(), body.end());
			res.bodyLength = res.body.size();
			std::ostringstream oss;
			oss << res.bodyLength;
			res.headers.set(std::string("Content-Length"), oss.str());
			if (decision.status >= 300 && decision.status < 400 && !decision.return_target.empty())
				res.headers.set(std::string("Location"), decision.return_target);
			return true;
		}
		// Evaluate try_files tokens if present
		if (!decision.try_files.empty())
		{
			for (std::vector<std::string>::const_iterator it = decision.try_files.begin(); it != decision.try_files.end(); ++it)
			{
				std::string token = *it;
				std::string candidate;
				if (token == std::string("$uri"))
					candidate = ctx.effective_root + ctx.rel_path;
				else if (!token.empty() && token[0] == '/')
					candidate = ctx.effective_root + token;
				else
					candidate = ctx.effective_root + token; // relative token

				struct stat st;
				if (stat(candidate.c_str(), &st) == 0)
				{
					// Found a matching file — rewrite rel_path to candidate-relative and dispatch static
					if (candidate.find(ctx.effective_root) == 0)
					{
						ctx.rel_path = candidate.substr(ctx.effective_root.size());
						if (ctx.rel_path.empty() || ctx.rel_path[0] != '/')
							ctx.rel_path = std::string("/") + ctx.rel_path;
					}
					decision.kind = RouteDecision::HK_STATIC;
					break;
				}
			}
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
}