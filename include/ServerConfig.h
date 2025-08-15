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


#include "VirtualServer.h"
#include <vector>
class ServerConfig
{
public:
	std::vector<VirtualServer> servers;
	ServerConfig();
	~ServerConfig();

private:
};

#endif // SERVERCONFIG_H
