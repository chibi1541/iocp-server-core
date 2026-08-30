#include "pch.h"
#include "CoreGlobal.h"
#include "ThreadManager.h"
#include "Memory.h"
#include "DeadLockProfiler.h"
#include "SocketUtils.h"
#include "SendBuffer.h"
#include "GlobalQueue.h"
#include "JobTimer.h"
#include "DBConnectionPool.h"
#include "Logger.h"
#include "ServerStats.h"
#include "CommandRegistry.h"
#include "ConsoleCommand.h"

ThreadManager*		GThreadManager = nullptr;
Memory*				GMemory = nullptr;
DeadLockProfiler*	GDeadLockProfiler = nullptr;
SendBufferManager*	GSendBufferManager = nullptr;
GlobalQueue*		GGlobalQueue = nullptr;
JobTimer*			GJobTimer = nullptr;
DBConnectionPool*	GDBConnectionPool = nullptr;
ConsoleLog*			GConsoleLogger = nullptr;
Logger*				GLogger = nullptr;
ServerStats*		GServerStats = nullptr;
CommandRegistry*	GCommandRegistry = nullptr;
ConsoleCommand*		GConsoleCommand = nullptr;


class CoreGlobal
{
public:
	CoreGlobal()
	{
		GThreadManager = new ThreadManager();
		GMemory = new Memory();
		GDeadLockProfiler = new DeadLockProfiler();
		GSendBufferManager = new SendBufferManager();
		GGlobalQueue = new GlobalQueue();
		GJobTimer = new JobTimer();
		GDBConnectionPool = new DBConnectionPool();
		GConsoleLogger = new ConsoleLog();

		// GLogger depends on GConsoleLogger (console sink), so it must come after it.
		GLogger = new Logger();
		GServerStats = new ServerStats();
		GCommandRegistry = new CommandRegistry();
		GCommandRegistry->RegisterBuiltins();
		GConsoleCommand = new ConsoleCommand();

		SocketUtils::Init();
	}

	~CoreGlobal()
	{
		// 0) Retire the deadlock profiler first. Everything released below runs
		//    during static destruction, where the thread_local it relies on
		//    (LLockStack) may already be gone.
		delete GDeadLockProfiler;	GDeadLockProfiler = nullptr;

		// 1) Command inputs first - they can dispatch work into everything else.
		delete GConsoleCommand;		GConsoleCommand = nullptr;
		delete GCommandRegistry;	GCommandRegistry = nullptr;

		// 2) Worker threads (their teardown may still log).
		delete GThreadManager;		GThreadManager = nullptr;

		// Everything holding StlAllocator-backed containers goes first,
		// and GMemory only after them.
		delete GSendBufferManager;	GSendBufferManager = nullptr;
		delete GGlobalQueue;		GGlobalQueue = nullptr;
		delete GJobTimer;			GJobTimer = nullptr;
		delete GDBConnectionPool;	GDBConnectionPool = nullptr;
		delete GMemory;				GMemory = nullptr;

		delete GServerStats;		GServerStats = nullptr;

		// 3) Logger drains its queue on destruction, so it must go before its
		//    console sink (GConsoleLogger).
		delete GLogger;				GLogger = nullptr;
		delete GConsoleLogger;		GConsoleLogger = nullptr;

		SocketUtils::Clear();
	}
} GCoreGlobal;