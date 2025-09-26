#if !defined(CLIENTHANDLER_H)
#define CLIENTHANDLER_H

#include "EventLoop.h"
#include "ClientConnection.h"
#include <sys/time.h>
#include <ctime>   // instead of <sys/time.h>
#include <climits> 

class ClientHandler : public EventLoop::Handler {

	private:
		unsigned long long nowMs() const
    {
        std::time_t sec = std::time(NULL);       // seconds since epoch
        std::clock_t ticks = std::clock();       // CPU ticks since program start
        unsigned long long ms = static_cast<unsigned long long>(sec) * 1000ULL;
        ms += static_cast<unsigned long long>(ticks) * 1000ULL / CLOCKS_PER_SEC;
        return ms;
    }
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
