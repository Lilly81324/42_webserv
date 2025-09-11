#ifndef CGISTREAMER_H
#define CGISTREAMER_H

#include "CgiProcess.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "EventLoop.h"
#include <string>
#include <vector>

class CGIStreamer
{
public:
	// Constructor
	CGIStreamer( HttpRequest &req, HttpResponse &res);

	// Starts the CGI process
	bool beginCgi(const CgiSpec &spec, const std::string &script_path, const std::vector<std::string> &envv);

	// Event handlers for readable and writable events
	void onCgiReadable(int fd);
	void onCgiWritable(int fd);

	int cgiStdoutFD(){return cgi_out_fd;}
	int cgiStdinFD() {return cgi_in_fd;}

private:
	// Helper to parse CGI headers
	bool parseCgiHeaders(std::string &buf, int &status, long &content_len);

	// Internal state management
	void resetDeadlineForWrite();
	// void setReadPaused(bool paused);

	// Member variables
	// EventLoop &loop;				 // Reference to the EventLoop
	HttpRequest &req;				 // Reference to the HTTP request
	HttpResponse &res;				 // Reference to the HTTP response
	CgiProcess proc;				 // Manages the CGI process
	bool cgi_active;				 // Whether the CGI process is active
	int cgi_in_fd;					 // File descriptor for CGI stdin
	int cgi_out_fd;					 // File descriptor for CGI stdout
	size_t cgi_body_off;			 // Offset for the request body
	std::string cgi_buf;			 // Buffer for CGI output
	bool cgi_headers_done;			 // Whether CGI headers are parsed
	int cgi_status;					 // HTTP status code from CGI
	long cgi_content_len;			 // Content length from CGI
	unsigned long long cgi_deadline; // Deadline for CGI operations

	// Output buffer for HTTP response
	std::vector<char> outBuffer;
	size_t outOffset;

	// State management
	enum State
	{
		READ,
		WRITE
	};
	State state;

	// Timeout constants
	static const unsigned long long WRITE_TIMEOUT_MS = 5000; // 5 seconds
};

#endif // CGISTREAMER_H
