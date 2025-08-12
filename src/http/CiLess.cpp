/* --- Headers.cpp --- */

/* ------------------------------------------
author: Lilly81324
Date: 12/08/2025
------------------------------------------ */

#include "CiLess.h"

CiLess::CiLess(void)
{
}

CiLess::~CiLess(void)
{
}

/**
 * Operator override, used for map key comparing
 * ---------------------------------------------
 * This test is run twice, once with left as the new key,
 * and once with right being the new key,
 * while the other parameter is the currently checked, existing key
 * test(a, b)	test(b, a)	Meaning
 * false		false		a == b (equal)
 * true			false		a < b (a before b)
 * false		true		b < a (b before a)
 * true			true		N/A
 * The result of both tests is used for sorting / overwriting
 * @returns true, if left is "less then" right
 * @returns false otherwise
 */
bool CiLess::operator()(const string &left, const string &right) const
{
	if (left.size() < right.size())
		return (true);
	for (size_t i = 0; i < left.size(); i++)
	{
		if (tolower(left[i]) != tolower(right[i]))
		{
			if (tolower(left[i]) < tolower(right[i]))
				return (true);
			else
				return (false);
		}
	}
	return (false);
}
