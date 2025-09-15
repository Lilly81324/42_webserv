/* --- CgiProcess.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "CgiProcess.h"
#include "VirtualServer.h" // full definition of CgiSpec for the 3-arg overload
#include <unistd.h> // pipe, fork, dup2, execve, close
#include <fcntl.h>  // fcntl
#include <cstring>  // strerror
#include <cerrno>
#include <stdexcept>
#include <sys/wait.h> // waitpid
#include <signal.h>   // kill
#include <sys/time.h> // gettimeofday
#include <vector>




/*  ================ What the file does ===================== 

Implements extension→CGI spec mapping and lookup; 
merges location and default registries deterministically for routing

*/




// ---- tiny helpers ---------------------------------------------------------


/* 

static inline int xclose(int &fd)
Small helper that closes a file descriptor if valid,
then resets it to -1.
It prevents accidental reuse of stale descriptors and simplifies cleanup paths.
By wrapping the close syscall, it avoids repeating boilerplate error-prone checks
throughout the class. Used by closeIn, closeOut, and closeBoth,
this ensures CGI pipes are always invalidated once closed.
It is important because dangling descriptors could confuse the event loop, cause leaks,
or inadvertently keep a pipe open, preventing EOF detection by the child process.


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

unsigned long long CgiProcess::nowMs()

Utility that returns the current wall-clock time in milliseconds using gettimeofday.
It’s used to compute absolute deadlines for CGI processes,
such as header timeouts or total runtime limits.
Having deadlines expressed in milliseconds allows precise non-blocking checks without sleep calls.
This function is critical for enforcing responsiveness: the event loop can periodically
compare nowMs() with stored deadlines, and terminate long-running or stalled CGI children.
Without this, clients might hang indefinitely if a script never responds,
violating project requirements that requests must never block forever.


*/

unsigned long long CgiProcess::nowMs()
{
	struct timeval tv;
	::gettimeofday(&tv, 0);
	return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)(tv.tv_usec / 1000ULL);
}


/* 

bool CgiProcess::setNonBlocking(int fd)
Configures a file descriptor to operate in non-blocking mode with fcntl.
This prevents any read or write on CGI pipes from stalling the server.
Returns false if unable to apply the flag.
Essential because the project requires all I/O multiplexed through poll, never blocking.
Ensuring non-blocking pipes lets the event loop safely attempt reads/writes and back off
if unavailable, maintaining fairness across connections.
It’s applied to the parent ends of stdin/stdout after fork,
ensuring the parent can stream body input and script output asynchronously.


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
Uses fcntl to apply the FD_CLOEXEC flag,
ensuring that file descriptors are automatically closed when calling execve.
Without this, unrelated descriptors could unintentionally leak into the CGI child,
potentially causing resource leaks or security issues.
It’s critical in a server spawning many scripts because leaked descriptors might keep sockets open,
prevent files from closing, or create confusing behavior.
Applying this to stdin and stdout pipe ends in the parent ensures only intended descriptors
survive in the child process environment, keeping isolation clean and predictable.


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

CgiProcess::CgiProcess()
Constructor initializing process state to a clean baseline: 
_pid = -1, _in = -1, _out = -1, and _deadline = 0.
These sentinel values mean “no active process and no open pipes.”
This design makes it safe to call cleanup functions later without double-closing.
Initializing clearly is critical for correctness in event-driven servers,
where many connections may repeatedly spawn or terminate CGI processes.
A known invalid state prevents misinterpretation of resources and allows robust error recovery.
It ensures a CgiProcess object always starts predictable and safe, avoiding undefined behaviors.


*/

CgiProcess::CgiProcess()
	: _pid(-1), _in(-1), _out(-1), _deadline(0ULL) {}


/* 

CgiProcess::~CgiProcess()
Destructor that calls terminate().
Guarantees that any still-running CGI child is killed and pipes closed when the object is destroyed.
This RAII behavior ensures system resources are reclaimed even if exceptions occur or
higher-level code forgets cleanup. In long-running servers, it’s critical to avoid zombie processes or descriptor leaks.
Without automatic termination, abandoned children would linger, consuming PIDs and memory.
By delegating cleanup to the destructor, CgiProcess enforces safe lifetimes regardless of caller discipline,
aligning with C++ idioms and project constraints requiring robust, leak-free operation.

*/

CgiProcess::~CgiProcess()
{
	terminate(); // safe if already reaped/closed
}

/* 

bool CgiProcess::spawn(const CgiSpec&, const std::string&, const std::vectorstd::string
&)
High-level spawn that forks a child, dup2’s pipes to stdin/stdout, and executes interpreter
plus script with environment. Parent keeps non-blocking write-end of stdin and read-end of stdout.
This overload builds argv/envp automatically from CgiSpec and script path.
It’s the main entrypoint for handlers launching CGI scripts. Critical because it abstracts raw fork/exec boilerplate into a reusable call.
By handling pipes, flags, and closing unused ends, it guarantees a clean separation between parent and child,
enabling the event loop to stream data safely without blocking.

*/


// High-level convenience overload: build argv/envp and delegate
bool CgiProcess::spawn(const CgiSpec &spec,
					const std::string &script_path,
					const std::vector<std::string> &envv)
{
	closeBoth(); // if previously used
	_pid = -1;
	_in  = -1;   // parent will WRITE to child's stdin
	_out = -1;   // parent will READ  from child's stdout

	int pin[2]  = { -1, -1 }; // pipe for child's stdin  (parent writes -> child reads)
	int pout[2] = { -1, -1 }; // pipe for child's stdout (child writes -> parent reads)

	if (::pipe(pin)  < 0)
		return false;
	if (::pipe(pout) < 0) { 
		::close(pin[0]); 
		::close(pin[1]); 
		return false; 
	}

	pid_t pid = ::fork();
	if (pid < 0) {
		::close(pin[0]);  
		::close(pin[1]);
		::close(pout[0]); 
		::close(pout[1]);
		return false;
	}

	if (pid == 0) {
		// ---- Child ----
		// stdin: read end of pin
		::dup2(pin[0],  STDIN_FILENO);
		// stdout: write end of pout
		::dup2(pout[1], STDOUT_FILENO);

		// close all pipe fds in child
		::close(pin[0]);  
		::close(pin[1]);
		::close(pout[0]); 
		::close(pout[1]);

		// Build argv and envp (you already have helpers; keep minimal here)
		std::vector<char*> argv;
		argv.push_back(const_cast<char*>(spec.bin.c_str())); // /usr/bin/python3
		argv.push_back(const_cast<char*>(script_path.c_str()));      // /path/to/script.py
		argv.push_back(0);

		std::vector<char*> envp;
		envp.reserve(envv.size() + 1);
		for (size_t i = 0; i < envv.size(); ++i)
			envp.push_back(const_cast<char*>(envv[i].c_str()));
		envp.push_back(0);

		::execve(spec.bin.c_str(), &argv[0], &envp[0]);
		_exit(127); // exec failed
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
	fl = ::fcntl(_in,  F_GETFL, 0); 
	if (fl >= 0) 
		::fcntl(_in,  F_SETFL,  fl | O_NONBLOCK);
	fl = ::fcntl(_out, F_GETFL, 0); 
	if (fl >= 0)
		::fcntl(_out, F_SETFL, fl | O_NONBLOCK);
	fl = ::fcntl(_in,  F_GETFD, 0); 
		if (fl >= 0) 
			::fcntl(_in,  F_SETFD, fl | FD_CLOEXEC);
	fl = ::fcntl(_out, F_GETFD, 0); 
	if (fl >= 0) 
		::fcntl(_out, F_SETFD, fl | FD_CLOEXEC);
	return true;
}


/* 

bool CgiProcess::spawn(const std::string&, const std::string&, char const, char* const*, int)**
Lower-level spawn overload allowing custom argv/envp and a timeout.
Forks, duplicates pipes, execves the binary, and merges stderr into stdout.
Parent applies non-blocking and close-on-exec to pipe ends.
It records a deadline if timeout_ms > 0. This variant gives fine-grained control for tests or advanced scenarios.
It underpins flexibility: CGI handling can pass arbitrary environment arrays beyond defaults.
Timeout integration ensures runaway scripts are killed deterministically.
By separating high-level convenience from low-level control,
the design supports both ease of use and robustness demanded by evaluation criteria.


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

	// Establish deadline
	_deadline = (timeout_ms > 0)
					? (nowMs() + (unsigned long long)timeout_ms)
					: 0ULL;

	int inPipe[2] = {-1, -1};  // parent writes -> child reads (stdin)
	int outPipe[2] = {-1, -1}; // child writes -> parent reads (stdout/stderr)

	if (::pipe(inPipe) < 0)
	{
		return false;
	}
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
			_exit(126);
		// stdout to outPipe[1]
		if (::dup2(outPipe[1], STDOUT_FILENO) < 0)
			_exit(126);
		// stderr merged to stdout
		(void)::dup2(outPipe[1], STDERR_FILENO);

		// close the originals after dup
		::close(inPipe[0]);
		::close(outPipe[1]);

		// Exec the interpreter/binary with provided argv/envp.
		// argv should look like: [bin, script, NULL]
		(void)script; // only to silence unused warning; script is already in argv
		::execve(bin.c_str(),
				const_cast<char *const *>(argv),
				const_cast<char *const *>(envp));
		_exit(127); // exec failed
	}

	// ---------------- parent ----------------
	_pid = pid;

	// parent keeps write-end of stdin and read-end of stdout
	::close(inPipe[0]);
	::close(outPipe[1]);

	_in = inPipe[1];
	_out = outPipe[0];

	// Make them non-blocking & close-on-exec
	(void)setNonBlocking(_in);
	(void)setNonBlocking(_out);
	(void)setCloseOnExec(_in);
	(void)setCloseOnExec(_out);

	return true;
}


/* 

void CgiProcess::closeIn() / closeOut() / closeBoth()
Helpers that close one or both parent pipe descriptors using xclose. 
These are essential to manage lifetimes: once the request body is fully written, 
closing stdin tells the child there’s no more input. 
Similarly, closing stdout after process end signals the event loop that output is complete. closeBoth provides a safe reset to initial state, 
preventing dangling descriptors. Explicit functions clarify semantics for connection code, avoiding misuse. 
Without them, code would repeat manual close calls, risking leaks or invalid reuse, especially under error conditions.

*/

void CgiProcess::closeIn() { 
	(void)xclose(_in);
}
void CgiProcess::closeOut() { 
	(void)xclose(_out); 
}
void CgiProcess::closeBoth()
{
	closeIn();
	closeOut();
}


/* 

int CgiProcess::waitNonBlocking(int raw_status)*
Non-blocking reap of the child using waitpid with WNOHANG. 
Returns 0 if still running, >0 if finished, -1 on error. 
If reaped, updates exit code (either normal exit or signal termination), resets _pid, and closes pipes. 
This allows the event loop to check CGI progress without stalling. 
Passing raw_status gives access to raw wait status for advanced inspection. 
Essential to enforce deadlines: caller can poll repeatedly, and if CGI exceeds limits, kill it. 
Without non-blocking reap, the server could deadlock waiting for CGI output.

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

void CgiProcess::terminate()
Ensures a CGI child is killed and cleaned up. First tries waitNonBlocking; 
if process still alive, sends SIGKILL then reaps with blocking waitpid (guaranteed to succeed because process is dying). 
Resets _pid and closes both pipes. Critical safety net: prevents zombie processes if CGI misbehaves or hangs. 
Called in destructor and error paths, guaranteeing no leftover children persist. 
Without termination logic, scripts could run indefinitely, consuming CPU or holding sockets open. 
This enforces server resilience, keeping process table clean and resources reclaimed predictably.

*/

void CgiProcess::terminate()
{
	if (_pid > 0)
	{
		// try to reap without killing first
		int dummy = 0;
		int rc = waitNonBlocking(&dummy);
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
