#include "VirtualServer.h"



/* 

Upstream::Upstream()

This constructor initializes a single upstream target—the definition of one backend 
server you might proxy to. It zeroes or defaults all fields so an instance starts 
in a valid, conservative state: host is empty (no destination yet), 
port is 0 (invalid until configured), healthy is true 
(optimistic default so selection logic can proceed until health checks run), 
and weight is 1 (neutral weight for load-balancing algorithms that consider weights). 
By centralizing default values here, the configuration parser and proxy code can safely 
create Upstream entries before all details are filled in, and tests can rely on predictable 
behavior when fields are omitted. The pattern supports copy/assignment trivially 
(plain POD-like members) and keeps invariants simple: you either 
fill host+port later from the config, or you skip the node if port==0. 
This minimizes branching in the proxy selection path—selection can iterate 
nodes and apply strategy/health/weight filters without worrying about uninitialized 
memory or stale flags. Overall, it’s a small but important piece that pins down 
the semantics of an upstream server entry at the moment of construction.



*/

// ---- Upstream / UpstreamPool ----
Upstream::Upstream()
	: host(), port(0), healthy(true), weight(1)
{
}

/* 

Upstream::~Upstream()

The destructor for Upstream is empty because the struct holds 
only standard, self-managed members (std::string and integers/booleans). 
RAII handles cleanup: when an Upstream object goes out of scope or is removed 
from a vector, its std::string host frees automatically, and primitive fields 
need no action. Having an explicit destructor—even if empty—documents 
intent: there are no hidden resources (file descriptors, sockets, timers) 
tied to a single upstream entry. That clarity helps maintainers: if one 
day you add something that would need explicit teardown (e.g., cached DNS 
results, health-check timers), the destructor is the place to concentrate 
that logic. Meanwhile, the empty destructor contributes to exception safety 
and simplicity in container operations (copy/move/erase). In short, it codifies 
“no special teardown required” and keeps the class future-proof, ready to grow 
if upstream entries ever manage active resources beyond pure configuration state.


*/


Upstream::~Upstream() {}


/* 

UpstreamPool::UpstreamPool()

UpstreamPool groups multiple Upstream nodes under a name—think 
upstream backend { server a:8080; server b:8080; }. This constructor 
initializes that pool with sensible defaults: an empty nodes vector, 
strategy set to "roundrobin" (a universally understood, fair default), 
empty health_path (no active health probe until configured), and health_interval_ms = 0 
(probing disabled). The defaults let you create pools before all tuning is known, 
and they avoid accidental health traffic. Starting with round-robin ensures 
traffic is distributed evenly across healthy nodes unless the config explicitly 
opts into weights or another algorithm. Keeping pool state small and value-type 
makes it easy to copy global pools into per-server overrides during parsing, 
supporting inheritance/override behavior without dynamic ownership headaches. 
Later, the proxy handler consults this pool to pick a node: it filters by health, 
applies the strategy (and weights), and dials host:port. By standardizing initialization 
here, the rest of the pipeline can treat a pool as a ready-to-use selection object 
the moment configuration parsing attaches it to a Location via proxy_pass.


*/

/* 

strategy("roundrobin") sets the pool’s load-balancing policy: 
requests rotate evenly across the node list (optionally honoring 
weight if your selection code accounts for it). Example sequence 
over A,B,C: A → B → C → A → … If a node is marked unhealthy, it’s skipped. 
Round-robin is simple, fair, and deterministic—great default behavior without 
needing per-request metrics.

*/

UpstreamPool::UpstreamPool()
	: nodes(), strategy("roundrobin"), health_path(), health_interval_ms(0)
{
}


/* 

UpstreamPool::~UpstreamPool()

Like Upstream, UpstreamPool has an empty destructor because it owns only STL 
containers and simple values. nodes (a std::vector<Upstream>) frees its contents 
automatically; strategy, health_path (strings), and the integer interval all 
clean up via standard destructors. The explicit but empty destructor conveys 
that the pool itself does not own OS resources—no sockets or threads—and that 
health checking (if implemented) is coordinated elsewhere, typically 
by a scheduler that references pools but doesn’t embed timers inside them. 
This separation of concerns keeps the configuration model decoupled 
from runtime mechanisms: a pool defines what to route to and how to choose, 
while other components define when to probe and how to update healthy. 
The result is easier testing, swapping strategies, and cloning of pools 
between global and per-server scopes without worrying about dangling 
handles or double-teardown.

*/


UpstreamPool::~UpstreamPool() {}

// ---- VirtualServer ----

/* 

VirtualServer::VirtualServer()

This constructor captures the defaults for an entire server 
{} block—the unit of virtual hosting. It initializes: listen_host 
 empty (wildcard until set), listen_port = 0 (invalid until configured), 
 server_names empty, root empty, index_files empty, error_pages empty map, 
 upstreams empty map (per-server overrides start blank), locations 
 empty vector, client_body_temp_path empty (no spill dir until configured), 
 and client_max_body_size = 0 (meaning “no explicit limit here”, 
 typically falling back to a global or handler default). These defaults 
 support a parser that sets fields in any order and fail fast later 
 if critical fields (like listen) remain unset. At runtime, Server 
 binds listeners based on listen_host:listen_port; the router resolves 
 server_names; the static/CGI/proxy handlers consult root, index_files, 
 locations, and error pages. By giving every member a deterministic 
 starting value, the code avoids undefined behavior and simplifies 
 equality, copying, and logging. It also cleanly supports 
 inheritance: you can copy in global upstream pools when 
 entering a server block, then override selectively without 
 needing special “initialized” flags.


*/


VirtualServer::VirtualServer()
	: listen_host(), listen_port(0), server_names(), root(),
	  index_files(), error_pages(), upstreams(), locations(),
	  client_body_temp_path(), client_max_body_size(0) {}


/* 

VirtualServer::~VirtualServer()

The destructor is empty because a VirtualServer aggregates 
only STL containers and plain values—std::string, std::vector, 
and std::map members release themselves. Having no custom teardown 
underscores that the virtual server is purely a configuration object: it doesn’t 
own live sockets, threads, or timers. The running Server component 
is responsible for creating listeners/handlers from this configuration 
and cleaning up those runtime resources separately. This separation ensures 
that discarding or reloading configuration (e.g., in tests) is cheap and safe. 
It also makes it easy to hold multiple VirtualServer instances while deciding 
how to route a request (e.g., name-based selection) without worrying about 
double-closing OS handles on copy or destruction. In practice, the empty destructor 
documents that the lifetime of virtual servers is independent from the lifetime 
of any networking primitives the Server creates from them.


*/

VirtualServer::~VirtualServer() {}
