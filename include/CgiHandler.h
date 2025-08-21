/* --- CgiHandler.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef CGIHANDLER_H
#define CGIHANDLER_H

#include "Handler.h"

class CgiHandler : virtual public Handler
{
	public:
		CgiHandler();
		~CgiHandler();
		bool handle(HttpRequest &req, HttpResponse &res, RequestContext &ctx);

	private:
};

#endif // CGIHANDLER_H
