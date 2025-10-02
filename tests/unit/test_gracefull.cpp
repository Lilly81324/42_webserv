#include <catch2/catch_all.hpp>
#include <sys/wait.h>
#include <fcntl.h>
#include <iostream>

TEST_CASE("Gracefull Shutdown", "[gracefull][shellscript]")
{
	SECTION("Simple")
	{
		// This will run the shell script for theese test cases
		// The shell script will compile our main, if it doesnt exist
		// It will then use a free port and its own config
		// So it will be pretty tough to mess up, unless the actual test case fails
		std::cout << "Testing Shutdown... (at most 10 seconds)" << std::endl;
		pid_t pid;

		pid = fork();
		if (pid == 0)
		{
			int fd = open("/dev/null", O_WRONLY);
			dup2(fd, STDOUT_FILENO);
			close(fd);
			execl("./tests/test_shutdown.sh", "./tests/test_shutdown.sh", NULL);
			exit(0);
		}
		int status;
		waitpid(-1, &status, 0);
		REQUIRE(WEXITSTATUS(status) == 0);
	}
}
