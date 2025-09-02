#include "CGIStreamer.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <stdexcept>
#include <unistd.h>

/***---------------------------CGI PROCCESSING AND EXECUTION--------------------------- ***/

CGIStreamer::CGIStreamer( HttpRequest& req, HttpResponse& res)
    : req(req), res(res), cgi_active(false), cgi_in_fd(-1), cgi_out_fd(-1),
      cgi_body_off(0), cgi_headers_done(false), cgi_status(200), cgi_content_len(-1),
      cgi_deadline(0), outOffset(0), state(READ) {
    // Initialize the output buffer
    outBuffer.clear();
}

// Parse CGI headers from buf. On success, removes headers from buf,
// sets status (default 200), and sets content_len (or -1 if unknown).
// ---- keep this near the top of CGIStreamer.cpp ----
bool CGIStreamer::parseCgiHeaders(std::string &buf, int &status, long &content_len)
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

// Analyze parsed headers and set internal markers for future enforcement.
// We keep this minimal: detect Content-Length, Transfer-Encoding: chunked,
// and Expect: 100-continue. Do not change state here; callers use these
// markers to decide next actions.

bool CGIStreamer::beginCgi(const CgiSpec &spec,
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
	// if (cgi_in_fd  >= 0) loop.addFD(cgi_in_fd,  /*read*/false, /*write*/true,  this);
	// if (cgi_out_fd >= 0) loop.addFD(cgi_out_fd, /*read*/true,  /*write*/false, this);

	return true; // success, CGI is now active and will be serviced by onReadable/onWritable
}

void CGIStreamer::resetDeadlineForWrite()
{
	cgi_deadline = CgiProcess::nowMs() + WRITE_TIMEOUT_MS;
}

void CGIStreamer::onCgiWritable(int fd)
{
	if (!cgi_active || fd != cgi_in_fd)
		return;
	// Support file-backed request body: stream from file if enabled.
	if (req.isBodyOnDisk())
	{
		const std::string path = req.getBodyFilePath();
		std::ifstream ifs(path.c_str(), std::ios::in | std::ios::binary);
		if (!ifs)
		{
			// nothing we can do; close stdingi
			proc.closeIn();
			cgi_in_fd = -1;
			return;
		}
		// seek to current offset
		ifs.seekg(static_cast<std::streamoff>(cgi_body_off));
		char tbuf[8192];
		ifs.read(tbuf, sizeof(tbuf));
		std::streamsize got = ifs.gcount();
		if (got <= 0)
		{
			proc.closeIn();
			cgi_in_fd = -1;
			return;
		}
		ssize_t n = ::write(cgi_in_fd, tbuf, static_cast<size_t>(got));
		if (n > 0)
		{
			cgi_body_off += static_cast<size_t>(n);
			if (static_cast<size_t>(n) < static_cast<size_t>(got))
			{
				// partial write; we'll continue later
			}
			if (req.getBodyLength() > 0 && cgi_body_off >= req.getBodyLength())
			{
				proc.closeIn();
				cgi_in_fd = -1;
			}
			cgi_deadline = CgiProcess::nowMs() + (WRITE_TIMEOUT_MS);
			return;
		}
		else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
		{
			proc.closeIn();
			cgi_in_fd = -1;
			return;
		}
		return;
	}

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

void CGIStreamer::onCgiReadable(int fd)
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