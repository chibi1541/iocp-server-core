#include "pch.h"
#include "SendBuffer.h"

/*--------------
	SendBuffer
---------------*/

SendBuffer::SendBuffer(SendBufferChunkRef owner, BYTE* buffer, int32 allocSize)
	: _owner(owner), _buffer(buffer), _allocSize(allocSize)
{
	
}

SendBuffer::~SendBuffer()
{

}

void SendBuffer::Close(uint32 writeSize)
{
	ASSERT_CRASH(_allocSize >= writeSize);
	_writeSize = writeSize;
	_owner->Close(writeSize);
}


/*--------------------
	SendBufferChunk
---------------------*/

SendBufferChunk::SendBufferChunk()
{

}

SendBufferChunk::~SendBufferChunk()
{

}

void SendBufferChunk::Reset()
{
	_usedSize = 0;
	_open = false;
}

SendBufferRef SendBufferChunk::Open(uint32 size)
{
	// 사이즈 상한 체크
	ASSERT_CRASH(SENDBUFFER_CHUNK_SIZE >= size);

	// 오픈 체크
	ASSERT_CRASH(_open == false);

	// 프리 사이즈 체크
	// 이거 예외처리하고 nullptr 반환하게 되어있는데 여기 크래쉬 내야하지 않나?
	if(FreeSize() < size)
		return nullptr;

	_open = true;
	
	// SendBuffer도 풀링
	return ObjectPool<SendBuffer>::MakeShared(shared_from_this(), Buffer(), size);
}


void SendBufferChunk::Close(uint32 size)
{
	ASSERT_CRASH(_open);
	_open = false;
	_usedSize += size;

	//cout << "FreeSize : " << FreeSize() << endl;
}

/*------------------------
	SendBufferManager
-------------------------*/

Atomic<bool> SendBufferManager::s_shuttingDown = false;

SendBufferManager::~SendBufferManager()
{
	s_shuttingDown.store(true);

	// Move the held chunks out first, then release them.
	// PushGlobal now takes the xdelete path, so there is no loop.
	// StlAllocator has no operator==, so std::vector::swap will not compile here.
	Vector<SendBufferChunkRef> chunks;
	{
		WRITE_LOCK;
		chunks.reserve(_sendBufferChunks.size());
		for (const SendBufferChunkRef& chunk : _sendBufferChunks)
			chunks.push_back(chunk);

		_sendBufferChunks.clear();
	}

	chunks.clear();
}

SendBufferRef SendBufferManager::Open(uint32 size)
{
	// TLS에 chunk가 없으면 pop해서 청크를 TLS에 할당
	// chunk가 open 상태인지 체크, open이면 어디서 로직이 꼬인거니 크래쉬
	// TLS 청크의 남은 프리사이즈가 오픈 하려는 크기보다 적을 때 pop해서 새 청크를 할당
	// 청크의 오픈을 호출하여 사이즈 만큼 할당한 SendBufferRef를 반환

	if(LSendBufferChunk == nullptr)
	{
		LSendBufferChunk = Pop();
		LSendBufferChunk->Reset();
	}

	ASSERT_CRASH(LSendBufferChunk->IsOpen() == false);

	if(LSendBufferChunk->FreeSize() < size)
	{
		LSendBufferChunk = Pop();
		LSendBufferChunk->Reset();
	}

	return LSendBufferChunk->Open(size);
}

SendBufferChunkRef SendBufferManager::Pop()
{
	{
		WRITE_LOCK;
		if (_sendBufferChunks.empty() == false)
		{
			SendBufferChunkRef sendBufferChunk = _sendBufferChunks.back();
			_sendBufferChunks.pop_back();
			return sendBufferChunk;
		}

	}

	return SendBufferChunkRef(xnew<SendBufferChunk>(), PushGlobal);
}

void SendBufferManager::Push(SendBufferChunkRef buffer)
{
	WRITE_LOCK;
	_sendBufferChunks.push_back(buffer);
}

void SendBufferManager::PushGlobal(SendBufferChunk* buffer)
{
	// Do not recycle once we are shutting down, or the manager is already gone.
	// (a thread's LSendBufferChunk can be released late, on thread exit)
	if (s_shuttingDown.load() || GSendBufferManager == nullptr)
	{
		xdelete(buffer);
		return;
	}

	LOG_TRACE(L"PushGlobal SendBufferChunk");

	GSendBufferManager->Push(SendBufferChunkRef(buffer, PushGlobal));
}