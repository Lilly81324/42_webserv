/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   ConfigPrinter.cpp                                  :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: vvelikov <vvelikov@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/08/09 17:13:51 by vvelikov          #+#    #+#             */
/*   Updated: 2025/08/09 17:29:37 by vvelikov         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#include "ConfigPrinter.hpp"
#include <iostream>
#include <vector>
#include <set>
#include <map>
#include <string>


namespace {

	void printStringList(std::ostream& os, const char* label,
							const std::vector<std::string>& v,
							const std::string& indent){
			if (v.empty())
				return;
			os << indent << label;
			for (std::vector<std::string>::const_iterator it = v.begin(); it != v.end(); ++it)
					os << " " << *it;
			os << "\n";	
		}

	void printListenList(std::ostream& os, const std::vector<Listen>& listens, const std::string& indent){
		os << indent << "listen:";
		for(std::vector<Listen>::const_iterator it = listens.begin(); it != listens.end(); ++it){
			os << " " << (it->host.empty() ? "0.0.0.0" : it->host) << ":" << it->port;
		}
		os << "\n";
	}

	void printErrorPages(std::ostream& os, const std::map<int,std::string>& eps,
                     const std::string& indent) {
    if (eps.empty()) return;
    os << indent << "error_pages:\n";
    for (std::map<int,std::string>::const_iterator it = eps.begin(); it != eps.end(); ++it) {
        os << indent << "  " << it->first << " -> " << it->second << "\n";
    }
}

void printMethods(std::ostream& os, const std::set<std::string>& methods,
                  const std::string& indent) {
    if (methods.empty()) return;
    os << indent << "methods:";
    for (std::set<std::string>::const_iterator it = methods.begin(); it != methods.end(); ++it)
        os << " " << *it;
    os << "\n";
}

void printCgiMap(std::ostream& os, const std::map<std::string,std::string>& cgi,
                 const std::string& indent) {
    if (cgi.empty()) return;
    os << indent << "cgi:\n";
    for (std::map<std::string,std::string>::const_iterator it = cgi.begin(); it != cgi.end(); ++it)
        os << indent << "  " << it->first << " -> " << it->second << "\n";
}

void printLocation(std::ostream& os, const Location& L, const std::string& indent) {
    os << indent << "location " << L.path << " {\n";
    if (!L.root.empty())
        os << indent << "  root: " << L.root << "\n";

    printStringList(os, "index:", L.index, indent + "  ");
    os << indent << "  autoindex: " << (L.autoindex ? "on" : "off") << "\n";
    printMethods(os, L.methods, indent + "  ");

    if (!L.upload_store.empty())
        os << indent << "  upload_store: " << L.upload_store << "\n";

    printCgiMap(os, L.cgi, indent + "  ");

    if (L.redirect_code != 0)
        os << indent << "  return: " << L.redirect_code << " " << L.redirect_target << "\n";

    os << indent << "}\n";
}

void printServerBlock(std::ostream& os, const ServerBlock& s) {
    os << "------ server ------\n";
    printListenList(os, s.listens, "");
    os << "root: " << s.root << "\n";
    printStringList(os, "index:", s.index, "");
    printErrorPages(os, s.error_pages, "");
    os << "client_max_body_size: " << s.client_max_body_size << " bytes\n";

    for (std::vector<Location>::const_iterator it = s.locations.begin(); it != s.locations.end(); ++it)
        printLocation(os, *it, "  ");
	}
	
}

void printConfigSummary(std::ostream& os, const Config& cfg) {
    const std::vector<ServerBlock>& sv = cfg.servers();
    os << "[INFO] Parsed servers: " << sv.size() << "\n";
    for (std::vector<ServerBlock>::const_iterator it = sv.begin(); it != sv.end(); ++it)
        printServerBlock(os, *it);
}