#include "pch.h"
#include "ServerStats.h"
#include "Service.h"
#include "Memory.h"
#include "GlobalQueue.h"
#include "JobTimer.h"
#include "Logger.h"
#include <share.h>

ServerStats::ServerStats()
{
	_startTick = ::GetTickCount64();
}

void ServerStats::RegisterService(std::shared_ptr<Service> service)
{
	if (service == nullptr)
		return;

	std::lock_guard<std::mutex> guard(_serviceMutex);

	// 죽은 weak_ptr 정리 겸
	for (auto it = _services.begin(); it != _services.end(); )
	{
		if (it->expired())
			it = _services.erase(it);
		else
			++it;
	}

	_services.push_back(service);
}

StatSnapshot ServerStats::Capture()
{
	StatSnapshot snapshot;

	snapshot.captureTick = ::GetTickCount64();
	snapshot.uptimeMs = snapshot.captureTick - _startTick;

	snapshot.acceptCount = _acceptCount.load();
	snapshot.disconnectCount = _disconnectCount.load();
	snapshot.recvBytes = _recvBytes.load();
	snapshot.sendBytes = _sendBytes.load();
	snapshot.recvCount = _recvCount.load();
	snapshot.sendCount = _sendCount.load();
	snapshot.jobExecuted = _jobExecuted.load();
	snapshot.workerThreadCount = _workerThreadCount.load();

	{
		std::lock_guard<std::mutex> guard(_serviceMutex);
		for (const std::weak_ptr<Service>& weak : _services)
		{
			std::shared_ptr<Service> service = weak.lock();
			if (service == nullptr)
				continue;

			snapshot.serviceCount++;
			snapshot.sessionCount += service->GetCurrentSessionCount();
			snapshot.maxSessionCount += service->GetMaxSessionCount();
		}
	}

	if (GGlobalQueue != nullptr)
		snapshot.globalQueueSize = GGlobalQueue->GetSize();

	if (GJobTimer != nullptr)
		snapshot.reservedTimerCount = GJobTimer->GetReservedCount();

	if (GSendBufferManager != nullptr)
		snapshot.sendBufferChunkCount = GSendBufferManager->GetChunkCount();

	if (GMemory != nullptr)
		GMemory->CollectPoolStats(OUT snapshot.memPoolUseCount, OUT snapshot.memPoolReserveCount);

#ifdef _STOMP
	snapshot.memPoolMeaningful = false;
#else
	snapshot.memPoolMeaningful = true;
#endif

	return snapshot;
}

std::wstring ServerStats::Format(const StatSnapshot& current)
{
	StatSnapshot zero;
	zero.captureTick = current.captureTick;
	return FormatWithDelta(zero, current);
}

std::wstring ServerStats::FormatWithDelta(const StatSnapshot& prev, const StatSnapshot& current)
{
	const uint64 elapsedMs = (current.captureTick > prev.captureTick) ? (current.captureTick - prev.captureTick) : 0;
	const double seconds = (elapsedMs > 0) ? (elapsedMs / 1000.0) : 0.0;

	auto perSec = [seconds](uint64 prevValue, uint64 currentValue) -> double
	{
		if (seconds <= 0.0 || currentValue < prevValue)
			return 0.0;
		return static_cast<double>(currentValue - prevValue) / seconds;
	};

	const uint64 uptimeSec = current.uptimeMs / 1000;

	WCHAR buffer[2048];
	::swprintf_s(buffer,
		L"---------------- Server Status ----------------\n"
		L" uptime          : %llud %02lluh %02llum %02llus\n"
		L" services        : %d\n"
		L" sessions        : %d / %d   (accept %llu, disconnect %llu)\n"
		L" recv            : %llu pkt, %llu bytes   (%.1f pkt/s, %.1f KB/s)\n"
		L" send            : %llu pkt, %llu bytes   (%.1f pkt/s, %.1f KB/s)\n"
		L" jobs executed   : %llu   (%.1f job/s)\n"
		L" global queue    : %d   reserved timers : %d\n"
		L" sendbuf chunks  : %d\n"
		L" memory pool     : use %d, reserve %d%s\n"
		L" worker threads  : %u\n"
		L" log level       : console=%s file=%s\n"
		L"-----------------------------------------------",
		uptimeSec / 86400, (uptimeSec % 86400) / 3600, (uptimeSec % 3600) / 60, uptimeSec % 60,
		current.serviceCount,
		current.sessionCount, current.maxSessionCount, current.acceptCount, current.disconnectCount,
		current.recvCount, current.recvBytes, perSec(prev.recvCount, current.recvCount), perSec(prev.recvBytes, current.recvBytes) / 1024.0,
		current.sendCount, current.sendBytes, perSec(prev.sendCount, current.sendCount), perSec(prev.sendBytes, current.sendBytes) / 1024.0,
		current.jobExecuted, perSec(prev.jobExecuted, current.jobExecuted),
		current.globalQueueSize, current.reservedTimerCount,
		current.sendBufferChunkCount,
		current.memPoolUseCount, current.memPoolReserveCount,
		current.memPoolMeaningful ? L"" : L"   (_STOMP on : pool bypassed, numbers not meaningful)",
		current.workerThreadCount,
		(GLogger != nullptr) ? Logger::LevelToString(GLogger->GetConsoleLevel()) : L"-",
		(GLogger != nullptr) ? Logger::LevelToString(GLogger->GetFileLevel()) : L"-");

	return std::wstring(buffer);
}

std::wstring ServerStats::FormatSessions(bool verbose)
{
	std::wstring result;
	WCHAR buffer[512];

	std::vector<std::shared_ptr<Service>> services;
	{
		std::lock_guard<std::mutex> guard(_serviceMutex);
		for (const std::weak_ptr<Service>& weak : _services)
		{
			std::shared_ptr<Service> service = weak.lock();
			if (service != nullptr)
				services.push_back(service);
		}
	}

	if (services.empty())
	{
		return std::wstring(L"no service registered (call GServerStats->RegisterService(service) after Start())");
	}

	int32 index = 0;
	for (const std::shared_ptr<Service>& service : services)
	{
		const WCHAR* typeName = (service->GetServiceType() == ServiceType::Server) ? L"Server" : L"Client";
		std::wstring ip = service->GetAddress().GetIpAddress();

		::swprintf_s(buffer, L"[%d] %s  %s:%u  sessions %d / %d",
			index++, typeName, ip.c_str(), service->GetAddress().GetPort(),
			service->GetCurrentSessionCount(), service->GetMaxSessionCount());

		if (result.empty() == false)
			result += L"\n";
		result += buffer;

		if (verbose == false)
			continue;

		Vector<SessionRef> sessions;
		service->CollectSessions(OUT sessions);

		int32 sessionIndex = 0;
		for (const SessionRef& session : sessions)
		{
			std::wstring sessionIp = session->GetAddress().GetIpAddress();
			::swprintf_s(buffer, L"    - #%d  %s:%u  socket=%llu  connected=%s",
				sessionIndex++, sessionIp.c_str(), session->GetAddress().GetPort(),
				static_cast<uint64>(session->GetSocket()),
				session->IsConnected() ? L"true" : L"false");

			result += L"\n";
			result += buffer;
		}
	}

	return result;
}

bool ServerStats::DumpToFile(const WCHAR* path)
{
	if (path == nullptr)
		return false;

	FILE* file = ::_wfsopen(path, L"wb", _SH_DENYWR);
	if (file == nullptr)
		return false;

	const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
	::fwrite(bom, 1, sizeof(bom), file);

	std::wstring text = Format(Capture());
	text += L"\n\n";
	text += FormatSessions(true);

	const std::string utf8 = Logger::ToUtf8(text);
	::fwrite(utf8.data(), 1, utf8.size(), file);

	::fflush(file);
	::fclose(file);
	return true;
}
