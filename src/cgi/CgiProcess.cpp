/* --- CgiProcess.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "CgiProcess.h"

#include <unistd.h>     // pipe, fork, dup2, execve, close
#include <fcntl.h>      // fcntl
#include <cstring>      // strerror
#include <cerrno>
#include <stdexcept>
#include <sys/wait.h>   // waitpid
#include <signal.h>     // kill
#include <sys/time.h>   // gettimeofday

// ---- tiny helpers ---------------------------------------------------------

static inline int xclose(int& fd) {
    if (fd >= 0) { ::close(fd); fd = -1; }
    return 0;
}

unsigned long long CgiProcess::nowMs() {
    struct timeval tv; ::gettimeofday(&tv, 0);
    return (unsigned long long)tv.tv_sec * 1000ULL
         + (unsigned long long)(tv.tv_usec / 1000ULL);
}

bool CgiProcess::setNonBlocking(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

bool CgiProcess::setCloseOnExec(int fd) {
    int fl = ::fcntl(fd, F_GETFD, 0);
    if (fl < 0) return false;
    return ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC) == 0;
}

// ---- lifecycle ------------------------------------------------------------

CgiProcess::CgiProcess()
: _pid(-1), _in(-1), _out(-1), _deadline(0ULL) {}

CgiProcess::~CgiProcess() {
    terminate(); // safe if already reaped/closed
}

bool CgiProcess::spawn(const std::string& bin,
                       const std::string& script,
                       char* const argv[],
                       char* const envp[],
                       int timeout_ms)
{
    (void)script;
    // Create pipes: parent → child (stdin), child → parent (stdout+stderr)
    int inpipe[2]  = {-1, -1}; // [0]=read (child stdin), [1]=write (parent)
    int outpipe[2] = {-1, -1}; // [0]=read (parent),     [1]=write (child)

    if (::pipe(inpipe)  != 0) return false;
    if (::pipe(outpipe) != 0) { xclose(inpipe[0]); xclose(inpipe[1]); return false; }

    pid_t p = ::fork();
    if (p < 0) {
        xclose(inpipe[0]); xclose(inpipe[1]);
        xclose(outpipe[0]); xclose(outpipe[1]);
        return false;
    }

    if (p == 0) {
        // --- Child ---
        // stdin ← inpipe[0]
        // stdout/err → outpipe[1]
        ::dup2(inpipe[0],  STDIN_FILENO);
        ::dup2(outpipe[1], STDOUT_FILENO);
        ::dup2(outpipe[1], STDERR_FILENO);

        // Close all pipe ends we duplicated
        xclose(inpipe[0]); xclose(inpipe[1]);
        xclose(outpipe[0]); xclose(outpipe[1]);

        // If you need to pass the script path via argv[1], do so in the caller.
        // Here we simply execve(bin, argv, envp).
        ::execve(bin.c_str(), argv, envp);

        // If execve fails:
        _exit(127);
    }

    // --- Parent ---
    _pid = p;

    // Parent owns: inpipe[1] (write), outpipe[0] (read)
    _in  = inpipe[1];
    _out = outpipe[0];

    // Close the child ends
    xclose(inpipe[0]);
    xclose(outpipe[1]);

    // Make parent ends non-blocking + close-on-exec
    setNonBlocking(_in);
    setNonBlocking(_out);
    setCloseOnExec(_in);
    setCloseOnExec(_out);

    // Set deadline (soft; caller enforces by comparing nowMs() > deadlineMs())
    if (timeout_ms <= 0) timeout_ms = 3000; // a sane default
    _deadline = nowMs() + (unsigned long long)timeout_ms;

    return true;
}

void CgiProcess::closeIn()  { (void)xclose(_in);  }
void CgiProcess::closeOut() { (void)xclose(_out); }
void CgiProcess::closeBoth(){ closeIn(); closeOut(); }

int CgiProcess::waitNonBlocking(int* raw_status) {
    if (_pid <= 0) return -1;

    int st = 0;
    pid_t r = ::waitpid(_pid, &st, WNOHANG);
    if (r == 0) {
        // still running
        return 0;
    }
    if (r < 0) {
        // wait error; treat as error
        return -1;
    }

    // child reaped
    if (raw_status) *raw_status = st;

    int code = 0;
    if (WIFEXITED(st)) {
        code = WEXITSTATUS(st);
    } else if (WIFSIGNALED(st)) {
        code = 128 + WTERMSIG(st); // common convention
    }

    _pid = -1;
    closeBoth();
    return code > 0 ? code : 1; // >0 means finished; return a positive number
}

void CgiProcess::terminate() {
    if (_pid > 0) {
        // try to reap without killing first
        int dummy = 0;
        int rc = waitNonBlocking(&dummy);
        if (rc == 0) {
            // still running → kill
            ::kill(_pid, SIGKILL);
            // reap
            (void)::waitpid(_pid, &dummy, 0);
        }
        _pid = -1;
    }
    closeBoth();
}
