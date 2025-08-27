/* --- PutPatchHandler.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/16/2025
------------------------------------------ */

#ifndef PUTPATCHHANDLER_H
#define PUTPATCHHANDLER_H

#include "Handler.h"

class PutPatchHandler : public Handler
{
	public:
		PutPatchHandler();
		~PutPatchHandler();
		bool handle(HttpRequest &req, HttpResponse &res, RequestContext &ctx);

	private:
};

#endif // PUTPATCHHANDLER_H
