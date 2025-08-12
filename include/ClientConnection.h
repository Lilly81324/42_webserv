/* --- ClientConnection.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef CLIENTCONNECTION_H
#define CLIENTCONNECTION_H

#include "EventLoop.h"
#include <string>

class Server; // fwd

class ClientConnection : public IFdHandler {
public:
    ClientConnection(int fd, EventLoop& loop, Server& server);
    ~ClientConnection();

    void on_readable(int fd);
    void on_writable(int fd);
    void on_error(int fd);

    int fd() const { return _fd; }

private:
    int         _fd;
    EventLoop&  _loop;
    Server&     _server;
    std::string _in;
    std::string _out;
    bool        _keepAlive;   // <-- NEW

    void close_and_cleanup();
};

#endif // CLIENTCONNECTION_H
