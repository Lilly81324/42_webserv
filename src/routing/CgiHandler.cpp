/* --- CgiHandler.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "CgiHandler.h"
#include "CgiRegistry.h"
#include "RequestContext.h"
#include "ServerConfig.h"
#include "VirtualServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Headers.h"
#include "HEADER_ENTRIES.h"

#include <sstream>
#include <vector>
#include <string>

// Small helpers
static std::string hostWithoutPort(const std::string& hostHdr) {
    if (hostHdr.empty()) return hostHdr;
    if (hostHdr[0] == '[') { // IPv6 [::1]:8080
        std::string::size_type rb = hostHdr.find(']');
        return (rb != std::string::npos) ? hostHdr.substr(0, rb + 1) : hostHdr;
    }
    std::string::size_type c = hostHdr.find(':');
    return (c == std::string::npos) ? hostHdr : hostHdr.substr(0, c);
}

static std::string joinFs(const std::string& root, const std::string& reqPath) {
    if (root.empty()) return reqPath;
    if (reqPath.empty()) return root;
    const bool rootSlash = !root.empty() && root[root.size()-1] == '/';
    const bool pathSlash = !reqPath.empty() && reqPath[0] == '/';
    if (rootSlash && pathSlash) return root + reqPath.substr(1);
    if (rootSlash || pathSlash) return root + reqPath;
    return root + "/" + reqPath;
}

CgiHandler::CgiHandler(): Handler() {}
CgiHandler::~CgiHandler() {}

bool CgiHandler::handle(HttpRequest &req, HttpResponse &res, RequestContext &ctx)
{
    // 1) Choose spec from location -> global defaults
    CgiRegistry reg;
    const std::map<std::string, CgiSpec>* locMap = (ctx.loc ? &ctx.loc->cgi_by_ext : 0);
    const std::map<std::string, CgiSpec>* defMap = (ctx.cfg ? &ctx.cfg->cgi_defaults : 0);
    reg.setSources(locMap, defMap);

    const CgiSpec* spec = reg.findByExtension(ctx.cgi_ext);
    if (!spec) {
        // No mapping for this extension: not handled here.
        (void)res;
        return false;
    }

    // 2) Build env
    std::vector<std::string> envv;
    buildEnv(req, *ctx.vs, envv);

    // 3) (Next step) pipe()/fork()/dup2()/execve(spec->bin, ...) with envv
    // For now return false so caller can map to 500/501 until exec is wired.
    (void)spec;
    return false;
}

int CgiHandler::buildEnv(const HttpRequest& req,
                         const VirtualServer& vs,
                         std::vector<std::string>& envv) const
{
    envv.clear();

    const Headers& H = req.getHeaders();

    // SERVER_NAME: Host header -> VS server_names[0] -> listen_host -> localhost
    std::string server_name = H.get(HDR_HOST);
    if (!server_name.empty()) server_name = hostWithoutPort(server_name);
    else if (!vs.server_names.empty()) server_name = vs.server_names[0];
    else if (!vs.listen_host.empty()) server_name = vs.listen_host;
    else server_name = "localhost";

    // SERVER_PORT
    std::ostringstream port_ss; port_ss << vs.listen_port;

    // CONTENT_LENGTH and CONTENT_TYPE
    std::string ctype = H.get(HDR_CONTENT_TYPE);
    std::string clen  = H.get(HDR_CONTENT_LENGTH);
    if (clen.empty() && req.getBodyLength() > 0) {
        std::ostringstream cl; cl << req.getBodyLength();
        clen = cl.str();
    }

    // REMOTE_ADDR (best effort: X-Forwarded-For if present; else blank for now)
    std::string remote = H.get(HDR_X_FORWARDED_FOR);

    // Required CGI env
    envv.push_back("GATEWAY_INTERFACE=CGI/1.1");
    envv.push_back(std::string("REQUEST_METHOD=")   + req.getMethod());
    envv.push_back(std::string("SCRIPT_NAME=")      + req.getPath());
    envv.push_back(std::string("QUERY_STRING=")     + req.getQuery());
    envv.push_back(std::string("SERVER_PROTOCOL=")  + req.getHttpVer());
    envv.push_back(std::string("SERVER_NAME=")      + server_name);
    envv.push_back(std::string("SERVER_PORT=")      + port_ss.str());
    if (!ctype.empty()) envv.push_back(std::string("CONTENT_TYPE=")   + ctype);
    if (!clen.empty())  envv.push_back(std::string("CONTENT_LENGTH=") + clen);
    envv.push_back(std::string("REMOTE_ADDR=")      + remote);

    // Common extras
    envv.push_back(std::string("DOCUMENT_ROOT=") + vs.root);
    envv.push_back(std::string("SCRIPT_FILENAME=") + joinFs(vs.root, req.getPath()));
    envv.push_back("REDIRECT_STATUS=200"); // php-cgi quirk

    return static_cast<int>(envv.size());
}


