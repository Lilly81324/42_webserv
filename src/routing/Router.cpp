#include "Router.h"
#include "RouteResolver.h"
#include "ServerConfig.h"
#include "VirtualServer.h"
#include "HTTPCODES.h"
#include <map>
#include <string>

static std::string path_from_uri(const std::string &uri)
{
	std::string::size_type q = uri.find('?');
	return (q == std::string::npos) ? uri : uri.substr(0, q);
}

static std::string ext_of(const std::string &path)
{
	std::string::size_type slash = path.rfind('/');
	std::string::size_type dot = path.rfind('.');
	if (dot == std::string::npos)
		return std::string();
	if (slash != std::string::npos && dot < slash)
		return std::string();
	return path.substr(dot);
}

// Helper: Validate VS index
static bool isValidVSIndex(const ServerConfig &cfg, int vs_idx)
{
	return vs_idx >= 0 && vs_idx < (int)cfg.servers().size();
}

// Helper: Find matching location
static const Location *findLocation(const VirtualServer &vs, const std::string &path)
{
	return RouteResolver::matchLocation(vs, path);
}

// Helper: Check allowed methods (except PUT/PATCH)
static bool isMethodAllowed(const Location *L, const std::string &method)
{
	if (!L || L->allowed_methods.empty() || method == "PUT" || method == "PATCH")
		return true;
	for (std::vector<std::string>::const_iterator it = L->allowed_methods.begin(); it != L->allowed_methods.end(); ++it)
	{
		if (*it == method)
			return true;
	}
	return false;
}

// Helper: Check PUT/PATCH allowed
static bool isPutPatchAllowed(const Location *L, const std::string &method)
{
	if (!L)
		return false;
	if (method == "PUT" && L->write_conf.allow_put)
		return true;
	if (method == "PATCH" && L->write_conf.allow_patch)
		return true;
	return false;
}

// Helper: Check proxy
static bool isProxy(const Location *L)
{
	return L && L->is_proxy;
}

// Helper: Check CGI
static bool isCGI(const Location *L, const std::string &ext)
{
	if (!L)
		return false;
	return L->cgi_by_ext.find(ext) != L->cgi_by_ext.end();
}

void Router::makeDecisionForVS(const ServerConfig &cfg,
							   int vs_idx,
							   const std::string &method,
							   const std::string &uri,
							   RouteDecision &out)
{
	if (!isValidVSIndex(cfg, vs_idx))
	{
		out.kind = RouteDecision::HK_ERROR;
		out.status = HTTP_BAD_REQUEST;
		out.vs = 0;
		out.loc = 0;
		return;
	}

	const VirtualServer &vs = cfg.servers()[vs_idx];
	out.vs = &vs;
	const std::string path = path_from_uri(uri);
	const Location *L = findLocation(vs, path);
	out.loc = L;

	// Check allowed methods (except PUT/PATCH)
	if (!isMethodAllowed(L, method))
	{
		out.kind = RouteDecision::HK_ERROR;
		out.status = HTTP_METHOD_FORBIDDEN;
		return;
	}

	// PUT/PATCH
	if ((method == "PUT" || method == "PATCH") && !isPutPatchAllowed(L, method))
	{
		out.kind = RouteDecision::HK_ERROR;
		out.status = HTTP_METHOD_FORBIDDEN;
		return;
	}
	if ((method == "PUT" || method == "PATCH") && isPutPatchAllowed(L, method))
	{
		out.kind = RouteDecision::HK_PUTPATCH;
		out.status = HTTP_OK;
		return;
	}

	// Proxy
	if (isProxy(L))
	{
		out.kind = RouteDecision::HK_PROXY;
		out.upstream_name = L->proxy_name;
		out.status = HTTP_OK;
		return;
	}

	// CGI
	const std::string ext = ext_of(path);
	if (!ext.empty() && isCGI(L, ext))
	{
		out.kind = RouteDecision::HK_CGI;
		out.cgi_ext = ext;
		out.status = HTTP_OK;
		return;
	}

	// Static (default)
	if (L)
	{
		out.kind = RouteDecision::HK_STATIC;
		out.status = HTTP_OK;
		return;
	}
	else
	{
		out.kind = RouteDecision::HK_STATIC;
		out.status = HTTP_OK;
		return;
	}
}

/**
 * Phase 0 — Inputs & preconditions (where you are now)

You’re in ClientConnection, state is READ_HEADERS.

You have:

Parsed method, target (URI), version, headers, Host.

The local port (via getsockname) and the listener mapping available through Server (for VS resolution).

The header is valid enough to proceed (e.g., Host present for HTTP/1.1).

You may (or may not) know if the request has a body (via Content-Length or Transfer-Encoding).

From here we split responsibilities into Routing, Core (ServerPipeline), and CGI (plus Static / PUT/PATCH / Proxy where relevant).

Layer A — Routing (RouteResolver + Router)

Goal: determine which virtual server and what handler to use, with all path decisions resolved.

Pick Virtual Server

Input: local port, Host header (normalized).

Use Server::resolveVirtualServerByPort(port, host):

If a hostname match exists: pick that VS.

Else: use default VS for that port (first declared).

If none: treat as routing error → 400/421 style response (for now 400/404 is fine).

Normalize Target

Split target into path and query (/alpha?x=1&y=2 → path /alpha, query x=1&y=2).

Normalize/clean the path (collapse //, reject .. traversal, etc.).

Resolve Location

With RouteResolver::matchLocation(vs, path) apply your precedence rules:

exact match (if you have),

“starts with” / longest prefix,

regex (if/when implemented).

Produce Location (or null if none—then VS root is used).

Router decision

Compute:

effective_root = location root if set, else VS root.

rel_path = path minus matched location’s prefix (used by static/CGI).

ext = file extension (for CGI by ext).

Check method policy (once, centrally):

Allowed methods list (405 if not allowed).

PUT/PATCH gates via write_conf (405 if disabled).

Decide HandlerKind:

PROXY if location.is_proxy.

CGI if extension matches cgi_by_ext (or cgi defaults).

PUTPATCH if method is PUT/PATCH and allowed here.

STATIC otherwise.

Output a RouteDecision (kind + pointers to VS/Location + extra bits like CGI ext, upstream name).

Deliverable from Routing to Core:
PreparedRequest (normalized facts: vs, loc, method, path, rel_path, effective_root, ext, has_body?, content_length?, expect-continue?, limits, etc.) + RouteDecision.

Layer B — Core (ServerPipeline orchestration)

Goal: make sure the request is ready for the chosen handler, then drive the handler and the connection state through I/O and timeouts.

Preflight (once, central)

Verify message framing and limits:

Content-Length sanity (integer, non-negative, no conflicting TE).

Decide if the request has a body (method + headers).

Enforce max_body_bytes from Location (413 if exceeded).

Optionally handle Expect: 100-continue policy (either send 100 or reject early).

If preflight fails → build error response (400/405/413/414/431/500) and go to WRITE.

Body readiness

If selected handler does not need the body (e.g., GET static without special behavior): you can dispatch immediately.

If the handler requires body (e.g., PUT/PATCH, many CGI/Proxy POST paths):

Switch connection state to READ_BODY and keep reading until:

bytes_read == Content-Length (or decode chunked later when implemented),

or a limit/time violation occurs → error response.

Decide buffering strategy (for now you can buffer in memory with a cap; later switch to temp files/streaming).

Dispatch

Based on RouteDecision.kind, instantiate/choose the handler:

STATIC → StaticHandler

CGI → CgiHandler

PROXY → ProxyHandler (later)

PUTPATCH → PutPatchHandler

Call handler.handle(ctx, req, res) (where ctx is your PreparedRequest).

Handlers fill HttpResponse: status, headers, and how to produce the body (e.g., memory buffer, file streaming, CGI pipe).

Emit response

Build and queue the response head:

Status line, headers (Date, Server, Content-Length or Transfer-Encoding, Connection, Content-Type, ETag/Last-Modified as available).

Body emission strategy:

For now: Content-Length + write in chunks from a buffer or a file fd (looped read/send).

Later: add chunked transfer when body size unknown ahead (CGI/proxy/large streams).

Switch interest to POLLOUT and drain outBuffer in onWritable() until done.

Connection persistence

For now: close after response (Connection: close).

Later: support keep-alive → when finished sending, reset state to READ_HEADERS, clear per-request buffers, keep socket open and register for POLLIN again.

Error mapping & logging

Convert internal failures to HTTP errors (e.g., 404 for missing file, 403 for forbidden, 502/504 for CGI failures/timeouts).

Log request line, status, bytes, duration, VS name (optional).

Layer C — Handler specifics
1) StaticHandler (serving files & directories)

Resolve filesystem path: effective_root + rel_path. Reject traversal again as a safety net.

stat() the target:

Directory:

If path lacks trailing / → 301/308 redirect to add /.

Else: try index_files in order; if none and autoindex is true → generate listing; else 403/404 per policy.

Regular file:

Open and serve; determine Content-Type using MIME mapping (from ServerConfig).

Support HEAD (send headers only).

For large files, stream in blocks (read→send loop via onWritable()).

2) PutPatchHandler (writes/uploads)

Require fully read body (per preflight).

Enforce write_conf.max_body_bytes.

Compute destination under write_conf.root_directory (or upload_dir if used) using rel_path.

Create/replace file (permissions up to you).

On success:

201 Created if new, 204 No Content if replaced (policy choice).

Optional ETag generation if generate_etag.

On errors: 403/409/500 as appropriate.

3) CgiHandler (execute scripts)

Environment (at minimum):

REQUEST_METHOD, QUERY_STRING, CONTENT_LENGTH, CONTENT_TYPE,

SERVER_PROTOCOL, SERVER_NAME, SERVER_PORT,

SCRIPT_FILENAME (full resolved path),

PATH_INFO if you split script vs. extra path,

REMOTE_ADDR if available.

Pipes:

Create pipe() for stdin/stdout of child.

fork(), in child dup2() pipe ends to 0 (stdin) and 1 (stdout), execve() CGI binary/script (mapped from extension).

Parent:

If request has body, stream it to child’s stdin (respect timeouts from CgiSpec.timeout_ms).

Read child’s stdout:

Parse CGI headers until \r\n\r\n,

Map them to HTTP response headers (handle Status:, Content-Type:, etc.),

Then forward body bytes.

Handle timeouts (kill child → 504), abnormal exits (→ 502), and I/O errors (→ 500).

4) ProxyHandler (later)

Choose upstream from UpstreamPool (strategy/weight).

Connect to upstream, forward request (adjust hop-by-hop headers), and stream the response back.

Handle connect/read/write timeouts and retry/failover.

Phase D — Event-driven state machine (how the loop interacts)

READ_HEADERS (POLLIN):

Read bytes; when \r\n\r\n found → Routing → Preflight.

If handler needs body → READ_BODY; else DISPATCH.

READ_BODY (POLLIN):

Read until required length; if too big/timeout → error response.

When complete → DISPATCH.

DISPATCH:

Call the chosen handler to prepare HttpResponse.

Queue headers/body; switch to WRITE and register POLLOUT.

WRITE (POLLOUT):

Send until all queued bytes are written.

If keep-alive: reset to READ_HEADERS; else CLOSE.

CLOSE:

Close fd; tear down connection object.

Phase E — Edge cases & policies (decide now, implement gradually)

Slowloris: per-connection header timeout and/or max header bytes; drop if exceeded.

Max body: enforce location/global caps (413).

Path traversal: reject .. and ambiguous encodings (normalize early).

Directory slash redirect vs. forbidding directory without slash (consistent 301/308).

CGI safety: close-on-exec for all unrelated fds; sanitize env; hard timeout.

Connection persistence: start with Connection: close; add keep-alive later.

Chunked: defer until needed (CGI/proxy streaming); design HttpResponse to support either Content-Length or chunked so you can add it later without rewrites.

Mental model summary

ClientConnection decides “headers complete → ask Core.”

Routing resolves VS + Location and chooses a handler (no I/O here).

Core/ServerPipeline ensures the request is ready (body, limits, policy), then dispatches the right handler.

Handler performs exactly one job (files / CGI / proxy / put&patch), fills HttpResponse.

ClientConnection streams the response out, controlling POLLIN/POLLOUT until done, then closes or keeps alive.

This plan keeps concerns separated, avoids duplicated checks, and lets you grow features (keep-alive, chunked, proxy) without tearing up your current structure.
 */