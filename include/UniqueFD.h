/* --- UniqueFD.h --- */

/* ------------------------------------------
Author: undefined
Date: 8/11/2025
------------------------------------------ */

#ifndef UNIQUEFD_H
#define UNIQUEFD_H

#include <unistd.h>

class UniqueFD
{
	int fd_;

	public:
		explicit UniqueFD(int fd = -1)
			: fd_(fd) {}

		~UniqueFD()
		{
			if (fd_ != -1)
			{
				::close(fd_);
			}
		}

	private:
		UniqueFD(const UniqueFD &);
		UniqueFD &operator=(const UniqueFD &);

	public:
		int get() const { return fd_; }

		operator bool() const { return fd_ != -1; }

		int release()
		{
			int temp = fd_;
			fd_ = -1;
			return temp;
		}

		void reset(int newFd = -1)
		{
			if (fd_ != -1)
			{
				::close(fd_);
			}
			fd_ = newFd;
		}
};

#endif // UNIQUEFD_H
