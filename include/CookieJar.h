/* --- CookieJar.h --- */

/* ------------------------------------------
author: Lilly81324
date: 11/08/2025
------------------------------------------ */

#ifndef COOKIEJAR_H
#define COOKIEJAR_H

#include <map>
#include <iostream>
#include "Headers.h"
// #include "CiLess.h"

using namespace std;

class CiLess;

class CookieJar
{
	public:
		CookieJar();
		~CookieJar();

		void	setCookieHeaders(Headers &h, string name, string value, string path, int maxAge, bool httpOnly, bool secure, string sameSite);
		void	set(string name, string value);
		void	erase(const string &key);
		void	clear(void);

		map<string, string>::const_iterator	CookieJar::getBegin(void) const;
		map<string, string>::const_iterator	CookieJar::getEnd(void) const;
		bool	keyExists(string key) const;
		void	show(ostream &out) const;
		int		getLength(void) const;
		bool	isEmpty(void) const;
		string	get(string key);
	private:
		map<string, string> cookies;
};

ostream	&operator<<(ostream, const CookieJar &target);

#endif // COOKIEJAR_H
