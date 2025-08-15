/* --- ServerConfig.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef SERVERCONFIG_H
#define SERVERCONFIG_H

#include <string>
#include <vector>
#include <set>
#include <map>

#include "VirtualServer.h"


/* ============= Tokenizer Declarations ============ */

class Token
{
public:
	std::string text;
	size_t line;
	size_t col;
};

class Lexer
{
public:
	std::vector<Token> lex(const std::string &src);
};

class ServerConfig
{
public:
	ServerConfig();
	~ServerConfig();
	bool canOpen(const char *path) const;
	void parseFile(const std::string &path);
	const std::vector<VirtualServer> &servers() const;

private:
	std::vector<VirtualServer> _servers;
	SessionConfig session;
	MimeConfig mime;
	CgiDefaultsConfig cgi;
};

#endif // SERVERCONFIG_H
