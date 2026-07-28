#include "pch.h"
#include "JobTimer.h"
#include "JobQueue.h"

void JobTimer::Reserve(uint64 tickAfter, weak_ptr<JobQueue> owner, JobRef job)
{
	// jobqueue에 등록되어야 하는 시간 계산
	const uint64 executeTime = ::GetTickCount64() + tickAfter;

	JobData* jobData = ObjectPool<JobData>::Pop(owner, job);

	WRITE_LOCK;
	_items.push(TimerItem{executeTime, jobData});

}

void JobTimer::Distribute(uint64 now)
{
	if(_distributing.exchange(true) == true)
		return;

	Vector<TimerItem> items;
	{
		WRITE_LOCK;

		while(_items.empty() == false)
		{
			TimerItem item = _items.top();
			if(now < item.executeTick)
				break;

			items.push_back(item);
			_items.pop();
		}
	}

	for(TimerItem& item : items)
	{
		if(JobQueueRef owner = item.jobData->owner.lock())
			owner->Push(item.jobData->job, true);

		ObjectPool<JobData>::Push(item.jobData);
	}

	// 끝났으면 풀어준다
	// 굳이 여기까지 와서 풀어주는 이유가 궁금했는데 jobqueue에 밀어 넣는 과정을 동시에 실행하게 되면 
	// 나중에 들어온 일감이 먼저 실행되는 상황이 발생할 수도 있기 때문이라고 함 
	_distributing.store(false);
}

void JobTimer::Clear()
{
	WRITE_LOCK;
	while(_items.empty() == false)
	{
		const TimerItem& timerItem = _items.top();
		ObjectPool<JobData>::Push(timerItem.jobData);
		_items.pop();
	}
}
