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

struct Listen{
	std::string host;
	unsigned short port;
};

struct Location{
	std::string path;
	std::string root;
	std::vector<std::string> index;
	bool autoindex;
	std::set<std::string> methods;
	std::string upload_store;
	std::map<std::string, std::string> cgi;
	int redirect_code;
	std::string redirect_target;

	Location() : autoindex(false), redirect_code(0){}
};

struct ServerBlock {
	std::vector<Listen> listens;
	std::string root;
	std::vector<std::string> index;
	std::map<int, std::string> error_pages;
	size_t client_max_body_size;
	std::vector<Location> locations;

	ServerBlock() : client_max_body_size(0){}
};

/* ============= Tokenizer Declarations ============ */

class Token{
	public:
		std::string text;
		size_t line;
		size_t col;
};

class Lexer{
	public:
		std::vector<Token> lex(const std::string& src);
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
