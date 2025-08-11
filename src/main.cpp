#include <string>
#include <iostream>
#include "Server.h"
#include "ServerConfig.h"
#include "Logger.h"
#include "ConfigPrinter.h"

static const char *DEFAULT_CONF = "config/default.conf";

int main (int argc, char **argv){
    std::string confPath = (argc >= 2) ? argv[1] : DEFAULT_CONF;

    LOG_INFO("webserv starting (C++98).");
    LOG_INFO("Using config: " + confPath);

    try{
        ServerConfig cfg;

        if(!cfg.canOpen(confPath.c_str())){
            LOG_ERROR("Cannot open configuration file: " + confPath);
            LOG_ERROR("Tip: pass a path like './webserv config/example.conf'");
            return 1;
        }

        cfg.parseFile(confPath);
        LOG_INFO("Config parsed successfully.");
        printConfigSummary(std::cout, cfg);

        Server server(cfg);
        LOG_INFO("Starting server event loop...");
        server.run();
    

    } catch(const std::exception &e){
        LOG_ERROR(std::string("fatal: ") + e.what());
        return 1;
    }
    return 0;
}

