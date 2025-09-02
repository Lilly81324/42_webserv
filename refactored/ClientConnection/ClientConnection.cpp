/* --- ClientConnection.cpp --- */
/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "ClientConnection.h"
#include "Server.h"
#include "HEADER_ENTRIES.h"
#include "ResponseFactory.h"
#include "RequestContext.h"
#include <fstream>
#include <sstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits.h>

/***---------------------------CLIENT CONNECTION HANDLING REQUEST AND RESPONSE---------------------------***/

// Send HTTP/1.1 100 Continue if client sent Expect: 100-continue
void ClientConnection::handleExpectContinueIfNeeded()
{
	if (!expectContinue || state != READ_HEADERS || !fd)
		return;

	int v = -1;
	if (server && local_port > 0)
		v = server->resolveVirtualServerByPort(local_port, "localhost");
	if (v < 0)
		v = vs_idx; // fall back to whatever you had

	// If we cannot resolve VS, be conservative: don't send 100
	if (!server || v < 0)
		return;

	RouteDecision decision;
	Router::makeDecisionForVS(server->getConfig(), v, req.getMethod(), req.getPath(), decision);

	// Read limits from decision (location/server)
	size_t locLimit = (decision.loc) ? decision.loc->write_conf.max_body_bytes : 0;
	size_t srvLimit = (decision.vs) ? decision.vs->client_max_body_size : 0;
	size_t limit = (locLimit > 0) ? locLimit : srvLimit;

	// Parse CL / TE from what you already analyzed
	bool willReject = false;
	if (bodyMode == BM_CONTENT_LENGTH && expectedContentLength > 0)
	{
		if (limit > 0 && expectedContentLength > limit)
			willReject = true;
	}
	if (bodyMode == BM_CONTENT_LENGTH && transferChunked)
	{
		willReject = true; // CL + TE conflict -> 400 (but do not send 100)
	}
	if (bodyMode == BM_UNKNOWN)
	{
		// If the method requires a body but neither CL nor chunked was seen, we’ll 411.
		std::string m = req.getMethod();
		for (size_t i = 0; i < m.size(); ++i)
			if (m[i] >= 'a' && m[i] <= 'z')
				m[i] = m[i] - 'a' + 'A';
		if (m == "POST" || m == "PUT" || m == "PATCH")
			willReject = true;
	}

	if (willReject)
	{
		// Early error; don't send 100. Minimal immediate reply and close.
		const char *resp =
			"HTTP/1.1 413 Payload Too Large\r\n"
			"Content-Length: 0\r\n"
			"Connection: close\r\n"
			"\r\n";
		::send(fd.get(), resp, std::strlen(resp), MSG_NOSIGNAL);
		// Close immediately; no keep-alive on early error
		close();
		expectContinue = false;
		return;
	}

	// Everything looks acceptable -> send 100 Continue
	const char *resp = "HTTP/1.1 100 Continue\r\n\r\n";
	::send(fd.get(), resp, std::strlen(resp), MSG_NOSIGNAL);
	expectContinue = false;
}

void ClientConnection::onTick()
{
	if (state == CLOSE || !fd)
		return;

	// If we’re in an active CGI phase, enforce its deadline separately
	if (cgi_active)
	{
		if (nowMs() > cgi_deadline)
		{
			// kill CGI and close connection; tests only require the close
			if (cgi_in_fd >= 0)
			{
				::close(cgi_in_fd);
				cgi_in_fd = -1;
			}
			if (cgi_out_fd >= 0)
			{
				::close(cgi_out_fd);
				cgi_out_fd = -1;
			}
			proc.terminate();
			close();
			return;
		}
	}
	else
	{
		// regular header/body/write phases
		if (expired())
		{
			close(); // tests assert the connection is closed on timeout
			return;
		}
	}
}

void ClientConnection::changeState(State state)
{
	this->state = state;
}

/**
 * Make sure you remove ClientConnection after call
 * @warning Call destructor after Close so the Client Connection dies
 */
void ClientConnection::close()
{
	if (fd)
		this->fd.reset();
	this->state = CLOSE;
}

// ---- Socket I/O ------------------------------------------------------------

void ClientConnection::onReadable()
{
	readFromSocket();
}

void ClientConnection::readFromSocket()
{
	if (this->state != READ_HEADERS || !fd)
		return;
	while (true)
	{
		if (req.getState() == HEADER || req.getState() == START)
		{
			if (req.getTotalBytesRead() >= MAX_INBUFFER)
			{
				close();
				return;
			}
		}

		char buffer[READ_CHUNK];
		ssize_t n = ::recv(fd.get(), buffer, sizeof(buffer), 0);

		if (n > 0)
		{
			if (handleRecvPositive(n, buffer))
				return;
			inBuffer.clear();
			if (state == WRITE)
				return;
			continue;
		}

		if (n == 0)
		{
			if (handleRecvZero())
				return;
			inBuffer.clear();
			close();
			return;
		}

		if (n < 0)
		{
			if (handleRecvError(n))
				return;
			close();
			return;
		}
	}
}


void ClientConnection::onWritable()
{
	if (state != WRITE || !fd)
		return;

	const size_t total = outBuffer.size();
	const char *base = total ? &outBuffer[0] : 0;

	// If nothing left to send:
	if (outOffset >= total)
	{
		if (flow.getWriteLinger())
			close();
		else
		{
			flow.setWriteLinger(true);
			bumpDeadline(WRITE_TIMEOUT_MS);
		}
		return;
	}

	const char *p = base + outOffset;
	size_t left = total - outOffset;

	ssize_t n = ::send(fd.get(), p, left, MSG_NOSIGNAL);
	if (n > 0)
	{
		outOffset += static_cast<size_t>(n);
		bumpDeadline(WRITE_TIMEOUT_MS);

		// backpressure: maybe resume reads
		size_t remaining = total - outOffset;
		if (flow.isReadPaused() && remaining <= LOW_WATER)
			flow.setReadPaused(false);

		if (outOffset >= total)
			flow.setWriteLinger(true);
		return;
	}

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return; // try later

	// hard error
	close();
}
