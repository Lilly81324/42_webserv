/* --- Server.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "Server.h"

Server::Server(ServerConfig &srvConfig) : srvConfig(srvConfig)
{
	this->loop = EventLoop();
}

Server::~Server()
{
	// Destructor
}

void Server::stop()
{
	
}

void Server::start()
{
}