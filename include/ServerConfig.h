/* --- ServerConfig.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef SERVERCONFIG_H
#define SERVERCONFIG_H

#include <string>
#include <vector>
#include <map>

#include "VirtualServer.h"   // brings in VirtualServer and CgiSpec

class ServerConfig
{
public:
    ServerConfig();
    ~ServerConfig();

    bool canOpen(const char *path) const;
    void parseFile(const std::string &path);

    const std::vector<VirtualServer>& servers() const { return _servers; }
    void push_back(const VirtualServer &vs);

private:
    static std::string                 readWholeFile(const std::string &path);
    static std::vector<std::string>    tokenize(const std::string &data);
    void                               parseTokens(const std::vector<std::string> &tok);
    void                               checkDuplicateListen_() const;

private:
    // Core data used by the runtime / tests
    std::vector<VirtualServer> _servers;

public:
    // Extra fields to align with your diagram (not required by tests yet)
    bool                        session_enabled;
    std::string                 session_cookie_name;
    int                         session_max_age;
    bool                        session_secure;
    bool                        session_http_only;
    std::string                 session_same_site;
    std::map<std::string,std::string> mime_mapping;
    std::map<std::string,CgiSpec>     cgi_defaults;
};

#endif // SERVERCONFIG_H

