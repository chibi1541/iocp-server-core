#include "pch.h"
#include "RecvBuffer.h"

RecvBuffer::RecvBuffer(int32 bufferSize) : _bufferSize(bufferSize)
{
	_capacity = _bufferSize * BUFFER_COUNT;
	_buffer.resize(_capacity);
}

RecvBuffer::~RecvBuffer()
{

}

void RecvBuffer::Clean()
{
	int32 dataSize = DataSize();

	if(dataSize == 0)
	{
		_writeIndex = _readIndex = 0;
	}
	else
	{
		// 남아 있는 버퍼 사이즈가 처음 할당했던 버퍼 사이즈 수치보다 적을 경우
		if(FreeSize() < _bufferSize)
		{
			// 버퍼에 남아있는 데이터를 버퍼 맨 앞으로 복사하고 인덱스 초기화
			::memcpy(_buffer.data(), ReadPos(), dataSize);
			_readIndex = 0;
			_writeIndex = dataSize;
		}
	}
}

bool RecvBuffer::OnWrite(int32 numOfBytes)
{
	if(numOfBytes > FreeSize())
	{
		return false;
	}

	_writeIndex += numOfBytes;
	return true;
}

bool RecvBuffer::OnRead(int32 numOfBytes)
{
	if (numOfBytes > FreeSize())
	{
		return false;
	}

	_readIndex = numOfBytes;
	return true;
}
