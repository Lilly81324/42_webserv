/* --- Server.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "Server.h"
#include "Logger.h"

#include <vector>
#include <algorithm>  
#include <cerrno>
#include <cstring>
#include <sstream> 

#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>

// -------- helpers --------
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

static std::string to_string_port(unsigned short p) {
    std::ostringstream oss;
    oss << p;
    return oss.str();
}

static int make_listener(const std::string& host, unsigned short port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            ::close(fd);
            errno = EINVAL;
            return -1;
        }
    }
    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0)   { ::close(fd); return -1; }
    if (set_nonblocking(fd) < 0)                          { ::close(fd); return -1; }
    if (::listen(fd, 128) < 0)                            { ::close(fd); return -1; }
    return fd;
}

// -------- Server --------
Server::Server(const ServerConfig &cfg) : _cfg(cfg) {
    LOG_DEBUG("Server stub constructed.");
}

Server::~Server() {}

void Server::run() {
   
    std::vector<int> listen_fds;
    const std::vector<ServerBlock>& sv = _cfg.servers();

    for (std::vector<ServerBlock>::const_iterator s = sv.begin(); s != sv.end(); ++s) {
        for (std::vector<Listen>::const_iterator L = s->listens.begin(); L != s->listens.end(); ++L) {
            int fd = make_listener(L->host, L->port);
            if (fd < 0) {
                LOG_ERROR(std::string("listen failed on ")
                          + (L->host.empty() ? std::string("0.0.0.0") : L->host)
                          + ":" + to_string_port(L->port)
                          + " (" + std::strerror(errno) + ")");
                continue;
            }
            listen_fds.push_back(fd);
            LOG_INFO(std::string("listening on ")
                     + (L->host.empty() ? std::string("0.0.0.0") : L->host)
                     + ":" + to_string_port(L->port));
        }
    }

    if (listen_fds.empty()) {
        LOG_ERROR("No listening sockets created. Exiting.");
        return;
    }

    std::vector<struct pollfd> pfds;
    for (size_t i = 0; i < listen_fds.size(); ++i) {
        struct pollfd p;
        p.fd = listen_fds[i];
        p.events = POLLIN;
        p.revents = 0;
        pfds.push_back(p);
    }

    while (true) {
        int rc = ::poll(&pfds[0], pfds.size(), 1000);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR(std::string("poll error: ") + std::strerror(errno));
            break;
        }
        if (rc == 0) {
            continue;
        }

        for (size_t i = 0; i < pfds.size(); ++i) {
            if ((pfds[i].revents & POLLIN) &&
                std::find(listen_fds.begin(), listen_fds.end(), pfds[i].fd) != listen_fds.end()) {
                while (true) {
                    sockaddr_in cli;
                    socklen_t len = sizeof(cli);
                    int cfd = ::accept(pfds[i].fd, (sockaddr*)&cli, &len);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        LOG_ERROR(std::string("accept error: ") + std::strerror(errno));
                        break;
                    }
                    set_nonblocking(cfd);
                    ::close(cfd);
                }
            }
        }
    }

    for (size_t i = 0; i < listen_fds.size(); ++i)
        ::close(listen_fds[i]);
}