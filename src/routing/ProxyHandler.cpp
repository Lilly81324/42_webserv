/* --- ProxyHandler.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/16/2025
------------------------------------------ */

#include "ProxyHandler.h"

ProxyHandler::ProxyHandler() {
    // Constructor
}

ProxyHandler::~ProxyHandler() {
    // Destructor
}

bool ProxyHandler::handle(HttpRequest &req, HttpResponse &res, RequestContext & ctx)
{
	(void) req;
	(void) res;
	(void) ctx;
	return true;
}