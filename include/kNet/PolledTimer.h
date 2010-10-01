/* Copyright 2010 Jukka Jyl�nki

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */
#pragma once

/** @file PolledTimer.h
	@brief PolledTimer is a timer object that can be used in "polled" or per-frame executed contexts. */

#include "Clock.h"

namespace kNet
{

class PolledTimer
{
	bool enabled;
	tick_t alarmTime;

public:
	PolledTimer():enabled(false)
	{
	}

	explicit PolledTimer(float msecs)
	:enabled(false)
	{
		StartMSecs(msecs);
	}

	/// Starts the timer in a non-periodic mode, to go off once in the given amount of milliseconds.
	void StartMSecs(float msecs)
	{
		StartTicks((tick_t)(Clock::TicksPerSec() * (msecs / 1000.f)));
	}

	/// Starts the timer in a non-periodic mode, to go off once in the given amount of high-precision Clock ticks.
	void StartTicks(tick_t ticks)
	{
		alarmTime = Clock::Tick() + ticks;
		enabled = true;
	}

	void Stop()
	{
		enabled = false;
	}

	void Reset()
	{
		Stop();
	}

	bool Enabled() const
	{
		return enabled;
	}

	/// Tests whether the timer has gone off, and resets it as well.
	bool Test()
	{
		if (!enabled)
			return false;
		if (Clock::IsNewer(Clock::Tick(), alarmTime))
		{
			Reset();
			return true;
		}
		return false;
	}

	bool TriggeredOrNotRunning()
	{
		return Test() || !Enabled();
	}

	/// @return The number of ticks left until the timer elapses, or (tick_t)-1 if the timer is not active.
	tick_t TicksLeft() const
	{
		if (!enabled)
			return (tick_t)(-1);

		tick_t now = Clock::Tick();
		if (Clock::IsNewer(now, alarmTime))
			return 0;
		else
			return Clock::TicksInBetween(alarmTime, now);
	}

	/// @return The number of msecs left until the timer elapses, 0.f if the timer has elapsed, or 
	/// -1.0f if the timer is not even active.
	float MSecsLeft() const
	{
		if (!enabled)
			return -1.f;

		tick_t now = Clock::Tick();
		if (Clock::IsNewer(now, alarmTime))
			return 0.f;
		else
			return Clock::TimespanToMillisecondsF(now, alarmTime);
	}

	/// Waits until the timer elapses. Uses OS Clock::Sleep() to avoid excessive CPU use.
	void WaitPrecise()
	{
		if (!enabled)
			return;

		tick_t timeLeft = TicksLeft();
		while(timeLeft > 0)
		{
			if (timeLeft > Clock::TicksPerMillisecond())
			{
				float msecs = Clock::TicksToMillisecondsF(timeLeft);
				Clock::Sleep((int)msecs);
			}
			else // If there's less than one ms left, we use spinwait to return precisely as near to the correct tick as possible.
			{
				SpinWait();
				return;
			}
			timeLeft = TicksLeft();
		}
	}

	/// Waits in an empty loop until the timer elapses. Will cause the the CPU use to go to max.
	void SpinWait()
	{
		while(enabled && TicksLeft() > 0)
			;
	}
};

} // ~kNet
