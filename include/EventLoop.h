/* --- EventLoop.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef EVENTLOOP_H
#define EVENTLOOP_H

#include <vector>
#include <poll.h>


class EventLoop
{
public:
	EventLoop();
	~EventLoop();

	// Register / modify / remove a file descriptor
	bool addFD(int fd, short events); // events: POLLIN|POLLOUT
	bool mod(int fd, short events);
	void removeFD(int fd);

	// Main event loop
	void run(int timeout_ms);
	void stop();

	// Polls and returns a vector of (fd, revents) pairs for ready FDs
	std::vector< std::pair<int, short> > handleEvents(int timeout_ms);

private:
	std::vector<struct pollfd> _pfds;
	bool _stop;

	int index_of_fd(int fd) const;
};

#endif // EVENTLOOP_H
