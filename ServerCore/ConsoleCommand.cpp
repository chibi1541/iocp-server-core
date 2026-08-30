#include "pch.h"
#include "ConsoleCommand.h"
#include "CommandRegistry.h"
#include "Logger.h"
#include "ThreadManager.h"
#include <iostream>

ConsoleCommand::~ConsoleCommand()
{
	Stop();
}

bool ConsoleCommand::Start(const WCHAR* prompt)
{
	if (_started.exchange(true))
		return true;

	const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
	if (input == nullptr || input == INVALID_HANDLE_VALUE)
	{
		// stdin이 없는 환경(서비스/백그라운드)에서는 조용히 포기한다
		_started.store(false);
		return false;
	}

	if (prompt != nullptr)
		_prompt = prompt;

	_running.store(true);
	_loopExited.store(false);
	_thread = std::thread([this]() { InputLoop(); });
	return true;
}

void ConsoleCommand::Stop()
{
	if (_started.exchange(false) == false)
		return;

	_running.store(false);

	if (_thread.joinable() == false)
		return;

	// ReadConsoleW / getline은 블로킹이다. 먼저 깨워본다.
	const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
	if (input != nullptr && input != INVALID_HANDLE_VALUE)
		::CancelIoEx(input, nullptr);

	if (_loopExited.load())
	{
		// stdin이 EOF라 루프가 스스로 끝난 경우
		_thread.join();
	}
	else
	{
		// 아직 stdin에서 블로킹 중이면 붙잡고 있을 수 없다.
		// 프로세스 종료 시점이므로 떼어낸다.
		_thread.detach();
	}
}

void ConsoleCommand::InputLoop()
{
	// Lock / LockQueue 계열이 LThreadId를 사용하므로 반드시 부여해야 한다
	ThreadManager::InitTLS();

	const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);

	DWORD consoleMode = 0;
	const bool isConsole = (::GetConsoleMode(input, OUT & consoleMode) != FALSE);

	if (GLogger != nullptr)
		GLogger->WriteRaw(L"[console] command input ready. type 'help'.");

	CommandReply reply = [](const std::wstring& text)
	{
		// 명령 응답은 레벨 필터를 타면 안 되므로 WriteRaw로 나간다.
		// (로깅 스레드를 거치므로 로그 출력과 섞이지 않는다)
		if (GLogger != nullptr)
			GLogger->WriteRaw(L"%s", text.c_str());
		else
			std::wcout << text << std::endl;
	};

	WCHAR buffer[1024];

	while (_running.load())
	{
		std::wstring line;

		if (isConsole)
		{
			if (_prompt.empty() == false && GConsoleLogger != nullptr)
				GConsoleLogger->WriteStdOut(Color::WHITE, L"%s", _prompt.c_str());

			DWORD read = 0;
			if (::ReadConsoleW(input, buffer, static_cast<DWORD>(std::size(buffer) - 1), OUT & read, nullptr) == FALSE)
				break;

			if (read == 0)
				continue;

			buffer[read] = L'\0';
			line = buffer;
		}
		else
		{
			if (!std::getline(std::wcin, line))
				break;
		}

		// 개행 정리
		while (line.empty() == false && (line.back() == L'\r' || line.back() == L'\n'))
			line.pop_back();

		if (_running.load() == false)
			break;

		if (line.empty())
			continue;

		if (GCommandRegistry != nullptr)
			GCommandRegistry->Execute(line, reply, false);
	}

	_running.store(false);
	_loopExited.store(true);
}
