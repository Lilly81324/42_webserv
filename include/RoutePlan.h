#if !defined(ROUTEPLAN_H)
#define ROUTEPLAN_H

#include <cstddef>

struct RoutePlan
{
	int vhost_idx;
	int location_idx;
	int handler_kind;
	bool needs_body;
	std::size_t max_body_bytes;

	RoutePlan()
		: vhost_idx(-1), location_idx(-1), handler_kind(0),
		  needs_body(false), max_body_bytes(0) {}
};

#endif //  ROUTEPLAN_H
