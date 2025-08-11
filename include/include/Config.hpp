/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   Config.hpp                                         :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: vvelikov <vvelikov@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/08/08 22:07:33 by vvelikov          #+#    #+#             */
/*   Updated: 2025/08/09 16:21:59 by vvelikov         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef CONFIG_HPP
#define CONFIG_HPP

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


/* ============= Config API ============ */

class Config{
	public:
		Config();
		~Config();
		bool canOpen(const char *path) const;
		void parseFile(const std::string& path);
		const std::vector<ServerBlock>& servers() const;
	private:
		std::vector<ServerBlock> _servers;
};


#endif