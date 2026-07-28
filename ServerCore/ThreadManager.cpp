#include "pch.h"
#include "ThreadManager.h"
#include "CoreTLS.h"
#include "CoreGlobal.h"
#include "GlobalQueue.h"
#include "JobQueue.h"

/*=====================
	ThreadManager
=======================*/

ThreadManager::ThreadManager()
{
	// 메인 스레드의 Thread Local Storage 초기화
	InitTLS();
}

ThreadManager::~ThreadManager()
{
	Join();
	DestoryTLS();
}

void ThreadManager::Launch(function<void(void)> callback)
{
	LockGuard guard(_mutex);

	_threads.push_back(thread([=]()
		{
			InitTLS();
			callback();
			DestoryTLS();
		}));
}

void ThreadManager::Join()
{
	for(thread& t : _threads)
	{
		if(t.joinable())
			t.join();
	}

	_threads.clear();
}

void ThreadManager::InitTLS()
{
	static Atomic<uint32> SThreadId = 1;
	LThreadId = SThreadId.fetch_add(1);
}

void ThreadManager::DestoryTLS()
{

}

void ThreadManager::DoGlobalQueueWork()
{
	while(true)
	{
		uint64 now = ::GetTickCount64();
		if(now < LEndTickCount)
		{
			break;		
		}

		JobQueueRef jobQueue = GGlobalQueue->Pop();
		if(jobQueue == nullptr)
		{
			break;
		}

		jobQueue->Execute();
	}

}

void ThreadManager::DistributeReservedJobs()
{
	const uint64 now = ::GetTickCount64();
	
	GJobTimer->Distribute(now);
}
