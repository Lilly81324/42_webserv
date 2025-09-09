/* --- ETagUtil.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

/* --- ETagUtil.h --- */
#ifndef ETAGUTIL_H
#define ETAGUTIL_H

#include <string>
#include <sys/stat.h>

class ETagUtil {
public:
    // Strong-enough ETag for static files: size+mtime
    static std::string generate(const struct stat& st);
	static bool strongComp(const std::string &s1, const std::string &s2);
    static bool weakComp(const std::string &s1, const std::string &s2);
};

#endif // ETAGUTIL_H

