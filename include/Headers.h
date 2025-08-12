/* --- Headers.h --- */

/* ------------------------------------------
Author: Lilly81324
Date: 11/08/2025
------------------------------------------ */

#ifndef HEADERS_H
#define HEADERS_H

# include <iostream>
# include <map>
# include "CiLess.h"

/**
 * A Class containing Header information
 * -------------------------------------
 * Holds a map container for key - value pairs
 * Use the get() and set() functions to interact with the values
 * For Debugging, use output operator << to display all content
 * Only holds unique instances of keys, set() overwrites, does not add
 */
class Headers
{
	public:
		Headers();
		~Headers();

		void set(std::string key, std::string value);
		void mergeFrom(const Headers &src);
		void erase(const std::string &key);
		void clear(void);

		std::map<std::string, std::string, CiLess>::const_iterator getBegin(void) const;
		std::map<std::string, std::string, CiLess>::const_iterator getEnd(void) const;
		bool keyExists(std::string key) const;
		void show(std::ostream &out) const;
		std::string serialize(void) const;
		std::string get(std::string key);
		int	getLength(void) const;
		bool isEmpty(void) const;
	private:
		std::map<std::string, std::string, CiLess> map;
};

std::ostream &operator<<(std::ostream &out, const Headers &target);

#endif // HEADERS_H
