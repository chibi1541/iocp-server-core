#include "pch.h"
#include "Allocator.h"
#include "Memory.h"

void* BaseAllocator::Alloc(int32 size)
{
	return ::malloc(size);
}

void BaseAllocator::Release(void* ptr)
{
	::free(ptr);
}

void* StompAllocator::Alloc(int32 size)
{
	const int64 pageCount = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	// StompAllocator를 활용하는 경우 메모리를 실제 필요량보다 크게 할당하기 때문에
	// 오버플로우에 대한 오류가 발생해도 못잡는 경우가 생김
	// 이를 막기 위해 할당받은 메모리를 뒤쪽에서부터 사용하도록 값을 반환
	const int64 dataOffset = pageCount * PAGE_SIZE - size;

	// @param : 메모리 주소(NULL 이면 알아서 처리), 할당 메모리 사이즈, 메모리 할당 정책(예약하고 | 바로 반환해서 사용), 메모리 접근 권한 
	void* baseAddress = ::VirtualAlloc(NULL, pageCount * PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

	// 실제 반환 메모리는 오프셋만큼을 건너뛴 위치
	return static_cast<void*>(static_cast<int8*>(baseAddress) + dataOffset);
}

void StompAllocator::Release(void* ptr)
{
	// 처음 할당 받은 메모리가 오프셋 만큼 떨어져 있으므로
	// 원본 메모리 시작 위치를 계산
	const int64 address = reinterpret_cast<int64>(ptr);
	const int64 baseAddress = address - (address % PAGE_SIZE);
	::VirtualFree(reinterpret_cast<void*>(baseAddress), 0, MEM_RELEASE);
}

/*-------------------
	PoolAllocator
-------------------*/

void* PoolAllocator::Alloc(int32 size)
{
	return GMemory->Allocate(size);
}

void PoolAllocator::Release(void* ptr)
{
	GMemory->Release(ptr);
}
