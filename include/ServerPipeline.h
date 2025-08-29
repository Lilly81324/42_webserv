/* --- ServerPipeline.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef SERVERPIPELINE_H
#define SERVERPIPELINE_H

#include <string>

class ServerConfig;
class HttpRequest;
class HttpResponse;
struct RouteDecision;

enum PipelineAction
{
	PIPE_ERR_SENT = 0, // an error response was produced (send/close)
	PIPE_HANDLE_NOW,   // no body needed: handle right away
	PIPE_READ_BODY	   // body required: ClientConnection should read the body
};
class ServerPipeline
{
public:
	bool processRequest(const ServerConfig &cfg, int vs_indx, HttpRequest &req, HttpResponse &res,RouteDecision &decision);
	PipelineAction policyGate(const ServerConfig &cfg, int vs_indx, HttpRequest &req, RouteDecision &decision, HttpResponse &res);

private:
};

#endif // SERVERPIPELINE_H
