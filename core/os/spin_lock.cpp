/**************************************************************************/
/*  spin_lock.cpp                                                         */
/**************************************************************************/

#include "spin_lock.h"

#if defined(THREADS_ENABLED) && !defined(__APPLE__)

#include "core/profiling/profiling.h"

// fridge perf: a pure spin never gives its core back. With every worker spinning on one lock, a
// holder the OS preempted waits for a spinner's quantum to expire: all cull threads froze 11-55 ms
// together (tooling/perf, 2026-09-28). Spin long enough to cover a normal hold, then yield.
static constexpr uint32_t SPINS_BEFORE_YIELD = 256;

void SpinLock::_lock_contended() const {
	uint32_t spins = 0;
	while (true) {
		do {
			if (spins >= SPINS_BEFORE_YIELD) {
				GodotProfileZone("SpinLock yield wait");
				do {
					Thread::yield();
				} while (locked.load(std::memory_order_relaxed));
				break;
			}
			spins++;
			_cpu_pause();
		} while (locked.load(std::memory_order_relaxed));
		bool expected = false;
		if (locked.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
			return;
		}
	}
}

#endif
