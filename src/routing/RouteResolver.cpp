// RouteResolver.cpp
#include "RouteResolver.h"
#include "VirtualServer.h"

#include <string>
#include <vector>
#include <map>	   // C++98: std::map< std::string, ... >
#include <cstddef> // size_t

// ---------- small helpers ----------

static bool starts_with(const std::string &s, const std::string &prefix)
{
	if (prefix.size() > s.size())
		return false;
	return s.compare(0, prefix.size(), prefix) == 0;
}

// ---------- Location matching ----------
//
// Strategy: longest-prefix match among non-regex locations.
// (If you add regex support later, give it higher or explicit precedence.)

const Location *RouteResolver::matchLocation(const VirtualServer &vs,
											 const std::string &path)
{
	std::string dummy;
	return matchLocation(vs, path, dummy);
}

const Location *RouteResolver::matchLocation(const VirtualServer &vs,
											 const std::string &path,
											 std::string &matched_prefix)
{
	const Location *best = 0;
	std::size_t best_len = 0;
	matched_prefix.clear();

	for (std::size_t i = 0; i < vs.locations.size(); ++i)
	{
		const Location &loc = vs.locations[i];

		// If regex matching is supported in your build, handle it here.
		// For now we prioritize simple prefix matches (regex==false).
		if (loc.regex)
		{
			// TODO: plug your regex engine here if needed.
			continue;
		}

		const std::string &pref = loc.path_prefix;
		if (pref.empty())
			continue;
		if (!starts_with(path, pref))
			continue;

		if (pref.size() > best_len)
		{
			best = &loc;
			best_len = pref.size();
			matched_prefix = pref;
		}
	}

	// If no explicit prefix matched, you might have a default "/" rule.
	if (!best)
	{
		for (std::size_t i = 0; i < vs.locations.size(); ++i)
		{
			const Location &loc = vs.locations[i];
			if (!loc.regex && (loc.path_prefix == "/" || loc.path_prefix.empty()))
			{
				best = &loc;
				matched_prefix = loc.path_prefix;
				break;
			}
		}
	}

	return best;
}

// ---------- Upstream resolution (round-robin) ----------
//
// We keep a global (file-scope) per-pool cursor. In a single-threaded,
// single event-loop server this is safe. If you hot-reload config, consider
// clearing this map.

static std::map<std::string, int> g_rrCursor;

// pick next healthy node by round-robin; returns node index or -1 if none
static int pick_round_robin_index(const UpstreamPool &pool, const std::string &poolName)
{
	// Build list of indices for healthy, usable nodes.
	std::vector<int> healthy;
	for (std::size_t i = 0; i < pool.nodes.size(); ++i)
	{
		const Upstream &u = pool.nodes[i];
		if (u.healthy && u.port > 0 && !u.host.empty())
			healthy.push_back(static_cast<int>(i));
	}
	if (healthy.empty())
		return -1;

	int &cursor = g_rrCursor[poolName]; // default-constructs to 0 on first use
	if (cursor < 0)
		cursor = 0;
	if (cursor >= static_cast<int>(healthy.size()))
		cursor = 0;

	const int chosen = healthy[cursor];
	cursor = (cursor + 1) % static_cast<int>(healthy.size());
	return chosen;
}

bool RouteResolver::resolveProxyTarget(const VirtualServer &vs,
									   const std::string &poolName,
									   std::string &outHost,
									   int &outPort)
{
	outHost.clear();
	outPort = 0;

	// Find the named upstream pool in this virtual server
	std::map<std::string, UpstreamPool>::const_iterator it = vs.upstreams.find(poolName);
	if (it == vs.upstreams.end())
		return false;

	const UpstreamPool &pool = it->second;

	// Choose strategy (default to roundrobin if unspecified/unknown)
	const std::string strategy = pool.strategy.empty()
									 ? std::string("roundrobin")
									 : pool.strategy;

	int idx = -1;
	if (strategy == "roundrobin")
	{
		idx = pick_round_robin_index(pool, poolName);
	}
	else
	{
		// Fallback for unrecognized strategies—use RR for now.
		idx = pick_round_robin_index(pool, poolName);
	}

	if (idx < 0 || idx >= static_cast<int>(pool.nodes.size()))
		return false;

	const Upstream &node = pool.nodes[static_cast<std::size_t>(idx)];
	if (node.host.empty() || node.port <= 0)
		return false;

	outHost = node.host;
	outPort = node.port;
	return true;
}
