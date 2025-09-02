// tests/unit/test_ClientConnection.cpp
#include <catch2/catch_all.hpp>

#define private public          // allow white-box access to getState()/deadlines for assertions
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

// ---------------- helpers ----------------

static void set_nonblock(int fd) {
	int fl = ::fcntl(fd, F_GETFL, 0);
	REQUIRE(fl >= 0);
	REQUIRE(::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
}

static std::string recv_all_now(int fd) {
	std::string s;
	char buf[4096];
	for (;;) {
		ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n <= 0) break;
		s.append(buf, buf + n);
	}
	return s;
}

static void drive_ticks(ClientConnection &c, int iters = 8) {
	for (int i = 0; i < iters; ++i) c.onTick(0);
}

// Minimal server with one vserver at /tmp for body temp files
static void build_min_server(ServerConfig &cfg, Server &srv_out) {
	(void)srv_out; // only signature convenience
}

// ---------------- test cases ----------------

TEST_CASE("ClientConnection starts in PH_READ_HEADERS and tolerates partial headers", "[conn][headers][partial]") {
	int sv[2]; REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]); set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs; vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server server(cfg);

	ClientConnection conn(sv[0], &server,1000000000);
	REQUIRE(conn.getState() == PH_READ_HEADERS);

	const std::string h1 = "GET / HTTP/1.1\r\nHost: example\r\nUser-Agent: te";
	REQUIRE(::send(sv[1], h1.data(), (int)h1.size(), 0) == (ssize_t)h1.size());
	conn.onTick(0);
	REQUIRE(conn.getState() == PH_READ_HEADERS); // no CRLFCRLF yet

	const std::string h2 = "st\r\n\r\n";
	REQUIRE(::send(sv[1], h2.data(), (int)h2.size(), 0) == (ssize_t)h2.size());
	conn.onTick(0);
	REQUIRE(conn.getState() != PH_READ_HEADERS);

	::shutdown(sv[1], SHUT_RDWR);
	::close(sv[1]);
}

TEST_CASE("Oversized headers trigger 431 and close flow", "[conn][headers][431]") {
	int sv[2]; REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]); set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs; vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server server(cfg);

	ClientConnection conn(sv[0], &server,1000000000);

	// Make header limit tiny
	conn.max_hdr_bytes = 64;

	std::string huge = "GET / HTTP/1.1\r\nHost: x\r\nUser-Agent: ";
	huge.append(4096, 'A');
	huge += "\r\n\r\n";
	REQUIRE(::send(sv[1], huge.data(), (int)huge.size(), 0) == (ssize_t)huge.size());

	drive_ticks(conn, 6);

	const std::string out = recv_all_now(sv[1]);
	REQUIRE(out.find(" 431 ") != std::string::npos); // "HTTP/1.1 431 ..."
	bool ok = (conn.getState() == PH_WRITE || conn.getState() == PH_CLOSE);
	REQUIRE(ok == true);

	::close(sv[1]);
}

TEST_CASE("Content-Length small body parses and responds 200; keep-alive allows second request", "[conn][content-length][keepalive]") {
	int sv[2]; REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]); set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs; vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server server(cfg);

	ClientConnection conn(sv[0], &server,1000000000);

	// First request
	std::string req1 = "POST /echo HTTP/1.1\r\nHost: ex\r\nContent-Length: 5\r\n\r\nhello";
	REQUIRE(::send(sv[1], req1.data(), (int)req1.size(), 0) == (ssize_t)req1.size());

	// Drive: should parse, read body, route (200 OK placeholder), and write
	drive_ticks(conn, 8);
	std::string out1 = recv_all_now(sv[1]);
	REQUIRE(out1.find("HTTP/1.1 ") != std::string::npos);
	REQUIRE(out1.find(" 200 ") != std::string::npos);

	// Connection should either close or reset for next request based on should_close flag.
	// By default, our placeholder uses keep-alive → send a second request.
	if (conn.getState() != PH_CLOSE) {
		std::string req2 = "GET / HTTP/1.1\r\nHost: ex\r\n\r\n";
		REQUIRE(::send(sv[1], req2.data(), (int)req2.size(), 0) == (ssize_t)req2.size());
		drive_ticks(conn, 6);
		std::string out2 = recv_all_now(sv[1]);
		REQUIRE(out2.find("HTTP/1.1 ") != std::string::npos);
	}
	::close(sv[1]);
}

TEST_CASE("Expect: 100-continue pre-response is emitted before body", "[conn][expect][100]") {
	int sv[2]; REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]); set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs; vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server server(cfg);

	ClientConnection conn(sv[0], &server,1000000000);

	// Send headers with Expect: 100-continue and small body coming
	const std::string h = "POST /u HTTP/1.1\r\nHost: ex\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n";
	REQUIRE(::send(sv[1], h.data(), (int)h.size(), 0) == (ssize_t)h.size());

	// Tick once or twice: should queue 100 Continue on the wire
	drive_ticks(conn, 3);
	std::string pre = recv_all_now(sv[1]);
	REQUIRE(pre.find("100 Continue") != std::string::npos);

	// Now send the body
	REQUIRE(::send(sv[1], "hello", 5, 0) == 5);
	drive_ticks(conn, 8);

	// Final response should show up
	std::string fin = recv_all_now(sv[1]);
	REQUIRE(fin.find("HTTP/1.1 ") != std::string::npos);

	::close(sv[1]);
}

TEST_CASE("413 Payload Too Large: reject known CL before reading body", "[conn][413]") {
	int sv[2]; REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]); set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs; vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server server(cfg);

	ClientConnection conn(sv[0], &server,1000000000);
	conn.max_body_bytes = 8; // tiny cap

	// Advertise bigger than allowed
	std::string req = "POST /u HTTP/1.1\r\nHost: ex\r\nContent-Length: 20\r\n\r\n";
	REQUIRE(::send(sv[1], req.data(), (int)req.size(), 0) == (ssize_t)req.size());

	drive_ticks(conn, 6);
	std::string out = recv_all_now(sv[1]);
	REQUIRE(out.find(" 413 ") != std::string::npos);
	bool ok = conn.getState() == PH_WRITE || conn.getState() == PH_CLOSE;
	REQUIRE(ok == true);

	::close(sv[1]);
}

TEST_CASE("Runtime size enforcement for chunked: 413 once bytes_received exceeds cap", "[conn][chunked][413]") {
	int sv[2]; REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]); set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs; vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server server(cfg);

	ClientConnection conn(sv[0], &server,1000000000);
	conn.max_body_bytes = 6; // cap lower than incoming body

	// chunked with total 9 bytes (Wikipedia)
	const std::string h = "POST /u HTTP/1.1\r\nHost: ex\r\nTransfer-Encoding: chunked\r\n\r\n";
	REQUIRE(::send(sv[1], h.data(), (int)h.size(), 0) == (ssize_t)h.size());
	drive_ticks(conn, 3);

	const std::string c1 = "4\r\nWiki\r\n";
	REQUIRE(::send(sv[1], c1.data(), (int)c1.size(), 0) == (ssize_t)c1.size());
	drive_ticks(conn, 2);

	// After first chunk (4), still under cap. Send next chunk to exceed.
	const std::string c2 = "5\r\npedia\r\n";
	REQUIRE(::send(sv[1], c2.data(), (int)c2.size(), 0) == (ssize_t)c2.size());

	drive_ticks(conn, 6);
	std::string out = recv_all_now(sv[1]);

	std::cout << out << std::endl;
	REQUIRE(out.find(" 413 ") != std::string::npos);

	::close(sv[1]);
}

TEST_CASE("Deadline expiry -> write then close", "[conn][timeout]") {
	int sv[2]; REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	set_nonblock(sv[0]); set_nonblock(sv[1]);

	ServerConfig cfg;
	VirtualServer vs; vs.client_body_temp_path = "/tmp";
	cfg.push_back(vs);
	Server server(cfg);

	ClientConnection conn(sv[0], &server,10);
	REQUIRE(conn.getState() == PH_READ_HEADERS);

	// Force deadline to the past
	conn.dl.deadline_ms_ = 1; // in the past relative to onTick(0) logic
	conn.onTick(0);

	// It should pivot toward write/close path
	bool ok = conn.getState() == PH_WRITE || conn.getState() == PH_CLOSE;
	REQUIRE(ok == true);

	// Drive until PH_CLOSE
	for (int i = 0; i < 10 && conn.getState() != PH_CLOSE; ++i)
		conn.onTick(0);

	REQUIRE(conn.getState() == PH_CLOSE);
	::close(sv[1]);
}
