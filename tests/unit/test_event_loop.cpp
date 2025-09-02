#include <catch2/catch_all.hpp>
#include "EventLoop.h"
#include <unistd.h>
#include <fcntl.h>

static void set_nonblock(int fd)
{
	int fl = ::fcntl(fd, F_GETFL, 0);
	REQUIRE(fl >= 0);
	REQUIRE(::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
}


TEST_CASE("EventLoop default construction", "[EventLoop]")
{
	EventLoop loop;
	// Should not have any file descriptors
	REQUIRE(loop.handleEvents(0).empty());
}

TEST_CASE("EventLoop add, mod, and remove FD", "[EventLoop]")
{
	EventLoop loop;
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	int rfd = fds[0], wfd = fds[1];
	// Add read end
	REQUIRE(loop.addFD(rfd, POLLIN));
	// Mod should succeed
	REQUIRE(loop.modFD(rfd, POLLIN | POLLOUT));
	// Remove should succeed (no return, but should not crash)
	loop.removeFD(rfd);
	// Remove again should be safe (no crash)
	loop.removeFD(rfd);
	close(rfd);
	close(wfd);
}

TEST_CASE("EventLoop handleEvents detects readable pipe", "[EventLoop]")
{
	EventLoop loop;
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	int rfd = fds[0], wfd = fds[1];
	REQUIRE(loop.addFD(rfd, POLLIN));
	// Write to pipe
	const char msg = 'x';
	REQUIRE(write(wfd, &msg, 1) == 1);
	// Should detect rfd as readable
	auto events = loop.handleEvents(100);
	bool found = false;
	for (size_t i = 0; i < events.size(); ++i)
	{
		if (events[i].first == rfd && (events[i].second & POLLIN))
			found = true;
	}
	REQUIRE(found);
	close(rfd);
	close(wfd);
}

TEST_CASE("EventLoop run/stop does not block forever with no fds", "[EventLoop]")
{
	EventLoop loop;
	loop.stop();  // Should not hang
	loop.run(10); // Should return quickly
}

TEST_CASE("EventLoop addFD returns false for invalid fd", "[EventLoop][error]")
{
	EventLoop loop;
	REQUIRE_FALSE(loop.addFD(-1, POLLIN));
}

TEST_CASE("EventLoop addFD returns false for duplicate fd", "[EventLoop][error]")
{
	EventLoop loop;
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	int rfd = fds[0];
	REQUIRE(loop.addFD(rfd, POLLIN));
	REQUIRE_FALSE(loop.addFD(rfd, POLLIN));
	close(fds[0]);
	close(fds[1]);
}

TEST_CASE("EventLoop mod returns false for missing fd", "[EventLoop][error]")
{
	EventLoop loop;
	REQUIRE_FALSE(loop.modFD(12345, POLLIN));
}

TEST_CASE("EventLoop handleEvents returns empty on invalid fd", "[EventLoop][error]")
{
	EventLoop loop;
	// Add an invalid fd (closed pipe)
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	int rfd = fds[0];
	loop.addFD(rfd, POLLIN);
	close(rfd);
	close(fds[1]);
	// Now rfd is invalid, poll should fail and return empty
	auto events = loop.handleEvents(10);
	REQUIRE(events.empty());
}


TEST_CASE("EventLoop basic add/mod/remove", "[loop]")
{
	EventLoop loop;
	int fds[2];
	REQUIRE(::pipe(fds) == 0);
	set_nonblock(fds[0]);
	set_nonblock(fds[1]);

	REQUIRE(loop.addFD(fds[0], POLLIN));
	REQUIRE_FALSE(loop.addFD(fds[0], POLLIN)); // cannot add twice
	REQUIRE(loop.modFD(fds[0], POLLIN | POLLOUT));
	loop.removeFD(fds[0]);
	REQUIRE(loop.indexOfFD(fds[0]) < 0);

	::close(fds[0]);
	::close(fds[1]);
}

TEST_CASE("EventLoop owners: set/clear", "[loop][owner]")
{
	EventLoop loop;
	int fds[2];
	REQUIRE(::pipe(fds) == 0);
	loop.setOwner(fds[0], 0);
	loop.clearOwner(fds[0]);
	::close(fds[0]);
	::close(fds[1]);
}