#include "CGIStreamer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "ChainBuf.h"
#include "CgiProcess.h"
#include "EventLoop.h"
#include <time.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <vector>
#include <poll.h>	  // POLLIN, POLLOUT
#include <fcntl.h>	  // fcntl, O_NONBLOCK, FD_CLOEXEC
#include <signal.h>	  // SIGKILL
#include <sys/stat.h> // fstat
#include <sys/time.h> //
#include <unistd.h>	  // read, write, pread, close
#include <sys/time.h> //

// void CGIStreamer::attachLoop(EventLoop* L) { loop_ = L; }

// --- tiny helper: RFC1123 Date string (GMT) ---

/*

static std::string http_date_now_gmt()

Formats the current time as an RFC-1123 Date header in GMT.
Many CGIs don’t emit complete HTTP headers; adding a correct
Date improves intermediaries’ behavior (caches, proxies) and makes
responses standards-compliant. The helper uses std::time, std::gmtime,
and std::strftime into a fixed buffer—no heap allocations aside from
returning the std::string. Keeping it local and tiny avoids locale
surprises and ensures every synthesized or finalized head can include a proper
date without duplicating code. It’s called from finalizeHeaders()
when the CGI didn’t supply a Date, guaranteeing a consistent header
set for all CGI responses your server forwards.

*/

static std::string http_date_now_gmt()
{
	char buf[64];
	std::time_t t = std::time(0);
	std::tm g = *std::gmtime(&t);
	std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &g);
	return std::string(buf);
}

// --- case-insensitive equals for ASCII keys ---

/*

static bool iequals_ascii(const std::string& a, const std::string& b)

Performs ASCII-only case-insensitive equality—ideal for HTTP header names
which are case-insensitive but ASCII. It short-circuits on length mismatch,
then lowercases A–Z by arithmetic and compares byte-by-byte.
Using a hand-rolled routine is faster and safer here than locale-aware transforms,
and avoids allocations. It’s used in parseOneHeaderLine to recognize Status,
Content-Type, and Set-Cookie regardless of capitalization, so your
CGI header parsing is robust to variant emitters (e.g., content-type, Content-type).
Keeping it local to the translation unit avoids polluting global
utilities and keeps hot-path parsing lean.

*/

static bool iequals_ascii(const std::string &a, const std::string &b)
{
	if (a.size() != b.size())
		return false;
	for (std::string::size_type i = 0; i < a.size(); ++i)
	{
		unsigned char ca = static_cast<unsigned char>(a[i]);
		unsigned char cb = static_cast<unsigned char>(b[i]);
		if (ca >= 'A' && ca <= 'Z')
			ca = char(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z')
			cb = char(cb - 'A' + 'a');
		if (ca != cb)
			return false;
	}
	return true;
}

/*

CGIStreamer::CGIStreamer(HttpRequest&, HttpResponse&) (ctor)

Initializes the streamer for one request/response lifecycle: clears pointers
(event loop loop_), marks CGI inactive, resets pipe fds (-1), request-body streaming
offsets/state (for in-mem or file-backed bodies), header parsing state (HDR_WAITING,
no Content-Type seen, zero status_code_), cookie accumulation, and output framing flags
(http_head_queued_, chunked_mode_, sent_final_chunk_). It also zeros deadlines
(hdr_deadline_, total_deadline_, write_deadline_ms_) and back-pressure hints.
No OS work happens here; spawning and fd registration occur in beginCgi.
This strict zeroing prevents cross-request contamination on keep-alive
connections and makes it safe to reuse an instance if you ever decide to
pool handlers. It sets a predictable baseline so later code can test
booleans and fds directly without extra guards.


*/

CGIStreamer::CGIStreamer(HttpRequest &req, HttpResponse &res)
	: req_(req), res_(res), proc_()
	  // ---- Event loop ----
	  ,
	  loop_(NULL) // will be set via attachLoop(&eventLoop)
	  // ---- CGI pipes / state ----
	  ,
	  cgi_active_(false), failed_(false), cgi_in_fd_(-1), cgi_out_fd_(-1)
	  // ---- Request-body streaming ----
	  ,
	  cgi_body_off_(0), body_fd_(-1), body_file_off_(0), body_path_()
	  // ---- Header parsing ----
	  ,
	  hdr_state_(HDR_WAITING), cgi_header_accum_(), saw_content_type_(false), status_code_(0) // keep 0 if your code interprets 0 as “unset”
	  ,
	  status_reason_(), set_cookie_()
	  // ---- Output framing ----
	  ,
	  http_head_queued_(false), chunked_mode_(false), sent_final_chunk_(false), out_buf_(), out_off_(0)
	  // ---- Deadlines / timeouts ----
	  ,
	  hdr_deadline_(), total_deadline_(), write_deadline_ms_(0)
	  // ---- Back-pressure hint ----
	  ,
	  client_out_bytes_(0)
	  // ---- Read throttling ----
	  ,
	  stdout_paused_(false),

	  // in CGIStreamer.cpp ctor init list
	  mem_body_(),

	  in_mem_body_(),
	  in_mem_off_(0),
	  using_mem_body_(false)

{
	// nothing else to do
}

/*

~CGIStreamer() (dtor)

Ensures descriptor hygiene at teardown by calling closeStdin()
and closeStdout(). The CgiProcess member owns the child’s lifecycle,
but the streamer owns the parent ends registered with the event loop.
Idempotent closing prevents stale poll registrations and spurious wakeups
after request completion or error. Keeping the destructor side-effect-free
beyond cleanup (no throwing, no heavy I/O) makes shutdown during
loop drain safe and predictable, and ensures that even if higher layers
forget to stop listening to the pipes, destruction will leave no dangling fds.


*/

CGIStreamer::~CGIStreamer()
{
	closeStdin();
	closeStdout();
}

// --- enqueue helpers (chunked-aware) ---

/*

void enqueueOut(const char* data, std::size_t len)

Queues response bytes into the internal buffer (out_buf_) and,
if chunked mode is enabled, wraps each payload in HTTP/1.1 chunk
framing: <hex-len>\r\n<data>\r\n. For HEAD requests, any post-head body
is suppressed entirely (HTTP semantics). Centralizing framing here lets
higher layers simply “enqueue body”; the streamer guarantees the right wire
format whether Content-Length is known or not. The hex encoder is small and C++98-f
riendly (no std::stringstream), minimizing overhead on hot paths.
The buffer is later drained into the connection’s ChainBuf via takeOutBytes,
decoupling CGI read pacing from client write pacing for fairness.

*/

// In CGIStreamer.cpp
void CGIStreamer::enqueueOut(const char *data, std::size_t len)
{
	if (!data || len == 0)
		return;

	// Allow the head, suppress any body for HEAD after the head is queued
	if (req_.getMethod() == "HEAD" && http_head_queued_)
	{
		return;
	}

	if (!chunked_mode_)
	{
		out_buf_.insert(out_buf_.end(), data, data + len);
		return;
	}

	// --- chunked framing: <hex>\r\n<data>\r\n ---
	// format len in hex (C++98-safe)
	char hexbuf_rev[32];
	int h = 0;
	unsigned long long v = (unsigned long long)len;
	if (v == 0)
		hexbuf_rev[h++] = '0';
	else
	{
		while (v)
		{
			unsigned d = (unsigned)(v & 0xF);
			hexbuf_rev[h++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
			v >>= 4;
		}
	}
	// write hex digits in forward order + CRLF
	char sz[36];
	int k = 0;
	for (int i = h - 1; i >= 0; --i)
		sz[k++] = hexbuf_rev[i];
	sz[k++] = '\r';
	sz[k++] = '\n';

	// size line
	out_buf_.insert(out_buf_.end(), sz, sz + k);
	// data
	out_buf_.insert(out_buf_.end(), data, data + len);
	// CRLF
	out_buf_.push_back('\r');
	out_buf_.push_back('\n');
}

/*

void enqueueFinalChunk()

Appends the terminating chunk "0\r\n\r\n" once when transfer-encoding is chunked,
marking end-of-message so keep-alive can continue. A guard (sent_final_chunk_)
prevents duplicate terminators if multiple code paths converge (EOF, timeout, or error).
For HEAD, it becomes a no-op: HTTP forbids a body, so no chunk framing is sent.
Centralizing end-of-stream emission makes completion logic uniform and
avoids subtle bugs with partially sent responses..

*/

void CGIStreamer::enqueueFinalChunk()
{
	if (sent_final_chunk_)
		return;

	// For HEAD, there is no body at all — skip any chunk terminator.
	if (req_.getMethod() == "HEAD")
	{
		sent_final_chunk_ = true; // mark done so callers don’t retry
		return;
	}

	sent_final_chunk_ = true;

	if (chunked_mode_)
	{
		static const char kFinal[] = "0\r\n\r\n";
		out_buf_.insert(out_buf_.end(), kFinal, kFinal + sizeof(kFinal) - 1);
	}
}

/*

void pauseStdoutReads() / void resumeStdoutReads()

Flow-control toggles for the child’s stdout: they update poll interest
(POLLIN vs no interest) and set a local flag. When the downstream (client socket) is backed up,
pausing reads prevents unbounded buffering in out_buf_. When pressure subsides,
resuming allows the streamer to continue draining CGI output. Tying this to event-loop
interest (rather than reading and discarding) keeps the system lossless, fair,
and compliant with the “single poll” design. Idempotence simplifies callers—safe to
call even if already paused/resumed.


*/

void CGIStreamer::pauseStdoutReads()
{
	if (stdout_paused_)
		return;
	if (loop_ && cgi_out_fd_ >= 0)
		loop_->modFD(cgi_out_fd_, 0);
	stdout_paused_ = true;
}

void CGIStreamer::resumeStdoutReads()
{
	if (!stdout_paused_)
		return;
	if (loop_ && cgi_out_fd_ >= 0)
		loop_->modFD(cgi_out_fd_, POLLIN);
	stdout_paused_ = false;
}

// --- header parsing ---

/*

void parseOneHeaderLine(const std::string& line)

Parses one Key: Value header from CGI output.
Special-cases Status: <code> <reason> to override the HTTP status line.
Tracks Content-Type presence to avoid defaulting later and accumulates all Set-Cookie lines
(multiple allowed). Any other header is stashed onto res_.headers for possible
propagation to the client. This incremental parsing allows onCgiReadable to accumulate a header
block and feed lines here, keeping concerns separated: this routine translates CGI conventions
into the outgoing HTTP head, while finalizeHeaders later enforces required defaults
(Date, Server, Connection, and framing).


*/

void CGIStreamer::parseOneHeaderLine(const std::string &line)
{
	// Expect "Key: Value"
	std::string::size_type col = line.find(':');
	if (col == std::string::npos)
		return;

	std::string key = line.substr(0, col);
	std::string val = (col + 1 < line.size() && line[col + 1] == ' ')
						  ? line.substr(col + 2)
						  : line.substr(col + 1);

	if (iequals_ascii(key, "Status"))
	{
		// "Status: 404 Not Found"
		int code = 0;
		std::string::size_type i = 0;
		while (i < val.size() && val[i] >= '0' && val[i] <= '9')
		{
			code = code * 10 + (val[i] - '0');
			++i;
		}
		while (i < val.size() && val[i] == ' ')
			++i;
		status_code_ = code;
		status_reason_ = (i < val.size()) ? val.substr(i) : std::string();
		return;
	}

	if (iequals_ascii(key, "Content-Type"))
	{
		saw_content_type_ = true;
		res_.headers.set("Content-Type", val);
		return;
	}

	if (iequals_ascii(key, "Set-Cookie"))
	{
		set_cookie_.push_back(val); // keep multiple
		return;
	}

	// Other headers can be forwarded if desired:
	res_.headers.set(key, val);
}

/*

void finalizeHeaders()

Builds and queues the HTTP status line and essential headers
exactly once. If the CGI didn’t emit Content-Type, it defaults to text/plain.
It sets Date and Server if missing, and decides framing: for non-HEAD responses,
if no Content-Length exists it enables chunked mode (Transfer-Encoding: chunked),
else it removes TE to avoid conflicting indicators. It also sets
Connection based on the request (Connection: close or HTTP/1.0) unless already specified,
and serializes all Set-Cookie headers. The routine temporarily disables
chunking to enqueue the head un-chunked, then restores mode for the body.
The http_head_queued_ guard prevents duplication. Result: every CGI
response becomes a well-formed HTTP/1.1 message even if the CGI
output was minimal or irregular.

*/

// Build and queue the HTTP head once (after CGI headers parsed).
void CGIStreamer::finalizeHeaders()
{
	if (http_head_queued_)
		return;

	const bool is_head = (req_.getMethod() == "HEAD");

	// --- status line pieces ---
	const int status = status_code_ ? status_code_ : 200;
	const std::string reason = status_reason_.empty() ? "OK" : status_reason_;

	// --- ensure basic headers (but don't stomp CGI-provided ones) ---
	if (!saw_content_type_ && !res_.headers.keyExists("Content-Type"))
		res_.headers.set("Content-Type", "text/plain");

	if (!res_.headers.keyExists("Date"))
		res_.headers.set("Date", http_date_now_gmt());

	if (!res_.headers.keyExists("Server"))
		res_.headers.set("Server", "webserv/1.0");

	// Connection policy unless CGI already set it
	if (!res_.headers.keyExists("Connection"))
	{
		bool want_close = false;
		const Headers &rq = req_.getHeaders();
		std::string c = rq.get("Connection");
		bool rq_says_close = false;
		if (!c.empty() && c.size() == 5)
		{
			// ASCII case-insensitive compare to "close"
			rq_says_close =
				((c[0] | 32) == 'c' && (c[1] | 32) == 'l' && (c[2] | 32) == 'o' && (c[3] | 32) == 's' && (c[4] | 32) == 'e');
		}
		if (rq_says_close || req_.getHttpVer() == "HTTP/1.0")
			want_close = true;

		res_.headers.set("Connection", want_close ? "close" : "keep-alive");
	}

	// --- choose body framing ---
	const bool have_cl = res_.headers.keyExists("Content-Length");

	if (is_head)
	{
		// For HEAD: never advertise chunked and don't enable chunking.
		chunked_mode_ = false;
		if (res_.headers.keyExists("Transfer-Encoding"))
			res_.headers.erase("Transfer-Encoding");
		// Keep Content-Length if CGI set it; otherwise send neither CL nor TE.
	}
	else
	{
		// For non-HEAD: use chunked if no Content-Length was provided.
		if (!have_cl)
		{
			chunked_mode_ = true;
			res_.headers.set("Transfer-Encoding", "chunked");
		}
		else
		{
			chunked_mode_ = false;
			if (res_.headers.keyExists("Transfer-Encoding"))
				res_.headers.erase("Transfer-Encoding");
		}
	}

	// --- build and queue the response head ---
	std::string head;
	head.reserve(512);

	// Status line
	head += "HTTP/1.1 ";
	{
		char buf[16];
		std::sprintf(buf, "%d", status);
		head += buf;
	}
	head += " ";
	head += reason;
	head += "\r\n";

	head += "Date: " + res_.headers.get("Date") + "\r\n";
	head += "Server: " + res_.headers.get("Server") + "\r\n";

	if (res_.headers.keyExists("Content-Type"))
		head += "Content-Type: " + res_.headers.get("Content-Type") + "\r\n";

	// For HEAD: include Content-Length if present; never include Transfer-Encoding.
	// For others: include either CL or TE (already decided above).
	if (have_cl)
	{
		head += "Content-Length: " + res_.headers.get("Content-Length") + "\r\n";
	}
	else if (!is_head)
	{
		head += "Transfer-Encoding: chunked\r\n";
	}

	head += "Connection: " + res_.headers.get("Connection") + "\r\n";

	for (std::vector<std::string>::size_type i = 0; i < set_cookie_.size(); ++i)
		head += "Set-Cookie: " + set_cookie_[i] + "\r\n";

	head += "\r\n";

	// Queue head bytes unchunked
	const bool saved = chunked_mode_;
	chunked_mode_ = false;
	enqueueOut(head.data(), head.size());
	chunked_mode_ = saved;

	http_head_queued_ = true;
}

// --- stdin/stdout helpers ---

#include <poll.h> // POLLIN, POLLOUT

/*

void closeStdin() / void closeStdout()

Unregister the corresponding CGI pipe fd from the event loop and close() it,
resetting to -1. Closing stdin (server→child) as soon as the body is fully sent signals
EOF to the CGI, unblocking scripts that wait for end-of-input before producing output.
Closing stdout at EOF or on error stops further reads and prevents stale events.
Encapsulating this in helpers keeps teardown idempotent and ensures loop state and fd state stay in sync.

*/

void CGIStreamer::closeStdin()
{
#if defined(DEBUG)
	std::fprintf(stderr, "[CGI][FD] closeStdin fd=%d\n", cgi_in_fd_);
#endif
	if (cgi_in_fd_ >= 0)
	{
		if (loop_)
			loop_->removeFD(cgi_in_fd_);
		::close(cgi_in_fd_);
		cgi_in_fd_ = -1;
	}
}

void CGIStreamer::closeStdout()
{
#if defined(DEBUG)
	std::fprintf(stderr, "[CGI][FD] closeStdout fd=%d\n", cgi_out_fd_);
#endif
	if (cgi_out_fd_ >= 0)
	{
		if (loop_)
			loop_->removeFD(cgi_out_fd_);
		::close(cgi_out_fd_);
		cgi_out_fd_ = -1;
	}
}

/*

void reset()

Returns the streamer to a pristine state for reuse: closes both pipe fds, clears failed_/cgi_active_,
resets body offsets and temp-file handle/path, wipes header accumulators and flags,
resets output buffers and framing flags (chunked_mode_ true by default), zeros deadlines and back-pressure counters.
Having a single authoritative reset prevents cross-request contamination on keep-alive connections and simplifies
higher layers (they can call reset() confidently after errors or before starting a new CGI).

*/

void CGIStreamer::reset()
{
	// stop timers/mark inactive AFTER fds are gone
	closeStdin();
	closeStdout();

	failed_ = false;
	cgi_active_ = false;

	cgi_body_off_ = 0;
	if (body_fd_ >= 0)
	{
		::close(body_fd_);
		body_fd_ = -1;
	}
	body_file_off_ = 0;
	body_path_.clear();

	hdr_state_ = HDR_WAITING;
	cgi_header_accum_.clear();
	saw_content_type_ = false;
	status_code_ = 0;
	status_reason_.clear();
	set_cookie_.clear();

	http_head_queued_ = false;
	chunked_mode_ = true;
	sent_final_chunk_ = false;
	out_buf_.clear();
	out_off_ = 0;

	client_out_bytes_ = 0;
	write_deadline_ms_ = 0;
	hdr_deadline_.clear();
	total_deadline_.clear();
}

/*

static unsigned long long monotonic_ms() / now_ms_mono() and resetWriteDeadline(...)

monotonic_ms()/now_ms_mono() provide millisecond timestamps used for watchdogs.
resetWriteDeadline(now) and the no-arg overload set write_deadline_ms_ = now + WRITE_TIMEOUT_MS,
refreshed after each successful stdin write. If the CGI stops reading, the deadline will fire in onTick and closeStdin()
to unblock the child and prevent the server from hanging. Separating deadline management
from I/O keeps I/O routines tight and lets onTick centralize timeout enforcement.e.

*/

// ---- monotonic milliseconds () ----
static unsigned long long monotonic_ms()
{
	return static_cast<unsigned long long>(std::time(0)) * 1000ULL;
}

// single, unique definition somewhere near the top of CGIStreamer.cpp
static unsigned long long now_ms_mono()
{
	return static_cast<unsigned long long>(std::time(0)) * 1000ULL;
}

void CGIStreamer::resetWriteDeadline(unsigned long long now_ms)
{
	write_deadline_ms_ = now_ms + WRITE_TIMEOUT_MS;
}

void CGIStreamer::resetWriteDeadline()
{
	write_deadline_ms_ = now_ms_mono() + WRITE_TIMEOUT_MS;
}

// --- public API ---
/*

bool beginCgi(const CgiSpec& spec, const std::string& script_path,
const std::vector<std::string>& envv, std::vector<int> tracked)

End-to-end setup: clears runtime state, spawns the CGI via proc_.spawn(...),
grabs parent-side stdin/stdout fds, and marks them O_NONBLOCK and FD_CLOEXEC.
It primes the request-body source: for in-memory bodies, it snapshots bytes
into in_mem_body_; for disk-backed bodies, it defers opening the file until first write.
Then it registers the fds in the event loop (POLLIN on stdout, POLLOUT on stdin if there’s a body).
If the request truly has no body, it proactively closes stdin to wake CGIs that block on read().
Finally, it arms the header and total timeouts and the write watchdog and marks cgi_active_ = true.
This single call transitions the connection into “CGI streaming” mode under the single-poll architecture.


*/

bool CGIStreamer::beginCgi(const CgiSpec &spec,
						   const std::string &script_path,
						   const std::vector<std::string> &envv,
						   std::vector<int> tracked)
{
	// ---- reset runtime state ----
	failed_ = false;
	cgi_active_ = false;

	cgi_in_fd_ = -1;  // server → child stdin (we write here)
	cgi_out_fd_ = -1; // child  → server stdout (we read here)

	cgi_body_off_ = 0;
	if (body_fd_ >= 0)
	{
		::close(body_fd_);
		body_fd_ = -1;
	}
	body_file_off_ = 0;
	body_path_.clear();
	if (req_.isBodyOnDisk())
		body_path_ = req_.getBodyFilePath();

	// stdout parsing / response framing
	hdr_state_ = HDR_WAITING;
	cgi_header_accum_.clear();
	saw_content_type_ = false;
	status_code_ = 0;
	status_reason_.clear();
	set_cookie_.clear();

	http_head_queued_ = false;
	chunked_mode_ = true;
	sent_final_chunk_ = false;
	out_buf_.clear();
	out_off_ = 0;
	stdout_paused_ = false;

	// ---- spawn child ----
	if (!proc_.spawn(spec, script_path, envv, tracked))
	{
		failed_ = true;
		return false;
	}
	cgi_in_fd_ = proc_.inFD();
	cgi_out_fd_ = proc_.outFD();

	// ---- set pipes non-blocking + CLOEXEC ----
#ifdef F_GETFL
	if (cgi_in_fd_ >= 0)
	{
		int fl = ::fcntl(cgi_in_fd_, F_GETFL, 0);
		if (fl >= 0)
			::fcntl(cgi_in_fd_, F_SETFL, fl | O_NONBLOCK);
#ifdef F_GETFD
		fl = ::fcntl(cgi_in_fd_, F_GETFD, 0);
		if (fl >= 0)
			::fcntl(cgi_in_fd_, F_SETFD, fl | FD_CLOEXEC);
#endif
	}
	if (cgi_out_fd_ >= 0)
	{
		int fl = ::fcntl(cgi_out_fd_, F_GETFL, 0);
		if (fl >= 0)
			::fcntl(cgi_out_fd_, F_SETFL, fl | O_NONBLOCK);
#ifdef F_GETFD
		fl = ::fcntl(cgi_out_fd_, F_GETFD, 0);
		if (fl >= 0)
			::fcntl(cgi_out_fd_, F_SETFD, fl | FD_CLOEXEC);
#endif
	}
#endif

	if (cgi_out_fd_ < 0)
	{
		failed_ = true;
		return false;
	}

	// ---- prime stdin feed (snapshot for in-mem body; lazy-open for disk) ----
	in_mem_body_.clear();
	in_mem_off_ = 0;
	using_mem_body_ = false;

	if (!req_.isBodyOnDisk())
	{
		const std::size_t blen = req_.getBodyLength();
		if (blen > 0)
		{
			in_mem_body_ = req_.readBodyToVector(); // stable snapshot
			using_mem_body_ = !in_mem_body_.empty();
		}
	}
	else
	{
		body_file_off_ = 0; // open file lazily in onCgiWritable()
	}

	// ---- register fds with event loop right after O_NONBLOCK ----
	if (loop_)
	{
		loop_->modFD(cgi_out_fd_, POLLIN); // read child's stdout
		if (cgi_in_fd_ >= 0)
			loop_->modFD(cgi_in_fd_, POLLOUT); // feed stdin
	}

	// If there is truly no body, close stdin so the CGI doesn’t block on read()
	if (!using_mem_body_ && !req_.isBodyOnDisk() && req_.getBodyLength() == 0)
	{
		closeStdin();
	}

	// ---- arm deadlines (monotonic) ----
	const unsigned long long now_ms = now_ms_mono();
	const int HDR_WAIT_MS = 3000;	  // time to see CGI headers
	const int TOTAL_LIMIT_MS = 15000; // total runtime cap

	hdr_deadline_.reset(now_ms, HDR_WAIT_MS);
	total_deadline_.reset(now_ms, TOTAL_LIMIT_MS);
	resetWriteDeadline(now_ms);

	cgi_active_ = true;
	return true;
}

/*


void startHeaderPhase(unsigned long long now_ms)

Re-arms the “header arrival” deadline from now_ms, giving the CGI a fresh window to
produce headers (e.g., after setup delays). If the new deadline is missed, onTick()
will fail the CGI cleanly. Keeping this separate from beginCgi gives you flexibility
to adjust header timing policy without respawning.

*/

void CGIStreamer::startHeaderPhase(unsigned long long now_ms)
{
	// Re-arm header deadline with default 3s (same as beginCgi()).
	hdr_deadline_.reset(now_ms, 10000);
}

/*

void onTick(unsigned long long now_ms)

Periodic watchdog: checks if header-phase or total runtime deadlines have expired and, if so,
calls timeout504() to tear down the CGI and mark failure. It also enforces the write watchdog:
if the child hasn’t read stdin by write_deadline_ms_, it closes stdin to unblock the pipeline.
Running this from the loop’s “tick” (timeout wake) guarantees no request can hang indefinitely,
meeting the project’s resilience requirement while keeping polling single-threaded and fair.


*/

void CGIStreamer::onTick(unsigned long long /*now_ms_ignored*/)
{
	const unsigned long long now_ms = monotonic_ms();

	if (hdr_state_ == HDR_WAITING && hdr_deadline_.expired(now_ms))
	{
		timeout504();
		return;
	}
	if (total_deadline_.expired(now_ms))
	{
		timeout504();
		return;
	}
	if (write_deadline_ms_ && now_ms > write_deadline_ms_)
	{
		closeStdin(); // unblock child that isn’t draining stdin
		write_deadline_ms_ = 0;
	}
}

/*

std::size_t takeOutBytes(ChainBuf& out, std::size_t max_bytes)

Moves up to max_bytes from the internal out_buf_ into the connection’s ChainBuf,
advancing an offset and compacting the buffer when drained. Returning the count tells
the caller whether to keep POLLOUT armed on the client socket. This function decouples
CGI framing/aggregation from actual socket writes, so you can throttle per-tick
send volume for fairness without touching the CGI read path.

*/

std::size_t CGIStreamer::takeOutBytes(ChainBuf &out, std::size_t max_bytes)
{
	if (out_off_ >= out_buf_.size())
	{
		out_buf_.clear();
		out_off_ = 0;
		return 0;
	}
	std::size_t avail = out_buf_.size() - out_off_;
	std::size_t n = (max_bytes < avail) ? max_bytes : avail;

	if (n == 0)
		return 0;

	out.push_copy(&out_buf_[out_off_], n);
	out_off_ += n;

	if (out_off_ == out_buf_.size())
	{
		out_buf_.clear();
		out_off_ = 0;
	}
	return n;
}

// --- I/O driving ---

/*

void onCgiReadable(int fd)

Handles POLLIN on the CGI’s stdout. While in HDR_WAITING,
it accumulates bytes until a header terminator (\r\n\r\n or \n\n) appears,
then parses each line with parseOneHeaderLine and calls finalizeHeaders().
If no terminator appears and a heuristic suggests it’s body (first line lacks :
or header block too large), it synthesizes a head and treats the data as body.
After headers are done, subsequent reads stream body chunks via enqueueOut,
respecting back-pressure (wantsRead()). On EOF, it ensures headers were finalized,
enqueues a final chunk (if chunked), closes stdout, and marks inactive.
The routine never branches on errno after read(): a negative value simply yields and waits
for the next readiness or HUP/ERR. This is the heart of robust CGI output handling under your single-poll model.

*/

void CGIStreamer::onCgiReadable(int fd)
{
	if (!cgi_active_ || fd != cgi_out_fd_ || cgi_out_fd_ < 0)
		return;

	// Back-pressure gate
	if (!wantsRead())
	{
#if defined(DEBUG)
		std::fprintf(stderr, "[CGI][RD] outFD=%d wantsRead=0 → defer\n", cgi_out_fd_);
#endif
		return;
	}

	for (;;)
	{
		char buf[16384];
		ssize_t r = ::read(cgi_out_fd_, buf, sizeof(buf));

		if (r > 0)
		{
			const char *p = buf;
			std::size_t n = static_cast<std::size_t>(r);
#if defined(DEBUG)
			std::fprintf(stderr, "[CGI][RD] outFD=%d got=%zu bytes (state=%s)\n",
						 cgi_out_fd_, n, (hdr_state_ == HDR_WAITING ? "HEADERS" : "BODY"));
#endif

			if (hdr_state_ == HDR_WAITING)
			{
				// Accumulate until CRLFCRLF or LFLF
				cgi_header_accum_.append(p, n);
#if defined(DEBUG)
				std::fprintf(stderr, "[CGI][RD] accum=%zu bytes\n",
							 cgi_header_accum_.size());
#endif
				std::string::size_type cut = std::string::npos;
				std::string::size_type k = cgi_header_accum_.find("\r\n\r\n");
				if (k != std::string::npos)
					cut = k + 4;
				else
				{
					k = cgi_header_accum_.find("\n\n");
					if (k != std::string::npos)
						cut = k + 2;
				}

				if (cut == std::string::npos)
				{
					// Heuristic: if first line lacks ":" or headers grow too large,
					// treat the whole thing as body.
					std::string::size_type eol = cgi_header_accum_.find_first_of("\r\n");
					std::string first = (eol == std::string::npos)
											? cgi_header_accum_
											: cgi_header_accum_.substr(0, eol);
					const bool first_has_colon = (first.find(':') != std::string::npos);

					if (!first_has_colon || cgi_header_accum_.size() > 8192u)
					{
#if defined(DEBUG)
						std::fprintf(stderr,
									 "[CGI][RD] no header terminator (first_has_colon=%d, size=%zu) → treat as body\n",
									 (int)first_has_colon, cgi_header_accum_.size());
#endif
						if (!http_head_queued_)
						{
#if defined(DEBUG)
							std::fprintf(stderr, "[CGI][RD] finalizeHeaders (synth)\n");
#endif
							finalizeHeaders(); // synthesize safe head
						}
						if (!cgi_header_accum_.empty())
						{
							enqueueOut(cgi_header_accum_.data(), cgi_header_accum_.size());
#if defined(DEBUG)
							std::fprintf(stderr, "[CGI][RD] queued body spill=%zu\n",
										 cgi_header_accum_.size());
#endif
						}
						cgi_header_accum_.clear();
						hdr_state_ = HDR_DONE;
					}
					// else: keep waiting for header terminator
					continue;
				}

				// We have a complete header block → parse it line-by-line
				const std::string hdr = cgi_header_accum_.substr(0, cut);
#if defined(DEBUG)
				std::fprintf(stderr, "[CGI][RD] header block complete len=%zu (cut at %zu)\n",
							 hdr.size(), cut);
#endif
				int set_cookie_count = 0;
				int parsed_lines = 0;

				std::string line;
				for (std::string::size_type i = 0; i < hdr.size();)
				{
					std::string::size_type j = i;
					while (j < hdr.size() && hdr[j] != '\r' && hdr[j] != '\n')
						++j;
					line.assign(hdr, i, j - i);
					if (j < hdr.size() && hdr[j] == '\r')
						++j;
					if (j < hdr.size() && hdr[j] == '\n')
						++j;
					i = j;

					if (!line.empty())
					{
						std::string lower = line;
						for (size_t q = 0; q < lower.size(); ++q)
						{
							char c = lower[q];
							if (c >= 'A' && c <= 'Z')
								lower[q] = char(c - 'A' + 'a');
						}
						if (lower.size() >= 11 && lower.compare(0, 11, "set-cookie:") == 0)
							++set_cookie_count;

						parseOneHeaderLine(line);
						++parsed_lines;
					}
				}
#if defined(DEBUG)
				std::fprintf(stderr, "[CGI][RD] parsed_lines=%d set-cookie=%d\n",
							 parsed_lines, set_cookie_count);
#endif

				// Anything after the terminator is body
				if (!http_head_queued_)
				{
#if defined(DEBUG)
					std::fprintf(stderr, "[CGI][RD] finalizeHeaders\n");
#endif
					finalizeHeaders();
				}
				if (cut < cgi_header_accum_.size())
				{
					const char *bp = cgi_header_accum_.data() + cut;
					const std::size_t bn = cgi_header_accum_.size() - cut;
					if (bn)
					{
						enqueueOut(bp, bn);
#if defined(DEBUG)
						std::fprintf(stderr, "[CGI][RD] queued post-header body=%zu\n", bn);
#endif
					}
				}
				cgi_header_accum_.clear();
				hdr_state_ = HDR_DONE;
				continue;
			}

			// Normal body streaming (headers already finalized)
			if (!http_head_queued_)
			{
#if defined(DEBUG)
				std::fprintf(stderr, "[CGI][RD] finalizeHeaders (late safety)\n");
#endif
				finalizeHeaders();
			}
			enqueueOut(p, n);
#if defined(DEBUG)
			std::fprintf(stderr, "[CGI][RD] queued body=%zu\n", n);
#endif
			continue;
		}

		if (r == 0)
		{
			// EOF from CGI
#if defined(DEBUG)
			std::fprintf(stderr, "[CGI][RD] EOF on outFD=%d\n", cgi_out_fd_);
#endif
			if (!http_head_queued_)
			{
#if defined(DEBUG)
				std::fprintf(stderr, "[CGI][RD] finalizeHeaders at EOF (no CGI headers)\n");
#endif
				finalizeHeaders();
			}
			enqueueFinalChunk(); // noop for HEAD inside helper
			closeStdout();		 // stop watching stdout
#if defined(DEBUG)
			std::fprintf(stderr, "[CGI][RD] queued final chunk\n");
#endif
			cgi_active_ = false; // producer done
			return;
		}

		// r < 0: no progress. Do NOT inspect errno; just stop and try again
		// when poll() signals readability or HUP/ERR.
		return;
	}
}

/*

void onCgiWritable(int fd)

Handles POLLOUT on the CGI’s stdin (server→child). It determines the body source:
disk-backed (open once and read small slices) or in-memory (copy from the request snapshot).
It writes in bounded chunks (16 KB) to avoid hogging the loop, updates cgi_body_off_ on progress,
and refreshes the write watchdog. When all bytes are sent, it closes stdin to signal EOF to the child
(many CGIs wait for EOF before producing output). If write() makes no progress or returns <0, it simply
yields—no errno checks—letting the poll loop deliver the next chance (or an error/hup).
This routine ensures large uploads are streamed safely and fairly without blocking.

*/

void CGIStreamer::onCgiWritable(int fd)
{
	if (!cgi_active_ || fd != cgi_in_fd_)
		return;

	const std::size_t total = req_.getBodyLength();
#if defined(DEBUG)
	std::fprintf(stderr, "[CGI][WR] enter inFD=%d off=%zu total=%zu disk=%d\n",
				 cgi_in_fd_, cgi_body_off_, total, (int)req_.isBodyOnDisk());
#endif

	// Nothing to send → close stdin immediately.
	if (total == 0)
	{
#if defined(DEBUG)
		std::fprintf(stderr, "[CGI][WR] total=0 → closeStdin\n");
#endif
		closeStdin();
		return;
	}

	// Already sent everything → close stdin.
	if (cgi_body_off_ >= total)
	{
#if defined(DEBUG)
		std::fprintf(stderr, "[CGI][WR] already sent all (%zu/%zu) → closeStdin\n",
					 cgi_body_off_, total);
#endif
		closeStdin();
		return;
	}

	// Limit per syscall to be fair to the loop.
	const std::size_t CHUNK = 16384u;

	// =========================
	// DISK-BACKED BODY
	// =========================
	if (req_.isBodyOnDisk())
	{
		if (body_fd_ < 0)
		{
			body_fd_ = ::open(body_path_.c_str(),
							  O_RDONLY
#ifdef O_CLOEXEC
								  | O_CLOEXEC
#endif
			);
			if (body_fd_ < 0)
			{
				// Can't access the temp file → give up sending stdin.
				closeStdin();
				return;
			}
		}

		const std::size_t remaining = total - cgi_body_off_;
		const std::size_t to_read = (remaining < CHUNK) ? remaining : CHUNK;
		if (to_read == 0)
		{
			closeStdin();
			return;
		}

		char small[16384];

		// Read a chunk from the temp file (regular file; no poll needed).
		// No errno-branching: if r < 0, just stop and try again next POLLOUT tick.
		ssize_t r = ::read(body_fd_, small, to_read);

		if (r <= 0)
		{
			// r == 0 → EOF early (unexpected) or r < 0 → read failed/transient.
			// Do NOT inspect errno; just retry on next wakeup.
			return;
		}

		std::size_t want = static_cast<std::size_t>(r);
		std::size_t sent = 0;
#if defined(DEBUG)
		std::fprintf(stderr, "[CGI][WR] disk chunk ready want=%zu\n", want);
#endif

		while (sent < want)
		{
			// Non-blocking pipe write to CGI stdin.
			ssize_t w = ::write(cgi_in_fd_, small + sent, want - sent);

			if (w > 0)
			{
				sent += static_cast<std::size_t>(w);
				cgi_body_off_ += static_cast<std::size_t>(w);
				resetWriteDeadline();

#if defined(DEBUG)
				std::fprintf(stderr, "[CGI][WR] wrote=%zd new_off=%zu/%zu\n",
							 w, cgi_body_off_, total);
#endif
				if (cgi_body_off_ >= total)
				{
#if defined(DEBUG)
					std::fprintf(stderr, "[CGI][WR] done sending body → closeStdin\n");
#endif
					closeStdin();
					return;
				}
				continue;
			}

			if (w == 0)
			{
				// No progress; rare on pipes, treat like backpressure.
				return;
			}

			// w < 0: do NOT check errno. Back off and wait for next POLLOUT or error/hup.
			return;
		}

		return;
	}

	// =========================
	// IN-MEMORY BODY
	// =========================
	std::vector<char> tmp = req_.getBody();
#if defined(DEBUG)
	std::fprintf(stderr, "[CGI][WR] mem tmp.size=%zu off=%zu total=%zu\n",
				 tmp.size(), cgi_body_off_, total);
#endif

	// Guard: no bytes available yet (body still being assembled)
	if (tmp.size() <= cgi_body_off_)
	{
#if defined(DEBUG)
		std::fprintf(stderr, "[CGI][WR] no bytes available yet (tmp.size=%zu, off=%zu) → retry later\n",
					 tmp.size(), cgi_body_off_);
#endif
		return;
	}

	std::size_t remaining = total - cgi_body_off_;
	std::size_t avail = tmp.size() - cgi_body_off_;
	std::size_t len = (remaining < avail) ? remaining : avail;
	if (len == 0)
	{
#if defined(DEBUG)
		std::fprintf(stderr, "[CGI][WR] len=0 after calc → retry later\n");
#endif
		return;
	}
	if (len > CHUNK)
		len = CHUNK;

	char small[16384];
	std::memcpy(small, &tmp[0] + cgi_body_off_, len);
#if defined(DEBUG)
	std::fprintf(stderr, "[CGI][WR] mem chunk write attempt len=%zu\n", len);
#endif

	std::size_t sent = 0;
	while (sent < len)
	{
		ssize_t w = ::write(cgi_in_fd_, small + sent, len - sent);

		if (w > 0)
		{
			sent += static_cast<std::size_t>(w);
			cgi_body_off_ += static_cast<std::size_t>(w);
			resetWriteDeadline();

#if defined(DEBUG)
			std::fprintf(stderr, "[CGI][WR] wrote=%zd new_off=%zu/%zu\n",
						 w, cgi_body_off_, total);
#endif
			if (cgi_body_off_ >= total)
			{
#if defined(DEBUG)
				std::fprintf(stderr, "[CGI][WR] done sending body → closeStdin\n");
#endif
				closeStdin();
				return;
			}
			continue;
		}

		if (w == 0)
		{
			// No progress; back off and let poll drive the next attempt.
			return;
		}

		// w < 0: do NOT inspect errno (EAGAIN/EWOULDBLOCK/EINTR/EPIPE, etc.).
		// Simply return; POLLOUT or HUP/ERR will wake us appropriately.
		return;
	}
}

/*

void timeout504()

Uniform timeout teardown: closes stdin/stdout, asks
CgiProcess to terminate() the child (TERM → grace → KILL),
marks cgi_active_ = false and failed_ = true. Higher layers
can then generate a 504 Gateway Timeout (or mapped error page) and cleanly
finish the HTTP exchange. Centralizing timeout cleanup guarantees
consistent descriptor state and leaves no zombies or dangling poll registrations.

*/

// --- timeout handling ---

void CGIStreamer::timeout504()
{
	closeStdin();
	closeStdout();
	proc_.terminate();
	cgi_active_ = false;
	failed_ = true;
}
