/* Small Deadline manager: stores an absolute deadline in ms and provides helpers.
 * C++98-friendly POD.
 */
#ifndef DEADLINEMANAGER_H
#define DEADLINEMANAGER_H

#include <stdint.h>

struct DeadlineManager
{
    unsigned long long deadline_ms;
    DeadlineManager() : deadline_ms(0ULL) {}

    void reset(unsigned long long now, int ms)
    {
        deadline_ms = now + (unsigned long long)ms;
    }
    void bump(unsigned long long now, int ms)
    {
        deadline_ms = now + (unsigned long long)ms;
    }
    bool expired(unsigned long long now) const
    {
        return now > deadline_ms;
    }
    unsigned long long remaining(unsigned long long now) const
    {
        if (now > deadline_ms)
            return 0ULL;
        return deadline_ms - now;
    }
};

#endif // DEADLINEMANAGER_H
