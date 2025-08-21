
#ifndef HANDLER_H
#define HANDLER_H

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "RequestContext.h"

class Handler
{

public:
	Handler() {};
	virtual ~Handler(){};
	virtual bool handle(HttpRequest &req, HttpResponse &res, RequestContext &ctx)
	{
		(void)req;
		(void)res;
		(void)ctx;
		return true;
	};
};

#endif