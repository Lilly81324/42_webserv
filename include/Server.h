/* --- Server.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef SERVER_H
#define SERVER_H

#include "ServerConfig.h"
#include <vector>
#include <map>

class ClientConnection; // fwd

class Server{
private:
    const ServerConfig &_cfg;
    std::vector<int>                    _listeners; // listening sockets
    std::map<int, ClientConnection*>    _clients;   // owned

    void openListeners(); // create _listeners from config

public:
    explicit Server(const ServerConfig &cfg);
    ~Server();

    void run();

    // called by ClientConnection when it closes
    void onClientClosed(int fd);

    // internal helpers (used by Server.cpp only)
    void registerClient(int fd, ClientConnection* c);

	 const ServerBlock* firstServer() const;
};

#endif // SERVER_H


