/* --- CgiHandler.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "CgiHandler.h"



CgiHandler::CgiHandler(): Handler() {
    // Constructor
}

CgiHandler::~CgiHandler() {
    // Destructor

}

bool CgiHandler::handle(HttpRequest &req, HttpResponse &res, RequestContext & ctx)
{
	(void) req;
	(void) res;
	(void) ctx;
	return true;
}