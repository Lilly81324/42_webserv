/* --- EventLoop.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef EVENTLOOP_H
#define EVENTLOOP_H

class EventLoop
{
public:
	EventLoop();
	~EventLoop();
	void addFD(int fd, int events);
	void removeFD(int fd);

private:
};

#endif // EVENTLOOP_H
