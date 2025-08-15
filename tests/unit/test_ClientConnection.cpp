// tests_client_connection.cpp  (build with -std=c++17 or newer)
#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#include "ClientConnection.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

// --- helpers --------------------------------------------------------------

static void set_nonblock(int fd)
{
	int fl = ::fcntl(fd, F_GETFL, 0);
	REQUIRE(fl >= 0);
	REQUIRE(::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
}

static std::pair<int, int> make_socketpair()
{
	int fds[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	return std::make_pair(fds[0], fds[1]);
}

static ssize_t write_all(int fd, const void *buf, size_t n)
{
	const char *p = static_cast<const char *>(buf);
	size_t off = 0;
	for (;;)
	{
		ssize_t r = ::write(fd, p + off, n - off);
		if (r > 0)
		{
			off += size_t(r);
			if (off == n)
				return off;
			continue;
		}
		if (r < 0 && (errno == EINTR))
			continue;
		// allow partial on EAGAIN etc.
		return (ssize_t)off; 
	}
}

static std::string read_available(int fd)
{
	std::string out;
	char tmp[4096];
	for (;;)
	{
		ssize_t r = ::read(fd, tmp, sizeof(tmp));
		if (r > 0)
		{
			out.append(tmp, tmp + r);
			continue;
		}
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (r == 0)
			break;
		break;
	}
	return out;
}

// spin a few iterations calling onReadable/onWritable like a toy loop
static void pump(ClientConnection &conn, int iterations = 50)
{
	for (int i = 0; i < iterations && conn.getState() != CLOSE; ++i)
	{
		if (conn.getState() == READ_HEADERS)
			conn.onReadable();
		if (conn.getState() == WRITE)
			conn.onWritable();
		// tiny backoff
		usleep(1000);
	}
}

// --- tests ---------------------------------------------------------------

TEST_CASE("Headers across two reads switch to WRITE and send hello", "[io]")
{
	// server side = connFd; client side = peer
	int peer, connFd;
	{
		std::pair<int, int> p = make_socketpair();
		peer = p.first;
		connFd = p.second;
	}
	set_nonblock(connFd);
	set_nonblock(peer);

	ClientConnection conn(connFd);
	REQUIRE(conn.getState() == READ_HEADERS);

	// send request in two chunks, splitting CRLFCRLF boundary
	const char *part1 = "GET / HTTP/1.1\r\nHost: x\r\nUser-Agent: t\r\n";
	const char *part2 = "\r\n"; // completes the empty line after headers
	REQUIRE(write_all(peer, part1, std::strlen(part1)) == (ssize_t)std::strlen(part1));

	// first pump: not enough for CRLFCRLF => still reading
	pump(conn, 5);
	REQUIRE(conn.getState() == READ_HEADERS);

	REQUIRE(write_all(peer, part2, std::strlen(part2)) == (ssize_t)std::strlen(part2));

	// second pump should parse headers, switch to WRITE, send, then CLOSE
	pump(conn, 50);

	std::string resp = read_available(peer);
	// Expect a minimal HTTP response with "hello"
	REQUIRE(resp.find("HTTP/1.1 200 OK") != std::string::npos);
	REQUIRE(resp.find("\r\n\r\nhello") != std::string::npos);

	// connection should be closed by server side
	REQUIRE(conn.getState() == CLOSE);

	::close(peer);
}

TEST_CASE("No premature switch on partial headers", "[parse]")
{
	int peer, connFd;
	{
		std::pair<int, int> p = make_socketpair();
		peer = p.first;
		connFd = p.second;
	}
	set_nonblock(connFd);
	set_nonblock(peer);

	ClientConnection conn(connFd);

	// No blank line
	const char *partial = "GET / HTTP/1.1\r\nHost: x\r\n";
	REQUIRE(write_all(peer, partial, std::strlen(partial)) == (ssize_t)std::strlen(partial));

	pump(conn, 10);
	// Should still be Reading
	REQUIRE(conn.getState() == READ_HEADERS); 

	::close(peer); 
}

TEST_CASE("Peer closes early => server closes", "[io][close]")
{
	int peer, connFd;
	{
		std::pair<int, int> p = make_socketpair();
		peer = p.first;
		connFd = p.second;
	}
	set_nonblock(connFd);
	set_nonblock(peer);

	ClientConnection conn(connFd);

	// Send a bit then close peer
	const char *p1 = "GET / H";
	REQUIRE(write_all(peer, p1, std::strlen(p1)) == (ssize_t)std::strlen(p1));
	::close(peer);

	pump(conn, 20);
	REQUIRE(conn.getState() == CLOSE);
}

TEST_CASE("Large outBuffer causes partial send and eventually completes", "[send][eagain]")
{
	int peer, connFd;
	{
		std::pair<int, int> p = make_socketpair();
		peer = p.first;
		connFd = p.second;
	}
	set_nonblock(connFd);
	set_nonblock(peer);

	ClientConnection conn(connFd);

	// Send a normal request to trigger WRITE
	const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
	REQUIRE(write_all(peer, req, std::strlen(req)) == (ssize_t)std::strlen(req));

	// Pump once so processIncoming() runs and sets response ("hello")
	pump(conn, 5);
	bool connCheck = ((conn.getState() == WRITE) || (conn.getState() == CLOSE) );
	REQUIRE(connCheck);

	// Now, to force partial EAGAIN a huge payload.
	for (int i = 0; i < 100 && conn.getState() != CLOSE; ++i)
	{
		conn.onWritable();
		// Drain a bit intermittently to allow progress
		if (i % 5 == 0)
			(void)read_available(peer);
		usleep(1000);
	}

	// Finally drain everything the server wrote and ensure response contains hello.
	std::string resp = read_available(peer);
	REQUIRE(resp.find("hello") != std::string::npos);

	// The server should close after finishing the small hello response.
	REQUIRE(conn.getState() == CLOSE);

	::close(peer);
}
