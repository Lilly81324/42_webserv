#if !defined(CGI_STREAMER_H)
#define CGI_STREAMER_H

#include <sys/types.h>
#include <vector>
#include "CgiProcess.h"
#include "PhaseDeadline.h"

class CGIStreamer
{

private:
	bool parseCgiHeaders(std::string &buf, int &status, long &content_len);

	// After headers parsed, serialize HTTP head and push to out (once).
	void emitHttpHead(ChainBuf& out);

	// If EOF comes before headers, synthesize a minimal head.
	void emitFallbackHead(ChainBuf& out);

	long nb_read_stdout();
	long nb_write_stdin();

public:
	CGIStreamer();
	~CGIStreamer();
	bool beginCgi(const CgiSpec &spec, const std::string &script_path, const std::vector<std::string> &envv);

	void onCgiReadable(int fd);
	void onCgiWritable(int fd);

	void queueStdin(const char *p, std::size_t n);

	bool active() const;
	void terminate();

	bool headersEmitted() const {return headers_emitted;}
	int statusCode() const	{return status_code;}

private:
	// process & fds
	CgiProcess proc;
	bool cgi_active;
	int cgi_in_fd;
	int cgi_out_fd;
	size_t cgi_body_off;
	std::string cgi_buf;
	bool cgi_headers_done;
	int cgi_status;
	long cgi_content_len;
	unsigned long long cgi_deadline;
	int status_code;
	bool headers_emitted;
	PhaseDeadline  dl;
	
};

#endif //  CGI_STREAMER_H
