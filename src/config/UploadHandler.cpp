// src/http/handlers/UploadHandler.cpp
#include "UploadHandler.h"
#include "RequestContext.h"
#include "VirtualServer.h"
#include "ResponseFactory.h"
#include "HEADER_ENTRIES.h"
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>
#include <stdlib.h>   // realpath
#include <limits.h>   // PATH_MAX
#include <unistd.h>   // mkstemp, close



/* 

static bool realpathString(const std::string& in, std::string& out)

Canonicalizes a filesystem path with ::realpath, writing an absolute, 
symlink-resolved path into out. Returns false if resolution fails. 
This is crucial for upload safety: before writing user-supplied filenames, 
the handler must ensure the configured upload_dir actually exists and refers to a directory, 
not a file or dangling path. Using the kernel’s resolution avoids purely string-based mistakes 
around .. segments or symlink tricks. Centralizing this into a tiny helper keeps the hot path 
readable and lets other guard functions, like ensureUploadDirIsSafe, 
rely on a single, consistent canonicalization routine across every incoming upload request.


*/


static bool realpathString(const std::string& in, std::string& out) {
	char tmp[4096];
	if (::realpath(in.c_str(), tmp) == 0)
		return false;
	out.assign(tmp);
	return true;
}



/* 

UploadHandler::UploadHandler()
Initializes per-request bookkeeping to safe defaults: cur_fd_ = -1 (no open file yet), 
cur_written_ = 0 (no bytes received), and last_error_code_ = 0 (no error). 
The constructor does not touch the filesystem; it merely sets a predictable baseline before any multipart parsing begins. 
This makes the handler reusable and safe under errors: subsequent invocations start from a clean state, 
ensuring previous runs never leak filenames, descriptors, or error messages into new uploads. 
Minimal work here is intentional because actual I/O happens later, during streaming callbacks driven by the multipart parser.

*/

UploadHandler::UploadHandler() : cur_fd_(-1), cur_written_(0), last_error_code_(0) {}


/* 

UploadHandler::~UploadHandler()
Ensures any still-open temporary descriptor is closed (::close(cur_fd_)). 
The destructor’s job is RAII cleanup so that aborted or failed uploads cannot leak file descriptors. 
It deliberately avoids removing files; renaming to the final destination happens only after a successful 
onPartEnd via finishMoveAtomic. If destruction occurs early (e.g., connection drops), 
the OS will reclaim any unlinked temp files, and the closed descriptor guarantees no lingering handles. 
Keeping destruction tiny avoids complicated teardown logic and supports clean 
shutdowns in error paths or when the server recycles handler instances under high concurrency.

*/

UploadHandler::~UploadHandler() { 
	if (cur_fd_>=0)
		::close(cur_fd_); 
}


/* 

std::string UploadHandler::sanitizeFilename(const std::string& in) const

Produces a safe basename from a potentially hostile client filename. 
It converts backslashes to slashes, strips directory components, rejects ./.., 
and whitelists characters [A–Z a–z 0–9 . _ -], replacing anything else with _. 
If the result empties, substitutes "file". Finally, clamps length to 128 characters. 
This prevents path traversal, odd encodings, or control characters from escaping the upload directory or creating unmanageable names. 
Sanitization is critical because browsers may submit full paths (Windows), 
and malicious clients might attempt to overwrite sensitive files or inject HTML/JS via filenames. 
Centralizing rules ensures consistent protection.


*/

std::string UploadHandler::sanitizeFilename(const std::string& in) const {
	// drop directories, keep basename
	std::string name = in;
	for (std::size_t i=0;i<name.size();++i) {
		if (name[i]=='\\') 
			name[i]='/';
	}
	std::string::size_type s = name.rfind('/');
	if (s != std::string::npos)
		name = name.substr(s+1);

	// reject dotdot
	if (name == "." || name == "..")
		name.clear();

	// allow [A-Za-z0-9._-], replace others with '_'
	std::string out; out.reserve(name.size());
	for (std::size_t i=0;i<name.size();++i) {
		char c = name[i];
		bool ok =
			(c>='A'&&c<='Z')||(c>='a'&&c<='z')||
			(c>='0'&&c<='9')|| c=='.'||c=='_'||c=='-';
		out.push_back(ok?c:'_');
	}
	if (out.empty())
		out = "file";
	if (out.size() > 128)
		out = out.substr(0,128);
	return out;
}


/* 

bool UploadHandler::ensureUploadDirIsSafe(const std::string& base) const

Verifies that upload_dir_ resolves via realpath and that the target exists and is a directory (::stat + S_ISDIR). 
Returning false immediately blocks uploads when configuration is missing or points somewhere unsafe. 
This guard complements filename sanitization: even with clean basenames, 
writes must happen inside a trusted directory. 
Using the filesystem’s real paths avoids purely lexical errors and defends 
against symlinks that could redirect writes elsewhere. Doing this check on each request 
keeps behavior predictable even if administrators change directories between reloads, 
and it yields clearer error messages when misconfigured.

*/

bool UploadHandler::ensureUploadDirIsSafe(const std::string& base) const {
	// ensure realpath(upload_dir) exists and is a directory
	std::string canon;
	if (!realpathString(base, canon))
		return false;
	struct stat st; 
	if (::stat(canon.c_str(), &st)!=0 || !S_ISDIR(st.st_mode))
		return false;
	return true;
}


/* 

bool UploadHandler::openTempInUpload(std::string& out_path, int& out_fd)

Creates a unique temporary file inside the configured upload directory using mkstemp(".upload-XXXXXX"), 
sets FD_CLOEXEC, and returns the path and descriptor. 
Writing to a temp first allows atomic rename later into the final sanitized filename, 
preventing partial files from appearing on failures and ensuring destination overwrites obey overwrite_ policy. 
Keeping the temp within the same filesystem enables a cheap ::rename rather than copy. 
This also avoids race conditions where two clients target the same name: the temp file is unique, 
and the final rename step enforces conflict policy deterministically

*/

bool UploadHandler::openTempInUpload(std::string& out_path, int& out_fd) {
	// create temp file in upload_dir for atomic rename within same fs
	std::string templ = upload_dir_;
	if (!templ.empty() && templ[templ.size()-1] != '/')
		templ += '/';
	templ += ".upload-XXXXXX";
	std::vector<char> buf(templ.begin(), templ.end());
	buf.push_back('\0');
	int fd = ::mkstemp(&buf[0]);
	if (fd < 0)
		return false;
	::fcntl(fd, F_SETFD, FD_CLOEXEC);
	out_path.assign(&buf[0]);
	out_fd = fd;
	return true;
}


/* 

bool UploadHandler::finishMoveAtomic()
Finalizes a single part: fsync to flush data, close the descriptor, 
compute the destination path (upload dir + sanitized basename), enforce overwrite_ policy 
(409 Conflict if file exists and overwriting disabled), and ::rename the temp into place. 
On success, records the saved filename and clears the temp path; on failure, sets last_error_code_/last_error_msg_. 
Atomic rename guarantees that readers never observe half-written files and that successful uploads become visible in a single, 
consistent step. This method is the critical integrity barrier for safe, concurrent file writes.

*/

// UploadHandler.cpp
bool UploadHandler::finishMoveAtomic() {
	if (cur_fd_ < 0)
		return false;
	::fsync(cur_fd_);
	::close(cur_fd_);
	cur_fd_ = -1;

	std::string dst = upload_dir_;
	if (!dst.empty() && dst[dst.size()-1] != '/')
		dst += '/';
	dst += cur_filename_sanit_;

	if (!overwrite_) {
		struct stat st;
		if (::stat(dst.c_str(), &st) == 0) {
			last_error_code_ = 409;
			last_error_msg_  = "File exists: " + cur_filename_sanit_;
			return false; // will cause parser to stop with sink error
		}
	}
	if (::rename(cur_tmp_path_.c_str(), dst.c_str()) != 0) {
		last_error_code_ = 500;
		last_error_msg_  = "Rename failed";
		return false;
	}
	saved_.push_back(cur_filename_sanit_);
	cur_tmp_path_.clear();
	return true;
}


// ---- IMultipartSink ----


/* 

bool UploadHandler::onPartBegin(const Part& p)

Multipart sink callback: captures the field name and raw filename, 
computes a sanitized basename, resets byte counters, 
clears any previous temp path, and opens a new temp file via openTempInUpload. 
Returning false aborts parsing immediately, causing the parser to surface an error. 
Beginning each part with a fresh temp ensures independent handling of multiple files within one request and avoids cross-contamination between parts. 
It also enables per-part size caps and per-file overwrite decisions later, 
keeping semantics aligned with how browsers submit multiple files in a single form field.

*/




bool UploadHandler::onPartBegin(const Part& p) {
	cur_name_          = p.name;
	cur_filename_raw_  = p.filename;
	cur_filename_sanit_= sanitizeFilename(cur_filename_raw_);
	cur_written_       = 0;
	cur_fd_            = -1;
	cur_tmp_path_.clear();

	if (!openTempInUpload(cur_tmp_path_, cur_fd_))
		return false;
	return true;
}


/* 

bool UploadHandler::onPartData(const char data, std::size_t n)*

Streams incoming part bytes to the open temp file using a looped ::write. 
Enforces an optional per-part size cap (per_part_cap_), 
returning false with last_error_code_=413 if exceeded. 
Handles EINTR and treats other write errors as 500. 
Accumulates cur_written_. Streaming directly to disk avoids memory blow-ups for large files and fits the server’s 
non-blocking philosophy: the parser calls this repeatedly with whatever the body source provides. 
This function is the workhorse for large uploads, 
ensuring throughput and consistent error signaling back to the multipart parser.

*/

bool UploadHandler::onPartData(const char* data, std::size_t n) {
	if (cur_fd_ < 0) return false;

	// per-part cap
	if (per_part_cap_ && cur_written_ + n > per_part_cap_) {
		last_error_code_ = 413;                  // <--- add
		last_error_msg_  = "part too large";     // <--- add
		return false; // sink refuses -> parser stops
	}

	std::size_t off = 0;
	while (off < n) {
		ssize_t w = ::write(cur_fd_, data + off, (n - off));
		if (w > 0)
			off += (std::size_t)w;
		else if (w < 0 && errno == EINTR)
			continue;
		else { 
			last_error_code_ = 500; last_error_msg_ = "write failed"; return false; 
		}
	}
	cur_written_ += n;
	return true;
}


/* 

bool UploadHandler::onPartEnd()
Called when the parser reaches a boundary ending the current part. 
It delegates to finishMoveAtomic to fsync, close, and rename the temp into the final destination. 
Returning false signals failure to the parser (e.g., conflicts or rename errors). 
By doing finalization only here, the handler ensures a part’s data is fully written before it becomes visible. 
It also centralizes final error mapping for per-file outcomes, 
enabling the top-level handler to present clean messages and status codes to clients after parsing completes.


*/


bool UploadHandler::onPartEnd() {
	return finishMoveAtomic();
}


/* 

bool UploadHandler::handle(HttpRequest& req, HttpResponse& res, RequestContext& ctx)
Orchestrates the whole upload: resets state; loads per-route config (upload_store, upload_overwrite, upload_max_file_size); 
validates upload dir; inspects Content-Type and initializes a MultipartReader for multipart/form-data; 
locates body source (temp file path from request or in-memory vector); 
feeds data into the parser, which calls the sink methods above; 
consolidates errors (including per-part cap/conflict) into meaningful HTTP responses; 
verifies the final boundary was seen; and returns 201 Created (or 200 OK) listing saved filenames. 
All responses set proper Content-Length, Content-Type, Connection, 
and Server headers for correctness and browser compatibility.


*/

// ---- handler entry ----
bool UploadHandler::handle(HttpRequest& req, HttpResponse& res, RequestContext& ctx)
{
	// per-request reset
	saved_.clear();
	last_error_code_ = 0;
	last_error_msg_.clear();
	if (cur_fd_ >= 0) { 
		::close(cur_fd_); cur_fd_ = -1; 
	}

	// helper for visible error
	struct Err {
		static bool send(HttpResponse& res, int code, const std::string& msg, bool close) {
			std::string text = msg + "\n";
			res.status = code;
			res.reason = (code == 415 ? "Unsupported Media Type" :
						(code >= 500 ? "Internal Server Error" :
										(code == 409 ? "Conflict" :
										(code == 413 ? "Payload Too Large" : "Bad Request"))));
			res.headers.set("Content-Type", "text/plain; charset=utf-8");
			std::ostringstream cl; cl << (unsigned long)text.size();
			res.headers.set("Content-Length", cl.str());
			res.body.assign(text.begin(), text.end());
			res.bodyLength = text.size();
			res.headers.set("Connection", close ? "close" : "keep-alive");
			res.headers.set("Server", "webserv/1.0");
			return true;
		}
	};

	// config
	const Location* L = ctx.loc;
	if (!L) { 
		fprintf(stderr, "[UPLOAD] no location\n"); 
		return Err::send(res, 500, "No location", true); 
	}
	upload_dir_   = L->upload_store;
	overwrite_    = L->upload_overwrite;
	per_part_cap_ = L->upload_max_file_size;

	fprintf(stderr, "[UPLOAD] dir='%s' overwrite=%d per_part_cap=%lu\n",
			upload_dir_.c_str(), overwrite_?1:0, (unsigned long)per_part_cap_);

	if (upload_dir_.empty())
		return Err::send(res, 501, "Uploads disabled (upload_store empty)", true);
	if (!ensureUploadDirIsSafe(upload_dir_))
		return Err::send(res, 500, "Bad upload_store (missing or not a directory)", true);

	// content-type
	const std::string ctype = req.getHeaders().get(HDR_CONTENT_TYPE);
	fprintf(stderr, "[UPLOAD] Content-Type: %s\n", ctype.c_str());
	MultipartReader mp;
	if (!mp.initFromContentType(ctype))
		return Err::send(res, 415, "multipart/form-data required", true);

	// body source
	std::string body_file = req.getBodyFilePath();
	std::vector<char> mem;
	if (body_file.empty())
		mem = req.readBodyToVector();

	fprintf(stderr, "[UPLOAD] body_file='%s' mem_size=%lu\n",
			body_file.c_str(), (unsigned long)mem.size());

	if (body_file.empty() && mem.empty())
		return Err::send(res, 400, "Body unavailable (no temp file, no memory body)", true);

	// stream into parser
	const std::size_t CHUNK = 64 * 1024;
	if (!body_file.empty()) {
		int fd = ::open(body_file.c_str(), O_RDONLY);
		if (fd < 0) {
			std::ostringstream m; m << "Body file open failed: " << body_file << " errno=" << errno;
			return Err::send(res, 400, m.str(), true);
		}
		std::vector<char> buf; buf.resize(CHUNK);
		for (;;) {
			ssize_t r = ::read(fd, &buf[0], (int)buf.size());
			if (r == 0)
				break;
			if (r < 0) { 
				if (errno==EINTR) 
					continue; 
				::close(fd); 
				return Err::send(res, 400, "Read error", true); 
			}
			mp.feed(&buf[0], (std::size_t)r, this);
			if (mp.error()) { 
				::close(fd); 
				break; 
			}
		}
		::close(fd);
	} else {
		if (!mem.empty())
			mp.feed(&mem[0], mem.size(), this);
	}

	// single, consolidated error mapping (incl. conflicts/limits from sink)
	if (mp.error()) {
		int code = last_error_code_ ? last_error_code_ : 400;
		std::string msg = last_error_msg_.empty()
			? std::string("multipart parse error: ") + mp.errorMsg()
			: last_error_msg_;
		return Err::send(res, code, msg, (code >= 500));
	}

	if (!mp.done())
		return Err::send(res, 400, "Incomplete multipart (no closing boundary)", false);

	// success
	std::ostringstream ok;
	ok << "Uploaded:";
	for (size_t i=0;i<saved_.size();++i)
		ok << (i? ", ":" ") << saved_[i];
	std::string text = ok.str() + "\n";
	res.status = saved_.empty() ? 200 : 201;
	res.reason = saved_.empty() ? "OK" : "Created";
	res.headers.set("Content-Type", "text/plain; charset=utf-8");
	std::ostringstream cl; cl << (unsigned long)text.size();
	res.headers.set("Content-Length", cl.str());
	res.body.assign(text.begin(), text.end());
	res.bodyLength = text.size();
	res.headers.set("Connection", "keep-alive");
	res.headers.set("Server", "webserv/1.0");
	return true;
}


