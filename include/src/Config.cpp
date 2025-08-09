/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   Config.cpp                                         :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: vvelikov <vvelikov@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/08/08 22:03:07 by vvelikov          #+#    #+#             */
/*   Updated: 2025/08/09 17:33:05 by vvelikov         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#include "Config.hpp"
#include <fstream>
#include <cctype>
#include <sstream>
#include <fstream>



std::vector<Token> Lexer::lex(const std::string& src){
	std::vector<Token> out;
	size_t i = 0;
	size_t n = src.size();
	size_t line = 1;
	size_t col = 1;

	while(i < n){
		char c = src[i];

	/* ---- Coments: '#' to end of line ----*/
		if(c == '#'){
			while (i < n && src[i] != '\n'){
				++i;
				++col;
			}
			continue;
		}

		/* ---- Whitespace ----*/

		if (c == ' ' || c == '\t' || c == '\r'){
			++i;
			++col;
			continue;
		}

		if (c == '\n') {
			++i;
			++line;
			col = 1;
			continue;
		}

		/* ---- Single char symbols ----*/
		
		if(c == '{' || c == '}' || c == ';'){
			Token t;
			t.text = std::string(1, c);
			t.line = line;
			t.col = col;
			out.push_back(t);
			++i;
			++col;
			continue;
		}

		/* ---- Word token ----*/
		size_t start = i;
		size_t startCol = col;
		while(i < n){
			char ch = src[i];
			if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'
    			|| ch == '{' || ch == '}' || ch == ';' || ch == '#')
   				 break;
			++i;
			++col;
		}
		if (i > start) {
            Token t; t.text = src.substr(start, i - start); t.line = line; t.col = startCol;
            out.push_back(t);
        }
	}
	return out;
}

static std::string readWholeFile(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
    if (!f) throw std::runtime_error("cannot open: " + path);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}


Config::Config(){}
Config::~Config(){}


bool Config::canOpen(const char *path) const{
	std::ifstream f(path);
	return f.good();
}

void Config::parseFile(const std::string& path) {
    std::string src = readWholeFile(path);
    Lexer lx;
    std::vector<Token> toks = lx.lex(src);
    (void)toks;

    _servers.clear();
}

const std::vector<ServerBlock>& Config::servers() const {
    return _servers;
}