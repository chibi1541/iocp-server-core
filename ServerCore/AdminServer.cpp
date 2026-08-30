#include "pch.h"
#include "AdminServer.h"
#include "AdminSession.h"
#include "Service.h"
#include "IocpCore.h"
#include "ThreadManager.h"
#include "ServerStats.h"
#include "Logger.h"

namespace
{
	std::mutex		SAdminTokenMutex;
	std::wstring	SAdminToken;

	// 길이에 무관한 상수 시간 비교
	bool ConstantTimeEquals(const std::wstring& lhs, const std::wstring& rhs)
	{
		const size_t maxLen = (lhs.size() > rhs.size()) ? lhs.size() : rhs.size();
		if (maxLen == 0)
			return false;

		uint32 diff = static_cast<uint32>(lhs.size() ^ rhs.size());
		for (size_t i = 0; i < maxLen; i++)
		{
			const wchar_t a = (i < lhs.size()) ? lhs[i] : L'\0';
			const wchar_t b = (i < rhs.size()) ? rhs[i] : L'\0';
			diff |= static_cast<uint32>(a ^ b);
		}

		return diff == 0;
	}
}

AdminServer::~AdminServer()
{
	Stop();
}

bool AdminServer::HasToken()
{
	std::lock_guard<std::mutex> guard(SAdminTokenMutex);
	return SAdminToken.empty() == false;
}

bool AdminServer::VerifyToken(const std::wstring& candidate)
{
	std::lock_guard<std::mutex> guard(SAdminTokenMutex);

	if (SAdminToken.empty())
		return false;

	return ConstantTimeEquals(SAdminToken, candidate);
}

bool AdminServer::Start(NetAddress address, const std::wstring& token, int32 maxSessionCount)
{
	if (_running.load())
		return true;

	if (token.empty())
	{
		LOG_ERROR(L"[admin] AdminServer::Start failed : empty token");
		return false;
	}

	{
		std::lock_guard<std::mutex> guard(SAdminTokenMutex);
		SAdminToken = token;
	}

	// 게임 워커와 완전히 분리된 IocpCore.
	// 워커가 전부 스톨해도 admin 접속과 응답은 계속 동작해야 한다.
	_core = MakeShared<IocpCore>();

	_service = MakeShared<ServerService>(
		address,
		_core,
		[]() { return static_pointer_cast<Session>(MakeShared<AdminSession>()); },
		maxSessionCount);

	if (_service->Start() == false)
	{
		LOG_ERROR(L"[admin] AdminServer::Start failed : listen %s:%u",
			address.GetIpAddress().c_str(), address.GetPort());
		_service = nullptr;
		_core = nullptr;
		return false;
	}

	if (GServerStats != nullptr)
		GServerStats->RegisterService(_service);

	_running.store(true);
	_worker = std::thread([this]() { WorkerLoop(); });

	LOG_WARN(L"[admin] listening on %s:%u (max %d sessions)",
		address.GetIpAddress().c_str(), address.GetPort(), maxSessionCount);

	return true;
}

void AdminServer::Stop()
{
	if (_running.exchange(false) == false)
		return;

	if (_worker.joinable())
		_worker.join();

	if (_service != nullptr)
	{
		_service->CloseService();
		_service = nullptr;
	}

	_core = nullptr;

	{
		std::lock_guard<std::mutex> guard(SAdminTokenMutex);
		SAdminToken.clear();
	}

	LOG_WARN(L"[admin] stopped");
}

void AdminServer::WorkerLoop()
{
	// ThreadManager::Launch를 쓰지 않으므로 TLS를 직접 초기화한다.
	// Lock / LockQueue가 LThreadId를 쓰기 때문에 반드시 필요하다.
	ThreadManager::InitTLS();

	uint64 lastSweepTick = ::GetTickCount64();

	while (_running.load())
	{
		LEndTickCount = ::GetTickCount64() + 64;

		_core->Dispatch(50);

		const uint64 now = ::GetTickCount64();
		if (now - lastSweepTick < 1000)
			continue;

		lastSweepTick = now;

		// 인증하지 않고 눌러앉은 연결 정리
		Vector<SessionRef> sessions;
		_service->CollectSessions(OUT sessions);

		for (const SessionRef& session : sessions)
		{
			std::shared_ptr<AdminSession> adminSession = std::static_pointer_cast<AdminSession>(session);
			if (adminSession->IsAuthExpired(now) == false)
				continue;

			adminSession->SendLine(L"authentication timeout. bye.");
			adminSession->Disconnect(L"admin auth timeout");
		}
	}
}
