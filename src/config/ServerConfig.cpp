#include "ServerConfig.h"
#include "HTTPCODES.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <cstdlib>
#include <set>
#include <climits> // INT_MAX
#include <limits.h>   // PATH_MAX
#include <stdlib.h>   // realpath



/* 

static std::string makeAbsolute(const std::string& path)

Canonicalizes configuration paths using realpath, 
returning an absolute, symlink-resolved filesystem path when possible, 
else the original on failure. Using absolute paths makes later joins, 
safety checks, and error reporting deterministic across working directories. 
It prevents surprises where relative root, upload_store, or client_body_temp_path might refer 
to different places depending on how the daemon is launched. Centralizing path resolution 
here keeps parsing code clean and avoids scattering realpath calls throughout modules. Even when resolution 
fails (nonexistent files at parse time), returning the input allows delayed existence 
checks elsewhere without throwing, supporting flexible workflows.


*/

static std::string makeAbsolute(const std::string &path) {
	char buf[PATH_MAX];
	if (::realpath(path.c_str(), buf))
		return std::string(buf);
	return path; // fallback if resolution fails
}

// ---------- tiny helpers (only those we use) ----------


/* 

static bool parseUnsigned(const std::string& s, int& out)

Strict decimal parser for small non-negative integers used for ports, weights, 
timeouts, and simple numeric directives. Rejects empty strings and non-digits, 
accumulates in a long, and applies a conservative guard to avoid pathological 
growth before casting to int. Returning false instead of throwing lets higher-level 
code decide error wording and context. This tiny helper keeps token processing lightweight, 
independent of locale, and deterministic—important for parsing configuration reliably in constrained environments. 
Its simplicity also improves testability and avoids tricky edge cases introduced by permissive C library conversions.

*/


static bool parseUnsigned(const std::string &s, int &out)
{
	if (s.empty())
		return false;
	long v = 0;
	for (std::string::size_type i = 0; i < s.size(); ++i)
	{
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10 + (s[i] - '0');
		if (v > 1000000L)
			return false; // guard
	}
	out = static_cast<int>(v);
	return true;
}


/* 

static void validateErrorStatusOrThrow(const std::string& raw)

Validates that an error_page directive’s status code token is a legal HTTP error status (between 400 and 599). 
It uses parseUnsigned and the project’s HTTP constants, throwing std::runtime_error with a precise message 
when the value is missing, malformed, or outside range. 
Doing strict validation early prevents nonsensical mappings (e.g., mapping 200 or negative numbers to error pages), 
which would later confuse response selection and testing. 
Centralizing this validation keeps the error_page parsing clear and consistent with other status-code checks in the server.

*/

static void validateErrorStatusOrThrow(const std::string &raw)
{
	int code = -1;
	if (!parseUnsigned(raw, code) || code < HTTP_BAD_REQUEST || code > HTTP_SERVER_ERROR) {
		std::ostringstream oss;
		oss << "invalid error_page status code: '" << raw << "'";
		throw std::runtime_error(oss.str());
	}
}

// ---------- ServerConfig impl ----------

/* 

ServerConfig::ServerConfig()

Constructs a fresh configuration with empty server list and sensible global defaults: 
sessions disabled, cookie attributes blank, MIME map and CGI defaults empty. 
This provides a neutral baseline before parsing any text, ensuring every field has a defined value. 
The design avoids unnecessary dynamic allocation and leaves ownership to normal STL containers, 
simplifying teardown and reloads. Having a clean base state makes partial parsing safe, 
because later functions can reset or append without risking stale data from previous loads.


*/

ServerConfig::ServerConfig()
	: _servers(), session_enabled(false), session_cookie_name(), session_max_age(0), session_secure(false), session_http_only(false), session_same_site(), mime_mapping(), cgi_defaults()
{
}

ServerConfig::~ServerConfig() {}


/* 

void ServerConfig::push_back(const VirtualServer& vs)

Appends an already-constructed VirtualServer to the internal list. 
This is used by the parser once a server { ... } block completes successfully. 
Keeping a thin method rather than exposing the vector directly makes intent clear and offers a single place to add future invariants 
(e.g., index or cross-validation) if desired. 
It maintains symmetry with other helper methods and preserves encapsulation of the server list.


*/

void ServerConfig::push_back(const VirtualServer &vs)
{
	_servers.push_back(vs);
}

/* 

bool ServerConfig::canOpen(const char* path) const

Quick existence/readability probe for configuration files using std::ifstream. 
Returns true if the file stream opens successfully, without reading contents. 
Useful for early checks and better error messages before invoking the full parser. 
By not throwing, it supports CLI behavior like “file not found” without a stack trace, 
keeping UX friendly. It’s intentionally conservative—subsequent parsing still handles I/O errors robustly.


*/

bool ServerConfig::canOpen(const char *path) const
{
	std::ifstream f(path);
	return f.good();
}


/* 

std::string ServerConfig::readWholeFile(const std::string& path)

Reads an entire configuration file into a string buffer (std::ostringstream << rdbuf()), 
throwing with context if the file can’t be opened. Isolating file I/O here keeps parseFile small and testable, 
and allows parseString to be exercised independently with in-memory text. Using binary mode avoids platform-specific 
newline transformations so tokenization sees raw characters. Centralizing the “read all” 
behavior also simplifies future enhancements like size caps or informative diagnostics.


*/

std::string ServerConfig::readWholeFile(const std::string &path)
{
	std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
	if (!f)
	{
		throw std::runtime_error("cannot open config: " + path);
	}
	std::ostringstream oss;
	oss << f.rdbuf();
	return oss.str();
}

/* 

std::vector<std::string> ServerConfig::tokenize(const std::string& data)

A minimal tokenizer for nginx-style config:
Skips # comments to end of line.
Splits on whitespace.
Emits {, }, ; as standalone tokens.
Accumulates other characters into words.
There’s no string-literal or escape processing—intentionally simple and predictable for this project’s grammar. 
Producing a flat token stream decouples lexing from parsing, making errors easier to report with token indices. 
The approach minimizes dependencies, allocations, and complexity while supporting the directives used.

*/

std::vector<std::string> ServerConfig::tokenize(const std::string &data)
{
	std::vector<std::string> out;
	std::string cur;

	for (std::string::size_type i = 0; i < data.size();)
	{
		char ch = data[i];

		// comments start with '#': skip until newline
		if (ch == '#')
		{
			while (i < data.size() && data[i] != '\n')
				++i;
			continue;
		}

		// whitespace splits tokens
		if (std::isspace(static_cast<unsigned char>(ch)))
		{
			if (!cur.empty())
			{
				out.push_back(cur);
				cur.clear();
			}
			++i;
			continue;
		}

		// single-character tokens
		if (ch == '{' || ch == '}' || ch == ';')
		{
			if (!cur.empty())
			{
				out.push_back(cur);
				cur.clear();
			}
			out.push_back(std::string(1, ch));
			++i;
			continue;
		}

		// otherwise collect into current word
		cur.push_back(ch);
		++i;
	}

	if (!cur.empty())
		out.push_back(cur);
	return out;
}


/* 

static std::string expectWord(std::size_t& i, const std::vector<std::string>& t) (anonymous namespace)

Fetches the next token as a “word,” advancing the index or throwing “unexpected end” if no token remains. 
Centralizing this guard avoids repetitive bounds checks scattered through parsing logic and yields clearer error messages. 
It’s used for directives expecting an immediate parameter, like root, server_name entries, or health_path. 
The helper’s simplicity contributes to robust, readable parser code.

*/

namespace
{

	// next token as word (or throw)
	static std::string expectWord(std::size_t &i, const std::vector<std::string> &t)
	{
		if (i >= t.size())
			throw std::runtime_error("unexpected end");
		return t[i++];
	}

	// parse an unsigned integer, tolerating:
	//  - a trailing ';' that might have stuck to the number (defensive)
	//  - nginx-style size suffixes: 1k (1024), 10m (10*1024*1024)
	static int expectInt(std::size_t &i, const std::vector<std::string> &t, const char *what)
	{
		if (i >= t.size())
			throw std::runtime_error(std::string("missing number for ") + what);
		std::string s = t[i++];

		// strip an accidental trailing ';' (defensive)
		if (!s.empty() && s[s.size() - 1] == ';')
			s.erase(s.size() - 1);

		// support nginx-like suffixes k/m (bytes)
		long multiplier = 1;
		if (!s.empty())
		{
			char last = s[s.size() - 1];
			if (last == 'k' || last == 'K')
			{
				multiplier = 1024;
				s.erase(s.size() - 1);
			}
			else if (last == 'm' || last == 'M')
			{
				multiplier = 1024L * 1024L;
				s.erase(s.size() - 1);
			}
		}

		// digits only
		if (s.empty())
			throw std::runtime_error(std::string("invalid number for ") + what);
		long v = 0;
		for (std::string::size_type j = 0; j < s.size(); ++j)
		{
			if (s[j] < '0' || s[j] > '9')
				throw std::runtime_error(std::string("invalid number for ") + what);
			v = v * 10 + (s[j] - '0');
			if (v > LONG_MAX / 10)
				break; // avoid UB; rough guard
		}
		v *= multiplier;
		if (v < 0 || v > INT_MAX)
			throw std::runtime_error(std::string("number too large for ") + what);
		return static_cast<int>(v);
	}

} // anonymous namespace


/* 

void ServerConfig::parseTokens(const std::vector<std::string>& tok)

The main recursive-descent style parser over a flat token vector. Supports:
Global blocks: types { ext mime; }, global cgi .ext [bin] timeout;, session settings, global upstream NAME { ... }.
Server blocks: server { listen ...; server_name ...; root ...; index ...; error_page ...; client_body_temp_path ...; client_max_body_size ...; upstream ...; location ... }.
Location blocks: root, index, autoindex on|off, methods (GET/POST/DELETE), upload_dir, cgi, proxy_pass POOL, rate-limit, allow_put/allow_patch/generate_etag/client_max_body_size, try_files, return CODE [target], allow/deny, upload_store, upload_overwrite, upload_max_file_size.
It throws clear exceptions on syntax/semantic errors, accumulates VirtualServers, and defers uniqueness checks.


*/

void ServerConfig::parseTokens(const std::vector<std::string> &tok)
{
	_servers.clear();

	const std::size_t N = tok.size();
	std::size_t i = 0;

	while (i < N)
	{
		const std::string &t0 = tok[i];

		// ================= GLOBAL: types { ext mime; ... }
		if (t0 == "types")
		{
			if (i + 1 >= N || tok[i + 1] != "{")
				throw std::runtime_error("expected '{' after 'types'");
			i += 2;
			while (i < N && tok[i] != "}")
			{
				const std::string ext = expectWord(i, tok);
				const std::string mime = expectWord(i, tok);
				if (i >= N || tok[i] != ";")
					throw std::runtime_error("expected ';' in types block");
				++i;
				mime_mapping[ext] = mime;
			}
			if (i >= N || tok[i] != "}")
				throw std::runtime_error("missing '}' to close types");
			++i;
			continue;
		}

		// ================= GLOBAL: cgi .ext [binary] timeout_ms ;
		if (t0 == "cgi")
		{
			if (i + 2 >= N)
				throw std::runtime_error("cgi expects: .ext [binary] timeout_ms ;");
			const std::string ext = tok[i + 1];
			std::size_t adv = 2;
			std::string bin;
			int timeout = 0;
			if (i + 2 < N && tok[i + 2] != ";" && tok[i + 2] != "{" && tok[i + 2] != "}")
			{
				if (parseUnsigned(tok[i + 2], timeout))
				{
					adv = 3; // cgi .php 3000 ;
				}
				else
				{
					bin = tok[i + 2];
					adv = 3; // cgi .php /path/to/bin 3000 ;
					if (i + 3 < N)
					{
						if (!parseUnsigned(tok[i + 3], timeout))
							throw std::runtime_error("cgi timeout must be integer");
						adv = 4;
					}
				}
			}
			if (i + adv < N && tok[i + adv] == ";")
				++adv;
			cgi_defaults[ext] = CgiSpec(bin, timeout);
			i += adv;
			continue;
		}

		// ================= GLOBAL: session flags
		if (t0 == "session")
		{
			if (i + 1 >= N)
				throw std::runtime_error("session on|off");
			const std::string v = tok[i + 1];
			if (v == "on")
				session_enabled = true;
			else if (v == "off")
				session_enabled = false;
			else
				throw std::runtime_error("session expects on|off");
			i += (i + 2 < N && tok[i + 2] == ";") ? 3 : 2;
			continue;
		}
		if (t0 == "session_cookie_name")
		{
			session_cookie_name = tok[i + 1];
			i += (i + 2 < N && tok[i + 2] == ";") ? 3 : 2;
			continue;
		}
		if (t0 == "session_max_age")
		{
			session_max_age = expectInt(++i, tok, "session_max_age");
			if (i < N && tok[i] == ";")
				++i;
			continue;
		}
		if (t0 == "session_secure")
		{
			const std::string v = tok[i + 1];
			session_secure = (v == "on");
			i += (i + 2 < N && tok[i + 2] == ";") ? 3 : 2;
			continue;
		}
		if (t0 == "session_http_only")
		{
			const std::string v = tok[i + 1];
			session_http_only = (v == "on");
			i += (i + 2 < N && tok[i + 2] == ";") ? 3 : 2;
			continue;
		}
		if (t0 == "session_same_site")
		{
			session_same_site = tok[i + 1];
			i += (i + 2 < N && tok[i + 2] == ";") ? 3 : 2;
			continue;
		}

		// ================= GLOBAL: upstream NAME { ... }
		if (t0 == "upstream")
		{
			if (i + 2 >= N || tok[i + 2] != "{")
				throw std::runtime_error("upstream NAME { ... }");
			const std::string upname = tok[i + 1];
			i += 3;
			UpstreamPool pool;
			while (i < N && tok[i] != "}")
			{
				const std::string kw = tok[i++];
				// tolerate stray semicolons inside upstream block
				if (kw == ";")
					continue;

				if (kw == "server")
				{
					if (i >= N)
						throw std::runtime_error("upstream server expects host:port");
					const std::string hp = tok[i++];
					std::string::size_type c = hp.find(':');
					if (c == std::string::npos)
						throw std::runtime_error("server expects host:port");
					const std::string host = hp.substr(0, c);
					const std::string pstr = hp.substr(c + 1);
					int port;
					if (!parseUnsigned(pstr, port) || port <= 0 || port > 65535)
						throw std::runtime_error("bad port");
					int weight = 1;
					while (i < N && tok[i] != ";")
					{
						const std::string kv = tok[i++];
						const std::string::size_type eq = kv.find('=');
						if (eq != std::string::npos && kv.substr(0, eq) == "weight")
						{
							int w;
							if (!parseUnsigned(kv.substr(eq + 1), w) || w <= 0)
								throw std::runtime_error("bad weight");
							weight = w;
						}
					}
					if (i >= N || tok[i] != ";")
						throw std::runtime_error("expected ';' after upstream server");
					++i;
					Upstream u;
					u.host = host;
					u.port = port;
					u.weight = weight;
					u.healthy = true;
					pool.nodes.push_back(u);
					continue;
				}
				if (kw == "health_path")
				{
					pool.health_path = expectWord(i, tok);
					if (i < N && tok[i] == ";")
						++i;
					continue;
				}
				if (kw == "health_interval_ms")
				{
					pool.health_interval_ms = expectInt(i, tok, "health_interval_ms");
					if (i < N && tok[i] == ";")
						++i;
					continue;
				}
				if (kw == "strategy")
				{
					pool.strategy = expectWord(i, tok);
					if (i < N && tok[i] == ";")
						++i;
					continue;
				}
				throw std::runtime_error("unknown directive in upstream block: " + kw);
			}
			if (i >= N || tok[i] != "}")
				throw std::runtime_error("missing '}' in upstream");
			++i;
			upstream_pools[upname] = pool;
			continue;
		}

		// ================= SERVER: server { ... }
		if (t0 == "server")
		{
			if (i + 1 >= N || tok[i + 1] != "{")
				throw std::runtime_error("expected '{' after 'server'");
			i += 2;

			VirtualServer vs;
			bool seen_listen = false;

			// inherit global upstream pools by default
			vs.upstreams = upstream_pools;

			while (i < N && tok[i] != "}")
			{
				const std::string kw = tok[i++];

				// tolerate stray semicolons inside server block
				if (kw == ";")
					continue;

				// listen
				if (kw == "listen")
				{
					if (i >= N)
						throw std::runtime_error("listen expects 'PORT' or 'HOST:PORT'");
					std::string a = tok[i++], b;
					if (i < N && tok[i] != ";" && tok[i] != "{" && tok[i] != "}")
					{
						b = tok[i++];
					}
					std::string host;
					int port = -1;
					if (b.empty())
					{
						std::string::size_type cpos = a.find(':');
						if (cpos == std::string::npos)
						{
							char *endp = 0;
							long p = ::strtol(a.c_str(), &endp, 10);
							if (!a.size() || (endp && *endp) || p <= 0 || p > 65535)
								throw std::runtime_error("invalid listen port");
							port = static_cast<int>(p);
							host.clear();
						}
						else
						{
							host = a.substr(0, cpos);
							std::string pstr = a.substr(cpos + 1);
							char *endp = 0;
							long p = ::strtol(pstr.c_str(), &endp, 10);
							if (!pstr.size() || (endp && *endp) || p <= 0 || p > 65535)
								throw std::runtime_error("invalid listen port");
							port = static_cast<int>(p);
						}
					}
					else
					{
						host = a;
						char *endp = 0;
						long p = ::strtol(b.c_str(), &endp, 10);
						if (!b.size() || (endp && *endp) || p <= 0 || p > 65535)
							throw std::runtime_error("invalid listen port");
						port = static_cast<int>(p);
					}
					if (i < N && tok[i] == ";")
						++i;
					vs.listen_host = host;
					vs.listen_port = port;
					seen_listen = true;
					continue;
				}

				if (kw == "server_name")
				{
					while (i < N && tok[i] != ";")
						vs.server_names.push_back(tok[i++]);
					if (i >= N || tok[i] != ";")
						throw std::runtime_error("expected ';'");
					++i;
					continue;
				}
				if (kw == "root")
				{
					if (i >= N)
						throw std::runtime_error("root expects path");
					vs.root = makeAbsolute(tok[i++]);
					if (i >= N || tok[i] != ";")
						throw std::runtime_error("expected ';' after root");
					++i;
					continue;
				}
				if (kw == "index")
				{
					while (i < N && tok[i] != ";")
						vs.index_files.push_back(tok[i++]);
					if (i >= N || tok[i] != ";")
						throw std::runtime_error("expected ';'");
					++i;
					continue;
				}
				if (kw == "error_page")
				{
					if (i + 1 >= N)
						throw std::runtime_error("error_page <status> <path>");
					validateErrorStatusOrThrow(tok[i]);
					int status = ::atoi(tok[i++].c_str());
					const std::string path = tok[i++];
					if (i < N && tok[i] == ";")
						++i;
					vs.error_pages[status] = path;
					continue;
				}
				if (kw == "client_body_temp_path")
				{
					if (i >= N)
						throw std::runtime_error("client_body_temp_path <path>");
					vs.client_body_temp_path = makeAbsolute(tok[i++]); 
					if (i < N && tok[i] == ";")
						++i;
					continue;
				}
				if (kw == "client_max_body_size")
				{
					vs.client_max_body_size = expectInt(i, tok, "client_max_body_size");
					if (i < N && tok[i] == ";")
						++i;
					continue;
				}

				// optional per-server upstream block
				if (kw == "upstream")
				{
					if (i + 1 >= N || tok[i + 1] != "{")
						throw std::runtime_error("upstream NAME { ... }");
					const std::string upname = tok[i++]; // NAME
					++i;                                 // skip "{"
					UpstreamPool pool;
					while (i < N && tok[i] != "}")
					{
						const std::string kw2 = tok[i++];

						// tolerate stray semicolons in per-server upstream block
						if (kw2 == ";")
							continue;

						if (kw2 == "server")
						{
							const std::string hp = tok[i++];
							std::string::size_type c = hp.find(':');
							if (c == std::string::npos)
								throw std::runtime_error("server host:port");
							Upstream u;
							u.host = hp.substr(0, c);
							int prt;
							if (!parseUnsigned(hp.substr(c + 1), prt))
								throw std::runtime_error("bad port");
							u.port = prt;
							u.weight = 1;
							u.healthy = true;
							while (i < N && tok[i] != ";")
							{
								const std::string kv = tok[i++];
								std::string::size_type e = kv.find('=');
								if (e != std::string::npos && kv.substr(0, e) == "weight")
								{
									int w;
									if (!parseUnsigned(kv.substr(e + 1), w) || w <= 0)
										throw std::runtime_error("bad weight");
									u.weight = w;
								}
							}
							if (i >= N || tok[i] != ";")
								throw std::runtime_error("expected ';'");
							++i;
							pool.nodes.push_back(u);
							continue;
						}
						if (kw2 == "health_path")
						{
							pool.health_path = expectWord(i, tok);
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (kw2 == "health_interval_ms")
						{
							pool.health_interval_ms = expectInt(i, tok, "health_interval_ms");
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (kw2 == "strategy")
						{
							pool.strategy = expectWord(i, tok);
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						throw std::runtime_error("unknown directive in upstream block");
					}
					if (i >= N || tok[i] != "}")
						throw std::runtime_error("missing '}' in upstream");
					++i;
					vs.upstreams[upname] = pool;
					continue;
				}

				// -------- location -----------
				if (kw == "location")
				{
					if (i >= N)
						throw std::runtime_error("location expects <path|~ regex> {");
					Location loc;
					if (tok[i] == "~")
					{ // regex form
						++i;
						if (i >= N)
							throw std::runtime_error("location ~ expects a pattern");
						loc.regex = true;
						loc.path_prefix = tok[i++];
					}
					else
					{
						loc.path_prefix = tok[i++];
					}
					if (i >= N || tok[i] != "{")
						throw std::runtime_error("expected '{' after location");
					++i;

					while (i < N && tok[i] != "}")
					{
						const std::string lkw = tok[i++];

						// tolerate stray semicolons inside location block
						if (lkw == ";")
							continue;
						if (lkw == "root")
						{
						loc.root = makeAbsolute(expectWord(i, tok));
							if (i >= N || tok[i] != ";")
								throw std::runtime_error("expected ';' after root");
							++i;
							continue;
						}
						if (lkw == "index")
						{
							while (i < N && tok[i] != ";")
								loc.index_files.push_back(tok[i++]);
							if (i >= N || tok[i] != ";")
								throw std::runtime_error("expected ';'");
							++i;
							continue;
						}
						if (lkw == "autoindex")
						{
							const std::string v = expectWord(i, tok);
							if (v == "on")
								loc.autoindex = true;
							else if (v == "off")
								loc.autoindex = false;
							else
								throw std::runtime_error("autoindex on|off");
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (lkw == "methods")
						{
							while (i < N && tok[i] != ";")
							{
								const std::string m = tok[i++];
								if (!(m == "GET" || m == "POST" || m == "DELETE"))
									throw std::runtime_error("unsupported method");
								loc.allowed_methods.push_back(m);
							}
							if (i >= N || tok[i] != ";")
								throw std::runtime_error("expected ';'");
							++i;
							continue;
						}
						if (lkw == "upload_dir")
						{
							loc.upload_dir = makeAbsolute(expectWord(i, tok)); 
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (lkw == "cgi")
						{
							const std::string ext = expectWord(i, tok);
							std::string bin;
							int to = 0;
							if (i < N && tok[i] != ";" && tok[i] != "}")
							{
								if (!parseUnsigned(tok[i], to))
								{
									bin = tok[i++];
									to = expectInt(i, tok, "cgi timeout");
								}
								else
								{
									to = ::atoi(tok[i++].c_str());
								}
							}
							if (i < N && tok[i] == ";")
								++i;
							loc.cgi_by_ext[ext] = CgiSpec(bin, to);
							continue;
						}
						if (lkw == "proxy_pass")
						{
							loc.is_proxy = true;
							loc.proxy_name = expectWord(i, tok);
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (lkw == "limit_req")
						{
							int reqs = 0, burst = 0;
							bool haveReq = false, haveBurst = false;
							bool enabled = true;
							while (i < N && tok[i] != ";")
							{
								const std::string kv = tok[i++];
								std::string::size_type e = kv.find('=');
								if (e != std::string::npos)
								{
									const std::string k = kv.substr(0, e), v = kv.substr(e + 1);
									int num;
									if (!parseUnsigned(v, num))
										throw std::runtime_error("limit_req expects numbers");
									if (k == "requests")
										reqs = num, haveReq = true;
									else if (k == "burst")
										burst = num, haveBurst = true;
								}
								else if (kv == "on")
									enabled = true;
								else if (kv == "off")
									enabled = false;
							}
							if (i >= N || tok[i] != ";")
								throw std::runtime_error("expected ';'");
							++i;
							loc.rate_limit.requests_per_minute = haveReq ? reqs : 0;
							loc.rate_limit.burst = haveBurst ? burst : 0;
							loc.rate_limit.enabled = enabled;
							continue;
						}
						if (lkw == "allow_put")
						{
							const std::string v = expectWord(i, tok);
							loc.write_conf.allow_put = (v == "on");
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (lkw == "allow_patch")
						{
							const std::string v = expectWord(i, tok);
							loc.write_conf.allow_patch = (v == "on");
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (lkw == "generate_etag")
						{
							const std::string v = expectWord(i, tok);
							loc.write_conf.generate_etag = (v == "on");
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (lkw == "client_max_body_size")
						{
							loc.write_conf.max_body_bytes = expectInt(i, tok, "client_max_body_size");
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (lkw == "try_files")
						{
							while (i < N && tok[i] != ";")
								loc.try_files.push_back(tok[i++]);
							if (i >= N || tok[i] != ";")
								throw std::runtime_error("expected ';'");
							++i;
							continue;
						}
						if (lkw == "return")
						{
							loc.return_status = expectInt(i, tok, "return");
							if (i < N && tok[i] != ";")
								loc.return_target = tok[i++];
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (lkw == "allow")
						{
							loc.allow_list.push_back(expectWord(i, tok));
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}
						if (lkw == "deny")
						{
							loc.deny_list.push_back(expectWord(i, tok));
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}

						if (lkw == "upload_store")
						{
							loc.upload_store = makeAbsolute(expectWord(i, tok));
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}

						if (lkw == "upload_overwrite")
						{
							const std::string v = expectWord(i, tok);
							if (v == "on")
								loc.upload_overwrite = true;
							else if (v == "off")
								loc.upload_overwrite = false;
							else
								throw std::runtime_error("upload_overwrite must be on|off");
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}

						if (lkw == "upload_max_file_size")
						{
							loc.upload_max_file_size = expectInt(i, tok, "upload_max_file_size");
							if (i < N && tok[i] == ";")
								++i;
							continue;
						}


						throw std::runtime_error(std::string("unknown directive in location: ") + lkw);
					}

					if (i >= N || tok[i] != "}")
						throw std::runtime_error("missing '}' to close location block");
					++i;
					vs.locations.push_back(loc);
					continue;
				}

				throw std::runtime_error(std::string("unknown directive in server block: ") + kw);
			}

			if (i >= N || tok[i] != "}")
				throw std::runtime_error("missing '}' to close server block");
			if (!seen_listen || vs.listen_port <= 0)
				throw std::runtime_error("server missing valid listen directive");
			++i;
			_servers.push_back(vs);
			continue;
		}

		// tolerate stray semicolons at top level
		if (t0 == ";")
		{
			++i;
			continue;
		}

		throw std::runtime_error(std::string("unknown top-level directive: ") + t0);
	}
}


/* 

void ServerConfig::checkDuplicateListen_() const

Validates there are no duplicate listen host:port pairs across servers. 
It normalizes empty hosts to 0.0.0.0, inserts pairs into a std::set, 
and throws with a detailed message on duplicates. 
Catching this at parse time prevents undefined binding behavior or surprising server selection at runtime. 
Ensuring unique listeners also simplifies the server’s 
acceptor planning and avoids accidental overshadowing of 
configuration blocks that would otherwise contend for the same socket


*/

void ServerConfig::checkDuplicateListen_() const
{
	std::set<std::pair<std::string, int> > seen;
	for (std::vector<VirtualServer>::size_type i = 0; i < _servers.size(); ++i)
	{
		const VirtualServer &vs = _servers[i];
		const int port = vs.listen_port;
		if (port <= 0 || port > 65535)
			continue;
		const std::string host = vs.listen_host.empty() ? std::string("0.0.0.0") : vs.listen_host;
		const std::pair<std::string, int> key(host, port);
		if (!seen.insert(key).second)
		{
			std::ostringstream oss;
			oss << "duplicate listen directive: " << host << ":" << port;
			throw std::runtime_error(oss.str());
		}
	}
}


/* 

void ServerConfig::parseFile(const std::string& path)

Convenience wrapper: reads the file via readWholeFile then calls parseString. 
Keeping file I/O separate from parsing allows test suites to feed text directly without filesystem dependencies. 
It also keeps error reporting cleaner, with I/O errors surfaced before tokenization. 
After parsing, duplicate listeners are checked by parseString.


*/

void ServerConfig::parseFile(const std::string &path)
{
	const std::string data = readWholeFile(path);
	parseString(data);
}

/* 

void ServerConfig::parseString(const std::string& text)

Top-level entry to parse configuration text already in memory. 
It tokenizes, early-returns with an empty server list if no tokens, 
then calls parseTokens and finally checkDuplicateListen_. 
Splitting responsibilities this way improves unit test coverage and reusability 
(e.g., embedding default configs). It ensures that even dynamically 
generated configuration strings receive the same rigorous syntax and semantic checks as files.


*/


void ServerConfig::parseString(const std::string &text)
{
	const std::vector<std::string> tok = tokenize(text);
	if (tok.empty())
	{
		_servers.clear();
		return;
	}
	parseTokens(tok);
	checkDuplicateListen_();
}
