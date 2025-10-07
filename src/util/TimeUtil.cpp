/* --- TimeUtil.cpp --- */

/* ------------------------------------------
author: undefined
date: 8/10/2025
------------------------------------------ */

#include "TimeUtil.h"

TimeUtil::TimeUtil() {
    // Constructor
}

TimeUtil::~TimeUtil() {
    // Destructor
}

unsigned long long TimeUtil::nowMs()
{
	return static_cast<unsigned long long>(std::time(0)) * 1000ULL;
}