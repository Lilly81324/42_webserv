/* --- ETagUtil.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

/* --- ETagUtil.cpp --- */
#include "ETagUtil.h"
#include <sstream>
#include <iomanip>

std::string ETagUtil::generate(const struct stat &st)
{
    unsigned long long size = static_cast<unsigned long long>(st.st_size);
    unsigned long long msec = static_cast<unsigned long long>(st.st_mtime);
#if defined(__APPLE__) || defined(st_mtim)
#ifdef __APPLE__
    msec = static_cast<unsigned long long>(st.st_mtimespec.tv_sec) * 1000ULL + static_cast<unsigned long long>(st.st_mtimespec.tv_nsec / 1000000ULL);
#else
    msec = static_cast<unsigned long long>(st.st_mtim.tv_sec) * 1000ULL + static_cast<unsigned long long>(st.st_mtim.tv_nsec / 1000000ULL);
#endif
#endif
    std::ostringstream oss;
    oss << "\"wsv-" << std::hex << size << "-" << msec << "\"";
    return oss.str();
}

/**
 * @brief Strong ETag Comparison
 * @returns true if neither string is a weak Etag and both are equal
 * @returns false otherwise
*/
bool ETagUtil::strongComp(const std::string &s1, const std::string &s2)
{
	if (s1.substr(0, 2) == "W/")
		return (false);
	return (s1 == s2);
}

/**
 * TODO: IF YOU WANT TO USE THIS, REWORK IT, THIS IS PROBABLY INSUFFICENT
 * @brief Weak ETag Comparison
 * @returns true if both strings are equal, ignoring weak/strong type
 * @returns false otherwise
*/
bool ETagUtil::weakComp(const std::string &s1, const std::string &s2)
{
	std::string cpy1(s1);
	std::string cpy2(s2);

	if (s1.substr(0, 2) == "W/")
		cpy1 = s1.substr(2, s1.length() - 2);
	if (s2.substr(0, 2) == "W/")
		cpy2 = s2.substr(2, s2.length() - 2);
	return (s1 == s2);
}