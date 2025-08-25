#include "Router.h"
#include "RouteResolver.h"
#include "ServerConfig.h"
#include "VirtualServer.h"
#include <map>
#include <string>

static std::string path_from_uri(const std::string &uri)
{
	std::string::size_type q = uri.find('?');
	return (q == std::string::npos) ? uri : uri.substr(0, q);
}

static std::string ext_of(const std::string &path)
{
	std::string::size_type slash = path.rfind('/');
	std::string::size_type dot = path.rfind('.');
	if (dot == std::string::npos)
		return std::string();
	if (slash != std::string::npos && dot < slash)
		return std::string();
	return path.substr(dot);
}

static std::string normalize_path(const std::string &in)
{
	std::string out;
	out.reserve(in.size());
	std::vector<std::string> parts;

	std::string cur;
	for (std::string::size_type i = 0; i < in.size(); ++i)
	{
		char c = in[i];
		if (c == '/')
		{
			if (!cur.empty())
			{
				if (cur == ".")
				{
					// skip
				}
				else if (cur == "..")
				{
					if (parts.empty())
						return std::string(); // illegal
					parts.pop_back();
				}
				else
				{
					parts.push_back(cur);
				}
				cur.clear();
			}
			// collapse slashes by skipping adding empty parts
		}
		else
		{
			cur.push_back(c);
		}
	}
	if (!cur.empty())
	{
		if (cur == ".")
		{
			// skip
		}
		else if (cur == "..")
		{
			if (parts.empty())
				return std::string();
			parts.pop_back();
		}
		else
		{
			parts.push_back(cur);
		}
	}

	// Reconstruct
	out = "/";
	for (std::vector<std::string>::const_iterator it = parts.begin(); it != parts.end(); ++it)
	{
		if (it != parts.begin())
			out += "/";
		out += *it;
	}

	return out;
}

// Helper: Validate VS index
static bool isValidVSIndex(const ServerConfig &cfg, int vs_idx)
{
	return vs_idx >= 0 && vs_idx < (int)cfg.servers().size();
}

// Helper: Check allowed methods (except PUT/PATCH)
static bool isMethodAllowed(const Location *L, const std::string &method)
{
	if (!L || L->allowed_methods.empty() || method == "PUT" || method == "PATCH")
		return true;
	for (std::vector<std::string>::const_iterator it = L->allowed_methods.begin(); it != L->allowed_methods.end(); ++it)
	{
		if (*it == method)
			return true;
	}
	return false;
}

// Helper: Check PUT/PATCH allowed
static bool isPutPatchAllowed(const Location *L, const std::string &method)
{
	if (!L)
		return false;
	if (method == "PUT" && L->write_conf.allow_put)
		return true;
	if (method == "PATCH" && L->write_conf.allow_patch)
		return true;
	return false;
}

// Helper: Check proxy
static bool isProxy(const Location *L)
{
	return L && L->is_proxy;
}

// Helper: Check CGI
static bool isCGI(const Location *L, const std::string &ext)
{
	if (!L)
		return false;
	return L->cgi_by_ext.find(ext) != L->cgi_by_ext.end();
}

void Router::makeDecisionForVS(const ServerConfig &cfg,
							   int vs_idx,
							   const std::string &method,
							   const std::string &uri,
							   RouteDecision &out)
{
	if (!isValidVSIndex(cfg, vs_idx))
	{
		out.kind = RouteDecision::HK_ERROR;
		out.status = 400;
		out.vs = 0;
		out.loc = 0;
		return;
	}

	const VirtualServer &vs = cfg.servers()[vs_idx];
	out.vs = &vs;
	const std::string raw_path = path_from_uri(uri);
	const std::string path = normalize_path(raw_path);
	if (path.empty())
	{
		out.kind = RouteDecision::HK_ERROR;
		out.status = 400;
		return;
	}
	std::string matched_prefix;
	const Location *L = RouteResolver::matchLocation(vs, path, matched_prefix);
	out.loc = L;

	// populate routing helper fields
	out.matched_prefix = matched_prefix;
	// rel_path = path minus matched_prefix
	if (!matched_prefix.empty() && path.size() >= matched_prefix.size())
		out.rel_path = path.substr(matched_prefix.size());
	else
		out.rel_path = path;
	// ensure rel_path starts with '/'
	if (out.rel_path.empty() || out.rel_path[0] != '/')
		out.rel_path = std::string("/") + out.rel_path;
	// effective root
	if (L && !L->root.empty())
		out.effective_root = L->root;
	else
		out.effective_root = vs.root;

	// Surface policy fields from Location into the decision so the pipeline can act on them
	if (L) {
		// try_files
		out.try_files.clear();
		for (std::vector<std::string>::const_iterator it = L->try_files.begin(); it != L->try_files.end(); ++it)
			out.try_files.push_back(*it);

		// return directive
		if (L->return_status != 0) {
			out.kind = RouteDecision::HK_RETURN;
			out.status = L->return_status;
			out.return_target = L->return_target;
			return;
		}

		// rate limit and allow/deny lists
		out.rate_limit = L->rate_limit;
		out.allow_list = L->allow_list;
		out.deny_list = L->deny_list;
	}

	// Check allowed methods (except PUT/PATCH)
	if (!isMethodAllowed(L, method))
	{
		out.kind = RouteDecision::HK_ERROR;
		out.status = 405;
		return;
	}

	// PUT/PATCH
	if ((method == "PUT" || method == "PATCH") && !isPutPatchAllowed(L, method))
	{
		out.kind = RouteDecision::HK_ERROR;
		out.status = 405;
		return;
	}
	if ((method == "PUT" || method == "PATCH") && isPutPatchAllowed(L, method))
	{
		out.kind = RouteDecision::HK_PUTPATCH;
		out.status = 200;
		return;
	}

	// Proxy
	if (isProxy(L))
	{
		out.kind = RouteDecision::HK_PROXY;
		out.upstream_name = L->proxy_name;
		out.status = 200;
		return;
	}

	// CGI
	const std::string ext = ext_of(path);
	if (!ext.empty() && isCGI(L, ext))
	{
		out.kind = RouteDecision::HK_CGI;
		out.cgi_ext = ext;
		out.status = 200;
		return;
	}

	// Static (default)
	if (L)
	{
		out.kind = RouteDecision::HK_STATIC;
		out.status = 200;
		return;
	}
	else
	{
		out.kind = RouteDecision::HK_STATIC;
		out.status = 200;
		return;
	}
}
