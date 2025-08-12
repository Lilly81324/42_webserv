#include <iostream>
#include "ServerConfig.h"
#include "Server.h"
using namespace std;

/*	Main */
int main(int argc, char const *argv[])
{

	ServerConfig srvConfig();
	Server server(srvConfig);

	server.run();

	(void)argc;
	(void)argv;
	cout << "OK this is a program" << endl;
	return 0;
}
