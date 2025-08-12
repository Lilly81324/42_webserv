/* --- ClientConnection.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "ClientConnection.h"
#include "Server.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h> 
#include <sys/stat.h> 
#include <cerrno>
#include <cstring>
#include <sstream>
#include <fstream>
#include <map>
#include <vector>

// ----- small helpers -----
static std::string trim(const std::string &s){
    size_t a = 0, b = s.size();
    while (a < b && (s[a]==' '||s[a]=='\t'||s[a]=='\r'||s[a]=='\n'))
        ++a;
    while (b > a && (s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\r'||s[b-1]=='\n'))
        --b;
    return s.substr(a, b-a);
}

static void to_lower(std::string &s){
    for (size_t i=0;i<s.size();++i) {
        if (s[i]>='A'&&s[i]<='Z')
            s[i]=char(s[i]-'A'+'a');
    }
}

static bool is_dir(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
}

static bool read_file_all(const std::string &path, std::string &out) {
    std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static std::string mime_from_ext(const std::string &path){
    size_t dot = path.find_last_of('.');
    std::string ext = (dot==std::string::npos)?"":path.substr(dot+1);
    to_lower(ext);
    if(ext=="html"||ext=="htm")
        return "text/html";
    if(ext=="txt")
        return "text/plain";
    if(ext=="css")
        return "text/css";
    if(ext=="js")
        return "application/javascript";
    if(ext=="json")
        return "application/json";
    if(ext=="png")
        return "image/png";
    if(ext=="jpg"||ext=="jpeg")
        return "image/jpeg";
    if(ext=="gif")
        return "image/gif";
    if(ext=="svg")
        return "image/svg+xml";
    return "application/octet-stream";
}

// join root + path safely (strip query + collapse ..)
static std::string safe_join(const std::string &root, const std::string &req_path){
    std::string p = req_path.empty()?"/":req_path;
    size_t q = p.find('?'); 
    if(q!=std::string::npos)
        p.erase(q);
    if(p.empty() || p[0]!='/')
        p = "/" + p;
    std::vector<std::string> parts; size_t i=0;
    while(i<p.size()){
        size_t j = p.find('/', i);
        if(j==std::string::npos)
            j=p.size();
        std::string seg = p.substr(i, j-i);
        if(!seg.empty()){
            if(seg==".."){ 
                if(!parts.empty())
                    parts.pop_back();
                }
            else if(seg!=".")
                parts.push_back(seg);
        }
        i = j+1;
    }
    std::ostringstream rel;
    for(size_t k=0;k<parts.size();++k)
        rel<<"/"<<parts[k];
    std::string joined = root;
    if(!joined.empty() && joined[joined.size()-1]=='/')
        joined.erase(joined.size()-1);
    std::string r = rel.str();
    if(r.empty()) r="/";
    return joined + r;
}

// ----- ClientConnection -----
ClientConnection::ClientConnection(int fd, EventLoop& loop, Server& server)
: _fd(fd), _loop(loop), _server(server), _in(), _out(), _keepAlive(false) {}

ClientConnection::~ClientConnection(){}

void ClientConnection::on_readable(int fd) {
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            _in.append(buf, n);

            // Do we have full headers?
            size_t hdr_end = _in.find("\r\n\r\n");
            if (_out.empty() && hdr_end != std::string::npos) {

                // --- Request line ---
                size_t eol = _in.find("\r\n");
                if (eol == std::string::npos) {
                    close_and_cleanup();
                    break; }
                std::string reqline = _in.substr(0, eol);
                std::istringstream rl(reqline);
                std::string method, target, version;
                rl >> method >> target >> version;

                // --- Headers (minimal, case-insensitive names) ---
                std::map<std::string, std::string> hdrs;
                size_t p = eol + 2;
                while (p < hdr_end) {
                    size_t nl = _in.find("\r\n", p);
                    if (nl == std::string::npos || nl == p) break;
                    std::string line = _in.substr(p, nl - p);
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string name = trim(line.substr(0, colon));
                        std::string val  = trim(line.substr(colon + 1));
                        to_lower(name);
                        hdrs[name] = val;
                    }
                    p = nl + 2;
                }

                // --- Keep-alive decision ---
                _keepAlive = false;
                if (version == "HTTP/1.1") {
                    std::map<std::string,std::string>::iterator it = hdrs.find("connection");
                    _keepAlive = (it == hdrs.end()) || (trim(it->second) != "close");
                } else if (version == "HTTP/1.0") {
                    std::map<std::string,std::string>::iterator it = hdrs.find("connection");
                    _keepAlive = (it != hdrs.end() && trim(it->second) == "keep-alive");
                }

                // --- Method handling + static files ---
                bool headOnly = false;
                int status = 200;
                std::string body;
                std::string ctype = "text/plain";

                if (method == "HEAD") {
                    headOnly = true;
                }

                if (method != "GET" && method != "HEAD") {
                    status = 405;
                    body = "Method Not Allowed\n";
                    ctype = "text/plain";
                } else {
                    const ServerBlock* sb = _server.firstServer();
                    std::string root = sb ? sb->root : std::string("./www");
                    std::vector<std::string> index = sb ? sb->index : std::vector<std::string>();
                    if (index.empty()) index.push_back("index.html");

                    std::string full = safe_join(root, target);
                    if (is_dir(full)) {
                        bool served = false;
                        for (size_t i = 0; i < index.size(); ++i) {
                            std::string cand = full;
                            if (!cand.empty() && cand[cand.size() - 1] != '/') cand += "/";
                            cand += index[i];
                            if (read_file_all(cand, body)) {
                                ctype = mime_from_ext(cand);
                                served = true;
                                break;
                            }
                        }
                        if (!served) {
                            status = 403;
                            body = "Forbidden\n";
                            ctype = "text/plain";
                        }
                    } else {
                        if (!read_file_all(full, body)) {
                            status = 404;
                            body = "Not Found\n";
                            ctype = "text/plain";
                        } else {
                            ctype = mime_from_ext(full);
                        }
                    }
                }

                // --- Build response ---
                const char* status_text =
                    (status == 200) ? "200 OK" :
                    (status == 403) ? "403 Forbidden" :
                    (status == 404) ? "404 Not Found" :
                    (status == 405) ? "405 Method Not Allowed" :
                                      "500 Internal Server Error";

                const size_t content_len = body.size();

                std::ostringstream oss;
                oss << "HTTP/1.1 " << status_text << "\r\n"
                    << "Content-Length: " << content_len << "\r\n"
                    << "Content-Type: " << ctype << "\r\n"
                    << "Connection: " << (_keepAlive ? "keep-alive" : "close") << "\r\n";
                if (status == 405) {
                    // Allowed methods for now
                    oss << "Allow: GET, HEAD\r\n";
                }
                oss << "\r\n";
                if (!headOnly) {
                    oss << body; // HEAD sends headers only
                }

                _out = oss.str();

                // Drop processed headers (we ignore request body at this stage)
                _in.erase(0, hdr_end + 4);

                // Write phase
                _loop.mod(_fd, POLLOUT);
            }
            continue; // keep draining recv() until EAGAIN
        } else if (n == 0) {
            // client closed
            close_and_cleanup();
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // recv error
            close_and_cleanup();
            break;
        }
    }
}


void ClientConnection::on_writable(int fd){
    if(_out.empty()){ _loop.mod(_fd, POLLIN); return; }
    ssize_t n = ::send(fd, _out.data(), _out.size(), 0);
    if(n<0){
        if(errno==EAGAIN || errno==EWOULDBLOCK) return;
        close_and_cleanup();
    }else{
        _out.erase(0, static_cast<size_t>(n));
        if(_out.empty()){
            if(_keepAlive){
                _loop.mod(_fd, POLLIN);
                if(_in.size()>65536) _in.clear();
            }else{
                close_and_cleanup();
            }
        }
    }
}

void ClientConnection::on_error(int){ close_and_cleanup(); }

void ClientConnection::close_and_cleanup(){
    _loop.remove(_fd);
    ::close(_fd);
    _server.onClientClosed(_fd);
}
