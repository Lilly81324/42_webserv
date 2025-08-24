/* --- CgiProcess.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef CGIPROCESS_H
#define CGIPROCESS_H

#include <string>
#include <vector>
#include <sys/types.h> // pid_t

/**
 * @brief Thin wrapper around a CGI child process with stdin/stdout pipes.
 *
 * Usage:
 *   CgiProcess p;
 *   if (!p.spawn(bin, script, argv, envp, timeout_ms)) { ...error... }
 *   // write request body to p.inFD(), read CGI output from p.outFD()
 *   // poll/select around those FDs in nonblocking mode
 *   // when done: p.terminate() (safe even if already exited)
 */
class CgiProcess {
public:
    CgiProcess();
    ~CgiProcess();

    /**
     * @param bin        path to interpreter/binary (e.g. /usr/bin/php-cgi)
     * @param script     script path (pass empty if your CGI uses SCRIPT_FILENAME only)
     * @param argv       null-terminated argv for execve (argv[0] should be bin)
     * @param envp       null-terminated envp for execve (CGI variables)
     * @param timeout_ms soft timeout tracked as a deadline (not enforced automatically)
     * @return true on success (child running), false on error
     */
    bool spawn(const std::string& bin,
               const std::string& script,
               char* const argv[],
               char* const envp[],
               int timeout_ms);

    // FDs for parent side: write body to inFD(), read response from outFD()
    int  inFD()  const { return _in;  }  // parent writes → child's stdin
    int  outFD() const { return _out; }  // parent reads  ← child's stdout/stderr
    pid_t pid()  const { return _pid; }

    // Close our pipe ends (idempotent)
    void closeIn();
    void closeOut();
    void closeBoth();

    // Nonblocking status check; returns:
    //  0 = still running, >0 = exited (status code), <0 = error
    int  waitNonBlocking(int* raw_status);

    // Send SIGKILL (if still running) and reap
    void terminate();

    // Helpers
    static bool setNonBlocking(int fd);
    static bool setCloseOnExec(int fd);

    // Deadline helpers (you can compare nowMs() with deadlineMs())
    static unsigned long long nowMs();
    unsigned long long        deadlineMs() const { return _deadline; }

private:
    pid_t              _pid;
    int                _in;       // write end to child's stdin
    int                _out;      // read end from child's stdout/stderr
    unsigned long long _deadline; // absolute ms (for external timeout logic)
};

#endif // CGIPROCESS_H

