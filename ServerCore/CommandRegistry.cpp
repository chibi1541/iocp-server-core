#include "pch.h"
#include "CommandRegistry.h"
#include "JobQueue.h"
#include "ServerStats.h"
#include "Logger.h"
#include "CrashDump.h"
#include "GlobalQueue.h"
#include "JobTimer.h"
#include <cstdarg>
#include <algorithm>
#include <cwctype>

/*---------------
	CommandContext
----------------*/

void CommandContext::Reply(const WCHAR* format, ...)
{
	if (reply == nullptr || format == nullptr)
		return;

	WCHAR buffer[4096];

	va_list ap;
	va_start(ap, format);
	const int32 written = ::_vsnwprintf_s(buffer, 4096, _TRUNCATE, format, ap);
	va_end(ap);

	if (written < 0)
		buffer[4095] = L'\0';

	reply(std::wstring(buffer));
}

const std::wstring& CommandContext::Arg(size_t index) const
{
	static const std::wstring empty;
	return (index < args.size()) ? args[index] : empty;
}

bool CommandContext::HasFlag(const WCHAR* flag) const
{
	if (flag == nullptr)
		return false;

	for (size_t i = 1; i < args.size(); i++)
	{
		if (::_wcsicmp(args[i].c_str(), flag) == 0)
			return true;
	}

	return false;
}

/*---------------
	CommandRegistry
----------------*/

void CommandRegistry::Register(const WCHAR* name, const WCHAR* usage, const WCHAR* description,
							   CommandHandler handler, CommandRunMode mode)
{
	if (name == nullptr || handler == nullptr)
		return;

	Entry entry;
	entry.name = name;
	entry.usage = (usage != nullptr) ? usage : name;
	entry.description = (description != nullptr) ? description : L"";
	entry.handler = std::move(handler);
	entry.mode = mode;

	const std::wstring key = ToLower(entry.name);

	std::lock_guard<std::mutex> guard(_mutex);

	if (_commands.find(key) == _commands.end())
		_order.push_back(key);

	_commands[key] = std::move(entry);
}

bool CommandRegistry::Unregister(const WCHAR* name)
{
	if (name == nullptr)
		return false;

	const std::wstring key = ToLower(name);

	std::lock_guard<std::mutex> guard(_mutex);

	if (_commands.erase(key) == 0)
		return false;

	_order.erase(std::remove(_order.begin(), _order.end(), key), _order.end());
	return true;
}

void CommandRegistry::SetGameJobQueue(JobQueueRef jobQueue)
{
	std::lock_guard<std::mutex> guard(_mutex);
	_gameJobQueue = jobQueue;
}

void CommandRegistry::SetShutdownHandler(std::function<void()> handler)
{
	std::lock_guard<std::mutex> guard(_mutex);
	_shutdownHandler = std::move(handler);
}

bool CommandRegistry::Execute(const std::wstring& line, CommandReply reply, bool isRemote)
{
	std::vector<std::wstring> args = Tokenize(line);
	if (args.empty())
		return true;	// 빈 줄은 조용히 무시

	const std::wstring key = ToLower(args[0]);

	Entry entry;
	JobQueueRef gameJobQueue;
	{
		std::lock_guard<std::mutex> guard(_mutex);

		auto it = _commands.find(key);
		if (it == _commands.end())
		{
			if (reply != nullptr)
				reply(L"unknown command : " + args[0] + L"   (type 'help')");
			return false;
		}

		entry = it->second;
		gameJobQueue = _gameJobQueue.lock();
	}

	CommandContext context;
	context.args = std::move(args);
	context.reply = reply;
	context.isRemote = isRemote;

	if (entry.mode == CommandRunMode::GameThread)
	{
		if (gameJobQueue == nullptr)
		{
			if (reply != nullptr)
				reply(L"command '" + entry.name + L"' needs a live game job queue (CommandRegistry::SetGameJobQueue)");
			return false;
		}

		CommandHandler handler = entry.handler;
		gameJobQueue->DoAsync([handler, context]() mutable { handler(context); });
		return true;
	}

	entry.handler(context);
	return true;
}

std::wstring CommandRegistry::BuildHelp(const WCHAR* name)
{
	std::lock_guard<std::mutex> guard(_mutex);

	WCHAR buffer[1024];

	if (name != nullptr && name[0] != L'\0')
	{
		auto it = _commands.find(ToLower(name));
		if (it == _commands.end())
			return std::wstring(L"unknown command : ") + name;

		::swprintf_s(buffer, L"%s\n  usage : %s\n  %s",
			it->second.name.c_str(), it->second.usage.c_str(), it->second.description.c_str());
		return std::wstring(buffer);
	}

	std::wstring result = L"---------------- Commands ----------------";
	for (const std::wstring& key : _order)
	{
		auto it = _commands.find(key);
		if (it == _commands.end())
			continue;

		::swprintf_s(buffer, L"\n %-28s %s", it->second.usage.c_str(), it->second.description.c_str());
		result += buffer;
	}
	result += L"\n------------------------------------------";

	return result;
}

/*---------------
	Builtins
----------------*/

void CommandRegistry::RegisterBuiltins()
{
	{
		std::lock_guard<std::mutex> guard(_mutex);
		if (_builtinsRegistered)
			return;
		_builtinsRegistered = true;
	}

	Register(L"help", L"help [command]", L"명령 목록 / 상세 도움말",
		[this](CommandContext& context)
		{
			const std::wstring& target = context.Arg(1);
			context.Reply(L"%s", BuildHelp(target.empty() ? nullptr : target.c_str()).c_str());
		});

	Register(L"status", L"status", L"서버 상태 요약 (세션/트래픽/잡/메모리)",
		[](CommandContext& context)
		{
			if (GServerStats == nullptr)
			{
				context.Reply(L"stats not available");
				return;
			}
			context.Reply(L"%s", GServerStats->Format(GServerStats->Capture()).c_str());
		});

	Register(L"sessions", L"sessions [-v]", L"서비스/세션 목록 (-v 면 세션 단위까지)",
		[](CommandContext& context)
		{
			if (GServerStats == nullptr)
			{
				context.Reply(L"stats not available");
				return;
			}
			context.Reply(L"%s", GServerStats->FormatSessions(context.HasFlag(L"-v")).c_str());
		});

	Register(L"mem", L"mem", L"메모리 풀 사용량",
		[](CommandContext& context)
		{
			int32 useCount = 0;
			int32 reserveCount = 0;
			if (GMemory != nullptr)
				GMemory->CollectPoolStats(OUT useCount, OUT reserveCount);

			int32 chunkCount = 0;
			if (GSendBufferManager != nullptr)
				chunkCount = GSendBufferManager->GetChunkCount();

#ifdef _STOMP
			const WCHAR* note = L"_STOMP is ON : StompAllocator in use, memory pool is bypassed (numbers stay 0)";
#else
			const WCHAR* note = L"_STOMP is OFF : memory pool active";
#endif
			context.Reply(L"memory pool : use %d, reserve %d, pools %d\nsendbuffer chunks : %d\n%s",
				useCount, reserveCount, (GMemory != nullptr) ? GMemory->GetPoolCount() : 0,
				chunkCount, note);
		});

	Register(L"jobs", L"jobs", L"잡 큐 / 타이머 상태",
		[](CommandContext& context)
		{
			context.Reply(L"global queue : %d\nreserved timers : %d",
				(GGlobalQueue != nullptr) ? GGlobalQueue->GetSize() : 0,
				(GJobTimer != nullptr) ? GJobTimer->GetReservedCount() : 0);
		});

	Register(L"threads", L"threads", L"워커 스레드 수 / 현재 스레드 정보",
		[](CommandContext& context)
		{
			uint32 workerCount = 0;
			if (GServerStats != nullptr)
				workerCount = GServerStats->Capture().workerThreadCount;

			context.Reply(L"worker threads : %u\nthis command runs on LThreadId=%u (win tid %lu)",
				workerCount, LThreadId, ::GetCurrentThreadId());
		});

	Register(L"loglevel", L"loglevel <console|file> <trace|debug|info|warn|error|fatal|off>",
		L"런타임 로그 레벨 변경",
		[](CommandContext& context)
		{
			if (GLogger == nullptr)
			{
				context.Reply(L"logger not available");
				return;
			}

			if (context.ArgCount() < 2)
			{
				context.Reply(L"console=%s file=%s",
					Logger::LevelToString(GLogger->GetConsoleLevel()),
					Logger::LevelToString(GLogger->GetFileLevel()));
				return;
			}

			if (context.ArgCount() < 3)
			{
				context.Reply(L"usage : loglevel <console|file> <trace|debug|info|warn|error|fatal|off>");
				return;
			}

			LogLevel level = LogLevel::Info;
			if (Logger::ParseLevel(context.Arg(2).c_str(), OUT level) == false)
			{
				context.Reply(L"invalid level : %s", context.Arg(2).c_str());
				return;
			}

			const std::wstring& sink = context.Arg(1);
			if (::_wcsicmp(sink.c_str(), L"console") == 0)
			{
				GLogger->SetConsoleLevel(level);
				context.Reply(L"console log level -> %s", Logger::LevelToString(level));
			}
			else if (::_wcsicmp(sink.c_str(), L"file") == 0)
			{
				GLogger->SetFileLevel(level);
				context.Reply(L"file log level -> %s", Logger::LevelToString(level));
			}
			else
			{
				context.Reply(L"invalid sink : %s (console|file)", sink.c_str());
			}
		});

	Register(L"logdump", L"logdump [path]", L"최근 로그 링버퍼를 파일로 덤프",
		[](CommandContext& context)
		{
			if (GLogger == nullptr)
			{
				context.Reply(L"logger not available");
				return;
			}

			WCHAR path[MAX_PATH];
			if (context.ArgCount() >= 2)
			{
				::wcscpy_s(path, context.Arg(1).c_str());
			}
			else
			{
				SYSTEMTIME now = {};
				::GetLocalTime(OUT &now);
				Logger::MakeDirectories(L"Logs");
				::swprintf_s(path, L"Logs\\ringdump_%04d%02d%02d_%02d%02d%02d.log",
					now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
			}

			GLogger->Flush();
			if (GLogger->DumpRingBuffer(path))
				context.Reply(L"ring buffer dumped : %s", path);
			else
				context.Reply(L"failed to dump ring buffer : %s", path);
		});

	Register(L"statdump", L"statdump [path]", L"현재 상태 스냅샷을 파일로 덤프",
		[](CommandContext& context)
		{
			if (GServerStats == nullptr)
			{
				context.Reply(L"stats not available");
				return;
			}

			WCHAR path[MAX_PATH];
			if (context.ArgCount() >= 2)
			{
				::wcscpy_s(path, context.Arg(1).c_str());
			}
			else
			{
				SYSTEMTIME now = {};
				::GetLocalTime(OUT &now);
				Logger::MakeDirectories(L"Logs");
				::swprintf_s(path, L"Logs\\statdump_%04d%02d%02d_%02d%02d%02d.txt",
					now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
			}

			if (GServerStats->DumpToFile(path))
				context.Reply(L"stat snapshot dumped : %s", path);
			else
				context.Reply(L"failed to dump stat snapshot : %s", path);
		});

	Register(L"minidump", L"minidump", L"예외 없이 지금 시점의 미니덤프를 남긴다",
		[](CommandContext& context)
		{
			if (CrashDump::IsInitialized() == false)
			{
				context.Reply(L"CrashDump::Init() was not called");
				return;
			}

			if (CrashDump::WriteDumpNow(L"AdminCommand"))
				context.Reply(L"minidump written : %s", CrashDump::GetLastDumpPath());
			else
				context.Reply(L"failed to write minidump");
		});

#ifdef _DEBUG
	Register(L"crashtest", L"crashtest", L"[Debug 전용] 일부러 크래시를 내서 덤프 경로를 검증",
		[](CommandContext& context)
		{
			context.Reply(L"crashing on purpose...");
			if (GLogger != nullptr)
				GLogger->Flush();

			CRASH("crashtest command");
		});
#endif

	Register(L"quit", L"quit", L"서버 종료 (SetShutdownHandler로 등록된 핸들러 호출)",
		[this](CommandContext& context)
		{
			std::function<void()> handler;
			{
				std::lock_guard<std::mutex> guard(_mutex);
				handler = _shutdownHandler;
			}

			if (handler == nullptr)
			{
				context.Reply(L"no shutdown handler registered (CommandRegistry::SetShutdownHandler)");
				return;
			}

			context.Reply(L"shutting down...");
			handler();
		});
}

/*---------------
	static utils
----------------*/

std::vector<std::wstring> CommandRegistry::Tokenize(const std::wstring& line)
{
	std::vector<std::wstring> result;
	std::wstring token;
	bool inQuote = false;

	for (wchar_t ch : line)
	{
		if (ch == L'"')
		{
			inQuote = !inQuote;
			continue;
		}

		const bool isSpace = (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n');
		if (isSpace && inQuote == false)
		{
			if (token.empty() == false)
			{
				result.push_back(token);
				token.clear();
			}
			continue;
		}

		token += ch;
	}

	if (token.empty() == false)
		result.push_back(token);

	return result;
}

std::wstring CommandRegistry::ToLower(const std::wstring& text)
{
	std::wstring result = text;
	std::transform(result.begin(), result.end(), result.begin(),
		[](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
	return result;
}
