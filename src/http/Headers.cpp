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

void Headers::set(std::string key, std::string value)
{
	this->map[key] = value;
}

void	Headers::mergeFrom(const Headers &src)
{
	std::map<std::string, std::string, CiLess>::const_iterator it;

	for(it = src.getBegin(); it != src.getEnd(); it++)
		this->set(it->first, it->second);
}

void	Headers::erase(const std::string &key)
{
	this->map.erase(key);
}

void	Headers::clear(void)
{
	this->map.clear();
}

std::map<std::string, std::string, CiLess>::const_iterator Headers::getBegin(void) const
{
	return (this->map.begin());
}


std::map<std::string, std::string, CiLess>::const_iterator Headers::getEnd(void) const
{
	return (this->map.end());
}

bool Headers::keyExists(std::string key) const
{
	if (!this->map.count(key))
		return (false);
	return (true);
}

void	Headers::show(std::ostream &out) const
{
	std::map<std::string, std::string, CiLess>::const_iterator it;

	for(it = this->map.begin(); it != this->map.end(); it++)
	{
		if (it != this->map.begin())
			out << std::endl;
		out << "["  << it->first << "] : [" << it->second << "]";
	}
}

std::string Headers::serialize(void) const
{
	std::map<std::string, std::string, CiLess>::const_iterator it;
	std::string out = "";

	for(it = this->map.begin(); it != this->map.end(); it++)
	{
		out.append(it->first);
		out.append(": ");
		out.append(it->second);
		out.append("\r\n");
	}
	out += "\r\n";
	return (out);
}

std::string Headers::get(std::string key)
{
	if (!this->keyExists(key))
		return ("");
	return (this->map[key]);
}

int	Headers::getLength(void) const
{
	int count = 0;
	std::map<std::string, std::string, CiLess>::const_iterator it;

	for(it = this->getBegin(); it != this->getEnd(); it++)
		count++;
	return (count);
}

bool	Headers::isEmpty(void) const
{
	return (this->map.empty());
}

std::ostream &operator<<(std::ostream &out, const Headers &target)
{
	target.show(out);
	return (out);
}
