#include "pch.h"
#include "CrashDump.h"
#include "Logger.h"
#include <DbgHelp.h>
#include <csignal>
#include <exception>
#include <cstdlib>
#include <process.h>
#include <crtdbg.h>

#pragma comment(lib, "dbghelp.lib")

namespace
{
	Atomic<bool>	SInitialized = false;
	LONG			SDumpWritten = 0;			// InterlockedExchange 가드 (1회만 기록)

	WCHAR			SDumpDir[MAX_PATH] = L"Dumps";
	WCHAR			SExeName[MAX_PATH] = L"Server";
	WCHAR			SLastDumpPath[MAX_PATH] = {};
	uint32			SDumpType = 0;

	// 핸들러 안에서 힙을 쓰지 않기 위해 정적 버퍼에 담아둔다
	WCHAR			SPendingCause[512] = {};

	LPTOP_LEVEL_EXCEPTION_FILTER SPrevFilter = nullptr;

	const uint32 DEFAULT_DUMP_TYPE =
		MiniDumpNormal |
		MiniDumpWithThreadInfo |
		MiniDumpWithIndirectlyReferencedMemory |
		MiniDumpWithHandleData |
		MiniDumpWithUnloadedModules;

	void InvalidParameterHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t)
	{
		if (GLogger != nullptr)
		{
			WCHAR text[512];
			::swprintf_s(text, L"[FATAL] Invalid CRT parameter : %s (%s, %s:%u)",
				expression ? expression : L"?", function ? function : L"?", file ? file : L"?", line);
			GLogger->WriteFatalDirect(text);
		}

		CrashDump::WriteDumpNow(L"InvalidParameter");
		::abort();
	}

	void PureCallHandler()
	{
		if (GLogger != nullptr)
			GLogger->WriteFatalDirect(L"[FATAL] Pure virtual function call");

		CrashDump::WriteDumpNow(L"PureCall");
		::abort();
	}

	void TerminateHandler()
	{
		if (GLogger != nullptr)
			GLogger->WriteFatalDirect(L"[FATAL] std::terminate called");

		CrashDump::WriteDumpNow(L"Terminate");
		::_exit(3);
	}

	void SignalHandler(int signalNumber)
	{
		if (GLogger != nullptr)
		{
			WCHAR text[128];
			::swprintf_s(text, L"[FATAL] signal raised : %d", signalNumber);
			GLogger->WriteFatalDirect(text);
		}

		CrashDump::WriteDumpNow(L"Signal");
		::_exit(3);
	}
}

/*---------------
	CrashDump
----------------*/

bool CrashDump::Init(const WCHAR* dumpDir, uint32 dumpTypeFlags)
{
	if (SInitialized.exchange(true))
		return true;

	if (dumpDir != nullptr && dumpDir[0] != L'\0')
		::wcscpy_s(SDumpDir, dumpDir);

	SDumpType = (dumpTypeFlags == 0) ? DEFAULT_DUMP_TYPE : dumpTypeFlags;

	if (Logger::MakeDirectories(SDumpDir) == false)
	{
		SInitialized.store(false);
		return false;
	}

	// 실행 파일 이름(확장자 제외)을 미리 뽑아둔다
	WCHAR modulePath[MAX_PATH] = {};
	if (::GetModuleFileNameW(nullptr, modulePath, MAX_PATH) > 0)
	{
		const WCHAR* fileName = ::wcsrchr(modulePath, L'\\');
		fileName = (fileName != nullptr) ? fileName + 1 : modulePath;

		::wcscpy_s(SExeName, fileName);

		WCHAR* dot = ::wcsrchr(SExeName, L'.');
		if (dot != nullptr)
			*dot = L'\0';
	}

	InstallHandlers();
	return true;
}

bool CrashDump::IsInitialized()
{
	return SInitialized.load();
}

void CrashDump::InstallHandlers()
{
	SPrevFilter = ::SetUnhandledExceptionFilter(UnhandledHandler);

	::_set_invalid_parameter_handler(InvalidParameterHandler);
	::_set_purecall_handler(PureCallHandler);
	std::set_terminate(TerminateHandler);
	::signal(SIGABRT, SignalHandler);
	// SIGSEGV는 일부러 잡지 않는다.
	// CRT가 접근 위반을 SIGSEGV로 번역해 버리면
	// EXCEPTION_POINTERS가 없는 반쪽짜리 덤프만 남기 때문에
	// SetUnhandledExceptionFilter 경로로 그대로 흘려보낸다.

#ifdef _DEBUG
	// 어설션 팝업 대신 그대로 크래시 경로를 타도록 한다
	// (Release에서는 _CrtSetReportMode 자체가 매크로라 :: 한정자를 붙일 수 없다)
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif
}

void CrashDump::OnFatal(const char* cause, const char* file, int32 line)
{
	// 여기서는 로그만 남긴다.
	// 호출부(CRASH 매크로)가 곧바로 널 역참조를 일으키고,
	// 그 예외를 UnhandledHandler가 받아 EXCEPTION_POINTERS와 함께 덤프를 뜬다.
	::swprintf_s(SPendingCause, L"%hs (%hs:%d)",
		cause ? cause : "unknown", file ? file : "?", line);

	if (GLogger != nullptr)
	{
		WCHAR text[640];
		::swprintf_s(text, L"[FATAL][T%02u] CRASH : %s", LThreadId, SPendingCause);
		GLogger->WriteFatalDirect(text);
		GLogger->Flush(1000);
	}
}

LONG WINAPI CrashDump::UnhandledHandler(EXCEPTION_POINTERS* exceptionPointers)
{
	WCHAR reason[128];
	if (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr)
	{
		::swprintf_s(reason, L"Exception_0x%08X", exceptionPointers->ExceptionRecord->ExceptionCode);
	}
	else
	{
		::wcscpy_s(reason, L"Unhandled");
	}

	if (GLogger != nullptr)
	{
		WCHAR text[768];
		if (SPendingCause[0] != L'\0')
			::swprintf_s(text, L"[FATAL] Unhandled exception (%s), pending cause : %s", reason, SPendingCause);
		else
			::swprintf_s(text, L"[FATAL] Unhandled exception (%s)", reason);

		GLogger->WriteFatalDirect(text);
	}

	WriteDump(exceptionPointers, reason, true);

	if (SPrevFilter != nullptr)
		return SPrevFilter(exceptionPointers);

	return EXCEPTION_EXECUTE_HANDLER;
}

bool CrashDump::WriteDumpNow(const WCHAR* reason)
{
	return WriteDump(nullptr, (reason != nullptr) ? reason : L"Manual", false);
}

void CrashDump::MakeDumpBaseName(OUT WCHAR* buffer, int32 count)
{
	SYSTEMTIME now = {};
	::GetLocalTime(OUT &now);

	::swprintf_s(buffer, count, L"%s\\%s_%04d%02d%02d_%02d%02d%02d_%u",
		SDumpDir, SExeName,
		now.wYear, now.wMonth, now.wDay,
		now.wHour, now.wMinute, now.wSecond,
		::GetCurrentProcessId());
}

bool CrashDump::WriteDump(EXCEPTION_POINTERS* exceptionPointers, const WCHAR* reason, bool oneShot)
{
	if (SInitialized.load() == false)
		return false;

	// 여러 스레드가 동시에 죽는 경우 첫 번째 것만 기록한다
	if (oneShot && ::InterlockedExchange(&SDumpWritten, 1) == 1)
		return false;

	WCHAR baseName[MAX_PATH];
	MakeDumpBaseName(OUT baseName, MAX_PATH);

	// 덤프와 직전 로그를 항상 짝으로 남긴다
	WCHAR logPath[MAX_PATH];
	::swprintf_s(logPath, L"%s.log", baseName);
	if (GLogger != nullptr)
		GLogger->DumpRingBuffer(logPath);

	WCHAR dumpPath[MAX_PATH];
	::swprintf_s(dumpPath, L"%s.dmp", baseName);

	HANDLE file = ::CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {};
	exceptionInfo.ThreadId = ::GetCurrentThreadId();
	exceptionInfo.ExceptionPointers = exceptionPointers;
	exceptionInfo.ClientPointers = FALSE;

	const BOOL result = ::MiniDumpWriteDump(
		::GetCurrentProcess(),
		::GetCurrentProcessId(),
		file,
		static_cast<MINIDUMP_TYPE>(SDumpType),
		(exceptionPointers != nullptr) ? &exceptionInfo : nullptr,
		nullptr,
		nullptr);

	::CloseHandle(file);

	if (result)
	{
		::wcscpy_s(SLastDumpPath, dumpPath);

		if (GLogger != nullptr)
		{
			WCHAR text[MAX_PATH + 128];
			::swprintf_s(text, L"[FATAL] MiniDump written (%s) : %s", reason, dumpPath);
			GLogger->WriteFatalDirect(text);
		}
	}

	return result != FALSE;
}

const WCHAR* CrashDump::GetDumpDir()
{
	return SDumpDir;
}

const WCHAR* CrashDump::GetLastDumpPath()
{
	return SLastDumpPath;
}
