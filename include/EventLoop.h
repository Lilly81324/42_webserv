/* --- EventLoop.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef EVENTLOOP_H
#define EVENTLOOP_H

#include <map>
#include <vector>
#include <poll.h>   // struct pollfd, POLLIN, POLLOUT, etc.

class IFdHandler {
public:
    virtual ~IFdHandler() {}
    virtual void on_readable(int fd) = 0;
    virtual void on_writable(int fd) = 0;
    virtual void on_error(int fd) = 0;
};

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    // Register / modify / remove a file descriptor
    bool add(int fd, short events, IFdHandler* h); // events: POLLIN|POLLOUT
    bool mod(int fd, short events);
    void remove(int fd);

    // Run the loop; call stop() to exit
    void run(int timeout_ms);
    void stop();

private:
    std::vector<struct pollfd> _pfds;
    std::map<int, IFdHandler*> _handlers;
    bool _stop;

    int index_of_fd(int fd) const; // linear search (small sets are fine)
};

#endif // EVENTLOOP_H

