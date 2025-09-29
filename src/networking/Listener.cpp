#include "Listener.h"

Listener::Listener() : fd(), port(0), is_ipv6(false), acceptor(NULL)
{}

Listener::Listener(int fd, std::string host, int port, bool ipv6) : fd(fd), host(host), port(port), is_ipv6(ipv6), acceptor(NULL)
{};

Listener::~Listener(void)
{
	if (acceptor)
		delete acceptor;
}

int Listener::getFD()
{ return this->fd.get(); };

int Listener::getPort()
{ return this->port; };

std::string Listener::getHost()
{ return this->host; };

bool Listener::IsIpv6()
{ return is_ipv6; };

AcceptorHandler *Listener::getAcceptor(void)
{ return acceptor; }

void Listener::setAcceptor(AcceptorHandler *acc)
{
	if (acceptor)
		delete acceptor;
	acceptor = acc;
}

void Listener::addVirtualServerIndex(int idx)
{ vs_indices.push_back(idx); }

void Listener::setVirtualServerIndices(const std::vector<int> &indices)
{ vs_indices = indices; }

void Listener::setVirtualServerIndices(const int *arr, size_t n)
{ vs_indices.assign(arr, arr + n); }
void Listener::reserveVirtualServers(size_t n)
{ vs_indices.reserve(n); }

const std::vector<int> &Listener::virtualServerIndices() const
{ return vs_indices; }

size_t Listener::virtualServerCount() const
{ return vs_indices.size(); }

int Listener::virtualServerIndexAt(size_t i) const
{ return vs_indices[i]; }

void Listener::swapVirtualServerIndices(std::vector<int> &other)
{ vs_indices.swap(other); }
