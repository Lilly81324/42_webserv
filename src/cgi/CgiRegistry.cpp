/* --- CgiRegistry.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

/* --- CgiRegistry.cpp --- */
#include "CgiRegistry.h"

/*

CgiRegistry::CgiRegistry() / ~CgiRegistry()

The constructor initializes the registry in a non-owning,
detached state: both internal pointers (local_, global_)
are set to NULL so the object starts with “no sources.”
That makes accidental use obvious—lookups simply return NULL until
configuration is attached—while keeping the type extremely cheap to construct
(no copies of maps, no allocations). This is helpful because you can create a
CgiRegistry per request or per routing decision with negligible cost.
The destructor is trivial because ownership stays with the parsed configuration
structures (Location, VirtualServer): the registry never allocates or frees those maps;
it only points at them. This design choice enforces a clean separation of concerns:
parsing fills authoritative CGI maps; routing selects which two maps should be visible
for a given request; CgiRegistry provides a tiny, cache-friendly façade for layered
lookup without duplicating data or risking lifetime bugs. In short,
the ctor/dtor make the registry a safe, lightweight handle to configuration,
perfect for high-frequency use on the hot path.

*/

CgiRegistry::CgiRegistry() : local_(0), global_(0) {}
CgiRegistry::~CgiRegistry() {}

/*

void CgiRegistry::setSources(const std::map<std::string,CgiSpec>*
local, const std::map<std::string,CgiSpec>* global)

setSources binds the registry to two optional maps: a local
(per-location) override and a global (per-server) default.
By storing pointers instead of copies, the registry reflects the current
configuration instantly and avoids any cloning cost; it also keeps
memory ownership clear (maps live with the config).
The precedence model matches intuitive server semantics:
check the narrower scope first (location), then fall back to the
broader scope (server). Either pointer may be NULL; that simply
removes that layer from consideration, allowing configurations
that define only global rules or only local overrides.
You typically call setSources immediately after routing
(once you know which Location and VirtualServer matched the request),
so that subsequent CGI resolution is aligned with the chosen route.
This one call turns the registry from a detached shell into a ready-to-use
resolver that is both fast (pointer dereferences + map lookup)
and deterministic (well-defined shadowing rules).

*/

void CgiRegistry::setSources(const std::map<std::string, CgiSpec> *local,
							 const std::map<std::string, CgiSpec> *global)
{
	local_ = local;
	global_ = global;
}

/*

static std::string normalizeExt(const std::string& e)

normalizeExt canonicalizes a filename extension into the exact
key used for lookups. If the incoming string is empty,
it returns an empty string (meaning “no extension”).
Otherwise it ensures the key starts with a dot: "php" becomes ".php",
while an already dotted value like ".py" is returned unchanged.
This tiny normalization step prevents subtle mismatches between how
configuration is authored and how extensions are extracted from request paths.
Without it, you might end up with duplicate rules ("php" and ".php") or, worse,
failed lookups when some code path strips the dot and another doesn’t.
Keeping normalization in a single, private helper guarantees
consistent behavior across the entire codebase and keeps findByExtension
focused on lookup logic, not input hygiene. Since the function performs
only O(1) checks and simple string concatenation, it’s effectively free
on the hot path while eliminating a whole class of “why didn’t CGI trigger?” bugs.


*/

static std::string normalizeExt(const std::string &e)
{
	if (e.empty())
		return e;
	return (e[0] == '.') ? e : std::string(".") + e;
}

/*

const CgiSpec* CgiRegistry::findByExtension(const std::string& raw) const

This is the resolver. It first canonicalizes the requested extension via normalizeExt
to avoid key mismatches, then consults the local map (if set). If a match is found,
it returns a pointer to the CgiSpec—that’s the per-location override.
If not, it consults the global map (if set) as a fallback.
If neither layer contains the extension,
it returns NULL, signaling “no CGI mapping” so the caller can fall back
to static handling or other mechanisms. Returning a pointer instead of a copy
keeps the call cheap and respects the registry’s non-owning contract;
the pointed-to spec remains valid as long as the underlying configuration objects
live (which is for the lifetime of the server). Operationally, this function encodes
the exact override semantics you want: location beats server, deterministic, and O(log n)
with std::map (and typically tiny n). Handlers such as CgiHandler call this once per
request to decide whether a path like /foo/bar.php
should be executed through a configured interpreter.


*/

const CgiSpec *CgiRegistry::findByExtension(const std::string &raw) const
{
	const std::string key = normalizeExt(raw);
	if (local_)
	{
		std::map<std::string, CgiSpec>::const_iterator it = local_->find(key);
		if (it != local_->end())
			return &it->second;
	}
	if (global_)
	{
		std::map<std::string, CgiSpec>::const_iterator it = global_->find(key);
		if (it != global_->end())
			return &it->second;
	}
	return 0;
}
