#include "ClientHandler.h"

#include <sys/time.h>
#include <poll.h>

void ClientHandler::onEvent(int fd, short revents)
{
	const unsigned long long now = nowMs();

	// 1) Socket events and timer ticks drive the HTTP state machine
	if (fd == clientConnection->fd())
	{
		// We don't branch on errno; connection decides using readiness + deadlines.
		(void)revents; // onTick pulls as needed (read/write) based on phase & backpressure
		clientConnection->onTick(now);
	}
	// 2) CGI stdout → stream into ChainBuf
	else if (fd == clientConnection->getCGIStreamer().cgiStdoutFD())
	{
		if (revents == 0 || (revents & POLLIN))
			clientConnection->getCGIStreamer().onCgiReadable(fd);
	}
	// 3) CGI stdin → feed request body to child
	else if (fd == clientConnection->getCGIStreamer().cgiStdinFD())
	{
		if (revents == 0 || (revents & POLLOUT))
			clientConnection->getCGIStreamer().onCgiWritable(fd);
	}

	// 4) If the connection closed, drop all its fds
	if (clientConnection->isClosed())
	{
		// drop CGI fds if present
		int cgiOut = clientConnection->getCGIStreamer().cgiStdoutFD();
		int cgiIn = clientConnection->getCGIStreamer().cgiStdinFD();
		if (cgiOut >= 0)
			eventLoop.removeFD(cgiOut);
		if (cgiIn >= 0)
			eventLoop.removeFD(cgiIn);
		eventLoop.removeFD(clientConnection->fd());
		return;
	}

	// 5) Refresh poll interests (socket + CGI pipes)
	updateInterests();
}

void ClientHandler::updateInterests()
{
	// ---- Socket interests ----
	short sock_ev = 0;
	if (clientConnection->wantsRead() && !clientConnection->isReadPaused())
		sock_ev |= POLLIN;
	if (clientConnection->hasPendingWrite())
		sock_ev |= POLLOUT;
	eventLoop.modFD(clientConnection->fd(), sock_ev);

	// ---- CGI stdout readable? ----
	const int cgiOut = clientConnection->getCGIStreamer().cgiStdoutFD();
	if (cgiOut >= 0)
	{
		if (eventLoop.indexOfFD(cgiOut) < 0)
			eventLoop.addFD(cgiOut, POLLIN, this);
		else
			eventLoop.modFD(cgiOut, POLLIN);
	}

	// ---- CGI stdin writable? ----
	const int cgiIn = clientConnection->getCGIStreamer().cgiStdinFD();
	if (cgiIn >= 0)
	{
		// We can always request POLLOUT while stdin is open; the kernel will edge us when writable.
		if (eventLoop.indexOfFD(cgiIn) < 0)
			eventLoop.addFD(cgiIn, POLLOUT, this);
		else
			eventLoop.modFD(cgiIn, POLLOUT);
	}

}