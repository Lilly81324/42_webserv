/* --- CgiProcess.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "CgiProcess.h"
#include "VirtualServer.h" // full definition of CgiSpec for the 3-arg overload
#include <unistd.h>		   // pipe, fork, dup2, execve, close
#include <fcntl.h>		   // fcntl
#include <cstring>		   // strerror
#include <cerrno>
#include <stdexcept>
#include <sys/wait.h> // waitpid
#include <signal.h>	  // kill

#include <vector>
#include <ctime>

// ---- tiny helpers ---------------------------------------------------------

/*

static inline int xclose(int &fd)

What it does. A tiny utility that closes a
file descriptor if it’s open (fd >= 0) and then sets it to -1.
Returning 0 keeps call sites terse.
Why we use it. Pipe/file-descriptor cleanup happens in many places
(error paths, normal shutdown, timeouts). Repeating if (fd>=0){close(fd); fd=-1;}
invites mistakes (double-close, forgetting to reset).
Centralizing the pattern eliminates those bugs and makes close
calls idempotent—safe to call multiple times.
How it fits. The class exposes closeIn(), closeOut(),
and closeBoth() that all defer to xclose. That keeps higher-level
logic readable: “we’re done writing → closeIn(),” “response complete → closeOut(),
” without worrying about current state. This is especially helpful under non-blocking I/O
where a write or read may race with a hangup: handlers can just call close helpers confidently.
The function also improves exception safety—if an early step in spawn fails,
the cleanup code can call xclose on all ends without branching.
Overall, xclose is a micro-RAII building block that keeps the descriptor
lifecycle sane across success and failure paths.


*/

static inline int xclose(int &fd)
{
	if (fd >= 0)
	{
		::close(fd);
		fd = -1;
	}
	return 0;
}

/*

void closeTrackedFds(std::vector<int> tracked)

What it does. Iterates a list of fds (collected in the parent’s event loop)
and closes them in the child after fork().
Why we use it. After fork, the child inherits all open descriptors from the parent.
If it then execves a CGI, those inherited fds can (1) keep client sockets alive unexpectedly,
(2) prevent listeners from closing cleanly, or (3) leak resources into the CGI process.
Closing tracked fds in the child collapses the set down to just the intended stdin/stdout pipes.
How it fits. In the high-level spawn path, right before building argv/envp and calling execve,
the child closes the pipe copies and then calls closeTrackedFds(tracked).
This removes every connection/listener fd your event loop knows about,
ensuring the CGI subprocess only sees the minimal standard streams.
That guarantees (a) no accidental sharing of sockets; (b) POLLHUP will propagate to
the parent when expected; and (c) resource usage is predictable.
It complements FD_CLOEXEC (used on parent ends)
by covering the pre-exec window and any fds that might not carry the flag.
In short, it’s defense-in-depth for descriptor hygiene around fork/exec.


*/

void closeTrackedFds(std::vector<int> tracked)
{
	std::vector<int>::const_iterator end = tracked.end();
	for (std::vector<int>::const_iterator it = tracked.begin();
		 it != end; it++)
	{
		close(*it);
	}
}

/*

bool CgiProcess::setNonBlocking(int fd)

What it does. Fetches the current flags with fcntl(F_GETFL),
ORs in O_NONBLOCK, and writes them back via fcntl(F_SETFL).
Returns true on success.
Why we use it. Your entire server runs under a single poll() invariant.
If the CGI pipes (stdin/out) were blocking, a slow child could stall the
reactor thread during read()/write(). Marking both ends non-blocking ensures
that all traffic to/from the CGI integrates with the event loop just like sockets do:
you only read after POLLIN, only write after POLLOUT, and never block.
How it fits. After the parent keeps its ends of the pipes
(_in for writing to child stdin, _out for reading child stdout),
it applies setNonBlocking to both. From that point, your ClientHandler/CGIStreamer
can safely stream request bodies into the child and relay CGI output back to the
client using the same readiness logic as sockets. Because the project forbids branching
on errno after read/write, non-blocking style plus poll readiness
is the compliant way to avoid guessing about EAGAIN conditions.
This helper encapsulates the exact fcntl dance so spawn logic stays clear.

*/

bool CgiProcess::setNonBlocking(int fd)
{
	int fl = ::fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		return false;
	return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

/*

bool CgiProcess::setCloseOnExec(int fd)

What it does. Reads descriptor flags via fcntl(F_GETFD)
and sets FD_CLOEXEC through fcntl(F_SETFD). Returns true on success.
Why we use it. FD_CLOEXEC ensures that if the parent later spawns any
other child process (another CGI, maintenance tool, etc.), these fds won’t leak
into that child. Leaked listeners or client sockets are a classic source of “port won’t
free” and “connection never closes” heisenbugs.
How it fits. Immediately after the parent adopts _in/_out, it marks them CLOEXEC.
In combination with closeTrackedFds in the child, this achieves robust isolation:
(1) current child gets only what it needs; (2) future children won’t inherit today’s
CGI pipes. Keeping this in a dedicated helper makes the spawn code readable and lets
you uniformly apply the policy to any new fds you add to the class.
It’s also cheap and C++98-friendly, fitting the project’s allowed syscalls.

*/

bool CgiProcess::setCloseOnExec(int fd)
{
	int fl = ::fcntl(fd, F_GETFD, 0);
	if (fl < 0)
		return false;
	return ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC) == 0;
}

// ---- lifecycle ------------------------------------------------------------

/*

CgiProcess::CgiProcess() / ~CgiProcess()

What they do. The constructor initializes _pid=-1, _in=-1, _out=-1, _deadline=0.
The destructor calls terminate() to ensure any live child is reaped
and both pipe ends are closed.
Why we use them. A CGI process is a heavyweight resource (child PID + 2 fds).
You want a safe default (no child, no fds) and a destructor that cannot
leak zombies or descriptors on exception/early return paths.
How they fit. Handlers can stack-allocate or member-own CgiProcess.
If a request aborts or the connection closes unexpectedly,
simply letting the object go out of scope guarantees cleanup: terminate()
will SIGTERM/SIGKILL as needed and xclose the pipes.
Keeping constructor/destructor light (no syscalls in the ctor)
means you can create the object cheaply when headers suggest CGI,
and only perform fork/pipe in spawn. This division of labor keeps
lifecycle explicit and predictable in the one-thread reactor.


*/

CgiProcess::CgiProcess()
	: _pid(-1), _in(-1), _out(-1), _deadline(0ULL) {}

CgiProcess::~CgiProcess()
{
	terminate(); // safe if already reaped/closed
}

/*

bool CgiProcess::spawn(const CgiSpec &spec, const std::string &script_path, const std::vector<std::string> &envv, std::vector<int> tracked)

What it does. High-level convenience spawn: creates two pipes, forks,
sets up stdio redirection in the child (dup2), closes extraneous fds (including parent-tracked ones),
builds argv=[bin, script, NULL] and envp, and calls execve(spec.bin, argv, envp).
The parent keeps _in (write to child stdin) and _out (read child stdout),
sets both non-blocking and CLOEXEC, and stores _pid.
Why we use it. Most CGI launches follow the same pattern:
run the configured interpreter (spec.bin) against the resolved
script path with a computed environment. This wrapper lets callers
avoid re-implementing fork/pipe/dup/exec ceremonies and descriptor hygiene every time.
How it fits. It’s the path typically used by CgiHandler/CGIStreamer:
they supply spec, script, and environment, plus the current set of tracked fds so the
child doesn’t inherit any. If execve fails (rare), the child writes a small
message to stderr and then blocks in a select(0,...) loop so the parent’s
timeout logic can kill it—this avoids spinning CPU or accidentally continuing.
The parent path returns true on successful fork + setup;
the event loop then starts monitoring _out for POLLIN and _in for POLLOUT.

*/

// High-level convenience overload: build argv/envp and delegate
bool CgiProcess::spawn(const CgiSpec &spec,
					   const std::string &script_path,
					   const std::vector<std::string> &envv,
					   std::vector<int> tracked)
{
	closeBoth(); // if previously used
	_pid = -1;
	_in = -1;  // parent will WRITE to child's stdin
	_out = -1; // parent will READ  from child's stdout

	int pin[2] = {-1, -1};	// pipe for child's stdin  (parent writes -> child reads)
	int pout[2] = {-1, -1}; // pipe for child's stdout (child writes -> parent reads)

	if (::pipe(pin) < 0)
		return false;
	if (::pipe(pout) < 0)
	{
		::close(pin[0]);
		::close(pin[1]);
		return false;
	}

	pid_t pid = ::fork();
	if (pid < 0)
	{
		::close(pin[0]);
		::close(pin[1]);
		::close(pout[0]);
		::close(pout[1]);
		return false;
	}

	if (pid == 0)
	{
		// ---- Child ----
		// stdin: read end of pin
		::dup2(pin[0], STDIN_FILENO);
		// stdout: write end of pout
		::dup2(pout[1], STDOUT_FILENO);

		// close all pipe fds in child
		::close(pin[0]);
		::close(pin[1]);
		::close(pout[0]);
		::close(pout[1]);

		closeTrackedFds(tracked);

		// Build argv and envp (you already have helpers; keep minimal here)
		std::vector<char *> argv;
		argv.push_back(const_cast<char *>(spec.bin.c_str()));	 // /usr/bin/python3 (or interpreter)
		argv.push_back(const_cast<char *>(script_path.c_str())); // /path/to/script.py
		argv.push_back(0);

		std::vector<char *> envp;
		envp.reserve(envv.size() + 1);
		for (size_t i = 0; i < envv.size(); ++i)
			envp.push_back(const_cast<char *>(envv[i].c_str()));
		envp.push_back(0);

		::execve(spec.bin.c_str(), &argv[0], &envp[0]);

		// ---- execve failed: stay within allowed calls ----
		// Best effort message (ignore errors)
		const char msg[] = "execve failed\n";
		(void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);

		// Close stdio so we don't hold any pipe ends
		(void)::close(STDIN_FILENO);
		(void)::close(STDOUT_FILENO);
		(void)::close(STDERR_FILENO);

		// Block forever; parent will enforce timeouts and reap us.
		// select() is allowed; this avoids CPU spin.
		for (;;)
			(void)::select(0, 0, 0, 0, 0);
	}

	// ---- Parent ----
	_pid = pid;

	// Parent writes to child's stdin -> keep pin[1], close pin[0]
	::close(pin[0]);
	_in = pin[1];

	// Parent reads child's stdout -> keep pout[0], close pout[1]
	::close(pout[1]);
	_out = pout[0];

	// Set parent ends to NONBLOCK + CLOEXEC
	int fl;
	fl = ::fcntl(_in, F_GETFL, 0);
	if (fl >= 0)
		::fcntl(_in, F_SETFL, fl | O_NONBLOCK);
	fl = ::fcntl(_out, F_GETFL, 0);
	if (fl >= 0)
		::fcntl(_out, F_SETFL, fl | O_NONBLOCK);
	fl = ::fcntl(_in, F_GETFD, 0);
	if (fl >= 0)
		::fcntl(_in, F_SETFD, fl | FD_CLOEXEC);
	fl = ::fcntl(_out, F_GETFD, 0);
	if (fl >= 0)
		::fcntl(_out, F_SETFD, fl | FD_CLOEXEC);

	return true;
}

/*

bool CgiProcess::spawn(const std::string &bin, const std::string &script,
char *const *argv, char *const *envp, int timeout_ms)

What it does. Lower-level spawn that accepts a fully prepared argv/envp
and sets a deadline (_deadline = now + timeout_ms) for later timeout enforcement.
Implementation mirrors the high-level variant: create pipes, fork, wire child stdio via dup2,
merge stderr into stdout, and execve(bin, argv, envp). Parent side closes the opposite ends,
stores _pid/_in/_out, and marks both ends non-blocking and CLOEXEC.
Why we use it. Some CGIs or alternate interpreters need fine-grained
control over args/env. Providing this overload separates policy (what to execute)
from mechanism (how to execute it safely and non-blocking).
How it fits. Advanced handlers can prebuild argument vectors
(e.g., wrapper scripts, shebang handling) and still rely on the same safe
fork/exec scaffolding. The deadline is not enforced here directly; instead,
your event-driven supervisor can periodically check time and call terminate()
if the child overstays—keeping the no-blocking rule intact. The function’s error
handling follows the same discipline: on failure, it cleans up all opened pipe
ends before returning false.


*/

// Low-level spawn: do the actual fork/exec (stub for now; returns false)
bool CgiProcess::spawn(const std::string &bin,
					   const std::string &script,
					   char *const *argv,
					   char *const *envp,
					   int timeout_ms)
{
	// Clean up any previous child
	terminate();

	// Establish deadline (uses your allowed nowMs())
	_deadline = (timeout_ms > 0)
					? (TimeUtil::nowMs() + (unsigned long long)timeout_ms)
					: 0ULL;

	int inPipe[2] = {-1, -1};  // parent writes -> child reads (stdin)
	int outPipe[2] = {-1, -1}; // child writes -> parent reads (stdout/stderr)

	if (::pipe(inPipe) < 0)
		return false;
	if (::pipe(outPipe) < 0)
	{
		::close(inPipe[0]);
		::close(inPipe[1]);
		return false;
	}

	pid_t pid = ::fork();
	if (pid < 0)
	{
		// fork failed
		::close(inPipe[0]);
		::close(inPipe[1]);
		::close(outPipe[0]);
		::close(outPipe[1]);
		return false;
	}

	if (pid == 0)
	{
		// ---------------- child ----------------
		// close parent ends
		::close(inPipe[1]);
		::close(outPipe[0]);

		// stdin from inPipe[0]
		if (::dup2(inPipe[0], STDIN_FILENO) < 0)
		{
			const char msg[] = "dup2(stdin) failed\n";
			(void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
			(void)::close(inPipe[0]);
			(void)::close(outPipe[1]);
			(void)::close(STDIN_FILENO);
			(void)::close(STDOUT_FILENO);
			(void)::close(STDERR_FILENO);
			for (;;)
				(void)::select(0, 0, 0, 0, 0);
		}

		// stdout to outPipe[1]
		if (::dup2(outPipe[1], STDOUT_FILENO) < 0)
		{
			const char msg[] = "dup2(stdout) failed\n";
			(void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
			(void)::close(inPipe[0]);
			(void)::close(outPipe[1]);
			(void)::close(STDIN_FILENO);
			(void)::close(STDOUT_FILENO);
			(void)::close(STDERR_FILENO);
			for (;;)
				(void)::select(0, 0, 0, 0, 0);
		}

		// stderr merged to stdout (ignore error; best effort)
		(void)::dup2(outPipe[1], STDERR_FILENO);

		// close the originals after dup
		(void)::close(inPipe[0]);
		(void)::close(outPipe[1]);

		// Exec the interpreter/binary with provided argv/envp.
		// argv should look like: [bin, script, NULL]
		(void)script; // script should already be present in argv
		::execve(bin.c_str(),
				 const_cast<char *const *>(argv),
				 const_cast<char *const *>(envp));

		// ---- execve failed: keep within allowed calls ----
		const char msg[] = "execve failed\n";
		(void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
		(void)::close(STDIN_FILENO);
		(void)::close(STDOUT_FILENO);
		(void)::close(STDERR_FILENO);
		for (;;)
			(void)::select(0, 0, 0, 0, 0);
	}

	// ---------------- parent ----------------
	_pid = pid;

	// parent keeps write-end of stdin and read-end of stdout
	::close(inPipe[0]);
	::close(outPipe[1]);

	_in = inPipe[1];
	_out = outPipe[0];

	// Make them non-blocking & close-on-exec (helpers already wrap fcntl)
	(void)setNonBlocking(_in);
	(void)setNonBlocking(_out);
	(void)setCloseOnExec(_in);
	(void)setCloseOnExec(_out);

	return true;
}

/*

void CgiProcess::closeIn() / closeOut() / closeBoth()

What they do. Close the parent’s write end to the child (_in)
and/or read end from the child (_out) using xclose, and keep fds set to -1.
Why we use them. Once the request body is fully streamed to the CGI, you
must close its stdin to signal EOF—otherwise many scripts will sit waiting.
Similarly, once you’ve consumed the CGI response, you should close the read end to
release resources and stop poll() from waking on a dead pipe.
How they fit. These are called from your streaming logic and from teardown paths
(timeouts, client disconnects). Because they’re idempotent and tiny, higher-level
code can call them liberally (e.g., after detecting POLLHUP on stdout, or after sending
the last byte into stdin) without worrying about current state.
Centralizing the close policy also ensures consistent behavior between success and error paths.

*/

void CgiProcess::closeIn()
{
	(void)xclose(_in);
}
void CgiProcess::closeOut()
{
	(void)xclose(_out);
}
void CgiProcess::closeBoth()
{
	closeIn();
	closeOut();
}

/*

int CgiProcess::waitNonBlocking(int *raw_status)

What it does. Uses waitpid(_pid, &st, WNOHANG) to probe child status
without blocking. Returns: 0 if still running; -1 on error or if _pid <= 0;
or a positive number when finished (maps exit/signal into a conventional code).
On success it also closes pipes and clears _pid.
Why we use it. The event loop can’t block waiting for CGI exit.
This function lets you integrate child-process lifecycle with your tick logic:
occasionally call waitNonBlocking, and if it reports finished,
proceed to final response handling and cleanup.
How it fits. terminate() uses it to check if the CGI already exited after a
SIGTERM grace period. Handlers can also call it when EOF is observed on stdout
to reap promptly and avoid zombies. Recording a “positive when finished” code helps downstream:
you can log WEXITSTATUS vs. 128+signal conventionally and translate crashes into 5xx responses.
The function keeps to your project rules—no errno-based branching
after read/write—and operates solely on process syscalls.

*/

int CgiProcess::waitNonBlocking(int *raw_status)
{
	if (_pid <= 0)
		return -1;

	int st = 0;
	pid_t r = ::waitpid(_pid, &st, WNOHANG);
	if (r == 0)
	{
		// still running
		return 0;
	}
	if (r < 0)
	{
		// wait error; treat as error
		return -1;
	}

	// child reaped
	if (raw_status)
		*raw_status = st;

	int code = 0;
	if (WIFEXITED(st))
	{
		code = WEXITSTATUS(st);
	}
	else if (WIFSIGNALED(st))
	{
		code = 128 + WTERMSIG(st); // common convention
	}

	_pid = -1;
	closeBoth();
	return code > 0 ? code : 1; // >0 means finished; return a positive number
}

/*
void wait(unsigned long long ms) (helper)

What it does. Busy-sleep helper that loops until now + ms,
calling select(0, NULL, NULL, NULL, &tv) with a 10ms timeout each iteration.
Why we use it. In terminate(), after sending SIGTERM, you want to give the
child a short grace period to exit on its own before escalating to
SIGKILL—without blocking the main event loop (this code runs in teardown paths,
not inside the loop). Using select is permitted and avoids CPU spin.
How it fits. This helper keeps timing logic simple and self-contained in
CgiProcess.cpp. Because it waits in small slices, it gives the OS chances
to schedule the child and reap it naturally. You don’t depend on nanosleep or
other calls; select is already on the allowed list and portable enough for this use.
The function is intentionally minimal: no signals, no interruptions to the reactor
thread (it’s called outside the poll cycle during controlled shutdown).



*/

// helper lambda-ish for waiting with timeout
void wait(unsigned long long ms)
{
	unsigned long long start = TimeUtil::nowMs();
	while (TimeUtil::nowMs() < start + ms)
	{
		// Timeout for 10 ms by doing nothing
		struct timeval tv = {0, 0};
		tv.tv_sec = 0;
		tv.tv_usec = 10 * 1000;
		select(0, NULL, NULL, NULL, &tv);
	}
}

/*

void CgiProcess::terminate()

What it does. Ensures the CGI is gone and fds are closed.
If a child exists, it first sends SIGTERM, calls waitNonBlocking to see if it exited,
then gives it ~1s using the wait() helper. If still running, it sends SIGKILL and
performs a blocking waitpid to reap. In all cases it clears _pid and calls closeBoth().
Why we use it. Timeouts, client disconnects, or server shutdown must not leave zombie
processes or open pipes behind. This is the failsafe—the final authority
to stop the child no matter what.
How it fits. Called from the destructor and from higher-level error/timeout handlers.
It follows a humane escalation (TERM → grace → KILL) that gives well-behaved scripts a chance
to clean up while guaranteeing progress for misbehaving ones. By reusing waitNonBlocking and the
local wait helper, it remains compact and compliant with your non-blocking architecture:
it’s only invoked off the hot path (not inside the poll dispatch), so the occasional
blocking waitpid on a process you just KILLed is acceptable and bounded. After terminate,
the event loop won’t see CGI fds again.

*/

void CgiProcess::terminate()
{
	if (_pid > 0)
	{
		// Try to kill child gently
		kill(_pid, SIGTERM);
		// try to reap without killing first
		int dummy = 0;
		int rc = waitNonBlocking(&dummy);
		// If process is still running after SIGTERM was sent, chill first, kill later
		if (rc == 0)
			wait(1000);

		// If still running, despite Signal and enough time, force kill it
		rc = waitNonBlocking(&dummy);
		if (rc == 0)
		{
			// still running → kill
			::kill(_pid, SIGKILL);
			// reap
			(void)::waitpid(_pid, &dummy, 0);
		}
		_pid = -1;
	}
	closeBoth();
}
