#include "pch.h"
#include "Lock.h"
#include "DeadLockProfiler.h"

void Lock::WriteLock(const char* name)
{
#if _DEBUG
	// A null GDeadLockProfiler means we are shutting down.
	// The thread_local LLockStack it relies on may already be destroyed,
	// so skip profiling.
	if (GDeadLockProfiler != nullptr)
		GDeadLockProfiler->PushLock(name);
#endif

	// 이미 해당 쓰레드가 Write Lock을 점유한 경우
	const uint32 lockThreadId = (_lockFlag.load() & WRITE_THREAD_MASK) >> 16;
	if(lockThreadId == LThreadId)
	{
		// 재귀적으로 Lock을 점유하도록 함
		_writeCount++;
		return;
	}

	const uint64 beginTick = ::GetTickCount64();
	// 아무도 Lock(Write + Read)을 점유하지 않을 때 경합 후에 Write Lock을 점유
	const uint32 desired = ((LThreadId << 16) & WRITE_THREAD_MASK);
	while(true)
	{
		for(uint32 spinCount = 0 ; spinCount < MAX_SPIN_COUNT ; spinCount++)
		{
			uint32 expected = EMPTY_FLAG;
			if(_lockFlag.compare_exchange_strong(OUT expected, desired))
			{
				// 재귀적으로 WriteLock을 점유할 수 있기 때문에
				// 카운트를 증가 시키고 함수를 빠져나감
				_writeCount++;
				return;
			}
		}

		if(::GetTickCount64() - beginTick >= AQUIRE_TIMEOUT_TICK)
			CRASH("LOCK_TIMEOUT");

		this_thread::yield();
	}
}

void Lock::WriteUnlock(const char* name)
{
#if _DEBUG
	if (GDeadLockProfiler != nullptr)
		GDeadLockProfiler->PopLock(name);
#endif

	// Read Lock을 점유 중인 상태에서 Write Lock을 해제할 수 없음
	if ((_lockFlag.load() & READ_COUNT_MASK) != 0)
		CRASH("INVALID_UNLOCK_ORDER");

	const int32 lockCount = --_writeCount;
	if(lockCount == 0)
		_lockFlag.store(EMPTY_FLAG);
}

void Lock::ReadLock(const char* name)
{
#if _DEBUG
	// A null GDeadLockProfiler means we are shutting down.
	// The thread_local LLockStack it relies on may already be destroyed,
	// so skip profiling.
	if (GDeadLockProfiler != nullptr)
		GDeadLockProfiler->PushLock(name);
#endif

	// 이미 Write Lock을 점유한 상태라면 (다른 쓰레드의 접근이 불가능하니)
	const uint32 lockThreadId = (_lockFlag.load() & WRITE_THREAD_MASK);
	if(lockThreadId == LThreadId)
	{
		// Read Lock 카운트를 하나 올려줌
		_lockFlag.fetch_add(1);
		return;
	}

	// 아무도 Write Lock을 점유 중이지 않다면
	// 경합하여 Read Lock 카운트를 증가(경합하지 않는다는 카운트가 꼬일 수 있음)
	const uint64 beginTick = ::GetTickCount64();
	while(true)
	{
		for(uint32 spinCount = 0; spinCount < MAX_SPIN_COUNT; spinCount++)
		{
			// 도중에 누가 Write Lock을 점유할 수도 있으니 
			// 아래와 같은 방식이면 위험함
			// uint32 expected = _lockFlag.load();
			uint32 expected = (_lockFlag.load() & READ_COUNT_MASK);
			if (_lockFlag.compare_exchange_strong(expected, expected+1))
			{
				return;
			}
		}

		if(::GetTickCount64() - beginTick > AQUIRE_TIMEOUT_TICK)
			CRASH("LOCK_TIMEOUT");

		this_thread::yield();
	}
}

void Lock::ReadUnlock(const char* name)
{
#if _DEBUG
	if (GDeadLockProfiler != nullptr)
		GDeadLockProfiler->PopLock(name);
#endif

	// 재약이 없나?
	// 혹시 모를 체크 정도?
	// fetch_add, fetch_sub는 연산 전에 값을 반환하고 연산을 수행
	// 그러니 0이 나오면 이상 상황
	if(_lockFlag.fetch_sub(1) == 0)
		CRASH("MULTIPLE_UNLOCK");
}
