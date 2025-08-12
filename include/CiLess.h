/* --- CiLess.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef CILESS_H
#define CILESS_H

# include <iostream>

using namespace std;

/**
 * Class for overwriting maps Compare Function,
 * in order to make comparing case insensitive, 
 * while storing the result as case sensitive
 */
class CiLess {
public:
	CiLess();
	~CiLess();
	bool operator()(const string &left, const string &right) const;
};

#endif // CILESS_H
