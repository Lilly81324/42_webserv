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
	if (expectContinue && state == READ_HEADERS && fd)
	{
		const char *resp = "HTTP/1.1 100 Continue\r\n\r\n";
		ssize_t n = ::send(fd.get(), resp, std::strlen(resp), MSG_NOSIGNAL);
		(void)n; // ignore short send, best effort
		expectContinue = false;
	}
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

void ClientConnection::analyzeHeaders(const HttpRequest &request)
{
	if (headersAnalyzed)
		return;

	// Defaults
	bodyMode = BM_NONE;
	expectedContentLength = 0;
	expectContinue = false;
	transferChunked = false;

	const Headers &h = request.getHeaders();
	// Iterate headers once: O(N) and avoid repeated map lookups and extra copies
	for (std::map<std::string, std::string, CiLess>::const_iterator it = h.getBegin(); it != h.getEnd(); ++it)
	{
		const std::string &k = it->first;
		const std::string &v = it->second;
		if (k == "Content-Length")
		{
			size_t len = 0;
			for (size_t i = 0; i < v.size(); ++i)
			{
				if (v[i] < '0' || v[i] > '9')
					break;
				len = len * 10 + (v[i] - '0');
			}
			expectedContentLength = len;
			bodyMode = BM_CONTENT_LENGTH;
		}
		else if (k == "Transfer-Encoding")
		{
			// lowercase check for 'chunked'
			for (size_t i = 0; i < v.size(); ++i)
			{
				char c = v[i];
				if (c >= 'A' && c <= 'Z')
					c = c - 'A' + 'a';
				// match substring "chunked"
				// simple find without allocations
				size_t rem = v.size() - i;
				if (rem >= 7 &&
					(v[i] == 'c' || v[i] == 'C') &&
					(v[i + 1] == 'h' || v[i + 1] == 'H'))
				{
					// fallback to lowercase search using std::string::find on a lowercased temp;
					std::string tmp = v;
					for (size_t j = 0; j < tmp.size(); ++j)
						if (tmp[j] >= 'A' && tmp[j] <= 'Z')
							tmp[j] = tmp[j] - 'A' + 'a';
					if (tmp.find("chunked") != std::string::npos)
					{
						transferChunked = true;
						bodyMode = BM_CHUNKED;
					}
					break;
				}
			}
		}
		else if (k == "Expect")
		{
			// check for 100-continue case-insensitively
			std::string tmp = v;
			for (size_t j = 0; j < tmp.size(); ++j)
				if (tmp[j] >= 'A' && tmp[j] <= 'Z')
					tmp[j] = tmp[j] - 'A' + 'a';
			if (tmp.find("100-continue") != std::string::npos)
				expectContinue = true;
		}
	}

	headersAnalyzed = true;
}

static std::string makeHelloResponse(bool keepAlive)
{
	const char *body = "hello";
	std::string resp;
	resp.reserve(128);
	resp += "HTTP/1.1 200 OK\r\n";
	resp += "Content-Length: 5\r\n";
	resp += std::string("Connection: ") + (keepAlive ? "keep-alive\r\n" : "close\r\n");
	resp += "Content-Type: text/plain\r\n";
	resp += "\r\n";
	resp += body;
	return resp;
}

static std::string makeErrorResponse()
{
	const char *body = "Internal Server Error\n";
	std::string resp;
	resp.reserve(128);
	resp += "HTTP/1.1 500 Internal Server Error\r\n";
	resp += "Content-Length: 22\r\n";
	resp += "Connection: close\r\n";
	resp += "Content-Type: text/plain\r\n";
	resp += "\r\n";
	resp += body;
	return resp;
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

bool ClientConnection::handleRecvPositive(ssize_t n, char *buffer)
{
	size_t spaceLeft = MAX_INBUFFER - inBuffer.size();
	size_t toCopy = static_cast<size_t>(n);
	toCopy = (toCopy > spaceLeft) ? spaceLeft : toCopy;
	inBuffer.insert(inBuffer.end(), buffer, buffer + toCopy);
	bumpDeadline(HDR_TIMEOUT_MS);

	// After appending bytes, let parser consume them. If headers are
	// complete we may need to offload the body to disk based on server
	// configuration; headersComplete() will mark request properties.
	if (headersComplete(inBuffer, req))
	{
		vs_idx = -1;
		if (server && local_port > 0)
			vs_idx = server->resolveVirtualServerByPort(local_port, "localhost");

		// Handle Expect: 100-continue if needed
		handleExpectContinueIfNeeded();

		// Possibly enable file-backed body storage if Content-Length is large
		if (req.getState() == BODY && !req.isBodyOnDisk())
			createBodyTempFileIfNeeded();

		// If the request is file-backed, write the bytes that were just
		// handled by the parser into the file. HttpRequest::getBytesHandledLast()
		// reports how many bytes the last parse() call consumed and those bytes
		// correspond to the prefix of inBuffer.
		if (req.isBodyOnDisk())
			writeParsedBytesToBodyFile();

		if (processIncoming())
			return true;
	}
	return false;
}

bool ClientConnection::handleRecvZero()
{
	if (headersComplete(inBuffer, req))
	{
		if (processIncoming())
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

void ClientConnection::createBodyTempFileIfNeeded()
{
	size_t expected = req.getBodyLength();
	size_t memThreshold = 1024 * 1024; // 1MiB
	if (server && vs_idx >= 0)
	{
		const std::vector<VirtualServer> &svs = server->getConfig().servers();
		if (vs_idx >= 0 && static_cast<size_t>(vs_idx) < svs.size())
		{
			int serverLimit = svs[vs_idx].client_max_body_size;
			if (serverLimit > 0 && static_cast<size_t>(serverLimit) < memThreshold)
				memThreshold = static_cast<size_t>(serverLimit);
		}
	}
	if (expected > memThreshold)
	{
		// create temp dir if needed
		std::string tmpdir = "/home/schiper/Desktop/Projects/webserv/tmp/webserv_bodies";
		if (server && vs_idx >= 0)
		{
			const std::vector<VirtualServer> &svs = server->getConfig().servers();
			if (vs_idx >= 0 && static_cast<size_t>(vs_idx) < svs.size())
			{
				std::string p = svs[vs_idx].client_body_temp_path;
				if (!p.empty())
					tmpdir = p;
			}
		}
		// ensure dir exists (best-effort)
		::mkdir(tmpdir.c_str(), 0700);
		// create unique filename
		char fname[PATH_MAX];
		snprintf(fname, sizeof(fname), "%s/webbody_%u_%llu.tmp", tmpdir.c_str(), (unsigned)getpid(), (unsigned long long)nowMs());
		// open file and move already-parsed body bytes into it
		std::ofstream ofs(fname, std::ios::out | std::ios::binary | std::ios::trunc);
		if (ofs)
		{
			std::vector<char> current = req.getBody();
			if (!current.empty())
				ofs.write(&current[0], current.size());
			ofs.flush();
			ofs.close();
			req.enableBodyOnDisk(std::string(fname));
		}
	}
}

void ClientConnection::writeParsedBytesToBodyFile()
{
	size_t handled = req.getBytesHandledLast();
	if (handled > 0 && handled <= inBuffer.size())
	{
		std::ofstream ofs(req.getBodyFilePath().c_str(), std::ios::out | std::ios::binary | std::ios::app);
		if (ofs)
		{
			ofs.write(&inBuffer[0], handled);
			ofs.flush();
		}
	}
}

/**
 * Needs to be extended to HTTP Request parsing later
 * @brief Called after onReadable() has appended bytes
 *
 */
bool ClientConnection::processIncoming()
{
	(void)this->parseOffset;
	(void)this->bytesErased;
	if (this->state != READ_HEADERS)
		return false;
	// TODO: parse method/target/Host from inBuffer. For now, we rely on HttpRequest
	// parser state (req) to determine if a body is expected and whether it's complete.

	if (req.getState() == BODY)
	{
		size_t expected = req.getBodyLength();
		size_t received = req.getBody().size();
		if (expected > 0 && received < expected)
			return false;
		if (expected == 0 && req.getState() == BODY)
			return false;
	}
	if (server && vs_idx >= 0)
	{
		bool ok = server->getPipeline()->processRequest(server->getConfig(), vs_idx, req, res);
		if (!ok)
		{
			std::string resp = makeErrorResponse();
			outBuffer.assign(resp.begin(), resp.end());
			flow.setWriteLinger(false);
			changeState(WRITE);
			resetDeadlineForWrite();
			return false;
		}

		// If pipeline populated a response (headers or body), use it. Otherwise fall
		// back to the simple hello response to preserve previous behavior in tests.
		bool hasResponse = (res.body.size() > 0) || (res.headers.getLength() > 0);
		if (hasResponse)
		{
			std::ostringstream oss;
			oss << "HTTP/1.1 200 OK\r\n";
			for (std::map<std::string, std::string, CiLess>::const_iterator it = res.headers.getBegin(); it != res.headers.getEnd(); ++it)
				oss << it->first << ": " << it->second << "\r\n";
			if (res.body.size() > 0 && !res.headers.keyExists("Content-Length"))
				oss << "Content-Length: " << res.body.size() << "\r\n";
			oss << "\r\n";
			const std::string head = oss.str();
			outBuffer.assign(head.begin(), head.end());
			outBuffer.insert(outBuffer.end(), res.body.begin(), res.body.end());
		}
		else
		{
			std::string resp = makeHelloResponse(req.keepAlive());
			outBuffer.assign(resp.begin(), resp.end());
		}

		if (!flow.isReadPaused() && outBuffer.size() >= HIGH_WATER)
			flow.setReadPaused(true);
		flow.setWriteLinger(false);
		changeState(WRITE);
		resetDeadlineForWrite();
		return true;
	}

	// No server -> default hello response
	std::string resp = makeHelloResponse(req.keepAlive());
	outBuffer.assign(resp.begin(), resp.end());
	if (!flow.isReadPaused() && outBuffer.size() >= HIGH_WATER)
		flow.setReadPaused(true);
	flow.setWriteLinger(false);
	changeState(WRITE);
	resetDeadlineForWrite();

	return true;
}

bool ClientConnection::headersComplete(const std::vector<char> &buf, HttpRequest &request)
{
	if (!request.parse(buf.data(), buf.size()))
		return (false);
	if (request.getState() <= HEADER || request.getState() == ERROR)
		return (false);
	// Update keep-alive flag based on Connection header if present.
	// HTTP/1.1 defaults to keep-alive, HTTP/1.0 defaults to close unless explicitly asked.
	if (request.getHeaders().keyExists(HDR_CONNECTION))
	{
		std::string val = request.getHeaders().get(HDR_CONNECTION);
		// lowercase
		for (size_t i = 0; i < val.size(); ++i)
			if (val[i] >= 'A' && val[i] <= 'Z')
				val[i] = val[i] - 'A' + 'a';
		if (val == "keep-alive")
			request.setKeepAlive(true);
		else if (val == "close")
			request.setKeepAlive(false);
	}
	else
	{
		// No Connection header: default based on HTTP version
		if (request.getHttpVer() == "HTTP/1.1")
			request.setKeepAlive(true);
		else
			request.setKeepAlive(false);
	}

	// Analyze headers to determine body handling mode and other markers.
	analyzeHeaders(request);
	HttpResponse tmp; // temp buffer for errors
	// if (!evaluate_request_policy(request, ctx, tmp))
	// 		return;

	// Note: Expect: 100-continue handling and streaming decisions are left to
	// processIncoming for now; analyzeHeaders only sets markers for future use.
	return true;
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

bool ClientConnection::send_keepalive(const HttpResponse &r)
{
	std::ostringstream ss;
	ss << r;
	const std::string s = ss.str();
	if (!fd)
		return false;
	ssize_t n = ::send(fd.get(), s.data(), s.size(), MSG_NOSIGNAL);
	(void)n; // assume queued or sent
	return true;
}

bool ClientConnection::send_and_close(const HttpResponse &r)
{
	bool ok = send_keepalive(r);
	this->close(); // or scheduleClose() if you have one
	return ok;
}

bool ClientConnection::evaluate_request_policy(HttpRequest &req, RequestContext &ctx, HttpResponse &res)
{
	(void)req;
	(void)ctx;
	(void)res;
	// Version
	// if (req.http_version != "HTTP/1.1")
	// {
	// 	res = ResponseFactory::makeError(505, "HTTP Version Not Supported");
	// 	return send_and_close(res), false;
	// }

	// // Host required for 1.1
	// if (!req.getHeaders().has("host"))
	// {
	// 	res = ResponseFactory::makeError(400, "Bad Request");
	// 	return send_and_close(res), false;
	// }
	// ctx.vs = Router::selectVirtualServer(req.getHeaders().get("host"));
	// ctx.loc = Router::matchLocation(*(ctx.vs), req.raw_path); 

	// if (!ctx.loc || !ctx.loc->methodAllowed(req.method))
	// {
	// 	res = ResponseFactory::makeError(405, "Method Not Allowed");
	// 	res.headers.set("Allow", ctx.loc ? ctx.loc->allowedMethodsString() : "GET, HEAD");
	// 	return send_and_close(res), false;
	// }

	// // Body policy
	// bool hasCL = req.getHeaders().has("content-length");
	// bool chunked = false;
	// const std::string *te = req.getHeaders().get("transfer-encoding");
	// if (te)
	// {
	// 	std::string x = *te;
	// 	for (size_t i = 0; i < x.size(); ++i)
	// 		if (x[i] >= 'A' && x[i] <= 'Z')
	// 			x[i] += 32;
	// 	if (x.find("chunked") != std::string::npos)
	// 		chunked = true;
	// }

	// if (hasCL && chunked)
	// {
	// 	res = ResponseFactory::makeError(400, "Bad Request");
	// 	return send_and_close(res), false;
	// }

	// if (req.methodNeedsBody() && !hasCL && !chunked)
	// {
	// 	res = ResponseFactory::makeError(411, "Length Required");
	// 	return send_and_close(res), false;
	// }

	// size_t cl = 0;
	// if (hasCL)
	// {
	// 	const std::string *clv = req.getHeaders().get("content-length");
	// 	if (!clv || clv->empty())
	// 	{
	// 		res = ResponseFactory::makeError(400, "Bad Request");
	// 		return send_and_close(res), false;
	// 	}
	// 	for (size_t i = 0; i < clv->size(); ++i)
	// 	{
	// 		char c = (*clv)[i];
	// 		if (c < '0' || c > '9')
	// 		{
	// 			res = ResponseFactory::makeError(400, "Bad Request");
	// 			return send_and_close(res), false;
	// 		}
	// 		cl = cl * 10 + (c - '0');
	// 	}
	// 	// Early size limit
	// 	if (ctx.loc && cl > ctx.loc->max_body_size)
	// 	{
	// 		res = ResponseFactory::makeError(413, "Payload Too Large");
	// 		return send_and_close(res), false;
	// 	}
	// }

	// // Expect: 100-continue (send only if we WILL accept a body)
	// const std::string *ex = req.getHeaders().get("expect");
	// if (ex)
	// {
	// 	std::string y = *ex;
	// 	for (size_t i = 0; i < y.size(); ++i)
	// 		if (y[i] >= 'A' && y[i] <= 'Z')
	// 			y[i] += 32;
	// 	if (y == "100-continue")
	// 	{
	// 		static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
	// 		if (fd)
	// 			::send(fd.get(), cont, sizeof(cont) - 1, MSG_NOSIGNAL);
	// 	}
	// }

	return true; // proceed to read body (CL/chunked) or handle no-body
}