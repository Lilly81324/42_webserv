// src/http/handlers/UploadHandler.cpp
#include "UploadHandler.h"
#include "RequestContext.h"
#include "VirtualServer.h"
#include "ResponseFactory.h"
#include "HEADER_ENTRIES.h"
#include "Util.h"
#include "PathUtil.h"

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>
#include <stdlib.h> // realpath
#include <limits.h> // PATH_MAX
#include <iostream>

/*

realpathString(const std::string& in, std::string& out) (static)

This tiny helper canonicalizes a path string: it calls Util::realpath 
into a fixed buffer, and if that succeeds it assigns the absolute, 
symlink-resolved result to out and returns true; otherwise false. 
We use it to ensure the configured upload directory points to a real, 
safe location before any file I/O begins. Canonicalization defuses tricks 
like ../ traversal or symlinks that could redirect writes outside the intended area. 
It keeps the rest of the handler simple: subsequent checks can rely on the path 
being absolute and real, reducing error ambiguity. By isolating the “turn a string 
into a canonical path” step, the code remains readable and testable—ensureUploadDirIsSafe 
can just call this and then stat to finish validation.


*/

static bool realpathString(const std::string &in, std::string &out)
{
    char tmp[4096];
    if (Util::realpath(in.c_str(), tmp) == 0)
        return false;
    out.assign(tmp);
    return true;
}

/*

UploadHandler::UploadHandler() / ~UploadHandler()

The constructor zeroes all per-request state so an instance is ready 
to accept the first multipart part: cur_fd_ = -1 (no file open yet), 
cur_written_ = 0 (no bytes written), and last_error_code_ = 0 (no error). 
The destructor is defensive RAII: if a temp file descriptor remains open 
when the handler is destroyed, it’s closed to avoid leaks. We create a new 
UploadHandler per request (or per upload phase), so having a stable, zeroed 
baseline matters on keep-alive connections—no state should carry across requests. 
The minimal dtor also ensures that early exits (parser errors, client disconnects) 
don’t orphan descriptors. This pairing keeps resource management localized and safe 
without sprinkling close calls everywhere.



*/

UploadHandler::UploadHandler() : cur_fd_(-1), cur_written_(0), last_error_code_(0) {}

UploadHandler::~UploadHandler()
{
    if (cur_fd_ >= 0)
        ::close(cur_fd_);
}

/*

sanitizeFilename(const std::string& in) const

This function generates a safe basename from a user-supplied filename. 
It first normalizes path separators (\ → /) and strips any directory components 
to prevent directory traversal. It rejects "." and ".." by clearing the name. 
Then it whitelists characters [A–Z a–z 0–9 . _ -], replacing everything else with _, 
ensuring the resulting name is filesystem-friendly and log-safe. If the result is empty, 
it falls back to "file". Finally, it truncates to 128 characters to keep names manageable. 
We use it to derive cur_filename_sanit_ at part start; later, the temp file 
(created with a unique name) is atomically renamed to this sanitized name. 
The separation “random temp name during streaming” and “sanitized final name on commit” 
prevents partial exposures and ensures predictable results for clients and logs.


*/

std::string UploadHandler::sanitizeFilename(const std::string &in) const
{
    // drop directories, keep basename
    std::string name = in;
    for (std::size_t i = 0; i < name.size(); ++i)
        if (name[i] == '\\')
            name[i] = '/';
    std::string::size_type s = name.rfind('/');
    if (s != std::string::npos)
        name = name.substr(s + 1);

    // reject dotdot
    if (name == "." || name == "..")
        name.clear();

    // allow [A-Za-z0-9._-], replace others with '_'
    std::string out;
    out.reserve(name.size());
    for (std::size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        bool ok =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
    }
    if (out.empty())
        out = "file";
    if (out.size() > 128)
        out = out.substr(0, 128);
    return out;
}

/*

ensureUploadDirIsSafe(const std::string& base) const

Validates the configured upload directory. It first canonicalizes 
base via realpathString; if that fails, the directory is considered invalid. 
Next it calls ::stat on the canonical path and checks S_ISDIR to confirm it 
is an existing directory—not a file, not a broken link. Only if both checks 
pass does it return true. We call this at the top of handle() so we can fail 
quickly with a clear error if the destination is missing or misconfigured, 
avoiding half-written files and confusing mid-stream write errors. This function 
focuses purely on safety preconditions; once it succeeds, later parts of the code 
can assume the base directory is legitimate and writable (subject to permissions), 
simplifying the control flow of the upload path.


*/

bool UploadHandler::ensureUploadDirIsSafe(const std::string &base) const
{
    // ensure realpath(upload_dir) exists and is a directory
    std::string canon;
    if (!realpathString(base, canon))
        return false;
    struct stat st;
    if (::stat(canon.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    return true;
}

/*


openTempInUpload(std::string& out_path, int& out_fd)

Creates a unique temporary file inside the configured upload directory. 
It builds a name like upload-tmp-<salt>-<i> and attempts open(O_CREAT|O_EXCL|O_RDWR, 0600) 
up to 256 times. Using O_EXCL with a random salt avoids races/clobbering and guarantees uniqueness; 
0600 ensures the temp file is not world-readable during transfer. On success it sets FD_CLOEXEC to prevent 
descriptor leakage into child processes, fills out_path and out_fd, and returns true. 
We call this at each onPartBegin so each multipart part streams into its own isolated temp. 
If all attempts fail (disk full/permissions), we record a 500 and abort. The design lets us 
stream immediately without coordinating final names; when the part ends we atomically 
rename to the sanitized target, so observers either see nothing or a complete file—never a partial.


*/

bool UploadHandler::openTempInUpload(std::string &out_path, int &out_fd)
{
    const std::string dir = upload_dir_;
    const std::string prefix = "upload";
    const int MAX_ATTEMPTS = 256;
    unsigned int salt = (unsigned int)std::rand();

    for (int i = 0; i < MAX_ATTEMPTS; ++i)
    {
        std::ostringstream name;
        name << (dir.empty() ? "." : dir);
        if (!dir.empty() && dir[dir.size() - 1] != '/')
            name << '/';
        name << (prefix.empty() ? "upload" : prefix)
             << "-tmp-"
             << salt << "-" << i;

        const std::string path = name.str();

        int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0)
        {
            (void)fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
            out_path = path;
            out_fd = fd;
            return true;
        }
    }
    return false;
}

/*

finishMoveAtomic()

Commits the current part: it closes cur_fd_, 
constructs the destination path as upload_dir_ + "/" + cur_filename_sanit_, 
and enforces overwrite policy. If overwrite_ is false and dst already exists 
(stat succeeds), it records a 409 Conflict and returns false. Otherwise it 
calls std::rename(cur_tmp_path_, dst) to move the finished temp into place 
atomically. On rename failure, it records a 500 and returns false. On success 
it pushes the saved filename into saved_, clears cur_tmp_path_, and returns true. 
We call this from onPartEnd. The atomic rename is the crux of safe uploads: 
consumers never see partial writes, and either the destination becomes the new 
file instantly or not at all. The explicit 409 behavior lets APIs be idempotent 
when desired. Errors propagate back to the multipart parser so the HTTP layer 
can return accurate status and stop further processing.


*/

bool UploadHandler::finishMoveAtomic()
{
    if (cur_fd_ < 0)
        return false;
    ::close(cur_fd_);
    cur_fd_ = -1;

    std::string dst = upload_dir_;
    if (!dst.empty() && dst[dst.size() - 1] != '/')
        dst += '/';
    dst += cur_filename_sanit_;

    if (!overwrite_)
    {
        struct stat st;
        if (::stat(dst.c_str(), &st) == 0)
        {
            last_error_code_ = 409;
            last_error_msg_ = "File exists: " + cur_filename_sanit_;
            return false; // will cause parser to stop with sink error
        }
    }
    if (std::rename(cur_tmp_path_.c_str(), dst.c_str()) != 0)
    {
        last_error_code_ = 500;
        last_error_msg_ = "Rename failed";
        return false;
    }
    saved_.push_back(cur_filename_sanit_);
    cur_tmp_path_.clear();
    return true;
}

// ---- IMultipartSink ----


/* 


onPartBegin(const Part& p)

This is the multipart sink hook called when a new part starts. It records the form 
field name and the raw filename, computes a sanitized filename, resets byte counters, 
clears any previous temp path, and calls openTempInUpload to create a per-part temp file. 
If temp creation fails, it sets a 500 and returns false, which instructs the parser to stop 
and bubble the error. We keep file creation here (rather than lazily on first data) to surface 
path/permission problems early and to simplify onPartData—it can assume a valid cur_fd_. 
The separation of “begin”/“data”/“end” keeps streaming linear and stateless between 
callbacks beyond the minimum fields needed to finish or abort.


*/

bool UploadHandler::onPartBegin(const Part &p)
{
    cur_name_ = p.name;
    cur_filename_raw_ = p.filename;
    cur_filename_sanit_ = sanitizeFilename(cur_filename_raw_);
    cur_written_ = 0;
    cur_fd_ = -1;
    cur_tmp_path_.clear();

    if (!openTempInUpload(cur_tmp_path_, cur_fd_))
    {
        last_error_code_ = 500;
        last_error_msg_ = "temporary file open failed";
        return false;
    }
    return true;
}


/* 

onPartData(const char* data, std::size_t n)

Writes a chunk of the current part into the temp file. 
First it enforces an optional per-part cap (per_part_cap_), 
returning false with 413 Payload Too Large if exceeded. Then it loops 
on ::write, advancing off on positive returns, retrying on EINTR, 
and treating any other result as a fatal write error (record 500, return false). 
On success it increments cur_written_ and returns true. We keep this loop tight 
and non-blocking friendly; with regular files it won’t block, but the EINTR 
retry still matters. Pushing the cap check here (rather than after the fact) 
saves disk and time on oversized uploads. Any false return signals 
the multipart reader to abort and propagate the error 
to handle(), which constructs the final HTTP response accordingly.


*/

bool UploadHandler::onPartData(const char *data, std::size_t n)
{
    if (cur_fd_ < 0)
        return false;

    // per-part cap
    if (per_part_cap_ && cur_written_ + n > per_part_cap_)
    {
        last_error_code_ = 413;             // <--- add
        last_error_msg_ = "part too large"; // <--- add
        return false;                       // sink refuses -> parser stops
    }

    std::size_t off = 0;
    while (off < n)
    {
        ssize_t w = ::write(cur_fd_, data + off, (n - off));
        if (w > 0)
            off += (std::size_t)w;
        else if (w < 0 && errno == EINTR)
            continue;
        else
        {
            last_error_code_ = 500;
            last_error_msg_ = "write failed";
            return false;
        }
    }
    cur_written_ += n;
    return true;
}


/* 

onPartEnd()

Closes out the current part by delegating to finishMoveAtomic(). 
Returning true means the temp was renamed into place (or there was nothing to do); 
returning false carries forward the specific error and message already 
set by finishMoveAtomic (e.g., 409 Conflict for existent target, or 500 for rename failure). 
We keep onPartEnd tiny so the end-of-part behavior remains predictable and auditable: 
commit or fail atomically, no partial states. This mirrors common upload best 
practices and makes the error path identical whether the issue was size policy, 
disk problems, or filename collisions.


*/


bool UploadHandler::onPartEnd()
{
    return finishMoveAtomic();
}

// ---- handler entry ----


/* 

handle(HttpRequest& req, HttpResponse& res, RequestContext& ctx)

Top-level entry for processing an upload request. It resets per-request state, 
pulls the Location config (upload_store, upload_overwrite, upload_max_file_size) 
from ctx, and validates the upload dir with ensureUploadDirIsSafe. It determines 
Content-Type: prefer the header; if missing, it sniffs the boundary from the first 
body line (either in memory or from the temp body file) using the nested Sniffer helper. 
It then demands a multipart/form-data prefix (case-insensitive). Next it constructs a MultipartReader, 
initializes it from the Content-Type, and feeds the body either from disk (chunked read loop 
with EINTR handling) or memory. The reader invokes our sink (onPartBegin/Data/End) as it parses. 
Any sink/parse error is translated into an HTTP response via the nested Err::send helper with the right 
status (415, 413, 409, 500…), content type text/plain, a short message body, and connection semantics. 
On success, it returns 201 Created (or 200 OK if nothing was saved) and lists saved filenames. 
This function ties configuration, validation, streaming, and HTTP response composition into one cohesive 
path while keeping each step small and testable.



*/




bool UploadHandler::handle(HttpRequest &req, HttpResponse &res, RequestContext &ctx)
{
    // ---------- helpers ----------
    struct Err
    {
        static bool send(HttpResponse &res, int code, const std::string &msg, bool close)
        {
            std::string text = msg + "\n";
            res.status = code;
            if (code == 415)
                res.reason = "Unsupported Media Type";
            else if (code == 409)
                res.reason = "Conflict";
            else if (code == 413)
                res.reason = "Payload Too Large";
            else if (code >= 500)
                res.reason = "Internal Server Error";
            else
                res.reason = "Bad Request";

            res.headers.set("Content-Type", "text/plain; charset=utf-8");
            std::ostringstream cl;
            cl << (unsigned long)text.size();
            res.headers.set("Content-Length", cl.str());
            res.body.assign(text.begin(), text.end());
            res.bodyLength = text.size();
            res.headers.set("Connection", close ? "close" : "keep-alive");
            res.headers.set("Server", "webserv/1.0");
            return true;
        }
    };

    // Sniff boundary from the beginning of a multipart body if Content-Type is missing.
    // We look for a first line like:  "--<boundary>\r\n"
    struct Sniffer
    {
        static std::string sniffFromMem(const std::vector<char> &v)
        {
            if (v.size() < 6)
                return std::string(); // minimal "--x\r\n"
            // find end of first line (CRLF or LF)
            std::size_t i = 0, n = v.size();
            while (i < n && v[i] != '\n' && !(i + 1 < n && v[i] == '\r' && v[i + 1] == '\n'))
                ++i;
            if (i == 0 || i >= n)
                return std::string();
            std::string line(&v[0], &v[0] + i);
            if (line.size() >= 3 && line[0] == '-' && line[1] == '-')
            {
                std::string b = line.substr(2);
                // trim simple spaces/tabs defensively
                while (!b.empty() && (b[b.size() - 1] == ' ' || b[b.size() - 1] == '\t'))
                    b.erase(b.size() - 1);
                while (!b.empty() && (b[0] == ' ' || b[0] == '\t'))
                    b.erase(0, 1);
                if (!b.empty())
                    return b;
            }
            return std::string();
        }
        static std::string sniffFromFile(const std::string &path)
        {
            int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0)
                return std::string();
            char buf[4096];
            ssize_t r = ::read(fd, buf, (int)sizeof(buf));
            ::close(fd);
            if (r <= 0)
                return std::string();
            std::vector<char> v(buf, buf + r);
            return sniffFromMem(v);
        }
    };

    // ---------- per-request reset ----------
    saved_.clear();
    last_error_code_ = 0;
    last_error_msg_.clear();
    if (cur_fd_ >= 0)
    {
        ::close(cur_fd_);
        cur_fd_ = -1;
    }

    // ---------- config ----------
    const Location *L = ctx.loc;
    if (!L)
    {
#if defined(DEBUG)
        std::fprintf(stderr, "[UPLOAD] no location\n");
#endif
        return Err::send(res, 500, "No location", true);
    }
    upload_dir_ = L->upload_store;
    overwrite_ = L->upload_overwrite;
    per_part_cap_ = L->upload_max_file_size;

#if defined(DEBUG)
    std::fprintf(stderr, "[UPLOAD] dir='%s' overwrite=%d per_part_cap=%lu\n",
                 upload_dir_.c_str(), overwrite_ ? 1 : 0, (unsigned long)per_part_cap_);
#endif

    if (upload_dir_.empty())
        return Err::send(res, 501, "Uploads disabled (upload_store empty)", true);
    if (!ensureUploadDirIsSafe(upload_dir_))
        return Err::send(res, 500, "Bad upload_store (missing or not a directory)", true);

    // ---------- content-type (prefer header; fallback to sniff) ----------
    std::string ctype = req.getHeaders().get("Content-Type");
#if defined(DEBUG)
    std::fprintf(stderr, "[UPLOAD] Content-Type(raw): %s\n", ctype.c_str());
#endif

    std::string body_file = req.getBodyFilePath();
    std::vector<char> mem;
    if (body_file.empty())
        mem = req.readBodyToVector();

    if (ctype.empty())
    {
        std::string b = !body_file.empty()
                            ? Sniffer::sniffFromFile(body_file)
                            : Sniffer::sniffFromMem(mem);
        if (!b.empty())
        {
            ctype = "multipart/form-data; boundary=" + b;
#if defined(DEBUG)
            std::fprintf(stderr, "[UPLOAD] inferred Content-Type: %s\n", ctype.c_str());
#endif
        }
    }

    // Validate multipart/form-data prefix (case-insensitive)
    bool ct_ok = false;
    if (!ctype.empty())
    {
        const char *want = "multipart/form-data";
        // C++98: lowercase the prefix and compare
        std::string pref = ctype.substr(0, 19);
        for (std::size_t i = 0; i < pref.size(); ++i)
        {
            char ch = pref[i];
            if (ch >= 'A' && ch <= 'Z')
                pref[i] = char(ch - 'A' + 'a');
        }
        ct_ok = (pref == want);
    }
    if (!ct_ok)
        return Err::send(res, 415, "multipart/form-data required", false);

    // ---------- parse multipart ----------
    MultipartReader mp;
    if (!mp.initFromContentType(ctype))
    {
#if defined(DEBUG)
        std::fprintf(stderr, "[UPLOAD] Multipart init failed; CT='%s'\n", ctype.c_str());
#endif
        return Err::send(res, 415, "multipart/form-data required", false);
    }

#if defined(DEBUG)
    std::fprintf(stderr, "[UPLOAD] body_file='%s' mem_size=%lu\n",
                 body_file.c_str(), (unsigned long)mem.size());
#endif
    if (body_file.empty() && mem.empty())
        return Err::send(res, 400, "Body unavailable (no temp file, no memory body)", true);

    const std::size_t CHUNK = 64 * 1024;
    if (!body_file.empty())
    {
        int fd = ::open(body_file.c_str(), O_RDONLY);
        if (fd < 0)
        {
            std::ostringstream m;
            m << "Body file open failed: " << body_file << " errno=" << errno;
            return Err::send(res, 400, m.str(), true);
        }
        std::vector<char> buf;
        buf.resize(CHUNK);
        for (;;)
        {
            ssize_t r = ::read(fd, &buf[0], (int)buf.size());
            if (r == 0)
                break;
            if (r < 0)
            {
                if (errno == EINTR)
                    continue;
                ::close(fd);
                return Err::send(res, 400, "Read error", true);
            }
            mp.feed(&buf[0], (std::size_t)r, this);
            if (mp.error())
            {
                ::close(fd);
                break;
            }
        }
        ::close(fd);
    }
    else
    {
        if (!mem.empty())
            mp.feed(&mem[0], mem.size(), this);
    }

    // ---------- errors ----------
    if (mp.error())
    {
        int code = last_error_code_ ? last_error_code_ : 400;
        std::string msg = last_error_msg_.empty()
                              ? std::string("multipart parse error: ") + mp.errorMsg()
                              : last_error_msg_;
        return Err::send(res, code, msg, (code >= 500));
    }
    if (!mp.done())
        return Err::send(res, 400, "Incomplete multipart (no closing boundary)", false);

    // ---------- success ----------
    std::ostringstream ok;
    ok << "Uploaded:";
    for (size_t i = 0; i < saved_.size(); ++i)
        ok << (i ? ", " : " ") << saved_[i];

    std::string text = ok.str() + "\n";
    res.status = saved_.empty() ? 200 : 201;
    res.reason = saved_.empty() ? "OK" : "Created";
    res.headers.set("Content-Type", "text/plain; charset=utf-8");
    std::ostringstream cl;
    cl << (unsigned long)text.size();
    res.headers.set("Content-Length", cl.str());
    res.body.assign(text.begin(), text.end());
    res.bodyLength = text.size();
    res.headers.set("Connection", "keep-alive");
    res.headers.set("Server", "webserv/1.0");
    return true;
}
