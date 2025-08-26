/* --- CgiProcess.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "CgiProcess.h"
#include "VirtualServer.h" // full definition of CgiSpec for the 3-arg overload

#include <unistd.h> // pipe, fork, dup2, execve, close
#include <fcntl.h>	// fcntl
#include <cstring>	// strerror
#include <cerrno>
#include <stdexcept>
#include <sys/wait.h> // waitpid
#include <signal.h>	  // kill
#include <sys/time.h> // gettimeofday
#include <vector>

// ---- tiny helpers ---------------------------------------------------------

static inline int xclose(int &fd)
{
	if (fd >= 0)
	{
		::close(fd);
		fd = -1;
	}
	return 0;
}

unsigned long long CgiProcess::nowMs()
{
	struct timeval tv;
	::gettimeofday(&tv, 0);
	return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)(tv.tv_usec / 1000ULL);
}

bool CgiProcess::setNonBlocking(int fd)
{
	int fl = ::fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		return false;
	return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

bool CgiProcess::setCloseOnExec(int fd)
{
	int fl = ::fcntl(fd, F_GETFD, 0);
	if (fl < 0)
		return false;
	return ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC) == 0;
}

// ---- lifecycle ------------------------------------------------------------

CgiProcess::CgiProcess()
	: _pid(-1), _in(-1), _out(-1), _deadline(0ULL) {}

CgiProcess::~CgiProcess()
{
	terminate(); // safe if already reaped/closed
}

// High-level convenience overload: build argv/envp and delegate
bool CgiProcess::spawn(const CgiSpec &spec, const std::string &scriptPath, const std::vector<std::string> &envv)
{
	// argv: [bin, script, NULL]
	std::vector<char *> argvv;
	argvv.push_back(const_cast<char *>(spec.bin.c_str()));
	argvv.push_back(const_cast<char *>(scriptPath.c_str()));
	argvv.push_back(0);

	// envp: ["K=V", ... , NULL]
	std::vector<char *> envp;
	envp.reserve(envv.size() + 1);
	for (std::vector<std::string>::const_iterator it = envv.begin(); it != envv.end(); ++it)
		envp.push_back(const_cast<char *>(it->c_str()));
	envp.push_back(0);

	return spawn(spec.bin, scriptPath, &argvv[0], &envp[0], spec.timeout_ms);
}

// Low-level spawn: do the actual fork/exec (stub for now; returns false)
bool CgiProcess::spawn(const std::string &bin, const std::string &script, char *const *argv, char *const *envp, int timeout_ms)
{
	// If there is an existing child, clean it up
	terminate();

	// Establish deadline (if timeout_ms <= 0, treat as no timeout)
	if (timeout_ms > 0)
		_deadline = nowMs() + (unsigned long long)timeout_ms;
	else
		_deadline = 0ULL;

	// TODO: implement real pipe()/fork()/dup2()/execve() here and set:
	//   _pid = child pid
	//   _in  = writable end for child's stdin
	//   _out = readable end for child's stdout (and/or merged stderr)
	//   setNonBlocking(_in/_out) and setCloseOnExec as appropriate
	// For now, suppress unused warnings and return false so callers can 500.
	(void)bin;
	(void)script;
	(void)argv;
	(void)envp;

	return false;
}

void CgiProcess::closeIn() { (void)xclose(_in); }
void CgiProcess::closeOut() { (void)xclose(_out); }
void CgiProcess::closeBoth()
{
	closeIn();
	closeOut();
}

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
