#include <catch2/catch_all.hpp>
#include "ClientConnection.h"
#include "Server.h"
#define private public
#include "ClientConnection.h"
#undef private

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>

static void set_nonblock(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    REQUIRE(fl >= 0);
    REQUIRE(::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
}

TEST_CASE("ClientConnection tolerates partial header delivery", "[integration][headers]") {
    int sv[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    set_nonblock(sv[0]);
    set_nonblock(sv[1]);

    ServerConfig cfg;
    // one virtual server with a tmp path to avoid -1 index uses during body readers
    VirtualServer vs;
    vs.client_body_temp_path = "/tmp";
    cfg.push_back(vs);
    Server s(cfg);

    ClientConnection conn(sv[0], &s);
    REQUIRE(conn.getState() == PH_READ_HEADERS);

    // send headers in two pieces split across CRLFCRLF
    const std::string h1 = "GET / HTTP/1.1\r\nHost: example\r\nUser-Agent: t\r\n";
    const std::string h2 = "\r\n";

    // write first part, tick
    REQUIRE(::send(sv[1], h1.data(), (int)h1.size(), 0) == (ssize_t)h1.size());
    conn.onTick(0); // read & parse
    REQUIRE(conn.getState() == PH_READ_HEADERS); // still waiting for end-of-headers

    // complete headers
    REQUIRE(::send(sv[1], h2.data(), (int)h2.size(), 0) == (ssize_t)h2.size());
    conn.onTick(0);

    // next phases depend on guards; at minimum we must have left PH_READ_HEADERS
    REQUIRE(conn.getState() != PH_READ_HEADERS);

    // Close peer and allow server to progress to closing eventually
    ::shutdown(sv[1], SHUT_RDWR);
    ::close(sv[1]);

    // drive some ticks to ensure no crash
    for (int i = 0; i < 5; ++i)
        conn.onTick(0);

    // no hard assertions on response text here; this test enforces parser behavior only
}
