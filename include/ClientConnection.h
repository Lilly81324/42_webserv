#if !defined( CLIENTCONNECTION_H)
#define  CLIENTCONNECTION_H

#include <string>
#include "ConnectionIO.h"
#include "HeaderProcessor.h"
#include "RequestGuards.h"
#include "PhaseDeadline.h"
#include "CGIStreamer.h"
#include "BodyReader.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "ResponseFactory.h"
#include "RequestContext.h"
#include "RouteResolver.h"
#include "Phase.h"
#include "RoutePlan.h"
// #include "RequestDecoder.h"

class Server;

class ClientConnection
{
	public:
		ClientConnection(int fd, Server * s);	
		~ClientConnection();

		void onTick(unsigned long long now_ms);
		bool isClosed() const;
		bool wantsRead() ;
		bool isReadPaused() ;
		bool hasPendingWrite() ;
		int fd() const { return io.getFD();}

	private:

		void flushOut();
		void readIn();
		void parseHeaders();
		void selectRouteOnce();
		void runPreflight();
		void decideBodyReader();
		void decideBodyReader(std::size_t content_lenght);
		void readBody();
		void routeAndBuild();
		void finishWriteOrNext();
		void fail(int code, const char *reason);

		Phase state;
		Server* server;
		ConnectionIO io;
		HttpRequest req;
		IBodyReader* body;
		CGIStreamer cgi;
		PhaseDeadline dl;
		std::size_t hdr_bytes;
		std::size_t max_hdr_bytes;
		std::size_t max_body_bytes;
		bool		should_close;
		bool		route_selected;
		RoutePlan	plan;
		int	local_port;
		
		RouteDecision* ctx;
		Preflight pr;
};

#endif //  CLIENTCONNECTION_H
