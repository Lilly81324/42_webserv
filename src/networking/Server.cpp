/* --- Server.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */
/* --- src/networking/Server.cpp --- */

/* --- src/networking/Server.cpp --- */
#include "Server.h"
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <set>
#include "UniqueFD.h"
#include "AcceptorHandler.h"



/* 

static void throwErr(const char *what)

Small utility that throws a std::runtime_error combining a human label 
(what) with the current errno string (std::strerror(errno)). 
It’s used immediately after low-level syscalls (like fcntl, bind, listen) 
to convert OS failures into C++ exceptions with a precise message. 
This keeps the main logic clean: instead of scattering if (x==-1) { perror; return; }, 
callers simply perform the syscall and, on failure, call throwErr("fcntl(F_GETFL)"). 
Centralizing the pattern guarantees consistent diagnostics across the file and ensures 
errors propagate to the Server::start() try/catch boundary, where partial 
resources can be cleaned up. Because it’s used with non-read/write ops (e.g., fcntl), 
it doesn’t conflict with your project’s “don’t branch on errno after read/write” rule. 
The helper also reduces duplication and makes it trivial to enrich messages later (e.g., include fd numbers). 
In short: it’s a thin adapter turning C API error reporting into idiomatic C++ exception flow, 
improving readability and maintainability of socket setup paths.


*/

static void throwErr(const char *what)
{
	throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}


/* 

EventLoop& Server::getLoop()

Simple accessor returning a reference to the server’s 
single EventLoop. Handlers (acceptors, clients, CGI streamers) 
often need to register, update, or remove file descriptors; 
passing a reference from Server is preferable to using globals. 
This function keeps ownership clear—Server creates and governs the loop, 
while other components borrow it—but avoids copying or exposing internals by value. 
It also decouples higher-level modules from the server’s internal layout: 
if you later switch from poll to epoll or adjust loop implementation details, 
code that simply needs “the loop” doesn’t change. In tests, returning a reference 
lets you inject or observe loop state without re-architecting the server. 
Conceptually: Server is the application root; EventLoop is the engine; 
this getter is the crankshaft connector everyone can hold to schedule 
readiness-driven work (POLLIN/POLLOUT) and to stop the loop on shutdown. 
Minimal, but essential for cohesion across modules.


*/

EventLoop& Server::getLoop() {
    return loop_;
}


/* 

Server::Server(ServerConfig &srvConfig)

Constructs the Server with a parsed ServerConfig reference, 
initializes the event loop, and allocates a ServerPipeline. 
Holding the config by reference avoids expensive copies and allows 
other subsystems (routing, handlers) to consult the authoritative configuration through 
Server. Instantiating the loop here establishes the invariant “one loop per server,” 
which your project requires (single poll for all I/O). Creating ServerPipeline 
centralizes per-connection phase orchestration; while not used directly in this file, 
it’s the component that will drive request lifecycles (read headers → route → body → dispatch → write). 
The constructor stays intentionally light—no sockets are opened here—so you can catch configuration 
errors early and keep start() responsible for allocating OS resources. This separation also helps in tests: 
you can create a server against a small config and only call start() when you truly want to bind ports. 
In short: wire up core members, keep ownership obvious, and defer riskier operations to explicit phases.

*/

Server::Server(ServerConfig &srvConfig) : srvConfig(srvConfig)
{
	this->loop_ = EventLoop();
	serverpipeline = new ServerPipeline();
}


/* 

Server::~Server()

Destroys the server by deleting the heap-allocated ServerPipeline. 
Cleanup here is intentionally minimal because most resource teardown 
(listeners, fds, loop entries) happens via explicit calls (stop(), 
unregisterListeners(), closeAll()) before destruction. This pattern avoids 
destructor surprises and makes shutdown order explicit: first drop from the event loop, 
then close sockets, then let the server object die. If later you add more heap-managed 
members (e.g., metrics sinks or thread pools), the destructor remains the central,
reliable place to free them—even when exceptions unwind construction midway.
The destructor’s simplicity reflects a design that pushes lifetime management
to clear server methods instead of implicit RAII for heavyweight OS resources.
That yields better control during error handling and tests where you may
start/stop multiple times.


*/

Server::~Server()
{
	delete serverpipeline;
}

/* 


void Server::registerListeners()

Takes every created Listener, attaches an AcceptorHandler to each, 
and registers the listener FD into the event loop with interest POLLIN. 
From that moment, new incoming connections on those ports will surface as 
readable events on the listener fds, and AcceptorHandler::onEvent will perform non-blocking 
accept() loops to create ClientConnections. The function checks pointer validity and only registers 
listeners with valid FDs, preventing accidental dispatch to null handlers. 
Splitting this into its own method clarifies startup sequencing: start() 
builds Listener objects and returns them; registerListeners() binds them to the loop. 
This separation also simplifies error cleanup—if registration fails mid-way, 
you can safely tear down already-created listeners because ownership remains with Server. 
Operationally, this is where the server “goes live” from the loop’s perspective—prior 
steps merely prepared sockets; now they’re visible to poll() and will trigger your one-loop invariant.

*/

void Server::registerListeners()
{
	for (std::vector<Listener *>::const_iterator it = listeners.begin();
		 it != listeners.end(); ++it)
	{
		Listener *lst = *it;
		if (lst && (lst)->getFD() >= 0)
		{
			lst->setAcceptor(new AcceptorHandler(loop_, *this, *it));
			loop_.addFD((*it)->getFD(), POLLIN, lst->getAcceptor());
		}
	}
}


/* 

void Server::unregisterListeners()

Removes every listener FD from the event loop, 
deletes each listener’s AcceptorHandler, then deletes the Listener objects 
themselves and nulls pointers. It also calls loop_.removeFD(fd) for each valid descriptor, 
ensuring no stale entries remain that could produce events after teardown. 
This method is the mirror of registerListeners() and is called during stop() 
to guarantee that accepting stops immediately and cleanly. By centralizing the entire 
unregistration and deletion process, you reduce the risk of dangling handler pointers 
(handlers hold a reference to the loop and the server). Clearing the vector and setting 
elements to 0 helps catch use-after-free in debug runs and keeps shutdown idempotent. 
In short: this is the safe, complete “detach and free” path for the listening side of your server.


*/

void Server::unregisterListeners()
{
	for (std::vector<Listener *>::iterator it = listeners.begin();
		it != listeners.end(); ++it)
	{
		Listener *lst = *it;
		if (!lst)
			continue;
        const int fd = lst->getFD();
        if (fd >= 0) {
            loop_.removeFD(fd);
        }
		if (lst->getAcceptor())
			delete (lst->getAcceptor());
		delete lst;
		*it = 0;
	}
	listeners.clear();
}


/* 

void Server::closeAll()

Secondary cleanup that iterates listeners and deletes them, 
then clears the container. While unregisterListeners() already 
deletes listeners as part of stop, closeAll() functions as a stronger 
safety net (or a legacy path) to ensure all Listener* are released even if 
they were not previously unregistered. Keeping it separate allows stop() 
to call both without branching and guarantees no leaks if the code path changes. 
This belt-and-suspenders approach makes teardown robust against refactors or partial 
failures earlier in startup—if something slipped through, closeAll() is a final sweep


*/

void Server::closeAll()
{
	for (std::vector<Listener *>::iterator it = listeners.begin();
		 it != listeners.end(); ++it)
	{
		delete *it;
	}
	listeners.clear();
}

/* 


void Server::setNonBlocking(int fd)

Marks an fd as non-blocking by fetching current flags 
with fcntl(F_GETFL) and setting O_NONBLOCK via fcntl(F_SETFL, flags | O_NONBLOCK). 
All network and pipe descriptors in your project must be non-blocking to satisfy the 
“single poll” requirement: you must never call read/write without prior readiness or 
risk blocking the loop. Doing this as a helper centralizes error handling (via throwErr) 
and avoids duplicated fcntl boilerplate. It’s called right after creating sockets and 
before connect/bind/listen, ensuring every subsequent I/O path behaves predictably with poll(). 
If the call fails (rare), converting it to an exception aborts that listener 
cleanly and surfaces a clear diagnostic to the caller.


*/

void Server::setNonBlocking(int fd)
{
	int flags = ::fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		throwErr("fcntl(F_GETFL)");
	if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
		throwErr("fcntl(F_SETFL O_NONBLOCK)");
}


/* 

void Server::setCloseOnExec(int fd)

Sets FD_CLOEXEC using fcntl(F_GETFD) / fcntl(F_SETFD, flags | FD_CLOEXEC). 
This ensures that when the server spawns child processes (CGI, for example), 
these sockets aren’t unintentionally inherited by the child. 
Leaking listening sockets or client fds into children can cause subtle bugs: ports
staying “in use” after parent exits, or children keeping connections alive.
Having a dedicated helper means every created fd can be systematically cloexec’d
right after creation—consistent, readable, and easy to audit.
Like setNonBlocking, errors are escalated through throwErr.


*/

void Server::setCloseOnExec(int fd)
{
	int flags = ::fcntl(fd, F_GETFD, 0);
	if (flags == -1)
		throwErr("fcntl(F_GETFD)");
	if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
		throwErr("fcntl(F_SETFD FD_CLOEXEC)");
}


/* 

void Server::stop()

Orderly shutdown entry point. It first unregisterListeners()
so no new accepts happen and no more events arrive for listener fds.
Then closeAll() to double-ensure listeners are freed. Finally, it calls loop_.stop()
to break the event loop’s run() iteration. Grouping these steps behind stop() provides a
single method the signal handler (on_sig) or internal logic can call to initiate graceful
termination. It also constrains shutdown ordering (detach → delete → stop loop),
which avoids race conditions where the loop might dispatch to handlers referencing
listeners that were already closed. This method is the bridge between the outside world
(signals, admin API) and the server’s internal lifecycle controls.


*/

void Server::stop()
{
	unregisterListeners();
	closeAll();
	loop_.stop();
}


/* 

void Server::buildListenerPlan(std::vector<std::pair<std::string,int>>& unique_pairs, std::map<std::pair<std::string,int>, std::vector<int>>& vsIndiciesByPair)

Scans the parsed VirtualServer list and computes: 
(a) the unique set of (host,port) pairs to bind, and (b) for each such pair, 
the indices of virtual servers that attach to it. It normalizes empty hosts to 0.0.0.0 
(INADDR_ANY), skips invalid ports, deduplicates with a std::set, and fills both outputs. 
The result is a compact plan that start() uses to open exactly one listening socket per unique 
(host,port), instead of one socket per virtual server. It also records which virtual servers share a 
listener so SNI/Host resolution can be done later. This preprocessing is vital: it prevents duplicate bind() 
attempts and provides a clean mapping from a listener to its vhost indices, which buildHostMaps() 
leverages to build host-header maps. In short, it translates config semantics 
into concrete sockets to open and a reverse index for routing.


*/

void Server::buildListenerPlan(std::vector<std::pair<std::string, int> > &unique_pairs,
							   std::map<std::pair<std::string, int>, std::vector<int> > &vsIndiciesByPair)
{

	unique_pairs.clear();
	vsIndiciesByPair.clear();
	std::set<std::pair<std::string, int> > uniq;
	const std::vector<VirtualServer>& servers = srvConfig.servers();
	for (std::vector<VirtualServer>::const_iterator it = servers.begin();
		 it != servers.end(); ++it)
	{
		const int idx = int(it - servers.begin());
		const int port = it->listen_port;
		if(port <= 0 || port > 65535)
			continue;
		const std::string host = it->listen_host.empty() ? std::string("0.0.0.0") : it->listen_host;
		const std::pair<std::string, int> key(host, port);
		if (uniq.insert(key).second)
			unique_pairs.push_back(key);
		vsIndiciesByPair[key].push_back(idx);
	}
}



/* 

int Server::createListenSocketRaw(const std::string& host, int port, bool& out_is_ipv6)

Creates a non-blocking, cloexec TCP listening socket for the given host/port. 
It uses getaddrinfo with AI_PASSIVE/AF_UNSPEC to handle both IPv4 and IPv6, 
iterating candidates until one binds and listens. For each candidate it: socket(), 
setsockopt(SO_REUSEADDR), setNonBlocking, setCloseOnExec, bind(), and listen(SOMAXCONN). 
On success, it records whether the address family is IPv6 (out param) and returns the fd 
(releasing it from a UniqueFD guard). On failure after trying all candidates, 
it crafts a descriptive exception including the last errno. 
This function encapsulates all the platform fiddliness of creating 
listeners and ensures every server socket obeys your non-blocking 
and cloexec policies. It also gracefully handles hostnames, 
literals, and unspecified addresses—making the higher-level 
code (start()) simple and robust.



*/

int Server::createListenSocketRaw(const std::string &host, int port, bool &out_is_ipv6)
{

	struct addrinfo hints;
	
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_addr = 0;
	hints.ai_canonname = 0;
	hints.ai_next = 0;

	std::ostringstream oss;
	oss << port;
	const std::string portstr = oss.str();
	struct addrinfo *res = 0;
	int rc = getaddrinfo(host.c_str(), portstr.c_str(), &hints, &res);
	if (rc != 0)
	{
		throw std::runtime_error(
			std::string("getaddrinfo(") + host + ":" + portstr + "): " + gai_strerror(rc));
	}
	UniqueFD guard;
	out_is_ipv6 = false;
	int last_errno = 0;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
	{
		int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd == -1)
		{
			last_errno = errno;
			continue;
		}
		guard.reset(fd);
		int yes = 1;
		if (setsockopt(guard.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1)
		{
			last_errno = errno;
			continue;
		}
		try
		{
			setNonBlocking(guard.get());
			setCloseOnExec(guard.get());
		}
		catch (const std::exception &)
		{
			last_errno = errno;
			continue;
		}
		if (bind(guard.get(), ai->ai_addr, ai->ai_addrlen) == -1)
		{
			last_errno = errno;
			continue;
		}
		if (listen(guard.get(), SOMAXCONN) == -1)
		{
			last_errno = errno;
			continue;
		}
		out_is_ipv6 = (ai->ai_family == AF_INET6);
		int ok = guard.release();
		freeaddrinfo(res);
		return ok;
	}

	// ----- 3) None succeeded -----
	freeaddrinfo(res);
	std::ostringstream emsg;
	emsg << "bind/listen failed for " << host << ":" << port
		 << (last_errno ? std::string(": ") + strerror(last_errno) : std::string(""));
	throw std::runtime_error(emsg.str());
}


/* 


static std::string lower_str(const std::string& s)

Lowercases ASCII letters in a string. 
Used to normalize hostnames (server_name entries) 
for case-insensitive matching against the Host header. 
Centralizing this tiny utility avoids locale-dependent 
transforms and keeps normalization rules consistent across the file. 
By operating byte-wise and only adjusting ‘A’–‘Z’, it’s fast and predictable—appropriate for 
HTTP header keys/values where ASCII semantics are expected. 
The result is used to build maps from lowercase hostnames to virtual server indices, 
avoiding duplicate entries caused by casing differences.


*/

static std::string lower_str(const std::string &s)
{
	std::string t = s;
	for (std::string::size_type i = 0; i < t.size(); ++i)
	{
		unsigned char c = static_cast<unsigned char>(t[i]);
		if (c >= 'A' && c <= 'Z')
			t[i] = char(c - 'A' + 'a');
	}
	return t;
}

/* 


void Server::buildHostMaps()

Builds two routing structures per local port: (1) default_vs_by_port — the first virtual server 
index attached to that listener (fallback when no host match), and (2) 
host_map_by_port — a map<lowercase host, vs_index>. It iterates all live Listener*, 
reads their attached vserver indices, and for each vserver collects its server_names. 
Each name is normalized to lowercase and inserted if not already present. 
The result lets resolveVirtualServerByPort() make O(log n) decisions per request: 
find the map by port, then lookup the host to find the exact vserver, 
else fall back to default. This separation by port is important because different ports may host 
different vhost sets. Building the maps after listeners are finalized ensures 
consistency between what is bound and what names can route there. 
It’s the key enabling virtual hosting without regexes.



*/



void Server::buildHostMaps()
{
    host_map_by_port.clear();
    default_vs_by_port.clear();
	for (std::vector<Listener*>::const_iterator it = listeners.begin();
			it != listeners.end(); ++it)
	{
		if (!*it)
			continue;
		const int port = (*it)->getPort();
		const std::vector<int>& vs_list = (*it)->virtualServerIndices();
		if (vs_list.empty())
			continue;
		if (default_vs_by_port.find(port) == default_vs_by_port.end())
			default_vs_by_port[port] = vs_list.front();
		std::map<std::string,int>& hmap = host_map_by_port[port];
		for (std::vector<int>::const_iterator vit = vs_list.begin();
				vit != vs_list.end(); ++vit)
		{
			const int vs_idx = *vit;
			const VirtualServer& vs = srvConfig.servers()[vs_idx];
			for (std::vector<std::string>::const_iterator sn = vs.server_names.begin();
					sn != vs.server_names.end(); ++sn)
			{
				const std::string key = lower_str(*sn);
				if (hmap.find(key) == hmap.end())
					hmap[key] = vs_idx;
			}
		}
	}
}


/* 


void Server::start()

End-to-end boot. First, buildListenerPlan() to discover unique (host,port) 
pairs and the vserver indices behind each. Then, in a try block, 
it creates each listening socket via createListenSocketRaw, 
constructs a Listener with the fd and address, and attaches the vserver 
indices for that pair. It collects all Listener* in a temporary vector; if any step throws, 
it deletes the ones already created and rethrows—preventing leaks. On success, 
it swaps the vector into listeners, calls registerListeners() to add them to 
the event loop with AcceptorHandlers, and calls buildHostMaps() to populate 
host routing tables. Finally, it deletes the temporary copies (the swap left them non-owning duplicates) 
and returns. Splitting responsibilities keeps error handling crisp and ensures that after start(), 
the server is fully live: listening, registered in the loop, 
and ready to route requests by host header.


*/


void Server::start()
{
	std::vector<std::pair<std::string,int> > hostPort;
	std::map<std::pair<std::string,int>, std::vector<int> > vsIndicesByPair;
	buildListenerPlan(hostPort, vsIndicesByPair);
	std::vector<Listener*> tmp;
	tmp.reserve(hostPort.size());
	try {
		for (std::vector<std::pair<std::string,int> >::const_iterator it = hostPort.begin();
				it != hostPort.end(); ++it)
		{
			const std::string& host = it->first;
			const int          port = it->second;
			bool is6 = false;
			int fd = createListenSocketRaw(host, port, is6);
			Listener* L = new Listener(fd, host, port, is6);
			std::map<std::pair<std::string,int>, std::vector<int> >::const_iterator vit =
				vsIndicesByPair.find(*it);
			if (vit != vsIndicesByPair.end()) {
				L->setVirtualServerIndices(vit->second);
			}
			tmp.push_back(L); 
		}
	} catch (...) {
		for (std::vector<Listener*>::iterator dit = tmp.begin(); dit != tmp.end(); ++dit) {
			delete *dit;
		}
		throw;
	}
	listeners.swap(tmp);
	registerListeners();
	buildHostMaps();
	for (std::vector<Listener*>::iterator dit = tmp.begin(); dit != tmp.end(); ++dit) {
		delete *dit;
	}
}


/* 

void Server::run(int poll_timeout_ms)

Starts the event loop. If no listeners exist yet (e.g., a caller used run() directly), 
it calls start() lazily. Then it calls loop_.run(poll_timeout_ms, this), 
which enters the single poll() cycle, dispatching all I/O events to 
registered handlers (acceptors, clients, CGI pipes, upstream sockets) 
and processing housekeeping ticks on timeout. Passing this allows the 
loop to call back into the server for termination hooks (terminate(srv) 
pattern in your loop). The poll_timeout_ms parameter defines the maximum 
sleep between ticks; a small value (e.g., 50ms) ensures deadlines and idle 
timeouts are evaluated promptly even without network activity. 
This method is the canonical entry point used by main.cpp after 
successful startup and is usually a blocking call until stop() is 
invoked (signal handler or admin command), at which point loop_.stop() 
breaks the run and returns control.


*/

void Server::run(int poll_timeout_ms) {
	if (listeners.empty())
		start();
	loop_.run(poll_timeout_ms, this);
}



/* 

static std::string normalize_host(const std::string& h)

Normalizes a Host header value for comparison: lowercases ASCII letters, 
then trims any :port suffix (origin-form host:port). For IPv6 literals, 
if the host is in [addr]:port form, it returns the bracketed host [addr] 
without the port. This is necessary because HTTP/1.1 Host may include a 
port that differs from the listener’s local port (e.g., proxies), and you want to 
match just the hostname against configured server_names. 
By explicitly handling brackets, it avoids truncating IPv6 addresses 
at the first colon (which would be incorrect). 
The function returns a canonical host key suitable for map 
lookups in host_map_by_port. Keeping this logic in one helper 
centralizes a notoriously fiddly part of HTTP host matching.


*/


static std::string normalize_host(const std::string &h)
{
	std::string s = h;
	for (size_t i = 0; i < s.size(); ++i)
	{
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c >= 'A' && c <= 'Z')
			s[i] = char(c - 'A' + 'a');
	}
	if (!s.empty() && s[0] == '[')
	{
		std::string::size_type rb = s.find(']');
		if (rb != std::string::npos)
			return s.substr(0, rb + 1);
		return s;
	}
	std::string::size_type cpos = s.find(':');
	return (cpos == std::string::npos) ? s : s.substr(0, cpos);
}


/* 

int Server::resolveVirtualServerByPort(int localPort, const std::string& hostHdr) const

Given the local listener port and the incoming Host header,
this chooses the virtual server index that should handle the request.
It first normalizes the host (normalize_host), then finds the host-map for that localPort.
If a lowercase host key exists in the map, it returns the mapped vserver index;
otherwise it falls back to the default_vs_by_port (usually the first declared server on that port).
If neither exists, it returns -1 (no match). This indirection enables hosting multiple sites on 
one IP/port pair (name-based vhosting) without regex, aligned with the project’s simplified goals. 
By keying maps per port, it avoids cross-talk when distinct ports serve different name sets.
It is called by routing logic when forming the RequestContext for a connection,
tying network address selection with HTTP host routing.



*/


int Server::resolveVirtualServerByPort(int localPort, const std::string &hostHdr) const
{
	const std::string key = normalize_host(hostHdr);
	std::map<int, std::map<std::string, int> >::const_iterator pm = host_map_by_port.find(localPort);
	if (pm != host_map_by_port.end())
	{
		std::map<std::string, int>::const_iterator it = pm->second.find(key);
		if (it != pm->second.end())
			return it->second;
	}
	std::map<int, int>::const_iterator d = default_vs_by_port.find(localPort);
	return (d != default_vs_by_port.end()) ? d->second : -1;
}

const ServerConfig& Server::getConfig()const
{
	return srvConfig;
}