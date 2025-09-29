#include "AcceptorHandler.h"

unsigned long long AcceptorHandler::nowMs() const
{
    std::time_t t = std::time(0);
    	return static_cast<unsigned long long>(t) * 1000ULL;
}

AcceptorHandler::AcceptorHandler(EventLoop& loop, Server& srv, Listener* L)
: eventLoop(loop), _srv(srv), listener(L)
{}

void AcceptorHandler::onEvent(int fd, short revents)
{
	if (!listener || listener->getFD() != fd) return;

	if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
		eventLoop.removeFD(fd); // stop accepting on this listener
		return;
	}

	if (revents & POLLIN) {
		for (;;) {
			struct sockaddr_storage ss;
			socklen_t sl = sizeof(ss);
			int cfd = ::accept(fd, (struct sockaddr*)&ss, &sl);
			if (cfd == -1) {
				if (errno == EINTR) continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK) break;
				break; // other errors: give up this turn
			}
			// configure client
			try {

				// disable Nagle to reduce small-write latency for tests
				int one = 1;
				::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			}
			catch (...) { ::close(cfd); continue; }
			// Check if IP is forbidden
			std::string ip = IpList::getIpFromSocket(&ss);
			if (!_srv.getConfig().ip_list.checkIp(ip))
			{
				std::string output = IpList::ipDeniedResponse();
				::send(cfd, output.c_str(), output.size(), 0);
				close(cfd);
				return ;
			}

			// register client handler
			ClientConnection* c = new ClientConnection(cfd,&_srv, nowMs());
			c->setIp(ip);
			ClientHandler *h = new ClientHandler(eventLoop,c);
			_srv.trackHandler(h);
			eventLoop.addFD(cfd, POLLIN, h);
			// If the client has already sent data (common in tests that write
			// immediately after connect), process it right away instead of
			// waiting for the next poll cycle. Adjust poll registration if
			// the connection now wants to write.
		}
	}
}
