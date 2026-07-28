#include "pch.h"
#include "NetAddress.h"

NetAddress::NetAddress(SOCKADDR_IN sockAddr)
{

}

NetAddress::NetAddress(wstring ip, uint16 port)
{
	::memset(&_sockAddr, 0, sizeof(_sockAddr));
	_sockAddr.sin_family = AF_INET;
	_sockAddr.sin_addr = Ip2Address(ip.c_str());
	_sockAddr.sin_port = ::htons(port);
}

std::wstring NetAddress::GetIpAddress()
{
	WCHAR buffer[100];
	// WCHAR이기 때문에 sizeof(buffer)는 100이 아니라 200을 반환함
	// 전달 받는 마지막 매개 변수가 배열의 크기이기 때문에 sizeof(buffer)를 전달하면 안됨
	::InetNtopW(AF_INET, &_sockAddr.sin_addr, buffer, len32(buffer));
	return wstring(buffer);
}

IN_ADDR NetAddress::Ip2Address(const WCHAR* ip)
{
	IN_ADDR address;
	// WCHAR이기 때문에 W가 붙는 버전을 사용
	::InetPtonW(AF_INET, ip, &address);
	return address;
}
