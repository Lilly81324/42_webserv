/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   main.cpp                                           :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: vvelikov <vvelikov@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/08/08 22:35:26 by vvelikov          #+#    #+#             */
/*   Updated: 2025/08/09 17:31:03 by vvelikov         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#include <string>
#include <iostream>
#include "Server.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include "ConfigPrinter.hpp"

static const char *DEFAULT_CONF = "config/default.conf";

int main (int argc, char **argv){
    std::string confPath = (argc >= 2) ? argv[1] : DEFAULT_CONF;

    LOG_INFO("webserv starting (C++98).");
    LOG_INFO("Using config: " + confPath);

    try{
        Config cfg;

        if(!cfg.canOpen(confPath.c_str())){
            LOG_ERROR("Cannot open configuration file: " + confPath);
            LOG_ERROR("Tip: pass a path like './webserv config/example.conf'");
            return 1;
        }

        cfg.parseFile(confPath);
        LOG_INFO("Config parsed successfully.");
        printConfigSummary(std::cout, cfg); 

        Server server(cfg);
        LOG_INFO("Server constructed (no sockets yet).");
        LOG_INFO("Day 2 complete: parsing + summary output.");
    } catch(const std::exception &e){
        LOG_ERROR(std::string("fatal: ") + e.what());
        return 1;
    }
    return 0;
}