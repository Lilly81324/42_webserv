#if !defined(PHASEDEADLINE_H)
#define PHASEDEADLINE_H

class PhaseDeadline
{
	public:
		PhaseDeadline() : active_(false), deadline_ms_(0) {}

		void reset(unsigned long long now_ms, int timeout_ms)
		{
			if (timeout_ms <= 0)
			{
				active_ = false;
				deadline_ms_ = 0;
				return;
			}
			active_ = true;
			deadline_ms_ = now_ms + static_cast<unsigned long long>(timeout_ms);
		}

		bool expired(unsigned long long now_ms) const
		{
			if (!active_)
				return false;
			return now_ms >= deadline_ms_;
		}

		unsigned long long remaining(unsigned long long now_ms) const
		{
			if (!active_ || now_ms >= deadline_ms_)
				return 0ULL;
			return deadline_ms_ - now_ms;
		}

	private:
		bool active_;
		unsigned long long deadline_ms_;
};

#endif //  PHASEDEADLINE_H
