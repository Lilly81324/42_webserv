/* --- PutPatchHandler.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/16/2025
------------------------------------------ */

#include "PutPatchHandler.h"



PutPatchHandler::PutPatchHandler() {
    // Constructor
}

PutPatchHandler::~PutPatchHandler() {
    // Destructor
}

bool PutPatchHandler::handle(HttpRequest &req, HttpResponse &res, RequestContext & ctx)
{
	(void) req;
	(void) res;
	(void) ctx;
	return true;
}