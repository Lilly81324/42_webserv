/* --- Headers.cpp --- */

/* ------------------------------------------
author: Lilly81324
Date: 11/08/2025
------------------------------------------ */

#include "Headers.h"

Headers::Headers()
{}

Headers::~Headers()
{
	this->map.clear();
}

/**
 * Sets value of given key or adds a new pair
 * @param key: Key by which to identify the field
 * @param value: New value to set the keys value to
 */
void Headers::set(std::string key, std::string value)
{
	this->map[key] = value;
}

/**
 * Removes key from Header
 * @param key: Key to remove
 */
void	Headers::erase(const std::string &key)
{
	this->map.erase(key);
}

/**
 * Deletes all entries in Header
 */
void	Headers::clear(void)
{
	this->map.clear();
}

/**
 * @returns Constant Interator to beginning of Header
 */
std::map<std::string, std::string>::const_iterator Headers::getBegin(void) const
{
	return (this->map.begin());
}

/**
 * @returns Constant Interator to end of Header
 */
std::map<std::string, std::string>::const_iterator Headers::getEnd(void) const
{
	return (this->map.end());
}

/**
 * Returns the value for the given key
 * @param key: Key by which to identify the value from the map
 * @returns Value at the given key or "" if non-existant
 * @note For checking if key exists, use get(std::string, int)
 * @note Cannot be a const function, due to how map indexing works
 */
std::string Headers::get(std::string key)
{
	if (!this->keyExists(key))
		return ("");
	return (this->map[key]);
}

/**
 * Returns the value for the given key and checks if it exists
 * @param key: Key by which to identify the value from the map
 * @param exists: Gets set to 1 if key exists, or 0 if not
 * @returns Value at the given key or "" if non-existant
 */
bool Headers::keyExists(std::string key) const
{
	if (!this->map.count(key))
		return (false);
	return (true);
}

/**
 * Returns the number of enties for a key
 * @param key: Key to count
 * @returns Returns the number of enties for a key
 */
int Headers::keyCount(std::string key) const
{
	return (this->map.count(key));
}

/**
 * Displays the Header on output
 * @param out: Output stream to show on
 */
void	Headers::show(std::ostream &out) const
{
	std::map<std::string, std::string>::const_iterator it;

	for(it = this->map.begin(); it != this->map.end(); it++)
	{
		if (it != this->map.begin())
			out << std::endl;
		out << "["  << it->first << "] : [" << it->second << "]";
	}
}

/**
 * @returns string containing all key-values of Header
 */
std::string Headers::serialize(void) const
{
	std::map<std::string, std::string>::const_iterator it;
	std::string out = "";

	for(it = this->map.begin(); it != this->map.end(); it++)
	{
		out.append(it->first);
		out.append(":");
		out.append(it->second);
		out.append("\n");
	}
	return (out);
}

/**
 * @returns Boolean wether map is empty or not
 */
bool	Headers::isEmpty(void) const
{
	return (this->map.empty());
}

/**
 * Merge Header into this one
 * @param src: Source Header that will be added on this one
 */
void	Headers::mergeFrom(const Headers &src)
{
	std::map<std::string, std::string>::const_iterator it;

	for(it = src.getBegin(); it != src.getEnd(); it++)
		this->set(it->first, it->second);
}

/**
 * @returns Amount of entries in Header
 */
int	Headers::getLength(void) const
{
	int count = 0;
	std::map<std::string, std::string>::const_iterator it;

	for(it = this->getBegin(); it != this->getEnd(); it++)
		count++;
	return (count);
}

/**
 * Displays Header on output
 */
std::ostream &operator<<(std::ostream &out, const Headers &target)
{
	target.show(out);
	return (out);
}
