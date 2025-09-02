#include "ClientConnection.h"
#include "ExpectContinue.h"
#include "RequestContext.h"
#include "ResponseFactory.h"
#include "ChunkedReader.h"
#include "FileBodyReader.h"
#include "ContentLenghtReader.h"
#include "ServerPipeline.h"
#include "Server.h"
#include <sstream>
#include <netinet/in.h>

static const int HDR_TIMEOUT_MS = 10000;
static const int BODY_TIMEOUT_MS = 45000;
static const int WR_TIMEOUT_MS = 15000;
static const int IDLE_TIMEOUT_MS = 30000;
static const std::size_t INMEM_BODY_LIMIT = 256 * 1024;

ClientConnection::ClientConnection(int fd, Server *s)
	: state(PH_READ_HEADERS),
	  server(s),
	  io(fd, 64 * 1024),
	  req(),
	  res(),
	  body(0),
	  cgi(req, res),
	  dl(),
	  hdr_bytes(0),
	  max_hdr_bytes(64 * 1024),
	  max_body_bytes(0),
	  should_close(false),
	  route_selected(false),
	  plan(),
	  pr()
{
	ctx = new RouteDecision;
	dl.reset(0, HDR_TIMEOUT_MS);
	struct sockaddr_storage ss;
	socklen_t sl = sizeof(ss);
	local_port = -1;
	if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0)
	{
		if (ss.ss_family == AF_INET)
		{
			local_port = (int)ntohs(((sockaddr_in *)&ss)->sin_port);
		}
		else if (ss.ss_family == AF_INET6)
		{
			local_port = (int)ntohs(((sockaddr_in6 *)&ss)->sin6_port);
		}
	}
}

ClientConnection::~ClientConnection()
{
	if (ctx)
		delete ctx;
}

bool ClientConnection::isClosed() const { return state == PH_CLOSE; }
bool ClientConnection::hasPendingWrite() { return io.getChainBuf().getByteSize() != 0; }
bool ClientConnection::isReadPaused() { return io.getFlow().isReadPaused(); }

bool ClientConnection::wantsRead()
{
	// We want read in header/body phases; can also allow lingering read in PH_CLOSE if you implement it
	return state == PH_READ_HEADERS || state == PH_ROUTE_SELECT || state == PH_PRECHECK || state == PH_READ_BODY;
}

void ClientConnection::onTick(unsigned long long now_ms)
{
	const std::size_t outbytes = io.getChainBuf().getByteSize();

	if (io.getFlow().shouldPauseRead(outbytes))
		io.getFlow().setReadPaused(true);
	if (io.getFlow().shouldResumeRead(outbytes))
		io.getFlow().setReadPaused(false);

	(void)io.nb_write();

	if (dl.expired(now_ms))
	{
		if (state == PH_READ_HEADERS || state == PH_READ_BODY || state == PH_PRECHECK || state == PH_ROUTE_SELECT)
		{
			fail(408, "Request Timeout");
		}
		else
		{
			state = PH_CLOSE;
		}
		return;
	}

	if (!io.getFlow().isReadPaused() && (state == PH_READ_HEADERS || state == PH_READ_BODY))
		(void)io.nb_read(32 * 1024);

	switch (state)
	{
	case PH_READ_HEADERS:
		parseHeaders();
		break;
	case PH_ROUTE_SELECT:
		selectRouteOnce();
		break;
	case PH_PRECHECK:
		runPreflight();
		break;
	case PH_READ_BODY:
		readBody();
		break;
	case PH_ROUTE:
		routeAndBuild();
		break;
	case PH_WRITE:
		finishWriteOrNext();
		break;

	case PH_CLOSE:
		io.getFlow().setReadPaused(true);

		(void)io.nb_write();
		if (io.getChainBuf().getByteSize() != 0)

			if (body)
			{
				delete body;
				body = 0;
			}
		req.cleanupBodyFile();
		server->releaseConnection(this);
		return;
	}
}

void ClientConnection::parseHeaders()
{
	const char *buf = io.getInputRing().readPtr();
	std::size_t avail = io.getInputRing().readAvail();
	if (avail == 0)
		return;

	std::size_t used = req.parse(buf, avail);

	if (used > 0)
	{
		io.getInputRing().consumed(used);
		hdr_bytes += used;
		if (hdr_bytes > max_body_bytes)
		{
			fail(431, "Request Header Fields to Larger");
			return;
		}
		dl.reset(0, HDR_TIMEOUT_MS);
	}

	if (!req.headersDone())
		return;

	state = PH_PRECHECK;
	dl.reset(0, BODY_TIMEOUT_MS);
}

void ClientConnection::selectRouteOnce()
{
	if (route_selected)
	{
		state = PH_PRECHECK;
		return;
	}

	plan.needs_body = false;
	plan.max_body_bytes = 0;
	// replace 'localhost' with HttpRequest actual domainName
	vs_idx = server->resolveVirtualServerByPort(local_port, "localhost");

	Preflight pr = RequestGuards::preflight(server->getConfig(),
											vs_idx,
											req.getMethod(),
											req.getUri(),
											req.getHeaders(),
											ctx);
	route_selected = true;
	state = PH_PRECHECK;
	dl.reset(0, BODY_TIMEOUT_MS);
}

void ClientConnection::runPreflight()
{
	const Headers &H = req.getHeaders();

	HeaderCheck hc = HeaderProcessor::analyze(req, H, max_body_bytes);

	if (!hc.ok)
	{
		fail(hc.error_status, 0);
		return;
	}

	if (!pr.ok)
	{
		fail(pr.reject_status ? pr.reject_status : 400, pr.reject_reason.c_str());
		return;
	}

	max_body_bytes = pr.max_body_bytes;

	if (!pr.needs_body)
	{
		state = PH_ROUTE;
		return;
	}

	if (hc.expect_continue && ExpectContinue::needed(req.getHeaders()))
		ExpectContinue::write100(io.getChainBuf()); // queued; nb_write() will flush it

	if (hc.chunked)
	{
		decideBodyReader(/*chunked*/);
	}
	else if (hc.content_length > 0)
	{
		if (max_body_bytes && hc.content_length > max_body_bytes)
		{
			fail(413, "Payload Too Large");
			return;
		}
		decideBodyReader(hc.content_length);
	}
	else
	{
		fail(411, "Length Required");
		return;
	}

	state = PH_READ_BODY;
	dl.reset(0, BODY_TIMEOUT_MS);
}

void ClientConnection::readBody()
{
	const char *buf = io.getInputRing().readPtr();
	const std::size_t avail = io.getInputRing().readAvail();

	if (avail == 0)
		goto maybe_flush;

	if (!body)
	{
		fail(400, "Bad Request");
		return;
	}
	{
		std::size_t used = body->consume(buf, avail);
		if (used > 0)
		{
			io.getInputRing().consumed(used);
			dl.reset(0, BODY_TIMEOUT_MS);
		}
	}

	if (plan.max_body_bytes && body->bytes_received() > plan.max_body_bytes)
	{
		fail(413, "Payload Too Larger");
		return;
	}

maybe_flush:
{
	FileBodyReader *fr = 0;
	// classic C++98 RTTI
	fr = dynamic_cast<FileBodyReader *>(body);
	if (fr)
		(void)fr->flush_to_disk(64 * 1024); // flush up to 64 KiB per tick

	if (ChunkedReader *cr = dynamic_cast<ChunkedReader *>(body))
		(void)cr->flush_to_disk(64 * 1024);
}
	if (!body->complete())
		return;
	state = PH_ROUTE;
	dl.reset(0, WR_TIMEOUT_MS);
}

void ClientConnection::routeAndBuild()
{

	(void)ServerPipeline::processRequest(server->getConfig(), vs_idx, req, res, *ctx);
	// Serialize → output
	std::ostringstream os;
	os << res;
	const std::string s = os.str();
	io.getChainBuf().push_copy(s.data(), s.size());

	// Keep-alive vs close
	std::string connHdr = req.getHeaders().get("Connection");
	should_close = (req.getHttpVer() == "HTTP/1.0") || (connHdr == "close" || connHdr == "Close");

	state = PH_WRITE;
	dl.reset(0 /*now*/, WR_TIMEOUT_MS);
}

void ClientConnection::finishWriteOrNext()
{
	if (io.getChainBuf().getByteSize() != 0)
		return;

	if (should_close)
	{
		state = PH_CLOSE;
		return;
	}

	// Reset req now
	hdr_bytes = 0;
	max_body_bytes = 0;
	if (body)
	{
		delete body;
		body = 0;
	}
	plan = RoutePlan();
	route_selected = false;

	state = PH_READ_HEADERS;
	dl.reset(0, HDR_TIMEOUT_MS);

	if (io.getInputRing().readAvail() > 0)
	{
		parseHeaders();
	}
}

void ClientConnection::fail(int code, const char *reason)
{

	HttpResponse r = ResponseFactory::makeText(code, "", reason ? reason : "", /*close*/ true);

	std::ostringstream os;
	os << r;
	const std::string s = os.str();
	io.getChainBuf().push_copy(s.data(), s.size()); // ChainBuf

	should_close = true;
	state = PH_WRITE;
	dl.reset(0, WR_TIMEOUT_MS);
}

void ClientConnection::decideBodyReader()
{
	if (body)
	{
		delete body;
		body = 0;
	}

	std::vector<char> tmp;

	body = new ChunkedReader(tmp, plan.max_body_bytes, server->getConfig().servers()[vs_idx].client_body_temp_path);
}

void ClientConnection::decideBodyReader(std::size_t content_length)
{
	if (body)
	{
		delete body;
		body = 0;
	}

	const std::size_t cap = (max_body_bytes != 0) ? max_body_bytes : (std::size_t)(~0);

	const bool use_inmem =
		(content_length > 0) &&
		(content_length <= INMEM_BODY_LIMIT) &&
		(content_length <= cap);

	if (use_inmem)
	{
		body = new ContentLenghtReader(content_length, req);
	}
	else
	{
		FileBodyReader *fr = new FileBodyReader(
			server->getConfig().servers()[vs_idx].client_body_temp_path);
		body = fr;
	}
}