#include "ProxyHandler.h"
#include "ResponseFactory.h"
#include "RequestContext.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "ClientConnection.h"  

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

static int make_nonblock(int fd) {
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0) return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int connect_nonblock(const std::string& host, const std::string& port) {
	struct addrinfo hints; std::memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	struct addrinfo *ai = 0;
	int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &ai);
	if (rc != 0) return -1;

	int fd = -1;
	for (struct addrinfo* p = ai; p; p = p->ai_next) {
		fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0) continue;
		if (make_nonblock(fd) < 0) { ::close(fd); fd = -1; continue; }

		if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
			break; // connected immediately
		}
		if (errno == EINPROGRESS) {
			break; // expected for nonblocking
		}
		::close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);
	return fd;
}

ProxyHandler::ProxyHandler() {}
ProxyHandler::~ProxyHandler() {}

bool ProxyHandler::handle(HttpRequest& req, HttpResponse& res, RequestContext& ctx)
{
    // Expect ctx.upstream_name like "127.0.0.1:9000" (or "host:port")
    const std::string up = ctx.upstream_name;
    if (up.empty()) {
        res = ResponseFactory::makeErrorOrPage(ctx, 502, "Bad Gateway", true);
        return true;
    }

    std::string host, port;
    const std::string::size_type c = up.find(':');
    if (c == std::string::npos) { host = up; port = "80"; }
    else { host = up.substr(0, c); port = up.substr(c + 1); }

    // Build target path by stripping the matched prefix and normalizing to a single leading '/'
    std::string rel = ctx.rel_path;                    // may or may not start with '/'
    if (!rel.empty() && rel[0] == '/') rel.erase(0,1); // drop leading '/' if present
    std::string target_path = rel.empty() ? "/" : ("/" + rel);

    // preserve original query
    const std::string q = req.getQuery();
    if (!q.empty()) { target_path += "?"; target_path += q; }

    // Open non-blocking upstream socket
    int ufd = connect_nonblock(host, port);
    if (ufd < 0) {
        std::fprintf(stderr, "[PROXY] connect failed to %s:%s (errno=%d)\n",
                     host.c_str(), port.c_str(), errno);
        res = ResponseFactory::makeErrorOrPage(ctx, 502, "Bad Gateway", true);
        return true;
    }

    // Hand off to the client connection so the event loop can drive the tunnel
    ClientConnection *cli = ctx.client;
    if (!cli) {
        ::close(ufd);
        res = ResponseFactory::makeErrorOrPage(ctx, 500, "Internal Server Error", true);
        return true;
    }

    const int connect_timeout_ms = 5000;
    const int io_idle_timeout_ms = 15000;

    // Use the overload that accepts a target path (path + optional query).
    if (!cli->beginProxyTunnel(ufd, host, port,
                               connect_timeout_ms, io_idle_timeout_ms,
                               req, target_path)) {
        ::close(ufd);
        res = ResponseFactory::makeErrorOrPage(ctx, 502, "Bad Gateway", true);
        return true;
    }

    // Tunnel is active; streaming happens asynchronously.
    return false;
}


