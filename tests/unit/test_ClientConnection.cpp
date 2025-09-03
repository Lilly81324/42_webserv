// tests/unit/test_ClientConnection.cpp
#include <catch2/catch_all.hpp>

#define private public // allow white-box access to getState()/deadlines for assertions
#include "ClientConnection.h"
#include "PhaseDeadline.h"
#undef private

#include "Server.h"
#include "ServerConfig.h"
#include "VirtualServer.h"

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include "helpers/net.hpp"

// ---------------- helpers ----------------



static std::string recv_all_now(int fd)
{
	std::string s;
	char buf[4096];
	for (;;)
	{
		ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n <= 0)
			break;
		s.append(buf, buf + n);
	}
	return s;
}

static void drive_ticks(ClientConnection &c, int iters = 14)
{
	for (int i = 0; i < iters; ++i)
		c.onTick(1000);
}

// Minimal server with one vserver at /tmp for body temp files
static void build_min_server(ServerConfig &cfg, Server &srv_out)
{
	(void)srv_out; // only signature convenience
}

// ---------------- test cases ----------------

TEST_CASE("ClientConnection starts in PH_READ_HEADERS and tolerates partial headers", "[conn][headers][partial]")
{
	int srv_fd, cli_fd, port;
	REQUIRE(create_loopback_tcp_pair(srv_fd, cli_fd, port) == 0);
	set_nonblock(srv_fd);
	set_nonblock(cli_fd);

	ServerConfig cfg;
	cfg.parseFile("tests/unit/config/with_locations.conf");
	VirtualServer vs = cfg.servers()[0];
	vs.listen_port = port; // use the actual TCP listen port
	vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);

	Server server(cfg);
	server.start();

	ClientConnection conn(srv_fd, &server, 0);
	REQUIRE(conn.getState() == PH_READ_HEADERS);

	const std::string h1 = "GET / HTTP/1.1\r\nHost: example\r\nUser-Agent: te";
	REQUIRE(::send(cli_fd, h1.data(), (int)h1.size(), 0) == (ssize_t)h1.size());
	conn.onTick(0);
	REQUIRE(conn.getState() == PH_READ_HEADERS); // no CRLFCRLF yet

	const std::string h2 = "st\r\n\r\n";
	REQUIRE(::send(cli_fd, h2.data(), (int)h2.size(), 0) == (ssize_t)h2.size());
	conn.onTick(0);
	REQUIRE(conn.getState() != PH_READ_HEADERS);

	server.stop();
	::close(cli_fd);
}

TEST_CASE("Oversized headers trigger 431 and close flow", "[conn][headers][431]")
{
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]);
	set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs;
	vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server server(cfg);

	ClientConnection conn(sv[0], &server, 0);

	// Make header limit tiny
	conn.max_hdr_bytes = 64;

	std::string huge = "GET / HTTP/1.1\r\nHost: x\r\nUser-Agent: ";
	huge.append(128 * 1048, 'A');
	huge += "\r\n\r\n";
	REQUIRE(::send(sv[1], huge.data(), (int)huge.size(), 0) == (ssize_t)huge.size());

	drive_ticks(conn, 14);

	const std::string out = recv_all_now(sv[1]);
	REQUIRE(out.find(" 431 ") != std::string::npos); // "HTTP/1.1 431 ..."
	bool ok = (conn.getState() == PH_WRITE || conn.getState() == PH_CLOSE);
	REQUIRE(ok == true);

	server.stop();
	::close(sv[1]);
}

TEST_CASE("Content-Length small body parses and responds 200; keep-alive allows second request", "[conn][content-length][keepalive]")
{
	int srv_fd, cli_fd, port;
	REQUIRE(create_loopback_tcp_pair(srv_fd, cli_fd, port) == 0);
	set_nonblock(srv_fd);
	set_nonblock(cli_fd);

	ServerConfig cfg;
	cfg.parseFile("tests/unit/config/with_locations.conf");
	VirtualServer vs = cfg.servers()[0];
	vs.listen_port = port; // use the actual TCP listen port
	vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);

	Server server(cfg);	
	server.start();

	ClientConnection conn(srv_fd, &server, 0);

	// First request
	std::string req1 = "POST /api HTTP/1.1\r\nHost: ex\r\nContent-Length: 5\r\n\r\nhello";
	REQUIRE(::send(cli_fd, req1.data(), (int)req1.size(), 0) == (ssize_t)req1.size());

	// Drive: should parse, read body, route (200 OK placeholder), and write
	drive_ticks(conn, 13);
	std::string out1 = recv_all_now(cli_fd);
	std::cout<<out1<<std::endl;
	REQUIRE(out1.find("HTTP/1.1 ") != std::string::npos);
	REQUIRE(out1.find(" 200 ") != std::string::npos);

	// Connection should either close or reset for next request based on should_close flag.
	// By default, our placeholder uses keep-alive → send a second request.
	if (conn.getState() != PH_CLOSE)
	{
		std::string req2 = "GET / HTTP/1.1\r\nHost: ex\r\n\r\n";
		REQUIRE(::send(cli_fd, req2.data(), (int)req2.size(), 0) == (ssize_t)req2.size());
		drive_ticks(conn, 13);
		std::string out2 = recv_all_now(cli_fd);
		REQUIRE(out2.find("HTTP/1.1 ") != std::string::npos);
	}

	server.stop();
	::close(cli_fd);
}

TEST_CASE("Expect: 100-continue pre-response is emitted before body", "[conn][expect][100]")
{
	int srv_fd, cli_fd, port;
	REQUIRE(create_loopback_tcp_pair(srv_fd, cli_fd, port) == 0);
	set_nonblock(srv_fd);
	set_nonblock(cli_fd);

	ServerConfig cfg;
	cfg.parseFile("tests/unit/config/with_locations.conf");
	VirtualServer vs = cfg.servers()[0];
	vs.listen_port = port; // use the actual TCP listen port
	vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);

	Server server(cfg);
	server.start();

	ClientConnection conn(srv_fd, &server, 0);

	const std::string h = "POST /api HTTP/1.1\r\nHost: ex\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n";
	REQUIRE(::send(cli_fd, h.data(), (int)h.size(), 0) == (ssize_t)h.size());

	drive_ticks(conn, 4);
	std::string pre = recv_all_now(cli_fd);
	REQUIRE(pre.find("100 Continue") != std::string::npos);

	REQUIRE(::send(cli_fd, "hello", 5, 0) == 5);
	drive_ticks(conn, 13);

	std::string fin = recv_all_now(cli_fd);
	REQUIRE(fin.find("HTTP/1.1 ") != std::string::npos);

	server.stop();
	::close(cli_fd);
}

TEST_CASE("413 Payload Too Large: reject known CL before reading body", "[conn][413][CL]")
{
	int srv_fd, cli_fd, port;
	REQUIRE(create_loopback_tcp_pair(srv_fd, cli_fd, port) == 0);
	set_nonblock(srv_fd);
	set_nonblock(cli_fd);

	ServerConfig cfg;
	cfg.parseFile("tests/unit/config/with_locations.conf");
	VirtualServer vs = cfg.servers()[0];
	vs.listen_port = port; // use the actual TCP listen port
	vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);

	Server server(cfg);
	server.start();

	ClientConnection conn(srv_fd, &server, 0);
	conn.max_body_bytes = 8; // tiny cap

	// Advertise bigger than allowed
	std::string req = "POST /localhost8080/api HTTP/1.1\r\nHost:ex \r\nContent-Length: 200000000000000\r\n\r\n";
	REQUIRE(::send(cli_fd, req.data(), (int)req.size(), 0) == (ssize_t)req.size());

	drive_ticks(conn, 13);
	std::string out = recv_all_now(cli_fd);
	REQUIRE(out.find(" 413 ") != std::string::npos);
	bool ok = conn.getState() == PH_WRITE || conn.getState() == PH_CLOSE;
	REQUIRE(ok == true);

	server.stop();
	::close(cli_fd);
}

/**
 * Somethings wrong with the setup of the virtual servers, and we get an fd of -1
 */
TEST_CASE("Runtime size enforcement for chunked: 413 once bytes_received exceeds cap", "[conn][chunked][413]")
{
	int srv_fd, cli_fd, port;
	REQUIRE(create_loopback_tcp_pair(srv_fd, cli_fd, port) == 0);
	set_nonblock(srv_fd);
	set_nonblock(cli_fd);

	ServerConfig cfg;
	cfg.parseFile("tests/unit/config/with_locations.conf");
	VirtualServer vs = cfg.servers()[0];
	vs.listen_port = port; // use the actual TCP listen port
	vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);

	Server server(cfg);
	server.start();

	ClientConnection conn(srv_fd, &server, 0);
	conn.max_body_bytes = 6; // cap lower than incoming body

	// chunked with total 9 bytes (Wikipedia)
	const std::string h = "POST /u HTTP/1.1\r\nHost: ex\r\nTransfer-Encoding: chunked\r\n\r\n";
	REQUIRE(::send(cli_fd, h.data(), (int)h.size(), 0) == (ssize_t)h.size());
	drive_ticks(conn, 13);

	const std::string c1 = "4\r\nWiki\r\n";
	REQUIRE(::send(cli_fd, c1.data(), (int)c1.size(), 0) == (ssize_t)c1.size());
	drive_ticks(conn, 13);

	// After first chunk (4), still under cap. Send next chunk to exceed.
	for (int i = 0; i < 10000; i++)
	{
		const std::string c2 = "5\r\npedia\r\n";
		REQUIRE(::send(cli_fd, c2.data(), (int)c2.size(), 0) == (ssize_t)c2.size());
	}
	drive_ticks(conn, 13);
	std::string out = recv_all_now(cli_fd);

	REQUIRE(out.find(" 413 ") != std::string::npos);

	server.stop();
	::close(cli_fd);
}

TEST_CASE("Deadline expiry -> write then close", "[conn][timeout]")
{
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]);
	set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs;
	vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server server(cfg);

	ClientConnection conn(sv[0], &server, 10);
	REQUIRE(conn.getState() == PH_READ_HEADERS);

	// Force deadline to the past
	conn.dl.deadline_ms_ = 1; // in the past relative to onTick(0) logic
	conn.onTick(10);

	// It should pivot toward write/close path
	bool ok = conn.getState() == PH_WRITE || conn.getState() == PH_CLOSE;
	REQUIRE(ok == true);

	// Drive until PH_CLOSE
	for (;conn.getState() != PH_CLOSE;)
		conn.onTick(0);

	REQUIRE(conn.getState() == PH_CLOSE);
	::close(sv[1]);
}
