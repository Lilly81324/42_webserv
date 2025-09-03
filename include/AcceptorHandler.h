#if !defined( ACCEPTORHANDLER_H)
#define  ACCEPTORHANDLER_H

#include "ClientHandler.h"
#include "Listener.h"
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "Server.h"

class AcceptorHandler : public EventLoop::Handler {

	private:
	unsigned long long nowMs() const
		{
			struct timeval tv; gettimeofday(&tv, 0);
			return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)tv.tv_usec / 1000ULL;
		}
		
	public:
		AcceptorHandler(EventLoop& loop, Server& srv, Listener* L)
		: eventLoop(loop), _srv(srv), listener(L) {}

		virtual void onEvent(int fd, short revents) {
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

					// register client handler
					ClientConnection* c = new ClientConnection(cfd,&_srv, nowMs());
					ClientHandler *h = new ClientHandler(eventLoop,c);
					std::cout<<"Added new Connection fd:"<<cfd<<std::endl;
					_srv.trackHandler(h);
					eventLoop.addFD(cfd, POLLIN, h);
					// If the client has already sent data (common in tests that write
					// immediately after connect), process it right away instead of
					// waiting for the next poll cycle. Adjust poll registration if
					// the connection now wants to write.
				}
			}
		}
	private:
		EventLoop& eventLoop;
		Server& _srv;
		Listener* listener; // not owned
};

#endif //  ACCEPTORHANDLER_H
