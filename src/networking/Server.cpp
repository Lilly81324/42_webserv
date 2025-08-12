/* --- Server.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */
/* --- src/networking/Server.cpp --- */

/* --- src/networking/Server.cpp --- */
#include "Server.h"
#include "Logger.h"

#include "EventLoop.h"
#include "ClientConnection.h"

#include <vector>
#include <map>
#include <cerrno>
#include <cstring>
#include <sstream>

#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace { // helpers

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

std::string to_string_port(unsigned short p) {
    std::ostringstream oss; oss << p; return oss.str();
}

int make_listener(const std::string& host, unsigned short port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr; std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty()) addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) { ::close(fd); errno = EINVAL; return -1; }

    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { 
        ::close(fd); 
        return -1; 
    }
    if (set_nonblocking(fd) < 0) { 
        ::close(fd); return -1; 
    }
    if (::listen(fd, 128) < 0) { 
        ::close(fd); return -1; 
    }
    return fd;
}

// One handler for all listeners
class ListenerHandler : public IFdHandler {
public:
    ListenerHandler(EventLoop& loop, Server& server) : _loop(loop), _server(server) {}
    void on_readable(int lfd) {
        while (true) {
            sockaddr_in cli; 
            socklen_t len = sizeof(cli);
            int cfd = ::accept(lfd, (sockaddr*)&cli, &len);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                LOG_ERROR(std::string("accept error: ") + std::strerror(errno));
                break;
            }
            set_nonblocking(cfd);
            ClientConnection* c = new ClientConnection(cfd, _loop, _server);
            _server.registerClient(cfd, c);
            _loop.add(cfd, POLLIN, c);
        }
    }
    void on_writable(int) {}
    void on_error(int) {}

private:
    EventLoop& _loop;
    Server&    _server;
};

} // namespace

// ---- Server ----

Server::Server(const ServerConfig &cfg) : _cfg(cfg) {
    LOG_DEBUG("Server stub constructed.");
}
Server::~Server() {
    // cleanup on destruction just in case
    for (std::map<int, ClientConnection*>::iterator it = _clients.begin(); it != _clients.end(); ++it) {
        ::close(it->first);
        delete it->second;
    }
    _clients.clear();
    for (size_t i = 0; i < _listeners.size(); ++i) ::close(_listeners[i]);
    _listeners.clear();
}

void Server::openListeners() {
    _listeners.clear();
    const std::vector<ServerBlock>& sv = _cfg.servers();
    for (std::vector<ServerBlock>::const_iterator s = sv.begin(); s != sv.end(); ++s) {
        for (std::vector<Listen>::const_iterator L = s->listens.begin(); L != s->listens.end(); ++L) {
            int fd = make_listener(L->host, L->port);
            if (fd < 0) {
                LOG_ERROR(std::string("listen failed on ")
                          + (L->host.empty()? "0.0.0.0" : L->host)
                          + ":" + to_string_port(L->port)
                          + " (" + std::strerror(errno) + ")");
                continue;
            }
            _listeners.push_back(fd);
            LOG_INFO(std::string("listening on ")
                     + (L->host.empty()? "0.0.0.0" : L->host)
                     + ":" + to_string_port(L->port));
        }
    }
}

void Server::registerClient(int fd, ClientConnection* c) {
    _clients[fd] = c;
}

const ServerBlock* Server::firstServer() const {
    const std::vector<ServerBlock>& sv = _cfg.servers();
    return sv.empty() ? 0 : &sv[0];
}


void Server::onClientClosed(int fd) {
    std::map<int, ClientConnection*>::iterator it = _clients.find(fd);
    if (it != _clients.end()) {
        delete it->second;
        _clients.erase(it);
    }
}

void Server::run() {
    openListeners();
    if (_listeners.empty()) {
        LOG_ERROR("No listening sockets created. Exiting.");
        return;
    }

    EventLoop loop;
    ListenerHandler lhandler(loop, *this);

    for (size_t i = 0; i < _listeners.size(); ++i) {
        loop.add(_listeners[i], POLLIN, &lhandler);
    }

    loop.run(1000); // 1s tick (housekeeping spot)
}


