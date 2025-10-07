#if !defined(CLIENTHANDLER_H)
#define CLIENTHANDLER_H

#include "EventLoop.h"
#include "ClientConnection.h"
#include "TimeUtil.h"
#include <sys/time.h>
#include <ctime>   // instead of <sys/time.h>
#include <climits> 

class ClientHandler : public EventLoop::Handler {
		void updateInterests();

	public:
		ClientHandler(EventLoop& loop, ClientConnection* c)
		: eventLoop(loop), clientConnection(c) {}
	
		virtual ~ClientHandler() { 
			clientConnection = 0; 
		}
	
		virtual void onEvent(int fd, short revents);

		ClientConnection* conn() const { 
			return clientConnection; 
		} 
	
	private:
		EventLoop&        eventLoop;
		ClientConnection* clientConnection; // owned
	};
	

#endif // CLIENTHANDLER_H
