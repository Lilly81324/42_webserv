/* --- EventLoop.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "EventLoop.h"
#include <cerrno>
#include <cstring>
#include <algorithm>


EventLoop::EventLoop() : _stop(false) {}
EventLoop::~EventLoop() {}

int EventLoop::indexOfFD(int fd) const {
    for (size_t i = 0; i < _pfds.size(); ++i)
        if (_pfds[i].fd == fd) return static_cast<int>(i);
    return -1;
}

bool EventLoop::addFD(int fd, short events) {
    if (fd < 0) return false;
    if (indexOfFD(fd) != -1) return false;
    struct pollfd p; p.fd = fd; p.events = events; p.revents = 0;
    _pfds.push_back(p);
    return true;
}

bool EventLoop::modFD(int fd, short events) {
    int idx = indexOfFD(fd);
    if (idx < 0) return false;
    _pfds[idx].events = events;
    return true;
}

void EventLoop::removeFD(int fd) {
    int idx = indexOfFD(fd);
    if (idx >= 0) {
        _pfds[idx] = _pfds.back();
        _pfds.pop_back();
    }
}

void EventLoop::run(int timeout_ms) {
    _stop = false;
    while (!_stop) {
        if (_pfds.empty()) break;
        ::poll(_pfds.empty() ? NULL : &_pfds[0], _pfds.size(), timeout_ms);
        // The user should call handleEvents() after run() returns or in each loop iteration
    }
}

std::vector< std::pair<int, short> > EventLoop::handleEvents(int timeout_ms) {
    std::vector< std::pair<int, short> > ev;
    if (_pfds.empty()) return ev;
    int rc = ::poll(&_pfds[0], _pfds.size(), timeout_ms);
    if (rc < 0) {
        if (errno == EINTR) return ev; // interrupted by signal
        return ev; // fatal poll error, return empty
    }
    if (rc == 0) return ev; // timeout, no events
    ev.reserve(_pfds.size());
    for (size_t i = 0; i < _pfds.size(); ++i) {
        if (_pfds[i].revents) ev.push_back(std::make_pair(_pfds[i].fd, _pfds[i].revents));
    }
    return ev;
}

void EventLoop::stop() { _stop = true; }
