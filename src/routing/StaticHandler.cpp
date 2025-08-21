/* --- StaticHandler.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "StaticHandler.h"

StaticHandler::StaticHandler() {
    // Constructor
}

StaticHandler::~StaticHandler() {
    // Destructor
}


bool StaticHandler::handle(HttpRequest &req, HttpResponse &res, RequestContext & ctx)
{
	(void) req;
	(void) res;
	(void) ctx;
	return true;
}