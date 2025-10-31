/* --- Location.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "VirtualServer.h"


/* 

The constructor initializes a location block to safe, explicit defaults so 
routing and handlers can rely on consistent semantics even before any 
configuration is applied. It sets path_prefix empty and regex=false, 
meaning a location matches by simple prefix unless the parser marks 
it as a regex location. root is empty (inherit from the owning VirtualServer 
unless overridden) and autoindex=false so directory listings are disabled by 
default (safer). index_files starts empty; handlers will consult server-level 
defaults or fall back to a direct file lookup.
allowed_methods is cleared, which your routing layer typically interprets as 
“use server defaults” or a conservative set (GET only) until populated. 
Upload-related fields are made explicit: legacy upload_dir empty; modern upload_store empty; 
upload_overwrite=false; upload_max_file_size=0 (unlimited unless policy sets it). 
CGI is disabled by default (cgi_by_ext empty). Proxying is off by default (is_proxy=false, 
proxy_name empty), but sensible timeouts are pre-seeded: proxy_connect_timeout_ms=3000 
and proxy_io_idle_timeout_ms=15000, providing resilience if a config enables proxying later.
Security knobs start neutral: allow_list/deny_list empty (inherit global IP rules), and rate limiting 
/ write configuration are zero-initialized structs. Finally, try_files is empty and return_status=0 
(no synthetic return), return_target empty. Net effect: a minimal, locked-down location 
that becomes active only when the parser fills fields from the config file


*/



Location::Location()
: path_prefix()
, regex(false)
, root()
, autoindex(false)
, index_files()
, allowed_methods()
, upload_dir()
, is_proxy(false)
, proxy_name()
, cgi_by_ext()
, rate_limit()
, write_conf()
, try_files()
, return_status(0)
, return_target()
, allow_list()
, deny_list()
, upload_store()
, upload_overwrite(false)
, upload_max_file_size(0)
, proxy_connect_timeout_ms(3000)
, proxy_io_idle_timeout_ms(15000)
{
}


/* 

Location::~Location() — destructor (~200 words)
The destructor is intentionally trivial because Location 
owns only standard-library objects that clean up themselves 
(e.g., std::string, std::vector, std::map). No file descriptors, sockets, 
or heap-allocated raw pointers are managed here, so there’s no custom teardown 
logic required. This design keeps lifetimes simple: a VirtualServer can hold 
many Locations by value (or in a std::vector<Location>), 
and when the virtual server is destroyed on server shutdown, each location’s 
destructor runs automatically with no side effects beyond releasing memory.
The absence of destructor work also reinforces configuration immutability at runtime: 
once parsed, a Location acts as read-only policy data that multiple concurrent 
requests may consult. There’s no hidden state to reset or cross-resource references to sever. 
If you later extend Location with resources that need explicit cleanup 
(e.g., compiled regex objects or pre-opened directories), you can still keep the destructor 
light by wrapping such resources in RAII types (objects whose own destructors handle cleanup). 
For now, the trivial destructor guarantees that teardown is deterministic and fast, 
preserving shutdown behavior in the single-threaded event loop and avoiding any risk of 
blocking or throwing during configuration object destruction.


*/

Location::~Location() {}

