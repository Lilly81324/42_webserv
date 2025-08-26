/* --- ClientConnection.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef CLIENTCONNECTION_H
#define CLIENTCONNECTION_H

#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include "CgiProcess.h"
#include "UniqueFD.h"
#include "Router.h"
#include "RouteResolver.h"
#include "HttpRequest.h"
#include "HttpResponse.h"

#include "DeadlineManager.h"
#include "FlowControl.h"

class Server;

enum State
{
	READ_HEADERS,
	READ_BODY,
	BODY_COMPLETE,
	WRITE,
	CLOSE,
};

struct CgiState
{
	CgiProcess proc;
	int in_fd;
	int out_fd;
	size_t body_off;
	std::string buf;
	bool headers_done;
	int status;
	long content_len;
	unsigned long long deadline;
};

class ClientConnection
{
	private:
		enum State state;
		UniqueFD fd;
		std::vector<char> inBuffer;
		std::vector<char> outBuffer;
		size_t parseOffset;
		size_t outOffset;
		size_t bytesErased;
		const Server *server;
		int vs_idx;

		// extracted helpers
		DeadlineManager phaseDeadline;
		FlowControl flow;

		HttpRequest req;
		HttpResponse res;

		// ---- markers discovered after parsing headers ----
		enum BodyMode
		{
			BM_NONE,
			BM_CONTENT_LENGTH,
			BM_CHUNKED,
			BM_UNKNOWN
		};
		BodyMode bodyMode;
		size_t expectedContentLength;
		bool expectContinue;
		bool transferChunked;
		bool headersAnalyzed;

		// ---- I/O limits ----
		static const size_t READ_CHUNK = 8192;
		static const size_t MAX_INBUFFER = (1u << 20);

		// helpers
		static u_int64_t nowMs();

		// read-from-socket helpers (extracted from readFromSocket for clarity)
		bool handleRecvPositive(ssize_t n, char *buffer);
		bool handleRecvZero();
		bool handleRecvError(ssize_t n);
		void createBodyTempFileIfNeeded();
		void writeParsedBytesToBodyFile();

		inline void resetDeadlineForHeaders() { phaseDeadline.reset(nowMs(), HDR_TIMEOUT_MS); }
		inline void resetDeadlineForBody() { phaseDeadline.reset(nowMs(), BODY_TIMEOUT_MS); }
		inline void resetDeadlineForWrite() { phaseDeadline.reset(nowMs(), WRITE_TIMEOUT_MS); }
		inline void bumpDeadline(int ms) { phaseDeadline.bump(nowMs(), ms); }
		inline bool expired() const { return phaseDeadline.expired(nowMs()); }

		void readFromSocket();
		bool processIncoming();
		bool headersComplete(const std::vector<char> &buf, HttpRequest &request);
		void analyzeHeaders(const HttpRequest &request);

	public:
		static const int HDR_TIMEOUT_MS = 10000;
		static const int BODY_TIMEOUT_MS = 20000;
		static const int WRITE_TIMEOUT_MS = 10000;
		static const unsigned POST_WRITE_LINGER_MS = 100;

		static const size_t HIGH_WATER = 256u * 1024u;
		static const size_t LOW_WATER = 64u * 1024u;

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

		bool wantsWriteLinger() const { return flow.getWriteLinger(); }

		explicit ClientConnection(int fd_)
			: state(READ_HEADERS), fd(fd_), inBuffer(), outBuffer(), parseOffset(0), outOffset(0), bytesErased(0), server(0), vs_idx(-1),
			phaseDeadline(), flow(), req(), res(), bodyMode(BM_NONE), expectedContentLength(0), expectContinue(false), transferChunked(false), headersAnalyzed(false),
			proc(), cgi_active(false), cgi_in_fd(-1), cgi_out_fd(-1), cgi_body_off(0), cgi_buf(), cgi_headers_done(false), cgi_status(200), cgi_content_len(-1), cgi_deadline(0ULL)
		{
			phaseDeadline.reset(nowMs(), HDR_TIMEOUT_MS);
			flow = FlowControl(HIGH_WATER, LOW_WATER);
		}

		explicit ClientConnection(int fd, const Server *srv)
			: state(READ_HEADERS), fd(fd), inBuffer(), outBuffer(), parseOffset(0), outOffset(0), bytesErased(0), server(srv), vs_idx(-1),
			phaseDeadline(), flow(), req(), res(), bodyMode(BM_NONE), expectedContentLength(0), expectContinue(false), transferChunked(false), headersAnalyzed(false),
			proc(), cgi_active(false), cgi_in_fd(-1), cgi_out_fd(-1), cgi_body_off(0), cgi_buf(), cgi_headers_done(false), cgi_status(200), cgi_content_len(-1), cgi_deadline(0ULL)
		{
			phaseDeadline.reset(nowMs(), HDR_TIMEOUT_MS);
			flow = FlowControl(HIGH_WATER, LOW_WATER);
		}

		~ClientConnection() {}

		bool wantsRead() const { return state == READ_HEADERS && !isReadPaused(); }
		bool hasPendingWrite() const { return outOffset < outBuffer.size(); }
		bool isReadPaused() const { return flow.isReadPaused(); }
		void setReadPaused(bool v) { flow.setReadPaused(v); }

		State getState() { return this->state; }

		void onReadable();
		void onWritable();
		void changeState(State);
		void onTick();

		int getFD() const { return fd.get(); }
		bool isClosed() const { return !fd; }
		bool wantsWrite() const { return state == WRITE; }

		void close();

		bool beginCgi(const CgiSpec &spec, const std::string &script_path, const std::vector<std::string> &envv);

		void onCgiReadable(int fd);
		void onCgiWritable(int fd);

	#ifdef UNIT_TEST
	public:
		std::vector<char> &getInBuffer() { return inBuffer; }
		std::vector<char> &getOutBuffer() { return outBuffer; }
		size_t &getParseOffset() { return parseOffset; }
		void setState(State state) { this->state = state; }
		bool processIncoming(std::string ok)
		{
			(void)ok;
			return this->processIncoming();
		}
		size_t getparseOffset() { return this->parseOffset; }
	#endif
};

#endif // CLIENTCONNECTION_H
