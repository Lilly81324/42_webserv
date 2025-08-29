#include "ClientConnection.h"
#include "Server.h"
#include "HEADER_ENTRIES.h"
#include <limits.h>
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
		// open file and move already-parsed			 body bytes into it
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
    if (headerEnd_ == (size_t)-1) return;
    const size_t available = inBuffer.size() - (headerEnd_ + 1);
    if (available == 0) return;

    const char* bodyPtr = &inBuffer[headerEnd_ + 1];
    if (bodyFd_ != -1) {
        if (!appendToBodyFile(bodyPtr, available)) {
            send_and_close(507, "Insufficient Storage");
            return;
        }
    } else {
        appendToBodyBuffer(bodyPtr, available);
    }

    // Free the already-consumed prefix to save memory:
    // keep only the body bytes (since headers were parsed)
    std::vector<char> tmp;
    tmp.insert(tmp.end(), bodyPtr, bodyPtr + available);
    inBuffer.swap(tmp);
    // Reset indices relative to new buffer
    headerEnd_ = (size_t)-1;
    bodyStart_ = 0;
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