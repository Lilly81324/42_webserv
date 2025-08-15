/* --- VirtualServer.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef VIRTUALSERVER_H
#define VIRTUALSERVER_H

#include <string>
#include <vector>
#include <map>

struct CgiSpec
{
	std::string bin;
	int timeout_ms;
};

struct RateLimitConfig
{
	int requests_per_minute;
	int burst;
	bool enabled;
};

struct PutPatchConfig
{
	std::string root_directory;
	int max_body_bytes;
	bool allow_put;
	bool allow_patch;
	bool generate_etag;
};

struct Upstream
{
	std::string host;
	int port;
	bool healthy;
	int weight;
};

struct UpstreamPool
{
	std::vector<Upstream> nodes;
	std::string strategy;
	std::string health_path;
	int health_interval_ms;
};

struct Location
{
	std::string path_prefix;
	bool regex;
	std::string root;
	bool autoindex;
	std::vector<std::string> index_files;
	std::vector<std::string> allowed_methods;
	std::string upload_dir;
	bool is_proxy;
	std::string proxy_name;
	std::map<std::string, CgiSpec> cgi_by_ext;
	RateLimitConfig rate_limit;
	PutPatchConfig write_conf;
};

struct SessionConfig
{
	bool enabled;
	std::string cookie_name;
	int max_age;
	bool secure;
	bool http_only;
	std::string same_site;
};

struct MimeConfig
{
	std::map<std::string, std::string> mapping;
};

struct CgiDefaultsConfig
{
	std::map<std::string, CgiSpec> cgi_defaults;
};

class VirtualServer
{
public:
	std::string listen_host;
	int listen_port;
	std::vector<std::string> server_names;
	std::string root;
	std::vector<std::string> index_files;
	std::map<int, std::string> error_pages;
	RateLimitConfig rate_limit;
	std::map<std::string, UpstreamPool> upstreams;
	std::vector<Location> locations;
	VirtualServer();
	~VirtualServer();
};
#endif // VIRTUALSERVER_H
