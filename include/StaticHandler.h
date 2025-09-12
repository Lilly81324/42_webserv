/* --- StaticHandler.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef STATICHANDLER_H
#define STATICHANDLER_H

#include <iostream>

#include "Handler.h"
class StaticHandler : public Handler
{
	public:
		StaticHandler();
		~StaticHandler();
		bool	handleGet(const std::string &canonPath, const std::string &rel, \
				bool is_head, HttpRequest &req, HttpResponse &res, RequestContext &ctx);
		bool handleDelete(const std::string &path, HttpRequest&req, HttpResponse res, RequestContext ctx);
		bool handle(HttpRequest &req, HttpResponse &res, RequestContext &ctx);

	private:
};

#endif // STATICHANDLER_H
