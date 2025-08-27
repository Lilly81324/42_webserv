/* --- ResponseFactory.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/27/2025
------------------------------------------ */

#ifndef RESPONSEFACTORY_H
#define RESPONSEFACTORY_H

#include "HttpResponse.h"

class ResponseFactory {

	public:
		static HttpResponse makeErorr(int code,const std::string & reasons ="",bool close= true, const std::string& bodyText= "");
		static HttpResponse makeText(int code,const std::string& text,const std::string & reasons ="",bool close= false);


private:

};

#endif // RESPONSEFACTORY_H
