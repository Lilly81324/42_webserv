#if !defined( PHASE_H)
#define  PHASE_H

enum Phase
{
	PH_READ_HEADERS,
	PH_PRECHECK,
	PH_ROUTE_SELECT,
	PH_READ_BODY,
	PH_ROUTE,
	PH_WRITE,
	PH_CLOSE,

};

#endif //  PHASE_H
