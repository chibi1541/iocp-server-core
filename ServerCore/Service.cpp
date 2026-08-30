#include "pch.h"
#include "Service.h"
#include "Session.h"
#include "Listener.h"
#include "ServerStats.h"

/*-------------
	Service
--------------*/

Service::Service(ServiceType type, NetAddress address, IocpCoreRef core, SessionFactory sessionFactory, int32 maxSessionCount /*= 1*/)
	:_type(type), _netAddress(address), _iocpCore(core), _sessionFactory(sessionFactory), _maxSessionCount(maxSessionCount)
{

}

Service::~Service()
{

}

void Service::CloseService()
{
	// TODO

}

SessionRef Service::CreateSession()
{
	SessionRef session = _sessionFactory();
	session->SetService(shared_from_this());
	
	if(_iocpCore->Register(session) == false)
	{
		return nullptr;
	}

	return session;
}

void Service::AddSession(SessionRef session)
{
	WRITE_LOCK;
	_sessionCount++;
	_sessions.insert(session);

	if (GServerStats != nullptr)
		GServerStats->OnAccept();
}

void Service::ReleaseSession(SessionRef session)
{
	WRITE_LOCK;
	ASSERT_CRASH(_sessions.erase(session) != 0);
	_sessionCount--;

	if (GServerStats != nullptr)
		GServerStats->OnDisconnect();
}

void Service::CollectSessions(OUT Vector<SessionRef>& sessions)
{
	READ_LOCK;
	sessions.reserve(_sessions.size());
	for (const auto& session : _sessions)
		sessions.push_back(session);
}

void Service::Broadcast(SendBufferRef sendBuffer)
{
	WRITE_LOCK;
	for (const auto& session : _sessions)
	{
		session->Send(sendBuffer);
	}
}

/*-----------------
	ClientService
------------------*/

ClientService::ClientService(NetAddress targetAddress, IocpCoreRef core, SessionFactory sessionFactory, int32 maxSessionCount /*= 1*/)
	:Service(ServiceType::Client, targetAddress, core, sessionFactory, maxSessionCount)
{

}

bool ClientService::Start()
{
	if(CanStart() == false)
		return false;

	for(int32 i = 0;i < _maxSessionCount; i++)
	{
		SessionRef session = CreateSession();
		if (session->Connect() == false)
			return false;
	}

	return true;
}

/*-----------------
	ServerService
------------------*/

ServerService::ServerService(NetAddress address, IocpCoreRef core, SessionFactory sessionFactory, int32 maxSessionCount /*= 1*/)
	:Service(ServiceType::Server, address, core, sessionFactory, maxSessionCount)
{

}

bool ServerService::Start()
{
	if(CanStart() == false)
		return false;

	_listener = MakeShared<Listener>();
	if(_listener == nullptr)
		return false;

	ServerServiceRef service = static_pointer_cast<ServerService>(shared_from_this());
	if(_listener->StartAccept(service) == false)
		return false;

	return true;
}

void ServerService::CloseService()
{
	// TODO

	Service::CloseService();
}
