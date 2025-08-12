/* --- Server.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef SERVER_H
#define SERVER_H

#include "EventLoop.h"
#include "ServerConfig.h"
#include "UniqueFD.h"
class Server
{
private:
	struct Listener
	{
		int fd = -1;
		std::string host;
		int port = 0;
		bool is_ipv = false;
	};
	EventLoop loop;
	ServerConfig &srvConfig;

public:
	explicit Server(ServerConfig &srvConfig);
	~Server();
	void start();
	void stop();
};

#endif // SERVER_H
