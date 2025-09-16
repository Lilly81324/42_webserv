/* --- CgiRegistry.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

/* --- CgiRegistry.cpp --- */
#include "CgiRegistry.h"


/* 

CgiRegistry::CgiRegistry()
Initializes an empty registry with local_ and global_ map pointers set to NULL.
The registry itself doesn’t own any data; it merely references configuration maps
that live inside Location or VirtualServer. Starting with null pointers makes intent explicit
and avoids accidental dereferencing before configuration is attached.
This lightweight design lets routing choose CGI executables quickly at request time without costly copies.
The constructor therefore prepares a fast,
read-only lookup structure whose lifetime is tied to the surrounding configuration.
It complements the handler’s need for quick “extension → interpreter” resolution.

*/

CgiRegistry::CgiRegistry() : local_(0), global_(0) {}


/* 

CgiRegistry::~CgiRegistry()
Trivial destructor because the registry does not allocate or own memory;
it only keeps raw pointers to configuration maps owned elsewhere.
Doing nothing here is intentional—ownership stays with ServerConfig
/ VirtualServer objects that outlive per-request structures.
This prevents double-frees and makes destruction constant-time even under heavy concurrency.
The destructor’s job is simply to mark lifecycle completion of a read-only lookup helper.
The simplicity mirrors other non-owning components in the codebase,
keeping teardown predictable and side-effect free across hot paths in the server. 

*/


CgiRegistry::~CgiRegistry() {}

/* 

void CgiRegistry::setSources(const std::mapstd::string,CgiSpec
 local, const std::mapstd::string,CgiSpec
 global)**
Binds the registry to two maps: per-location overrides (local) and server-wide defaults (global).
This layered approach mirrors nginx-style configuration—location rules take precedence, then server fallbacks.
The method stores bare pointers because the maps are owned by parsed configuration objects;
avoiding copies ensures lookups remain fast and memory-efficient.
By deferring decisions until request time, the server can honor dynamic routing results (which location matched)
and select the correct interpreter consistently.
After calling this, findByExtension becomes a single, cheap dictionary probe in the right order.

*/

void CgiRegistry::setSources(const std::map<std::string, CgiSpec>* local,
							const std::map<std::string, CgiSpec>* global) {
	local_  = local;
	global_ = global;
}



/* 

static std::string normalizeExt(const std::string& e)
Small helper that ensures extensions are normalized to the 
canonical key form with a leading dot, e.g., "php" → ".php". 
Consistent normalization prevents mismatch between configuration 
entries and requests that may include or omit the dot. 
It also avoids subtle bugs in case multiple subsystems build keys differently. 
By centralizing normalization here, the registry can treat all
callers uniformly and keep logic tiny and testable. 
This function operates purely on strings, 
without filesystem interactions, supporting safe usage across routing and handler code paths.


*/

static std::string normalizeExt(const std::string& e) {
	if (e.empty())
		return e;
	return (e[0] == '.') ? e : std::string(".") + e;
}


/* 

const CgiSpec CgiRegistry::findByExtension(const std::string& raw) const*
Looks up the interpreter specification for a requested file extension.
First normalizes the extension with normalizeExt,
then searches the per-location map.
If not found, it searches the global/server map.
Returning a pointer (or NULL) keeps results lightweight and avoids copies.
This method is the central bridge between routing decisions and actual execution:
CgiHandler relies on it to decide whether to spawn a CGI process and which binary to use.
The precedence order allows overrides like using a different Python or PHP runtime for particular URL subtrees.

*/


const CgiSpec* CgiRegistry::findByExtension(const std::string& raw) const {
	const std::string key = normalizeExt(raw);
	if (local_) {
		std::map<std::string, CgiSpec>::const_iterator it = local_->find(key);
		if (it != local_->end())
			return &it->second;
	}
	if (global_) {
		std::map<std::string, CgiSpec>::const_iterator it = global_->find(key);
		if (it != global_->end())
			return &it->second;
	}
	return 0;
}
