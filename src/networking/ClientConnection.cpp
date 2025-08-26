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


#include "CgiProcess.h"   // make sure this is reachable here
#include <vector>
#include <string>
#include <sstream>   // std::istringstream, std::ostringstream
#include <cstdlib>   // std::atoi, std::atol
#include <cstring>   // std::strlen




// Parse CGI headers from buf. On success, removes headers from buf,
// sets status (default 200), and sets content_len (or -1 if unknown).
// ---- keep this near the top of ClientConnection.cpp ----
static bool parseCgiHeaders(std::string& buf, int& status, long& content_len) {
    std::string::size_type p = buf.find("\r\n\r\n");
    if (p == std::string::npos) return false;

    std::string head = buf.substr(0, p);
    std::string body = buf.substr(p + 4);
    status = 200;
    content_len = -1;

    std::istringstream is(head);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
        std::string::size_type c = line.find(':');
        if (c == std::string::npos) continue;

        std::string k = line.substr(0, c);
        std::string v = line.substr(c + 1);
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t'))
            v.erase(0, 1);

        if (k == "Status") {
            int s = std::atoi(v.c_str());
            if (s >= 100 && s <= 599) status = s;
        } else if (k == "Content-Length") {
            long L = std::atol(v.c_str());
            if (L >= 0) content_len = L;
        }
        // (Other CGI headers ignored here; you can extend later.)
    }

    buf.swap(body);
    return true;
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



u_int64_t ClientConnection::nowMs() {
    struct timeval tv; gettimeofday(&tv, 0);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

void ClientConnection::onTick() {
    if (state == CLOSE || !fd) return;

    // If we’re in an active CGI phase, enforce its deadline separately
    if (cgi_active) {
        if (nowMs() > cgi_deadline) {
            // kill CGI and close connection; tests only require the close
            if (cgi_in_fd >= 0) { ::close(cgi_in_fd); cgi_in_fd = -1; }
            if (cgi_out_fd >= 0) { ::close(cgi_out_fd); cgi_out_fd = -1; }
            proc.terminate();
            close();
            return;
        }
    } else {
        // regular header/body/write phases
        if (expired()) {
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

bool ClientConnection::beginCgi(const CgiSpec& spec,
                                const std::string& script_path,
                                const std::vector<std::string>& envv)
{
    // --- argv = [bin, script, NULL] ---
    std::vector<char*> argvv;
    argvv.push_back(const_cast<char*>(spec.bin.c_str()));
    argvv.push_back(const_cast<char*>(script_path.c_str()));
    argvv.push_back(0);

    // --- envp = ["K=V", ... , NULL] ---
    std::vector<char*> envp;
    envp.reserve(envv.size() + 1);
    for (size_t i = 0; i < envv.size(); ++i)
        envp.push_back(const_cast<char*>(envv[i].c_str()));
    envp.push_back(0);

    // --- spawn the CGI process (non-blocking pipes etc.) ---
    if (!proc.spawn(spec.bin, script_path, &argvv[0], &envp[0], spec.timeout_ms)) {
        // Build a minimal 500 response directly into outBuffer
        static const char* body = "CGI spawn failed\n";

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
    cgi_active       = true;
    cgi_in_fd        = proc.inFD();
    cgi_out_fd       = proc.outFD();
    cgi_body_off     = 0;
    cgi_buf.clear();
    cgi_headers_done = false;
    cgi_status       = 200;
    cgi_content_len  = -1;
    cgi_deadline     = CgiProcess::nowMs() + (unsigned long long)spec.timeout_ms;

    // Pause socket reads while we push the request body into the CGI stdin
    setReadPaused(true);

    // (If your EventLoop API requires explicit registration, do it here)
    // Example:
    // EventLoop& loop = server->loop();
    // if (cgi_in_fd  >= 0) loop.addFD(cgi_in_fd,  /*read*/false, /*write*/true,  this);
    // if (cgi_out_fd >= 0) loop.addFD(cgi_out_fd, /*read*/true,  /*write*/false, this);

    return true; // success, CGI is now active and will be serviced by onReadable/onWritable
}




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

/**
 *  Just a placeholder until he have a proper Parser
 *  Look for \r\n\r\n using parseOffset to avoid rescanning.
 */
static bool headersComplete(const std::vector<char> &buf, HttpRequest &request)
{
	if (!request.parse(buf.data(), buf.size()))

		return (false);

	if (request.getState() <= HEADER || request.getState() == ERROR)
	{

		return (false);
	}
	// changeState(READ_BODY);
	if(request.getHeaders().keyExists(HDR_CONNECTION))
	{	if( request.keepAlive() || request.getHeaders().get(HDR_CONNECTION) == "keep-alive")
			request.setKeepAlive(true);
		else if (request.getHeaders().get(HDR_CONNECTION) == "closed")
			request.setKeepAlive(false);
	}
	return true;
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

	if (headersComplete(inBuffer, req))
	{
		// TODO: parse method/target/Host from inBuffer. For now, placeholders:

		const int local_port = get_local_port(fd.get());
		vs_idx = -1;
		if (server && local_port > 0)
		{
			vs_idx = server->resolveVirtualServerByPort(local_port, "localhost");
		}
		// RouteDecision plan; // defaults HK_ERROR/500
		if (server && vs_idx >= 0)
		{
			// Router::makeDecisionForVS(server->getConfig(), vs_idx, "GET", "/", plan);
			if (!server->getPipeline()->processRequest(server->getConfig(), vs_idx, req, res))
			{
				std::string resp = makeErrorResponse();
				outBuffer.assign(resp.begin(), resp.end());
				writeLingerArmed = false;
				changeState(WRITE);
				resetDeadlineForWrite();

				return false;
			}
		}
		std::string resp = makeHelloResponse();
		outBuffer.assign(resp.begin(), resp.end());
		if (!readPaused && outBuffer.size() >= HIGH_WATER)
    		readPaused = true;
		writeLingerArmed = false;
		changeState(WRITE);
		resetDeadlineForWrite();
		return true;
	}
	return false;
}

void ClientConnection::onReadable()
{
	readFromSocket();
	// processIncoming();
}

void ClientConnection::onWritable()
{
    if (state != WRITE || !fd) return;

    const size_t total = outBuffer.size();
    const char*  base  = total ? &outBuffer[0] : 0;

    // If nothing left to send:
    if (outOffset >= total) {
        if (writeLingerArmed) {
            // second writable with nothing to send -> close now
            close();
        } else {
            // first time we see fully flushed: arm a one-shot linger
            // so the handler will keep POLLOUT and poll will fire again,
            // then we'll close on that next writable.
            writeLingerArmed = true;
            // keep the write deadline alive a tiny bit
            bumpDeadline(WRITE_TIMEOUT_MS);
        }
        return;
    }

    const char* p    = base + outOffset;
    size_t      left = total - outOffset;

    ssize_t n = ::send(fd.get(), p, left, MSG_NOSIGNAL);
    if (n > 0) {
        outOffset += static_cast<size_t>(n);
        bumpDeadline(WRITE_TIMEOUT_MS);

        // backpressure: maybe resume reads
        size_t remaining = total - outOffset;
        if (readPaused && remaining <= LOW_WATER)
            readPaused = false;

        if (outOffset >= total) {
            // fully flushed on this call: do NOT close immediately
            writeLingerArmed = true; // ask for one more POLLOUT to close
        }
        return;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return; // try later

    // hard error
    close();
}



void ClientConnection::onCgiWritable(int fd) {
    if (!cgi_active || fd != cgi_in_fd) return;

    const std::vector<char>& body = req.getBody(); // <-- your API returns vector<char>
    if (cgi_body_off >= body.size()) {
        // nothing left to send; close stdin to signal EOF to CGI
        proc.closeIn();
        cgi_in_fd = -1;
        return;
    }

    const char* data = body.empty() ? NULL : &body[0];
    size_t left = body.size() - cgi_body_off;
    ssize_t n = ::write(cgi_in_fd, data + cgi_body_off, left);
    if (n > 0) {
        cgi_body_off += static_cast<size_t>(n);
        // keep stdin open until we finish sending everything
        if (cgi_body_off == body.size()) {
            proc.closeIn();
            cgi_in_fd = -1;
        }
        // refresh deadline on progress
        cgi_deadline = CgiProcess::nowMs() + (WRITE_TIMEOUT_MS);
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        proc.closeIn();
        cgi_in_fd = -1;
    }
}

void ClientConnection::onCgiReadable(int fd) {
    if (!cgi_active || fd != cgi_out_fd) return;

    char buf[8192];
    ssize_t n = ::read(cgi_out_fd, buf, sizeof(buf));
    if (n > 0) {
        cgi_buf.append(buf, buf + n);
        // Try to parse CGI headers once
        if (!cgi_headers_done) {
            if (parseCgiHeaders(cgi_buf, cgi_status, cgi_content_len)) {
                cgi_headers_done = true;
            }
        }
        // progress => refresh deadline
        cgi_deadline = CgiProcess::nowMs() + WRITE_TIMEOUT_MS;
        return;
    }
    if (n == 0) {
        // EOF on stdout
        proc.closeOut();
        cgi_out_fd = -1;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        proc.closeOut();
        cgi_out_fd = -1;
    }

    // If both FDs are closed, the child likely finished; reap it.
    int status = 0;
    int rc = proc.waitNonBlocking(&status);
    if ((cgi_in_fd < 0 && cgi_out_fd < 0) || rc > 0) {
        // Finalize response.
        if (!cgi_headers_done) {
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




void ClientConnection::readFromSocket()
{
	if (this->state != READ_HEADERS  || !fd)
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
			if (processIncoming())
				return;
			inBuffer.clear();
			continue;
		}

		if (n == 0)
		{
			if (processIncoming())
				return;
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
