/* --- ServerConfig.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "ServerConfig.h"
#include <fstream>
#include <cctype>
#include <sstream>
#include <fstream>

// ---------------- Lexer (yours, kept) ----------------
std::vector<Token> Lexer::lex(const std::string &src)
{
	std::vector<Token> out;
	size_t i = 0, n = src.size();
	size_t line = 1, col = 1;

	while (i < n)
	{
		char c = src[i];

		// Comments
		if (c == '#')
		{
			while (i < n && src[i] != '\n')
			{
				++i;
				++col;
			}
			continue;
		}
		// Whitespace
		if (c == ' ' || c == '\t' || c == '\r')
		{
			++i;
			++col;
			continue;
		}
		if (c == '\n')
		{
			++i;
			++line;
			col = 1;
			continue;
		}

		// Single-char symbols
		if (c == '{' || c == '}' || c == ';')
		{
			Token t;
			t.text = std::string(1, c);
			t.line = line;
			t.col = col;
			out.push_back(t);
			++i;
			++col;
			continue;
		}

		// Word token
		size_t start = i, startCol = col;
		while (i < n)
		{
			char ch = src[i];
			if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '{' || ch == '}' || ch == ';' || ch == '#')
				break;
			++i;
			++col;
		}
		if (i > start)
		{
			Token t;
			t.text = src.substr(start, i - start);
			t.line = line;
			t.col = startCol;
			out.push_back(t);
		}
	}
	return out;
}

static std::string readWholeFile(const std::string &path)
{
	std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
	if (!f)
		throw std::runtime_error("cannot open: " + path);
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static std::string upper(const std::string &s)
{
	std::string r(s);
	for (size_t i = 0; i < r.size(); ++i)
		r[i] = static_cast<char>(std::toupper(r[i]));
	return r;
}

// static size_t parseSizeBytes(const std::string &s)
// {
// 	if (s.empty())
// 		throw std::runtime_error("empty size");
// 	std::string num;
// 	char suf = 0;
// 	for (size_t i = 0; i < s.size(); ++i)
// 	{
// 		if (std::isdigit(s[i]))
// 			num += s[i];
// 		else
// 		{
// 			suf = static_cast<char>(std::tolower(s[i]));
// 			if (i != s.size() - 1)
// 				throw std::runtime_error("invalid size '" + s + "'");
// 		}
// 	}
// 	if (num.empty())
// 		throw std::runtime_error("invalid size '" + s + "'");
// 	std::istringstream is(num);
// 	unsigned long base = 0;
// 	is >> base;
// 	if (!is)
// 		throw std::runtime_error("invalid number '" + s + "'");
// 	if (suf == 'k')
// 		base *= 1024ul;
// 	else if (suf == 'm')
// 		base *= 1024ul * 1024ul;
// 	else if (suf == 'b' || suf == 0)
// 	{ /* bytes */
// 	}
// 	else
// 		throw std::runtime_error("invalid size suffix in '" + s + "'");
// 	return static_cast<size_t>(base);
// }

static std::runtime_error parseErr(const Token &t, const std::string &msg)
{
	std::ostringstream os;
	os << msg << " at line " << t.line << ", col " << t.col;
	return std::runtime_error(os.str());
}

// ---------------- Parser ----------------

namespace
{

	class Parser
	{
	public:
		Parser(const std::vector<Token> &toks, std::vector<VirtualServer> &out)
			: t(toks), i(0), outServers(out) {}

		void parse()
		{
			outServers.clear();
			while (!atEnd())
				outServers.push_back(parseVirtualServer());
			if (outServers.empty())
				throw std::runtime_error("no server blocks found");
			// No validateServer for now (fields may differ)
		}

	private:
		const std::vector<Token> &t;
		size_t i;
		std::vector<VirtualServer> &outServers;

		bool atEnd() const
		{
			return i >= t.size();
		}
		const Token &cur() const
		{
			if (atEnd())
				return t[t.size() - 1];
			return t[i];
		}
		bool is(const char *s) const
		{
			return !atEnd() && t[i].text == s;
		}

		bool match(const char *s)
		{
			if (is(s))
			{
				++i;
				return true;
			}
			return false;
		}
		const Token &expect(const char *s, const char *what)
		{
			if (!match(s))
				throw parseErr(cur(), std::string("expected '") + s + "' for " + what);
			return t[i - 1];
		}
		const Token &takeWord(const char *what)
		{
			if (atEnd())
				throw std::runtime_error(std::string("unexpected EOF while reading ") + what);
			const Token &k = t[i];
			if (k.text == "{" || k.text == "}" || k.text == ";")
				throw parseErr(k, std::string("unexpected '") + k.text + "' while reading " + what);
			++i;
			return k;
		}

		VirtualServer parseVirtualServer()
		{
			const Token &k = cur();
			if (k.text != "server")
				throw parseErr(k, "expected 'server'");
			++i;
			expect("{", "server block");
			VirtualServer vs;

			while (!atEnd() && !is("}"))
			{
				if (is("location"))
				{
					vs.locations.push_back(parseLocationBlock());
				}
				else
				{
					parseServerDirective(vs);
				}
			}
			expect("}", "end of server block");
			return vs;
		}

		void parseServerDirective(VirtualServer &vs)
		{
			const Token &k = cur();
			if (k.text == "listen")
			{
				++i;
				const Token &v = takeWord("listen value");
				// Only support host:port or port for now
				std::string host, portstr;
				size_t pos = v.text.find(':');
				if (pos == std::string::npos)
				{
					portstr = v.text;
				}
				else
				{
					host = v.text.substr(0, pos);
					portstr = v.text.substr(pos + 1);
				}
				if (portstr.empty())
				{
					std::ostringstream oss;
					oss << "missing port in listen '" << v.text << "'";
					throw parseErr(v, oss.str());
				}
				std::istringstream is(portstr);
				int p = 0;
				is >> p;
				if (!is || p < 1 || p > 65535)
				{
					std::ostringstream oss;
					oss << "invalid port in listen '" << v.text << "'";
					throw parseErr(v, oss.str());
				}
				vs.listen_host = host;
				vs.listen_port = p;
				expect(";", "listen");
			}
			else if (k.text == "root")
			{
				++i;
				const Token &p = takeWord("root path");
				vs.root = p.text;
				expect(";", "root");
			}
			else if (k.text == "index")
			{
				++i;
				while (!is(";"))
				{
					const Token &f = takeWord("index filename");
					vs.index_files.push_back(f.text);
				}
				expect(";", "index");
			}
			else if (k.text == "error_page")
			{
				++i;
				const Token &codeTok = takeWord("error code");
				std::istringstream is(codeTok.text);
				int code = 0;
				is >> code;
				if (!is || code < 100 || code > 599)
					throw parseErr(codeTok, "invalid error code");
				const Token &pathTok = takeWord("error page path");
				vs.error_pages[code] = pathTok.text;
				expect(";", "error_page");
			}
			else
			{
				throw parseErr(k, std::string("unknown server directive '") + k.text + "'");
			}
		}

		Location parseLocationBlock()
		{
			const Token &kw = expect("location", "location");
			(void)kw;
			const Token &pathTok = takeWord("location path");
			expect("{", "location block");
			Location loc;
			loc.path_prefix = pathTok.text;

			while (!atEnd() && !is("}"))
			{
				parseLocationDirective(loc);
			}
			expect("}", "end of location block");
			// allowed_methods default: GET, POST, DELETE
			if (loc.allowed_methods.empty())
			{
				loc.allowed_methods.push_back("GET");
				loc.allowed_methods.push_back("POST");
				loc.allowed_methods.push_back("DELETE");
			}
			return loc;
		}

		void parseLocationDirective(Location &loc)
		{
			const Token &k = cur();
			if (k.text == "methods")
			{
				++i;
				while (!is(";"))
				{
					const Token &m = takeWord("HTTP method");
					std::string M = upper(m.text);
					if (M != "GET" && M != "POST" && M != "DELETE")
						throw parseErr(m, "unsupported method '" + m.text + "'");
					loc.allowed_methods.push_back(M);
				}
				expect(";", "methods");
			}
			else if (k.text == "root")
			{
				++i;
				const Token &p = takeWord("root path");
				loc.root = p.text;
				expect(";", "root");
			}
			else if (k.text == "index")
			{
				++i;
				while (!is(";"))
				{
					const Token &f = takeWord("index filename");
					loc.index_files.push_back(f.text);
				}
				expect(";", "index");
			}
			else if (k.text == "autoindex")
			{
				++i;
				const Token &v = takeWord("autoindex value");
				std::string val = upper(v.text);
				if (val == "ON")
					loc.autoindex = true;
				else if (val == "OFF")
					loc.autoindex = false;
				else
					throw parseErr(v, "autoindex expects 'on' or 'off'");
				expect(";", "autoindex");
			}
			else if (k.text == "upload_store")
			{
				++i;
				const Token &p = takeWord("upload_store path");
				loc.upload_dir = p.text;
				expect(";", "upload_store");
			}
			else
			{
				throw parseErr(k, std::string("unknown location directive '") + k.text + "'");
			}
		}
	};

}

// ---------------- ServerConfig API ----------------

ServerConfig::ServerConfig() {}
ServerConfig::~ServerConfig() {}

bool ServerConfig::canOpen(const char *path) const
{
	std::ifstream f(path);
	return f.good();
}

void ServerConfig::parseFile(const std::string &path)
{
	std::string src = readWholeFile(path);
	Lexer lx;
	std::vector<Token> toks = lx.lex(src);
	Parser p(toks, _servers);
	p.parse();
}

const std::vector<VirtualServer> &ServerConfig::servers() const
{
	return _servers;
}

void ServerConfig::push_back(VirtualServer vs)
{
	this->_servers.push_back(vs);
}