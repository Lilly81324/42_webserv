/* --- Server.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef SERVER_H
#define SERVER_H

#include "ServerConfig.h"
class Server{
	private:
		const ServerConfig &_cfg;
	public:
		explicit Server(const ServerConfig &cfg);
		~Server();
		void run();
};

#endif // SERVER_H
