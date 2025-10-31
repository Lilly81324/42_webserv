/* --- Server.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/10/2025
------------------------------------------ */

#ifndef SERVER_H
#define SERVER_H

#ifdef USE_STUBS
#include "stubs/EventLoop.h"
#include "stubs/ServerConfig.h"
#else
#include "EventLoop.h"
#include "ServerConfig.h"
#endif

#include "Listener.h"
#include "ClientConnection.h"
#include "ServerPipeline.h"
#include "ClientHandler.h"
#include <vector>
#include <string>
#include <poll.h>
#include <map>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <set>


class Server
{
	private:
		EventLoop loop_;
		ServerConfig &srvConfig;
		std::vector<Listener*> listeners;
		std::map<int, std::map<std::string, int> > host_map_by_port; // port -> (host -> vs_index)
		std::map<int, int> default_vs_by_port;						 // port -> default vs_index
		ServerPipeline	*serverpipeline;
		std::set<ClientHandler*> server_handlers;
		
		int createListenSocketRaw(const std::string &host, int port, bool &out_is_ipv6);
		void registerListeners();
		void unregisterListeners();
		void closeAll();
		void buildListenerPlan(std::vector<std::pair<std::string, int> > &unique_pairs,
							std::map<std::pair<std::string, int>, std::vector<int> > &vs_indices_by_pair);
		void buildHostMaps();

	public:
		void shutdownAllHandlers() {
			std::set<ClientHandler*>::iterator it = server_handlers.begin();
			while (it != server_handlers.end()) {
			ClientHandler* h = *it;
			if (h->conn())
				delete (h->conn()); 
			++it;
			delete h;
			}
			server_handlers.clear();
		}

		explicit Server(ServerConfig &srvConfig);
		~Server();
		EventLoop& getLoop();  
		void start();
		void stop();
		void run(int poll_timeout_ms);
		void setNonBlocking(int fd);
		void setCloseOnExec(int fd);
		int resolveVirtualServerByPort(int localPort, const std::string& hostHdr) const;
		const ServerConfig&	getConfig()const;
		ServerPipeline* getPipeline() const{ 
			return serverpipeline;
		};
		void trackHandler(ClientHandler* h)
		{
			if (h) server_handlers.insert(h);
		}
		void releaseHandler(ClientHandler* h) 
		{
			if (!h)
				return;
			std::set<ClientHandler*>::iterator it = server_handlers.find(h);
			if (it != server_handlers.end()) server_handlers.erase(it);
			delete h;
		}
		void releaseConnection(ClientConnection* c)
		{
			if (!c)
				return;
			loop_.removeOwner(c);
			for (std::set<ClientHandler*>::iterator it = server_handlers.begin();
				it != server_handlers.end(); ++it)
			{
				ClientHandler* h = *it;
				if (h && h->conn() == c)
				{
					// loop.removeFD(c->getFD());
					server_handlers.erase(it);
					delete h;
					break;
				}
			}
			loop_.removeFD(c->getFD());
			delete c;
		}


		#ifdef UNIT_TEST
		public:
			size_t listenerCount() const { return listeners.size(); }
			int    listenerFD(size_t i) const { return (i < listeners.size() && listeners[i]) ? listeners[i]->getFD() : -1; }
			int    listenerPortAt(size_t i) const { return (i < listeners.size() && listeners[i]) ? listeners[i]->getPort() : -1; }
		#endif
};








#endif // SERVER_H


