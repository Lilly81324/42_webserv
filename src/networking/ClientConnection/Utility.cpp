#include "ClientConnection.h"
#include "ResponseFactory.h"
#include <sstream>

// ---------------- Queue + serialize ----------------

void ClientConnection::serializeToOutBuffer(const HttpResponse &r)
{
	std::ostringstream ss;
	ss << r;
	const std::string s = ss.str();
	outBuffer.assign(s.begin(), s.end());
}

void ClientConnection::queueResponseKeepAlive(const HttpResponse &r)
{
	serializeToOutBuffer(r);
	if (!flow.isReadPaused() && outBuffer.size() >= HIGH_WATER)
		flow.setReadPaused(true);
	flow.setWriteLinger(false);
	changeState(WRITE);
	resetDeadlineForWrite();
}

void ClientConnection::queueResponseClose(const HttpResponse &r)
{
	serializeToOutBuffer(r);
	flow.setWriteLinger(true); // close after write
	changeState(WRITE);
	resetDeadlineForWrite();
}

// ---------------- send_* overloads -----------------

void ClientConnection::send_keepalive(const HttpResponse &r)
{
	queueResponseKeepAlive(r);
}

void ClientConnection::send_and_close(const HttpResponse &r)
{
	queueResponseClose(r);
}

void ClientConnection::send_keepalive(int code, const std::string &reason)
{
	HttpResponse r = ResponseFactory::makeError(code, reason, /*close*/ false);
	queueResponseKeepAlive(r);
}

void ClientConnection::send_keepalive(int code)
{
	HttpResponse r = ResponseFactory::makeError(code, std::string(), /*close*/ false);
	queueResponseKeepAlive(r);
}

void ClientConnection::send_and_close(int code, const std::string &reason)
{
	HttpResponse r = ResponseFactory::makeError(code, reason, /*close*/ true);
	queueResponseClose(r);
}

void ClientConnection::send_and_close(int code)
{
	HttpResponse r = ResponseFactory::makeError(code, std::string(), /*close*/ true);
	queueResponseClose(r);
}

void ClientConnection::send_and_close_413()
{
	HttpResponse r = ResponseFactory::makeError(413, "Payload Too Large", /*close*/ true);
	queueResponseClose(r);
}

// ------------- Decision/limit helpers ---------------

size_t ClientConnection::effectiveLimitFromDecision(const RouteDecision &d) const
{
	const size_t locLimit = (d.loc) ? d.loc->write_conf.max_body_bytes : 0;
	const size_t srvLimit = (d.vs) ? d.vs->client_max_body_size : 0;
	return (locLimit > 0) ? locLimit : srvLimit;
}

void ClientConnection::cacheDecisionAndLimits(const RouteDecision &d)
{
	cache = d;
	decisionValid = true;
	maxBodyLimit_ = effectiveLimitFromDecision(d);
	// if you want, cache also handler kind / upload path etc. here
}

void ClientConnection::resetResponse()
{

	res.http_version = "HTTP/1.1";
	res.status = 200;
	res.reason = "OK";
	res.session_id.clear();
	res.bodyLength = 0;
	res.headers.clear(); // your Headers should have clear()
	res.cookies.clear(); // your CookieJar should have clear(); if not, no-op
	res.body.clear();
}