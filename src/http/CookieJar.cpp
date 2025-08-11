/* --- CookieJar.cpp --- */

/* ------------------------------------------
author: Lilly81324
date: 11/08/2025
------------------------------------------ */

#include "CookieJar.h"
#include "Headers.h"

using namespace std;

CookieJar::CookieJar()
{
}

CookieJar::~CookieJar()
{
	this->cookies.clear();
}

/**
 * ?
 */
void CookieJar::setCookieHeaders(\
	Headers &h, string key, string value, string path, \
	int maxAge, bool httpOnly, bool secure, string sameSite)
{
	(void)h;
	(void)path;
	(void)maxAge;
	(void)httpOnly;
	(void)secure;
	(void)sameSite;
	// this->cookies.insert(key,value);
	(void)value;
	(void)key;
}

/**
 * Sets value of given key or adds a new pair
 * @param key: Key by which to identify the field
 * @param value: New value to set the keys value to
 */
void CookieJar::set(string key, string value)
{
	// this->cookies.insert(key, value);
	(void)value;
	(void)key;
}

/**
 * Removes key from CookieJar
 * @param key: Key to remove
 */
void	CookieJar::erase(const string &key)
{
	this->cookies.erase(key);
}

/**
 * Deletes all entries in CookieJar
 */
void	CookieJar::clear(void)
{
	this->cookies.clear();
}

/**
 * @returns Constant Interator to beginning of CookieJar
 */
multimap<string, string>::const_iterator CookieJar::getBegin(void) const
{
	return (this->cookies.begin());
}

/**
 * @returns Constant Interator to end of CookieJar
 */
multimap<string, string>::const_iterator CookieJar::getEnd(void) const
{
	return (this->cookies.end());
}

/**
 * Returns the value for the given key and checks if it exists
 * @param key: Key by which to identify the value from the cookies
 * @param exists: Gets set to 1 if key exists, or 0 if not
 * @returns Value at the given key or "" if non-existant
 */
bool CookieJar::keyExists(string key) const
{
	if (!this->cookies.count(key))
		return (false);
	return (true);
}

/**
 * Displays the CookieJar on output
 * @param out: Output stream to show on
 */
void	CookieJar::show(ostream &out) const
{
	map<string, string>::const_iterator it;

	for(it = this->cookies.begin(); it != this->cookies.end(); it++)
	{
		if (it != this->cookies.begin())
			out << endl;
		out << "["  << it->first << "] : [" << it->second << "]";
	}
}

/**
 * @returns Amount of entries in CookieJar
 */
int	CookieJar::getLength(void) const
{
	int count = 0;
	map<string, string>::const_iterator it;

	for(it = this->getBegin(); it != this->getEnd(); it++)
		count++;
	return (count);
}

/**
 * @returns Boolean wether cookies is empty or not
 */
bool	CookieJar::isEmpty(void) const
{
	return (this->cookies.empty());
}

/**
 * Returns the value for the given key
 * @param key: Key by which to identify the value from the cookies
 * @returns Value at the given key or "" if non-existant
 * @note For checking if key exists, use get(string, int)
 * @note Cannot be a const function, due to how cookies indexing works
 */
string CookieJar::get(string key)
{
	if (!this->keyExists(key))
		return ("");
	return (this->cookies.find(key)->second);
}

/**
 * Displays CookieJar on output
 */
ostream &operator<<(ostream &out, const CookieJar &target)
{
	target.show(out);
	return (out);
}
