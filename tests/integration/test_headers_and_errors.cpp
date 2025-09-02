#include <catch2/catch_all.hpp>

#define private public
#include "ClientConnection.h"
#undef private
#include "Server.h"
#include "ServerConfig.h"
#include "VirtualServer.h"

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>

static void set_nonblock(int fd)
{
	int fl = ::fcntl(fd, F_GETFL, 0);
	REQUIRE(fl >= 0);
	REQUIRE(::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
}

static std::string recv_all(int fd)
{
	std::string s;
	char buf[4096];
	for (;;)
	{
		ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		s.append(buf, buf + n);
	}
	return s;
}

TEST_CASE("partial headers are tolerated (remain in PH_READ_HEADERS until CRLFCRLF)", "[integration][headers]")
{
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]);
	set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs;
	vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server s(cfg);

	ClientConnection conn(sv[0], &s);
	REQUIRE(conn.state == PH_READ_HEADERS);

	const std::string h1 = "GET / HTTP/1.1\r\nHost: example\r\nUser-Agent: x";
	REQUIRE(::send(sv[1], h1.data(), (int)h1.size(), 0) == (ssize_t)h1.size());
	conn.onTick(0);
	REQUIRE(conn.state == PH_READ_HEADERS); // not yet CRLFCRLF

	const std::string h2 = "\r\n\r\n";
	REQUIRE(::send(sv[1], h2.data(), (int)h2.size(), 0) == (ssize_t)h2.size());
	conn.onTick(0);
	REQUIRE(conn.state != PH_READ_HEADERS);

	::shutdown(sv[1], SHUT_RDWR);
	::close(sv[1]);
}

TEST_CASE("oversized headers -> 431 and close", "[integration][headers][431]")
{
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]);
	set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs;
	vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server s(cfg);

	ClientConnection conn(sv[0], &s);
	// make the limit tiny for the test
	conn.max_hdr_bytes = 64;

	std::string huge = "GET / HTTP/1.1\r\nHost: x\r\nUser-Agent: ";
	huge.append(4096, 'A');
	huge += "\r\n\r\n";
	REQUIRE(::send(sv[1], huge.data(), (int)huge.size(), 0) == (ssize_t)huge.size());

	// tick until the server responds
	for (int i = 0; i < 8; ++i)
		conn.onTick(0);

	std::string got = recv_all(sv[1]);
	REQUIRE(got.find(" 431 ") != std::string::npos); // "HTTP/1.1 431 ..."
	// the connection should be closing afterwards
	bool ok = conn.state == PH_WRITE || conn.state == PH_CLOSE;
	REQUIRE(ok == true);

	::close(sv[1]);
}
