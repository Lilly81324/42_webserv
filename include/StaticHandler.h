/* --- StaticHandler.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef STATICHANDLER_H
#define STATICHANDLER_H

#include "Handler.h"
class StaticHandler : public Handler
{
	public:
		StaticHandler();
		~StaticHandler();
		bool handle(HttpRequest &req, HttpResponse &res, RequestContext &ctx);

	private:
};

#endif // STATICHANDLER_H
