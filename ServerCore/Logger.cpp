#include "pch.h"
#include "Logger.h"
#include <cstdarg>
#include <cwchar>
#include <chrono>
#include <share.h>

/*---------------
	Logger
----------------*/

Logger::Logger()
{
	_ring.resize(RING_CAPACITY);

#ifdef _DEBUG
	_fileLevel.store(LogLevel::Debug);
	_consoleLevel.store(LogLevel::Debug);
#endif

	_running.store(true);
	_worker = std::thread([this]() { WorkerLoop(); });
}

Logger::~Logger()
{
	Shutdown();
}

bool Logger::Init(const WCHAR* dir, const WCHAR* prefix)
{
	if (dir == nullptr || prefix == nullptr)
		return false;

	if (MakeDirectories(dir) == false)
		return false;

	{
		std::lock_guard<std::mutex> guard(_queueMutex);
		_dir = dir;
		_prefix = prefix;
		_fileEnabled = true;
			_fileDate = 0;	// 다음 기록 때 강제로 롤링(= 파일 오픈)
	}

	// 크래시 핸들러에서 쓸 경로를 미리 만들어 둔다 (핸들러 내부 힙 할당 금지)
	::swprintf_s(_fatalFilePath, L"%s\\%s_fatal.log", dir, prefix);

	_queueCv.notify_one();
	return true;
}

void Logger::Shutdown()
{
	if (_shutdownDone.exchange(true))
		return;

	{
		std::lock_guard<std::mutex> guard(_queueMutex);
		_stopRequested = true;
	}
	_queueCv.notify_all();

	if (_worker.joinable())
		_worker.join();

	_running.store(false);

	if (_file != nullptr)
	{
		::fflush(_file);
		::fclose(_file);
		_file = nullptr;
	}
}

bool Logger::IsEnabled(LogLevel level) const
{
	return level >= _fileLevel.load() || level >= _consoleLevel.load();
}

void Logger::Write(LogLevel level, const char* file, int32 line, const WCHAR* format, ...)
{
	if (format == nullptr || _running.load() == false)
		return;

	if (IsEnabled(level) == false)
		return;

	WCHAR buffer[MAX_MESSAGE_LEN];

	va_list ap;
	va_start(ap, format);
	const int32 written = ::_vsnwprintf_s(buffer, MAX_MESSAGE_LEN, _TRUNCATE, format, ap);
	va_end(ap);

	if (written < 0)
		buffer[MAX_MESSAGE_LEN - 1] = L'\0';

	Entry entry;
	entry.level = level;
	entry.threadId = LThreadId;
	entry.file = BaseName(file);
	entry.line = line;
	entry.message = buffer;
	::GetLocalTime(OUT &entry.time);

	Enqueue(std::move(entry));
}

void Logger::WriteRaw(const WCHAR* format, ...)
{
	if (format == nullptr || _running.load() == false)
		return;

	WCHAR buffer[MAX_MESSAGE_LEN];

	va_list ap;
	va_start(ap, format);
	const int32 written = ::_vsnwprintf_s(buffer, MAX_MESSAGE_LEN, _TRUNCATE, format, ap);
	va_end(ap);

	if (written < 0)
		buffer[MAX_MESSAGE_LEN - 1] = L'\0';

	Entry entry;
	entry.level = LogLevel::Info;
	entry.threadId = LThreadId;
	entry.raw = true;
	entry.message = buffer;
	::GetLocalTime(OUT &entry.time);

	Enqueue(std::move(entry));
}

void Logger::Enqueue(Entry&& entry)
{
	{
		std::lock_guard<std::mutex> guard(_queueMutex);
		_queue.push_back(std::move(entry));
		_pushedCount++;
	}
	_queueCv.notify_one();
}

void Logger::Flush(uint32 timeoutMs)
{
	if (_shutdownDone.load())
		return;

	uint64 target = 0;
	{
		std::lock_guard<std::mutex> guard(_queueMutex);
		target = _pushedCount;
	}

	_queueCv.notify_all();

	std::unique_lock<std::mutex> lock(_queueMutex);
	_drainCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
		[this, target]() { return _consumedCount >= target; });
}

void Logger::WorkerLoop()
{
	std::deque<Entry> batch;

	while (true)
	{
		bool stopping = false;

		{
			std::unique_lock<std::mutex> lock(_queueMutex);
			_queueCv.wait(lock, [this]() { return _queue.empty() == false || _stopRequested; });

			batch.swap(_queue);
			stopping = _stopRequested;
		}

		for (Entry& entry : batch)
			Consume(entry);

		const size_t consumed = batch.size();
		batch.clear();

		if (_file != nullptr)
			::fflush(_file);

		{
			std::lock_guard<std::mutex> guard(_queueMutex);
			_consumedCount += consumed;
		}
		_drainCv.notify_all();

		if (stopping)
			break;
	}
}

void Logger::Consume(Entry& entry)
{
	const std::wstring line = FormatLine(entry);

	PushRing(line);

	if (entry.raw || entry.level >= _fileLevel.load())
	{
		RollFileIfNeeded(entry.time);
		WriteToFile(line);
	}

	if (entry.raw || entry.level >= _consoleLevel.load())
		WriteToConsole(entry.level, line);
}

std::wstring Logger::FormatLine(const Entry& entry) const
{
	WCHAR buffer[MAX_LINE_LEN];

	if (entry.raw)
		return entry.message;

	if (entry.file != nullptr)
	{
		::swprintf_s(buffer,
			L"[%04d-%02d-%02d %02d:%02d:%02d.%03d][%-5s][T%02u][%hs:%d] %s",
			entry.time.wYear, entry.time.wMonth, entry.time.wDay,
			entry.time.wHour, entry.time.wMinute, entry.time.wSecond, entry.time.wMilliseconds,
			LevelToString(entry.level), entry.threadId,
			entry.file, entry.line,
			entry.message.c_str());
	}
	else
	{
		::swprintf_s(buffer,
			L"[%04d-%02d-%02d %02d:%02d:%02d.%03d][%-5s][T%02u] %s",
			entry.time.wYear, entry.time.wMonth, entry.time.wDay,
			entry.time.wHour, entry.time.wMinute, entry.time.wSecond, entry.time.wMilliseconds,
			LevelToString(entry.level), entry.threadId,
			entry.message.c_str());
	}

	return std::wstring(buffer);
}

void Logger::RollFileIfNeeded(const SYSTEMTIME& now)
{
	if (_fileEnabled == false)
		return;

	const int32 date = now.wYear * 10000 + now.wMonth * 100 + now.wDay;
	if (_file != nullptr && date == _fileDate)
		return;

	if (_file != nullptr)
	{
		::fflush(_file);
		::fclose(_file);
		_file = nullptr;
	}

	WCHAR path[MAX_PATH];
	::swprintf_s(path, L"%s\\%s_%04d%02d%02d.log", _dir.c_str(), _prefix.c_str(), now.wYear, now.wMonth, now.wDay);

	const bool exists = (::GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES);

	// _SH_DENYWR : 다른 프로세스가 tail / 에디터로 열어볼 수 있게 읽기는 허용한다.
	// (_wfopen_s는 _SH_DENYRW라 서버가 돌아가는 동안 로그를 볼 수 없다)
	FILE* file = ::_wfsopen(path, L"ab", _SH_DENYWR);
	if (file == nullptr)
	{
		// 파일을 못 열면 콘솔 전용으로 계속 동작한다 (여기서 로그를 남기면 재진입)
		_fileEnabled = false;
		return;
	}

	if (exists == false)
	{
		const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
		::fwrite(bom, 1, sizeof(bom), file);
	}

	_file = file;
	_fileDate = date;
	_filePath = path;
}

void Logger::WriteToFile(const std::wstring& line)
{
	if (_file == nullptr)
		return;

	const std::string utf8 = ToUtf8(line);
	::fwrite(utf8.data(), 1, utf8.size(), _file);
	::fwrite("\r\n", 1, 2, _file);
}

void Logger::WriteToConsole(LogLevel level, const std::wstring& line)
{
	if (GConsoleLogger == nullptr)
		return;

	Color color = Color::WHITE;
	switch (level)
	{
		case LogLevel::Trace:
		case LogLevel::Debug:	color = Color::WHITE;	break;
		case LogLevel::Info:	color = Color::GREEN;	break;
		case LogLevel::Warn:	color = Color::YELLOW;	break;
		case LogLevel::Error:
		case LogLevel::Fatal:	color = Color::RED;		break;
		default:				color = Color::WHITE;	break;
	}

	if (level == LogLevel::Fatal)
		GConsoleLogger->WriteStdErr(color, L"%s\n", line.c_str());
	else
		GConsoleLogger->WriteStdOut(color, L"%s\n", line.c_str());
}

void Logger::PushRing(const std::wstring& line)
{
	std::lock_guard<std::mutex> guard(_ringMutex);

	_ring[_ringHead] = line;
	_ringHead = (_ringHead + 1) % RING_CAPACITY;
	if (_ringCount < RING_CAPACITY)
		_ringCount++;
}

bool Logger::DumpRingBuffer(const WCHAR* path)
{
	if (path == nullptr)
		return false;

	FILE* file = ::_wfsopen(path, L"wb", _SH_DENYWR);
	if (file == nullptr)
		return false;

	const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
	::fwrite(bom, 1, sizeof(bom), file);

	{
		std::lock_guard<std::mutex> guard(_ringMutex);

		const uint32 start = (_ringCount < RING_CAPACITY) ? 0 : _ringHead;
		for (uint32 i = 0; i < _ringCount; i++)
		{
			const std::wstring& line = _ring[(start + i) % RING_CAPACITY];
			const std::string utf8 = ToUtf8(line);
			::fwrite(utf8.data(), 1, utf8.size(), file);
			::fwrite("\r\n", 1, 2, file);
		}
	}

	::fflush(file);
	::fclose(file);
	return true;
}

void Logger::WriteFatalDirect(const WCHAR* text)
{
	if (text == nullptr)
		return;

	// 크래시 경로. 큐도 워커도 믿을 수 없으므로 동기로 직접 쓴다.
	if (GConsoleLogger != nullptr)
		GConsoleLogger->WriteStdErr(Color::RED, L"%s\n", text);

	const WCHAR* path = (_fatalFilePath[0] != L'\0') ? _fatalFilePath : L"Server_fatal.log";

	FILE* file = ::_wfsopen(path, L"ab", _SH_DENYWR);
	if (file != nullptr)
	{
		static char utf8[MAX_LINE_LEN * 3];
		const int32 len = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
		if (len > 1)
			::fwrite(utf8, 1, static_cast<size_t>(len) - 1, file);
		::fwrite("\r\n", 1, 2, file);
		::fflush(file);
		::fclose(file);
	}
}

std::wstring Logger::GetLogFilePath() const
{
	return _filePath;
}

/*---------------
	static utils
----------------*/

const WCHAR* Logger::LevelToString(LogLevel level)
{
	switch (level)
	{
		case LogLevel::Trace:	return L"TRACE";
		case LogLevel::Debug:	return L"DEBUG";
		case LogLevel::Info:	return L"INFO";
		case LogLevel::Warn:	return L"WARN";
		case LogLevel::Error:	return L"ERROR";
		case LogLevel::Fatal:	return L"FATAL";
		case LogLevel::Off:		return L"OFF";
	}
	return L"?????";
}

bool Logger::ParseLevel(const WCHAR* text, OUT LogLevel& outLevel)
{
	if (text == nullptr)
		return false;

	struct LevelName { const WCHAR* name; LogLevel level; };
	static const LevelName table[] =
	{
		{ L"trace", LogLevel::Trace },
		{ L"debug", LogLevel::Debug },
		{ L"info",  LogLevel::Info  },
		{ L"warn",  LogLevel::Warn  },
		{ L"error", LogLevel::Error },
		{ L"fatal", LogLevel::Fatal },
		{ L"off",   LogLevel::Off   },
	};

	for (const LevelName& row : table)
	{
		if (::_wcsicmp(text, row.name) == 0)
		{
			outLevel = row.level;
			return true;
		}
	}

	return false;
}

bool Logger::MakeDirectories(const WCHAR* path)
{
	if (path == nullptr || path[0] == L'\0')
		return false;

	WCHAR buffer[MAX_PATH];
	::wcscpy_s(buffer, path);

	for (WCHAR* cursor = buffer; *cursor != L'\0'; cursor++)
	{
		if (*cursor != L'\\' && *cursor != L'/')
			continue;

		const WCHAR saved = *cursor;
		*cursor = L'\0';
		if (buffer[0] != L'\0')
			::CreateDirectoryW(buffer, nullptr);
		*cursor = saved;
	}

	::CreateDirectoryW(buffer, nullptr);

	const DWORD attr = ::GetFileAttributesW(buffer);
	return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Logger::ToUtf8(const std::wstring& text)
{
	if (text.empty())
		return std::string();

	const int32 len = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (len <= 0)
		return std::string();

	std::string result(static_cast<size_t>(len), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), len, nullptr, nullptr);
	return result;
}

const char* Logger::BaseName(const char* path)
{
	if (path == nullptr)
		return nullptr;

	const char* result = path;
	for (const char* cursor = path; *cursor != '\0'; cursor++)
	{
		if (*cursor == '\\' || *cursor == '/')
			result = cursor + 1;
	}

	return result;
}
