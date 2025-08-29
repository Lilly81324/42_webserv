#include "ClientConnection.h"
#include "Server.h"
#include "HttpResponse.h"
#include "ResponseFactory.h"

#include <sstream>

bool ClientConnection::handleRecvPositive(ssize_t n, char *buffer)
{
	// -------- READ_HEADERS path --------
	if (state == READ_HEADERS)
	{
		// Append into header buffer with a hard cap
		const size_t spaceLeft =
			(inBuffer.size() < MAX_INBUFFER) ? (MAX_INBUFFER - inBuffer.size()) : 0;
		if (spaceLeft == 0)
		{
			send_and_close(431, "Request Header Fields Too Large");
			state = WRITE; // set by queue helper; keep for clarity
			return true;
		}
		size_t toCopy = static_cast<size_t>(n);
		if (toCopy > spaceLeft)
			toCopy = spaceLeft;
		inBuffer.insert(inBuffer.end(), buffer, buffer + toCopy);
		bumpDeadline(HDR_TIMEOUT_MS);

		// Parse; if not complete, keep reading
		if (!headersComplete(inBuffer, req))
			return false;

		// Resolve VS index once per request (reuse your logic)
		if (server && local_port > 0)
			vs_idx = server->resolveVirtualServerByPort(local_port, "localhost");

		// Route + early policy (stateless) — do not send here; just decide
		RouteDecision decision;
		PipelineAction act = server->getPipeline()->policyGate(
			server->getConfig(), vs_idx, req, decision, res);

		if (act == PIPE_ERR_SENT)
		{
			// Build + queue error (ResponseFactory already set headers)
			send_and_close(res);
			state = WRITE;
			return true;
		}

		// Cache router decision & limits (no re-routing later)
		cacheDecisionAndLimits(decision);

		if (act == PIPE_HANDLE_NOW)
		{
			// No body required — immediately produce final response
			server->getPipeline()->processRequest(
				server->getConfig(), vs_idx, req, res, cache);
			send_keepalive(res);
			state = WRITE;
			return true;
		}

		// PIPE_READ_BODY — clean transition to body streaming
		state = READ_BODY;
		bodyReceived = 0;

		// Only now that we accept the body, honor Expect: 100-continue
		if (expectContinue)
		{
			static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
			::send(fd.get(), cont, sizeof(cont) - 1, MSG_NOSIGNAL);
			expectContinue = false;
		}

		// Ensure temp file exists if you’ll store to disk; flush any parsed prefix
		if (!req.isBodyOnDisk())
			createBodyTempFileIfNeeded();
		if (req.isBodyOnDisk())
			writeParsedBytesToBodyFile();

		return false; // keep reading body
	}

	// -------- READ_BODY streaming path --------
	if (state == READ_BODY)
	{
		// Content-Length mode
		if (!transferChunked)
		{
			bodyReceived += static_cast<size_t>(n);

			// Strict CL enforcement
			if (bodyReceived > expectedContentLength)
			{
				send_and_close(400, "Bad Request");
				state = WRITE;
				return true;
			}

			// Append to sink (RAM/tmp)
			if (req.isBodyOnDisk())
			{
				if (!appendToBodyFile(buffer, static_cast<size_t>(n)))
				{
					send_and_close(507, "Insufficient Storage");
					state = WRITE;
					return true;
				}
			}
			else
			{
				appendToBodyBuffer(buffer, static_cast<size_t>(n));
				if (maxBodyLimit_ && req.getBodyLength() > maxBodyLimit_)
				{
					send_and_close(413, "Payload Too Large");
					state = WRITE;
					return true;
				}
			}

			// Done?
			if (bodyReceived == expectedContentLength)
			{
					server->getPipeline()->processRequest(
					server->getConfig(), vs_idx, req, res, cache);
				send_keepalive(res);
				state = WRITE;
				return true;
			}
			return false; // need more body bytes
		}

		// Chunked mode — enforce running total (use your own decoder if present)
		if (maxBodyLimit_ > 0 && req.getBodyLength() > maxBodyLimit_)
		{
			send_and_close(413, "Payload Too Large");
			state = WRITE;
			return true;
		}

		// If your parser can signal "chunked complete"
		if (req.isChunkedComplete())
		{
			server->getPipeline()->processRequest(
				server->getConfig(), vs_idx, req, res, cache);
			send_keepalive(res);
			state = WRITE;
			return true;
		}

		return false; // need more chunk data
	}

	// Other states: nothing to do on read
	return false;
}

bool ClientConnection::handleRecvZero()
{
	// Peer closed

	if (state == READ_HEADERS)
	{
		if (headersComplete(inBuffer, req))
		{
			RouteDecision decision;
			if (server && local_port > 0)
				vs_idx = server->resolveVirtualServerByPort(local_port, "localhost");
			PipelineAction act = server->getPipeline()->policyGate(
				server->getConfig(), vs_idx, req, decision, res);

			if (act == PIPE_ERR_SENT)
			{
				send_and_close(res);
				state = WRITE;
				return true;
			}

			cacheDecisionAndLimits(decision);

			if (act == PIPE_HANDLE_NOW)
			{
				server->getPipeline()->processRequest(
					server->getConfig(), vs_idx, req,res,cache);
				send_keepalive(res);
				state = WRITE;
				return true;
			}

			// PIPE_READ_BODY but socket EOF → incomplete body
			send_and_close(400, "Bad Request");
			state = WRITE;
			return true;
		}

		// Headers incomplete and EOF → bad request
		send_and_close(400, "Bad Request");
		state = WRITE;
		return true;
	}

	if (state == READ_BODY)
	{
		// Incomplete body at EOF → 400
		if (!transferChunked && bodyReceived < expectedContentLength)
		{
			send_and_close(400, "Bad Request");
			state = WRITE;
			return true;
		}
		if (transferChunked && !req.isChunkedComplete())
		{
			send_and_close(400, "Bad Request");
			state = WRITE;
			return true;
		}

		// Rare: exactly complete at EOF; handle as usual
		server->getPipeline()->processRequest(
			server->getConfig(), vs_idx, req,res,cache);
		send_keepalive(res);
		state = WRITE;
		return true;
	}

	return false;
}


bool ClientConnection::handleRecvError(ssize_t n)
{
	(void)n;
	if (errno == EAGAIN || errno == EWOULDBLOCK)
		return true; // caller will simply return and try later
	return false;
}