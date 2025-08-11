/* --- ClientConnection.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef CLIENTCONNECTION_H
#define CLIENTCONNECTION_H

#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <string>
#include <sys/socket.h>

#include "UniqueFD.h"

enum State
{
	READ_HEADERS,
	WRITE,
	CLOSE,
};

class ClientConnection
{
private:
	enum State state;
	UniqueFD fd;
	std::vector<char> inBuffer;
	std::vector<char> outBuffer;
	size_t parseOffset;
	size_t outOffset;
	void readFromSocket();
	static const size_t READ_CHUNK = 8192;
	/* Place a limit on Infiles to avoid Issues*/
	static const size_t MAX_INBUFFER = 1 << 20;

public:
	explicit ClientConnection(int fd) : state(READ_HEADERS), fd(fd), parseOffset(0), outOffset(0) {}
	~ClientConnection() {};
	State getState() { return this->state; }
	void onReadable();
	void onWritable();
	void changeState(State);
	void processIncoming();
	void close();
};

#endif // CLIENTCONNECTION_H
