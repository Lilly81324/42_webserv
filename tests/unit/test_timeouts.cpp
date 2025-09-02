#include <catch2/catch_all.hpp>

#define private public
#include "ClientConnection.h"
#undef private

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include "Server.h"

static void set_nonblock(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    REQUIRE(fl >= 0);
    REQUIRE(::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
}

static unsigned long long now_ms() {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (unsigned long long)tv.tv_sec * 1000ull + (unsigned long long)(tv.tv_usec / 1000);
}

TEST_CASE("Phase deadline expiry triggers write/close flow", "[timeout]") {
    int sv[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    set_nonblock(sv[0]);
    set_nonblock(sv[1]);

    // Minimal ServerConfig/Server just to satisfy ctor, but we won't route
    ServerConfig cfg;
    Server s(cfg);

    ClientConnection conn(sv[0], &s,1000000000);
    REQUIRE(conn.state == PH_READ_HEADERS);
    REQUIRE_FALSE(conn.isClosed());

    // Simulate a timeout immediately
    conn.dl.deadline_ms_ = now_ms() - 1;
    // Tick once — should flip to WRITE and then CLOSE after flush
    conn.onTick(now_ms());

    // Drain any queued bytes
    (void)conn.io.nb_write();

    // Drive until it reaches PH_CLOSE
    for (int i = 0; i < 5 && conn.state != PH_CLOSE; ++i)
        conn.onTick(now_ms());

    REQUIRE(conn.state == PH_CLOSE);

    ::close(sv[1]);
    // conn destructor path via server.release happens elsewhere in integration;
    // here just ensure we don't segfault in the state machine.
}
