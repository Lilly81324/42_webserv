/* --- Location.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "VirtualServer.h"


/* 

Location::Location()
Constructs a Location with safe defaults: empty path_prefix, 
regex=false, root empty, autoindex=false, empty index_files, 
allowed_methods, and upload_dir. It also initializes proxy flags 
(is_proxy=false, proxy_name empty), CGI mapping (cgi_by_ext), 
rate-limit/write configs, try_files, and redirect fields (return_status=0, return_target empty), 
plus allow/deny lists. These defaults ensure routing decisions don’t accidentally enable features; 
handlers must be explicitly configured per route. Clear initialization prevents undefined behavior 
during route matching, MIME/autoindex checks, method authorization, 
uploads, CGI selection, and redirects. It’s the foundation for nginx-style per-location policy.

*/

Location::Location()
	: path_prefix(), regex(false), root(), autoindex(false),
	index_files(), allowed_methods(), upload_dir(), is_proxy(false),
	proxy_name(), cgi_by_ext(), rate_limit(), write_conf(),
	try_files(), return_status(0), return_target(), allow_list(), deny_list() {}

Location::~Location() {}
