#if !defined( ACCEPTORHANDLER_H)
#define  ACCEPTORHANDLER_H

#include "ClientHandler.h"
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "Server.h"
#include "IpList.h"
#include <ctime>

class Listener;

class AcceptorHandler : public EventLoop::Handler
{
	public:
		AcceptorHandler(EventLoop& loop, Server& srv, Listener* L);
		virtual void onEvent(int fd, short revents);
	private:
		EventLoop& eventLoop;
		Server& _srv;
		Listener* listener; // not owned
		unsigned long long nowMs() const;
};

#endif //  ACCEPTORHANDLER_H
