// main.cpp (C++98 compatible)
#include "ClientConnection.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>  // perror
#include <cstring> // memset
#include <cerrno>  // errno

static int make_listen_socket(int port)
{
	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		std::perror("socket");
		return -1;
	}

	int yes = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((unsigned short)port);

	if (::bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		std::perror("bind");
		::close(fd);
		return -1;
	}
	if (::listen(fd, 16) < 0)
	{
		std::perror("listen");
		::close(fd);
		return -1;
	}
	return fd;
}

int main()
{
	int listen_fd = make_listen_socket(8080);
	if (listen_fd < 0)
		return 1;
	std::printf("Listening on port 8080...\n");

	struct sockaddr_in cliaddr;
	socklen_t clilen = sizeof(cliaddr);
	int client_fd = ::accept(listen_fd, (struct sockaddr *)&cliaddr, &clilen);
	if (client_fd < 0)
	{
		std::perror("accept");
		::close(listen_fd);
		return 1;
	}

	// Make client socket non-blocking
	int flags = fcntl(client_fd, F_GETFL, 0);
	if (flags == -1 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1)
	{
		std::perror("fcntl");
		::close(client_fd);
		::close(listen_fd);
		return 1;
	}

	// Hand off to your ClientConnection
	ClientConnection conn(client_fd);

	// Minimal manual "loop"
	// NOTE: this assumes you have getters; if not, add them or inline a quick accessor.
	while (conn.getState() != CLOSE)
	{
		if (conn.getState() == READ_HEADERS)
		{
			conn.onReadable(); // will call processIncoming() inside if you wired it that way
		}
		else if (conn.getState() == WRITE)
		{
			conn.onWritable();
		}
		// tiny sleep to avoid busy spinning on EAGAIN
		usleep(1000); // 1ms
	}

	::close(listen_fd);
	std::printf("Connection closed, exiting.\n");
	return 0;
}
