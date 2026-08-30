#include "pch.h"
#include "AdminSession.h"
#include "AdminServer.h"
#include "CommandRegistry.h"
#include "Logger.h"

AdminSession::AdminSession()
{
	_connectTick = ::GetTickCount64();
}

AdminSession::~AdminSession()
{
}

bool AdminSession::IsAuthExpired(uint64 nowTick) const
{
	if (_authenticated)
		return false;

	return (nowTick - _connectTick) > AUTH_TIMEOUT_MS;
}

std::wstring AdminSession::DescribePeer()
{
	WCHAR buffer[128];
	std::wstring ip = GetAddress().GetIpAddress();
	::swprintf_s(buffer, L"%s:%u", ip.c_str(), GetAddress().GetPort());
	return std::wstring(buffer);
}

void AdminSession::SendLine(const std::wstring& text)
{
	if (IsConnected() == false)
		return;

	std::string utf8 = Logger::ToUtf8(text);
	utf8 += "\r\n";

	if (utf8.size() > MAX_LINE_BYTES)
		utf8.resize(MAX_LINE_BYTES);

	SendBufferRef sendBuffer = GSendBufferManager->Open(static_cast<uint32>(utf8.size()));
	if (sendBuffer == nullptr)
		return;

	::memcpy(sendBuffer->Buffer(), utf8.data(), utf8.size());
	sendBuffer->Close(static_cast<uint32>(utf8.size()));

	Send(sendBuffer);
}

void AdminSession::OnConnected()
{
	_connectTick = ::GetTickCount64();

	LOG_WARN(L"[admin] connected from %s", DescribePeer().c_str());

	SendLine(L"ServerCore admin console.");
	SendLine(L"authenticate first : AUTH <token>");
}

void AdminSession::OnDisconnected()
{
	LOG_WARN(L"[admin] disconnected %s (authenticated=%s)",
		DescribePeer().c_str(), _authenticated ? L"true" : L"false");
}

int32 AdminSession::OnRecv(BYTE* buffer, int32 len)
{
	// 개행 단위로만 소비한다. 남은 조각은 다음 recv에서 이어 붙는다.
	int32 processLen = 0;

	for (int32 i = 0; i < len; i++)
	{
		const char ch = static_cast<char>(buffer[i]);

		if (ch == '\n')
		{
			std::string lineUtf8;
			lineUtf8.swap(_pending);

			while (lineUtf8.empty() == false && (lineUtf8.back() == '\r' || lineUtf8.back() == '\n'))
				lineUtf8.pop_back();

			processLen = i + 1;

			if (lineUtf8.empty() == false)
			{
				const int32 wideLen = ::MultiByteToWideChar(CP_UTF8, 0, lineUtf8.c_str(), static_cast<int>(lineUtf8.size()), nullptr, 0);
				std::wstring line(static_cast<size_t>(wideLen > 0 ? wideLen : 0), L'\0');
				if (wideLen > 0)
					::MultiByteToWideChar(CP_UTF8, 0, lineUtf8.c_str(), static_cast<int>(lineUtf8.size()), line.data(), wideLen);

				HandleLine(line);

				if (IsConnected() == false)
					return processLen;
			}

			continue;
		}

		_pending += ch;

		if (_pending.size() > MAX_LINE_BYTES)
		{
			_pending.clear();
			SendLine(L"line too long");
			Disconnect(L"admin line overflow");
			return i + 1;
		}
	}

	// 개행까지 못 읽은 잔여분은 _pending에 들고 있으므로 전부 소비 처리한다
	return len;
}

void AdminSession::HandleLine(const std::wstring& line)
{
	if (_authenticated == false)
	{
		HandleAuth(line);
		return;
	}

	if (::_wcsicmp(line.c_str(), L"quit") == 0 || ::_wcsicmp(line.c_str(), L"exit") == 0)
	{
		SendLine(L"bye");
		Disconnect(L"admin quit");
		return;
	}

	LOG_WARN(L"[admin] %s > %s", DescribePeer().c_str(), line.c_str());

	if (GCommandRegistry == nullptr)
	{
		SendLine(L"command registry not available");
		return;
	}

	// 세션 수명이 명령 실행보다 짧을 수 있으므로 shared_ptr을 캡처한다
	std::shared_ptr<AdminSession> self = std::static_pointer_cast<AdminSession>(GetSessionRef());

	CommandReply reply = [self](const std::wstring& text)
	{
		self->SendLine(text);
	};

	GCommandRegistry->Execute(line, reply, true);
}

void AdminSession::HandleAuth(const std::wstring& line)
{
	std::vector<std::wstring> args = CommandRegistry::Tokenize(line);

	const bool isAuthCommand = (args.size() >= 1) && (::_wcsicmp(args[0].c_str(), L"auth") == 0);

	if (isAuthCommand == false)
	{
		SendLine(L"not authenticated. use : AUTH <token>");
		LOG_WARN(L"[admin] %s tried '%s' before auth", DescribePeer().c_str(), line.c_str());
		return;
	}

	const std::wstring token = (args.size() >= 2) ? args[1] : std::wstring();

	if (AdminServer::VerifyToken(token))
	{
		_authenticated = true;
		SendLine(L"authenticated. type 'help'.");
		LOG_WARN(L"[admin] %s authenticated", DescribePeer().c_str());
		return;
	}

	_authFailCount++;
	LOG_WARN(L"[admin] %s auth failed (%d/%d)", DescribePeer().c_str(), _authFailCount, static_cast<int32>(MAX_AUTH_FAIL));

	if (_authFailCount >= MAX_AUTH_FAIL)
	{
		SendLine(L"too many failures. bye.");
		Disconnect(L"admin auth failed");
		return;
	}

	SendLine(L"authentication failed.");
}
