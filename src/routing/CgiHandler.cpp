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
#include "ClientConnection.h" 

#include <sstream>
#include <vector>
#include <string>


#include <sstream>
#include <cstdlib>

// keep it TU-local
// Returns true once headers are fully parsed (buf becomes just the body)
bool parseCgiHeaders(std::string& buf, HttpResponse& res, int& status, long& content_len) {
    std::string::size_type p = buf.find("\r\n\r\n");
    if (p == std::string::npos) return false;

    std::string head = buf.substr(0, p);
    std::string body = buf.substr(p + 4);
    content_len = -1;
    status = 200;

    std::istringstream is(head);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);

        std::string::size_type c = line.find(':');
        if (c == std::string::npos) continue;

        std::string k = line.substr(0, c);
        std::string v = line.substr(c + 1);

        // trim leading spaces
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t'))
            v.erase(0, 1);

        if (k == "Status") {
            int s = std::atoi(v.c_str());
            if (s >= 100 && s <= 599) status = s;
        } else if (k == "Content-Length") {
            long L = std::atol(v.c_str());
            if (L >= 0) content_len = L;
            (void)res.headers.set("Content-Length", v);
        } else {
            (void)res.headers.set(k, v);
        }
    }

    // hand remaining body back to caller
    buf.swap(body);
    return true;
}


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

static std::string joinFs(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const bool aSlash = a[a.size()-1] == '/';
    const bool bSlash = b[0] == '/';
    if (aSlash && bSlash) return a + b.substr(1);
    if (!aSlash && !bSlash) return a + "/" + b;
    return a + b;
}

CgiHandler::CgiHandler(): Handler() {}
CgiHandler::~CgiHandler() {}

bool CgiHandler::handle(HttpRequest &req, HttpResponse &res, RequestContext &ctx)
{
     (void)res;
    // 0) Resolve CGI spec (location overrides global)
    CgiRegistry reg;
    const std::map<std::string, CgiSpec>* locMap = ctx.loc ? &ctx.loc->cgi_by_ext : 0;
    const std::map<std::string, CgiSpec>* defMap = ctx.cfg ? &ctx.cfg->cgi_defaults : 0;
    reg.setSources(locMap, defMap);

    const CgiSpec* spec = reg.findByExtension(ctx.cgi_ext);
    if (!spec) {
        // not for us – let Router try other handlers
        return false;
    }

    // 1) Build env
    std::vector<std::string> envv;
    buildEnv(req, *ctx.vs, envv);

    // 2) Compute script path (prefer location root, else server root)
    const std::string& root = (ctx.loc && !ctx.loc->root.empty()) ? ctx.loc->root : ctx.vs->root;
    std::string script_path = joinFs(root, req.getPath());

    
    if (!ctx.connection) return false;
    if (!ctx.connection->beginCgi(*spec, script_path, envv)) {
        return true; // it already wrote a 500 to outBuffer
    }
    return true; // async: event loop will complete response


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


