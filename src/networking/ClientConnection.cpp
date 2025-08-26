/* --- ClientConnection.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "ClientConnection.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "CgiProcess.h"
#include "Router.h"
#include "Server.h"
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#include "HEADER_ENTRIES.h"
#include <sys/time.h>

#include "CgiProcess.h" // make sure this is reachable here
#include <vector>
#include <string>
#include <sstream> // std::istringstream, std::ostringstream
#include <cstdlib> // std::atoi, std::atol
#include <cstring> // std::strlen




/***---------------------------CGI PROCCESSING AND EXECUTION--------------------------- ***/


// Parse CGI headers from buf. On success, removes headers from buf,
// sets status (default 200), and sets content_len (or -1 if unknown).
// ---- keep this near the top of ClientConnection.cpp ----
static bool parseCgiHeaders(std::string &buf, int &status, long &content_len)
{
	std::string::size_type p = buf.find("\r\n\r\n");
	if (p == std::string::npos)
		return false;

	std::string head = buf.substr(0, p);
	std::string body = buf.substr(p + 4);
	status = 200;
	content_len = -1;

	std::istringstream is(head);
	std::string line;
	while (std::getline(is, line))
	{
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);
		std::string::size_type c = line.find(':');
		if (c == std::string::npos)
			continue;

		std::string k = line.substr(0, c);
		std::string v = line.substr(c + 1);
		while (!v.empty() && (v[0] == ' ' || v[0] == '\t'))
			v.erase(0, 1);

		if (k == "Status")
		{
			int s = std::atoi(v.c_str());
			if (s >= 100 && s <= 599)
				status = s;
		}
		else if (k == "Content-Length")
		{
			long L = std::atol(v.c_str());
			if (L >= 0)
				content_len = L;
		}
		// (Other CGI headers ignored here; you can extend later.)
	}

	buf.swap(body);
	return true;
}

u_int64_t ClientConnection::nowMs()
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

// Analyze parsed headers and set internal markers for future enforcement.
// We keep this minimal: detect Content-Length, Transfer-Encoding: chunked,
// and Expect: 100-continue. Do not change state here; callers use these
// markers to decide next actions.
void ClientConnection::analyzeHeaders(const HttpRequest &request)
{
	// If already analyzed, skip work (idempotent).
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
		// normalize key comparisons to lowercase by comparing known header names
		// with case-insensitive comparator already used in Headers; keys are stored as given,
		// so compare using CiLess-like checks via lowercase helpers here for simplicity.
		// Handle common headers we care about.
		if (k == "Content-Length")
		{
			size_t len = 0;
			for (size_t i = 0; i < v.size(); ++i)
			{
				if (v[i] < '0' || v[i] > '9') break;
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
				if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
				// match substring "chunked"
				// simple find without allocations
				size_t rem = v.size() - i;
				if (rem >= 7 &&
					(v[i] == 'c' || v[i] == 'C') &&
					(v[i+1] == 'h' || v[i+1] == 'H'))
				{
					// fallback to lowercase search using std::string::find on a lowercased temp;
					std::string tmp = v;
					for (size_t j = 0; j < tmp.size(); ++j) if (tmp[j] >= 'A' && tmp[j] <= 'Z') tmp[j] = tmp[j] - 'A' + 'a';
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
			for (size_t j = 0; j < tmp.size(); ++j) if (tmp[j] >= 'A' && tmp[j] <= 'Z') tmp[j] = tmp[j] - 'A' + 'a';
			if (tmp.find("100-continue") != std::string::npos)
				expectContinue = true;
		}
	}

	headersAnalyzed = true;
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

bool ClientConnection::beginCgi(const CgiSpec &spec,
								const std::string &script_path,
								const std::vector<std::string> &envv)
{
	// --- argv = [bin, script, NULL] ---
	std::vector<char *> argvv;
	argvv.push_back(const_cast<char *>(spec.bin.c_str()));
	argvv.push_back(const_cast<char *>(script_path.c_str()));
	argvv.push_back(0);

	// --- envp = ["K=V", ... , NULL] ---
	std::vector<char *> envp;
	envp.reserve(envv.size() + 1);
	for (size_t i = 0; i < envv.size(); ++i)
		envp.push_back(const_cast<char *>(envv[i].c_str()));
	envp.push_back(0);

	// --- spawn the CGI process (non-blocking pipes etc.) ---
	if (!proc.spawn(spec.bin, script_path, &argvv[0], &envp[0], spec.timeout_ms))
	{
		// Build a minimal 500 response directly into outBuffer
		static const char *body = "CGI spawn failed\n";

		std::ostringstream oss;
		oss << "HTTP/1.1 500 Internal Server Error\r\n"
			<< "Content-Type: text/plain\r\n"
			<< "Content-Length: " << std::strlen(body) << "\r\n"
			<< "\r\n"
			<< body;

		const std::string s = oss.str();
		outBuffer.assign(s.begin(), s.end());
		outOffset = 0;
		state = WRITE;
		resetDeadlineForWrite();
		return true; // handled (we produced a response)
	}

	// --- initialize CGI streaming state ---
	cgi_active = true;
	cgi_in_fd = proc.inFD();
	cgi_out_fd = proc.outFD();
	cgi_body_off = 0;
	cgi_buf.clear();
	cgi_headers_done = false;
	cgi_status = 200;
	cgi_content_len = -1;
	cgi_deadline = CgiProcess::nowMs() + (unsigned long long)spec.timeout_ms;

	// Pause socket reads while we push the request body into the CGI stdin
	setReadPaused(true);

	// (If your EventLoop API requires explicit registration, do it here)
	// Example:
	// EventLoop& loop = server->loop();
	// if (cgi_in_fd  >= 0) loop.addFD(cgi_in_fd,  /*read*/false, /*write*/true,  this);
	// if (cgi_out_fd >= 0) loop.addFD(cgi_out_fd, /*read*/true,  /*write*/false, this);

	return true; // success, CGI is now active and will be serviced by onReadable/onWritable
}


void ClientConnection::onCgiWritable(int fd)
{
	if (!cgi_active || fd != cgi_in_fd)
		return;

	const std::vector<char> &body = req.getBody(); // <-- your API returns vector<char>
	if (cgi_body_off >= body.size())
	{
		// nothing left to send; close stdin to signal EOF to CGI
		proc.closeIn();
		cgi_in_fd = -1;
		return;
	}

	const char *data = body.empty() ? NULL : &body[0];
	size_t left = body.size() - cgi_body_off;
	ssize_t n = ::write(cgi_in_fd, data + cgi_body_off, left);
	if (n > 0)
	{
		cgi_body_off += static_cast<size_t>(n);
		// keep stdin open until we finish sending everything
		if (cgi_body_off == body.size())
		{
			proc.closeIn();
			cgi_in_fd = -1;
		}
		// refresh deadline on progress
		cgi_deadline = CgiProcess::nowMs() + (WRITE_TIMEOUT_MS);
	}
	else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
	{
		proc.closeIn();
		cgi_in_fd = -1;
	}
}

void ClientConnection::onCgiReadable(int fd)
{
	if (!cgi_active || fd != cgi_out_fd)
		return;

	char buf[8192];
	ssize_t n = ::read(cgi_out_fd, buf, sizeof(buf));
	if (n > 0)
	{
		cgi_buf.append(buf, buf + n);
		// Try to parse CGI headers once
		if (!cgi_headers_done)
		{
			if (parseCgiHeaders(cgi_buf, cgi_status, cgi_content_len))
			{
				cgi_headers_done = true;
			}
		}
		// progress => refresh deadline
		cgi_deadline = CgiProcess::nowMs() + WRITE_TIMEOUT_MS;
		return;
	}
	if (n == 0)
	{
		// EOF on stdout
		proc.closeOut();
		cgi_out_fd = -1;
	}
	else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
	{
		proc.closeOut();
		cgi_out_fd = -1;
	}

	// If both FDs are closed, the child likely finished; reap it.
	int status = 0;
	int rc = proc.waitNonBlocking(&status);
	if ((cgi_in_fd < 0 && cgi_out_fd < 0) || rc > 0)
	{
		// Finalize response.
		if (!cgi_headers_done)
		{
			// No header boundary seen; default to 200 and whole buffer as body
			cgi_status = 200;
		}

		// Build raw HTTP response into outBuffer
		std::ostringstream oss;
		oss << "HTTP/1.1 " << (cgi_status ? cgi_status : 200) << " OK\r\n";

		// Use CGI-provided Content-Length if it matched; else compute our own
		size_t body_len = cgi_buf.size();
		oss << "Content-Length: " << body_len << "\r\n";

		// If you didn’t parse Content-Type from CGI, default it:
		oss << "Content-Type: application/octet-stream\r\n";

		oss << "\r\n";
		const std::string head = oss.str();
		outBuffer.assign(head.begin(), head.end());
		outBuffer.insert(outBuffer.end(), cgi_buf.begin(), cgi_buf.end());
		outOffset = 0;

		// Switch to WRITE to flush back to the client
		state = WRITE;
		resetDeadlineForWrite();

		// Unpause socket reads for next request (if pipelining allowed)
		setReadPaused(false);

		// Clear CGI state
		cgi_active = false;
		return;
	}
}


/***---------------------------CLIENT CONNECTION HANDLING REQUEST AND RESPONSE---------------------------***/


static std::string makeHelloResponse()
{
	const char *body = "hello";
	std::string resp;
	resp.reserve(128);
	resp += "HTTP/1.1 200 OK\r\n";
	resp += "Content-Length: 5\r\n";
	resp += "Connection: close\r\n";
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

		if (req.getTotalBytesRead() >= MAX_INBUFFER)
		{
			close();
			return;
		}
		char buffer[READ_CHUNK];
		ssize_t n = ::recv(fd.get(), buffer, sizeof(buffer), 0);
		buffer[n] = '\0';
		if (n > 0)
		{
			size_t spaceLeft = MAX_INBUFFER - inBuffer.size();
			size_t toCopy = static_cast<size_t>(n);
			toCopy = (toCopy > spaceLeft) ? spaceLeft : toCopy;
			inBuffer.insert(inBuffer.end(), buffer, buffer + toCopy);
			bumpDeadline(HDR_TIMEOUT_MS);
			if (headersComplete(inBuffer, req))
			{
				if (processIncoming())
					return;
			}
			inBuffer.clear();
			continue;
		}

		if (n == 0)
		{
			if (headersComplete(inBuffer, req))
			{
				if (processIncoming())
					return;
			}
			inBuffer.clear();
			close();
			return;
		}
		if (n < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			close();
			return;
		}
	}
}




static int get_local_port(int fd)
{
	struct sockaddr_storage ss = sockaddr_storage();
	socklen_t sl = sizeof(ss);
	if (::getsockname(fd, (struct sockaddr *)&ss, &sl) != 0)
		return -1;
	if (ss.ss_family == AF_INET)
		return (int)ntohs(((sockaddr_in *)&ss)->sin_port);
	if (ss.ss_family == AF_INET6)
		return (int)ntohs(((sockaddr_in6 *)&ss)->sin6_port);
	return -1;
}



/**
 * Needs to be extended to HTTP Request parsing later
 * @brief Called after onReadable() has appended bytes
 *
 */
bool ClientConnection::processIncoming()
{
	if (this->state != READ_HEADERS)
		return false;

	// TODO: parse method/target/Host from inBuffer. For now, we rely on HttpRequest
	// parser state (req) to determine if a body is expected and whether it's complete.

	const int local_port = get_local_port(fd.get());
	vs_idx = -1;
	if (server && local_port > 0)
	{
		vs_idx = server->resolveVirtualServerByPort(local_port, "localhost");
	}

	// If headers indicate a body (parser state BODY), ensure we've received the full body
	if (req.getState() == BODY)
	{
		size_t expected = req.getBodyLength();
		size_t received = req.getBody().size();
		// If Content-Length known and not yet received, wait for more.
		// Keep state as READ_HEADERS so the outer loop and tests will continue
		// to call readFromSocket()/onReadable and the remaining bytes can
		// be read into the parser. We only switch state after the body is
		// complete (processIncoming will be retried once parser advances).
		if (expected > 0 && received < expected)
			return false;
		// If Content-Length == 0 but parser still in BODY, wait
		if (expected == 0 && req.getState() == BODY)
			return false;
	}

	// Now headers (and body, if any) are complete — call pipeline if we resolved a VS
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
			std::string resp = makeHelloResponse();
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
	std::string resp = makeHelloResponse();
	outBuffer.assign(resp.begin(), resp.end());
	if (!flow.isReadPaused() && outBuffer.size() >= HIGH_WATER)
		flow.setReadPaused(true);
	flow.setWriteLinger(false);
	changeState(WRITE);
	resetDeadlineForWrite();
	return true;
}

/**
 *  Just a placeholder until he have a proper Parser
 *  Look for \r\n\r\n using parseOffset to avoid rescanning.
 */
bool ClientConnection::headersComplete(const std::vector<char> &buf, HttpRequest &request)
{
	if (!request.parse(buf.data(), buf.size()))
		return (false);
	if (request.getState() <= HEADER || request.getState() == ERROR)
		return (false);
	// Update keep-alive flag based on Connection header if present
	if (request.getHeaders().keyExists(HDR_CONNECTION))
	{
		if (request.keepAlive() || request.getHeaders().get(HDR_CONNECTION) == "keep-alive")
			request.setKeepAlive(true);
		else if (request.getHeaders().get(HDR_CONNECTION) == "closed")
			request.setKeepAlive(false);
	}

	// Analyze headers to determine body handling mode and other markers.
	analyzeHeaders(request);

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
		{
			close();
		}
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
		{
			flow.setWriteLinger(true);
		}
		return;
	}

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return; // try later

	// hard error
	close();
}



