/* --- TimeUtil.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef TIMEUTIL_H
#define TIMEUTIL_H
#include <ctime>

class TimeUtil {
	public:
		TimeUtil();
		~TimeUtil();

		/* 

		unsigned long long nowMs()

		Returns current wall-clock time in milliseconds using
		Used for deadline tracking in CGI processes, especially timeouts configured via spawn. 
		Millisecond precision is sufficient for CGI lifetime management without heavy dependencies. 
		By wrapping this call, other parts of the server can enforce time-based rules (like aborting stuck scripts) consistently.

		*/
		static unsigned long long nowMs();

	private:

};

#endif // TIMEUTIL_H
