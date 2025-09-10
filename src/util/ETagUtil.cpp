/* --- ETagUtil.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

/* --- ETagUtil.cpp --- */
#include "ETagUtil.h"
#include <sstream>
#include <iomanip>

std::string ETagUtil::generate(const char *filename)
{
	struct stat st;
	if (stat(filename, &st) != 0)
		return ("");
    unsigned long long size = static_cast<unsigned long long>(st.st_size);
    unsigned long long msec = static_cast<unsigned long long>(st.st_mtime);
    std::ostringstream oss;
    oss << "\"wsv-" << std::hex << size << "-" << msec << "\"";
    return oss.str();
}

bool ETagUtil::strongComp(const std::string &s1, const std::string &s2)
{
	if (s1.substr(0, 2) == "W/")
		return (false);
	return (s1 == s2);
}

// IF YOU WANT TO USE THIS, REWORK IT, THIS IS PROBABLY INSUFFICENT
bool ETagUtil::weakComp(const std::string &s1, const std::string &s2)
{
	std::string cpy1(s1);
	std::string cpy2(s2);

	if (s1.substr(0, 2) == "W/")
		cpy1 = s1.substr(2, s1.length() - 2);
	if (s2.substr(0, 2) == "W/")
		cpy2 = s2.substr(2, s2.length() - 2);
	return (cpy1 == cpy2);
}