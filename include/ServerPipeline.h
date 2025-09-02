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

class ServerPipeline
{
public:
	static bool processRequest(const ServerConfig &cfg, int vs_indx, HttpRequest &req, HttpResponse &res, RouteDecision &decision);

private:
};

#endif // SERVERPIPELINE_H
