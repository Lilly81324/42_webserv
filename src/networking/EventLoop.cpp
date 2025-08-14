/* --- EventLoop.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "EventLoop.h"

EventLoop::EventLoop()
{
	// Constructor
}

EventLoop::~EventLoop()
{
	// Destructor
}

void EventLoop::addFD(int fd, int event)
{
	(void)fd;
	(void)event;
}

void EventLoop::removeFD(int fd)
{
	(void)fd;
}