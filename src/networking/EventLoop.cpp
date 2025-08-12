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

int EventLoop::index_of_fd(int fd) const {
    for (size_t i = 0; i < _pfds.size(); ++i)
        if (_pfds[i].fd == fd) return static_cast<int>(i);
    return -1;
}

bool EventLoop::add(int fd, short events, IFdHandler* h) {
    if (!h || fd < 0) return false;
    if (_handlers.find(fd) != _handlers.end()) return false;
    struct pollfd p; p.fd = fd; p.events = events; p.revents = 0;
    _pfds.push_back(p);
    _handlers[fd] = h;
    return true;
}

bool EventLoop::mod(int fd, short events) {
    int idx = index_of_fd(fd);
    if (idx < 0) return false;
    _pfds[idx].events = events;
    return true;
}

void EventLoop::remove(int fd) {
    std::map<int, IFdHandler*>::iterator it = _handlers.find(fd);
    if (it != _handlers.end()) _handlers.erase(it);

    int idx = index_of_fd(fd);
    if (idx >= 0) {
        _pfds[idx] = _pfds.back();
        _pfds.pop_back();
    }
}

void EventLoop::run(int timeout_ms) {
    _stop = false;
    while (!_stop) {
        if (_pfds.empty()) break;

        int rc = ::poll(&_pfds[0], _pfds.size(), timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) continue; // interrupted by signal
            break;                         // fatal poll error
        }
        if (rc == 0) continue;             // tick

        // Snapshot events to avoid iterator invalidation if handlers mutate the loop
        std::vector< std::pair<int, short> > ev;
        ev.reserve(_pfds.size());
        for (size_t i = 0; i < _pfds.size(); ++i) {
            if (_pfds[i].revents) ev.push_back(std::make_pair(_pfds[i].fd, _pfds[i].revents));
        }

        for (size_t k = 0; k < ev.size(); ++k) {
            int fd = ev[k].first; short re = ev[k].second;
            std::map<int, IFdHandler*>::iterator hit = _handlers.find(fd);
            if (hit == _handlers.end()) continue; // handler may have been removed
            IFdHandler* h = hit->second;

            if (re & POLLIN)  h->on_readable(fd);
            if (re & POLLOUT) h->on_writable(fd);
            if (re & (POLLERR | POLLHUP | POLLNVAL)) h->on_error(fd);
        }
    }
}

void EventLoop::stop() { _stop = true; }
