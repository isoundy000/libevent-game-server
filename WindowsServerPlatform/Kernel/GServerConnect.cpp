#include "pch.h"
#include "GServerConnect.h"
#include "InternalMessageDefine.h"
#include "log.h"
#include "configManage.h"
#include "Function.h"

//////////////////////////////////////////////////////////////////////////
// CGServerClient
CGServerClient::CGServerClient()
{
	m_pDataLine = NULL;			//共享的dataline对象
	m_pCGServerConnect = NULL;
	m_index = 0;
	m_port = 0;
	memset(m_ip, 0, sizeof(m_ip));
	m_ReConnectCount = 0;
	Clear();
}

CGServerClient::~CGServerClient()
{

}

bool CGServerClient::Init(CDataLine* pDataLine, CGServerConnect* pCGServerConnect, int index, char ip[24], int port)
{
	if (pDataLine == NULL || pCGServerConnect == NULL || ip == NULL)
	{
		return false;
	}

	m_pDataLine = pDataLine;
	m_pCGServerConnect = pCGServerConnect;
	m_index = index;
	strcpy(m_ip, ip);
	m_port = port;

	return true;
}

bool CGServerClient::Connect()
{
	if (m_isConnected)
	{
		return false;
	}

	SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET)
	{
		return false;
	}

	sockaddr_in svrAddr;

	svrAddr.sin_family = AF_INET;
	svrAddr.sin_addr.s_addr = inet_addr(m_ip);
	svrAddr.sin_port = htons(m_port);

	int ret = 0;
	int optval = 0;
	int optLen = sizeof(int);

	optval = GSERVER_SOCKET_RECV_BUF;
	ret = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)& optval, sizeof(optval));
	if (ret == SOCKET_ERROR)
	{
		ERROR_LOG("setsockopt SO_RCVBUF ERROR");
		return false;
	}

	optval = GSERVER_SOCKET_SEND_BUF;
	ret = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)& optval, sizeof(optval));
	if (ret == SOCKET_ERROR)
	{
		ERROR_LOG("setsockopt SO_SNDBUF ERROR");
		return false;
	}

	ret = connect(sock, (sockaddr*)& svrAddr, sizeof(sockaddr_in));
	if (ret == 0)
	{
		m_isConnected = true;
		m_ReConnectCount = 0;
		m_socket = sock;

		//发送认证消息
		PlatformLogonServerVerify msg;
		msg.roomID = m_pCGServerConnect->GetRoomID();
		memcpy(msg.passwd, ConfigManage()->m_loaderServerConfig.logonserverPasswd, sizeof(msg.passwd));
		m_pCGServerConnect->SendData(m_index, &msg, sizeof(msg), COMMON_VERIFY_MESSAGE, 0, 0, 0);

		return true;
	}

	ERROR_LOG("连接登陆服务器失败，稍后会重连。roomID=%d,ip=%s,port=%d", m_pCGServerConnect->GetRoomID(), m_ip, m_port);

	return false;
}

bool CGServerClient::OnRead()
{
	//CSignedLockObject lock(&m_lock, true);

	if (m_isConnected == false || m_socket == INVALID_SOCKET)
	{
		return false;
	}

	if (!m_pDataLine)
	{
		return false;
	}

	// 接受数据
	int recvBytes = recv(m_socket, m_recvBuf + m_remainRecvBytes, sizeof(m_recvBuf) - m_remainRecvBytes, 0);
	if (recvBytes <= 0)
	{
		return OnClose();
	}

	m_remainRecvBytes += recvBytes;

	NetMessageHead* pHead = (NetMessageHead*)m_recvBuf;
	if (!pHead)
	{
		// 这里不太可能
		return false;
	}

	// 处理数据
	while (m_remainRecvBytes >= sizeof(NetMessageHead) && m_remainRecvBytes >= (int)pHead->uMessageSize)
	{
		UINT messageSize = pHead->uMessageSize;
		int realSize = messageSize - sizeof(NetMessageHead);
		void* pData = NULL;
		if (realSize < 0)
		{
			ERROR_LOG("包头字节大小错误。已丢弃：%d字节，收到数据：%d字节", m_remainRecvBytes, recvBytes);

			// 丢包
			m_remainRecvBytes = 0;
			return false;
		}

		if (realSize > 0)
		{
			// 没数据就为NULL
			pData = (void*)(m_recvBuf + sizeof(NetMessageHead));
		}

		if (pHead->uMainID != MDM_CONNECT)	// 过滤掉连接测试消息
		{
			SocketReadLine msg;

			msg.uHandleSize = realSize;
			msg.uIndex = m_index;
			msg.dwHandleID = 0;
			msg.uAccessIP = 0;
			msg.NetMessageHead = *pHead;

			unsigned int addBytes = m_pDataLine->AddData(&msg.LineHead, sizeof(SocketReadLine), HD_SOCKET_READ, pData, realSize);

			if (addBytes == 0)
			{
				ERROR_LOG("投递消息失败,size=%d", realSize);
			}
		}

		MoveMemory(m_recvBuf, m_recvBuf + messageSize, m_remainRecvBytes - messageSize);
		m_remainRecvBytes -= messageSize;
	}

	return true;
}

bool CGServerClient::Send(const void* pData, int size)
{
	//CSignedLockObject lock(&m_lock, true);

	if (!pData || size <= 0)
	{
		return false;
	}

	// 检查是否有滞留数据
	int bytes = 0;
	while (m_remainSendBytes > 0)
	{
		bytes = send(m_socket, m_sendBuf, m_remainSendBytes, 0);
		if (bytes == 0)
		{
			return false;
		}

		if (bytes < 0)
		{
			break;
		}

		memmove(m_sendBuf, m_sendBuf + bytes, m_remainSendBytes - bytes);
		m_remainSendBytes -= bytes;
	}

	if (m_remainSendBytes + size >= sizeof(m_sendBuf))
	{
		return false;
	}

	memcpy(m_sendBuf + m_remainSendBytes, pData, size);
	m_remainSendBytes += size;

	// 就只发送一次吧
	bytes = send(m_socket, m_sendBuf, m_remainSendBytes, 0);
	if (bytes > 0)
	{
		memmove(m_sendBuf, m_sendBuf + bytes, m_remainSendBytes - bytes);
		m_remainSendBytes -= bytes;
	}

	return true;
}

bool CGServerClient::OnClose()
{
	//CSignedLockObject lock(&m_lock, true);

	closesocket(m_socket);

	Clear();

	return true;
}

bool CGServerClient::IsConnected()
{
	if (m_socket != INVALID_SOCKET && m_isConnected == true)
	{
		return true;
	}

	return false;
}

void CGServerClient::Clear()
{
	m_socket = INVALID_SOCKET;
	m_isConnected = false;

	memset(m_recvBuf, 0, sizeof(m_recvBuf));
	m_remainRecvBytes = 0;

	memset(m_sendBuf, 0, sizeof(m_sendBuf));
	m_remainSendBytes = 0;
}

bool CGServerClient::IsNeedReConnect()
{
	if (MAX_RECONNECT_COUNT <= 0)
	{
		return true;
	}

	if (m_ReConnectCount > MAX_RECONNECT_COUNT)
	{
		return false;
	}

	return true;
}

bool CGServerClient::ReConnect()
{
	//CSignedLockObject lock(&m_lock, true);

	if (m_isConnected)
	{
		return false;
	}

	closesocket(m_socket);

	Clear();

	Connect();

	m_ReConnectCount++;

	return true;
}

//////////////////////////////////////////////////////////////////////////
//CGServerConnect

CGServerConnect::CGServerConnect()
{
	m_pDataLine = NULL;
	m_roomID = 0;
	m_running = false;
	m_socketVec.clear();
	m_hEventThread = NULL;
	m_hThreadCheckConnect = NULL;
}

CGServerConnect::~CGServerConnect()
{

}

bool CGServerConnect::Start(CDataLine* pDataLine, int roomID)
{
	INFO_LOG("service CGServerConnect start begin...");

	if (pDataLine == NULL || roomID <= 0)
	{
		ERROR_LOG("pDataLine == NULL || roomID <= 0");
		return false;
	}

	if (m_running == true)
	{
		ERROR_LOG("service CGServerConnect already have been running");
		return false;
	}

	m_running = true;
	m_pDataLine = pDataLine;
	m_roomID = roomID;

	//建立事件
	m_hEventThread = ::CreateEvent(NULL, TRUE, false, NULL);

	// 分配内存
	m_socketVec.clear();
	int iSocketIndex = 0;
	for (auto itr = ConfigManage()->m_logonBaseInfoMap.begin(); itr != ConfigManage()->m_logonBaseInfoMap.end(); itr++)
	{
		CGServerClient* pGServerClient = new CGServerClient;
		if (!pGServerClient)
		{
			ERROR_LOG("alloc a pGServerClient object failed");
			return false;
		}
		pGServerClient->Init(pDataLine, this, iSocketIndex, itr->second.intranetIP, itr->second.port);
		m_socketVec.push_back(pGServerClient);
		iSocketIndex++;
	}

	// 获取线程数量参数
	int recvThreadNumber = ConfigManage()->m_loaderServerConfig.recvThreadNumber;
	if (recvThreadNumber < 1)
	{
		//获取系统信息
		SYSTEM_INFO sysInfo;
		GetSystemInfo(&sysInfo);

		recvThreadNumber = sysInfo.dwNumberOfProcessors * 2;
	}
	recvThreadNumber = min(recvThreadNumber, (int)m_socketVec.size());

	// 建立检测连接线程
	unsigned int connectThreadID = 0;
	m_hThreadCheckConnect = (HANDLE)_beginthreadex(NULL, 0, ThreadCheckConnect, this, 0, &connectThreadID);
	if (!m_hThreadCheckConnect)
	{
		ERROR_LOG("begin ThreadCheckConnect failed");
		return false;
	}

	GameLogManage()->AddLogFile(connectThreadID, THREAD_TYPE_ACCEPT, roomID);

	::WaitForSingleObject(m_hEventThread, INFINITE);
	::ResetEvent(m_hEventThread);

	// 建立收发数据线程
	m_threadIDToIndexMap.clear();
	for (int i = 0; i < recvThreadNumber; i++)
	{
		unsigned int threadID = 0;
		HANDLE hThreadHandle = (HANDLE)_beginthreadex(NULL, 0, ThreadRSSocket, this, 0, &threadID);
		if (!hThreadHandle)
		{
			ERROR_LOG("begin ThreadRSSocket failed");
			return false;
		}

		m_threadIDToIndexMap.insert(std::make_pair(threadID, i));
		GameLogManage()->AddLogFile(threadID, THREAD_TYPE_RECV, roomID);

		WaitForSingleObject(m_hEventThread, INFINITE);
		ResetEvent(m_hEventThread);
		CloseHandle(hThreadHandle);
	}

	INFO_LOG("service CGServerConnect start end");

	return true;
}

bool CGServerConnect::Stop()
{
	INFO_LOG("service CGServerConnect stop begin...");

	if (!m_running)
	{
		ERROR_LOG("CGServerConnect is not running......");
		return false;
	}

	m_running = false;

	//关闭 SOCKET
	for (size_t i = 0; i < m_socketVec.size(); i++)
	{
		CGServerClient* pGServerClient = m_socketVec[i];
		if (pGServerClient)
		{
			pGServerClient->OnClose();
		}
	}

	//关闭事件
	if (m_hEventThread)
	{
		CloseHandle(m_hEventThread);
		m_hEventThread = NULL;
	}

	// 关闭检测连接线程句柄（强杀线程）
	if ((m_hThreadCheckConnect != NULL) && (::WaitForSingleObject(m_hThreadCheckConnect, 50) == WAIT_TIMEOUT))
	{
		TerminateThread(m_hThreadCheckConnect, 0);
		CloseHandle(m_hThreadCheckConnect);
		m_hThreadCheckConnect = NULL;
	}

	// 释放内存
	for (size_t i = 0; i < m_socketVec.size(); i++)
	{
		SAFE_DELETE(m_socketVec[i]);
	}
	m_socketVec.clear();

	INFO_LOG("service CGServerConnect stop end...");

	return true;

}

bool CGServerConnect::SendData(int idx, void* pData, int size, int mainID, int assistID, int handleCode, unsigned int uIdentification)
{
	if (idx < 0 || idx >= (int)m_socketVec.size())
	{
		ERROR_LOG("invalid socketIdx, idx=%d, mainID=%d assistID=%d", idx, mainID, assistID);
		return false;
	}

	if (size < 0 || size > MAX_TEMP_SENDBUF_SIZE - sizeof(NetMessageHead))
	{
		ERROR_LOG("invalid message size size=%d", size);
		return false;
	}

	CGServerClient* pTcpSocket = m_socketVec[idx];
	if (!pTcpSocket)
	{
		ERROR_LOG("get pTcpSocket failed, idx=%d", idx);
		return false;
	}

	// 整合一下数据
	char buf[MAX_TEMP_SENDBUF_SIZE] = "";

	int pos = 0;

	// 拼接包头
	NetMessageHead* pHead = (NetMessageHead*)buf;
	pHead->uMainID = mainID;
	pHead->uAssistantID = assistID;
	pHead->uMessageSize = sizeof(NetMessageHead) + size;
	pHead->uHandleCode = handleCode;
	pHead->uIdentification = uIdentification;

	pos += sizeof(NetMessageHead);

	// 包体
	if (pData && size > 0)
	{
		memcpy(buf + pos, pData, size);
		pos += size;
	}

	// 交给具体的socket
	pTcpSocket->Send(buf, pos);

	return true;
}

int CGServerConnect::GetRoomID()
{
	return m_roomID;
}

const std::vector<CGServerClient*>& CGServerConnect::GetSocketVec()
{
	return m_socketVec;
}

void CGServerConnect::GetIndexByThreadID(unsigned int uThreadID, size_t& uMin, size_t& uMax)
{
	// 负载均衡算法：

	// 初始化
	uMin = 0;
	uMax = 0;

	auto itr = m_threadIDToIndexMap.find(uThreadID);
	if (itr == m_threadIDToIndexMap.end())
	{
		return;
	}

	int iIndex = itr->second;
	int iCount = m_socketVec.size() / m_threadIDToIndexMap.size();

	uMin = iIndex * iCount;
	uMax = (iIndex + 1) * iCount;

	if (iIndex == m_threadIDToIndexMap.size() - 1)
	{
		uMax = m_socketVec.size();
	}
}

unsigned __stdcall CGServerConnect::ThreadCheckConnect(LPVOID pThreadData)
{
	Sleep(1);

	INFO_LOG("ThreadCheckConnect thread begin...");

	CGServerConnect* pThis = (CGServerConnect*)pThreadData;
	if (!pThis)
	{
		return -1;
	}

	// 通知主线程读取线程参数完成
	SetEvent(pThis->m_hEventThread);

	while (true)
	{
		if (!pThis->m_running)
		{
			break;
		}

		// 获取socket数组
		const std::vector<CGServerClient*>& vecSocket = pThis->GetSocketVec();

		try
		{
			// 检测连接
			for (size_t i = 0; i < vecSocket.size(); i++)
			{
				CGServerClient* pGServerClient = vecSocket[i];
				if (!pGServerClient)
				{
					continue;
				}

				if (!pGServerClient->IsConnected() && pGServerClient->IsNeedReConnect())
				{
					pGServerClient->ReConnect();
				}
			}
		}
		catch (...)
		{
			ERROR_LOG("CATCH:%s with %s\n", __FILE__, __FUNCTION__);
			return 0;
		}

		// 过一段时间执行一次
		Sleep(6000);
	}

	INFO_LOG("ThreadCheckConnect thread exit.");

	return 0;
}

unsigned __stdcall CGServerConnect::ThreadRSSocket(LPVOID pThreadData)
{
	INFO_LOG("CGServerConnect::ThreadRSSocket thread begin...");

	CGServerConnect* pThis = (CGServerConnect*)pThreadData;
	if (!pThis)
	{
		return -1;
	}

	// 通知主线程读取线程参数完成
	SetEvent(pThis->m_hEventThread);

	Sleep(1500);

	unsigned int threadID = GetCurrentThreadId();
	size_t uMinIndex = 0, uMaxIndex = 0;
	pThis->GetIndexByThreadID(threadID, uMinIndex, uMaxIndex);

	if (uMinIndex == uMaxIndex)
	{
		INFO_LOG("登录服数量过少，当前线程（%d）不需要参与收发数据", threadID);
		return 0;
	}

	INFO_LOG("监听索引 min=%d,max=%d", uMinIndex, uMaxIndex);

	// 设置select超时时间
	timeval selectTv = { 3, 0 };

	// 获取socket数组
	const std::vector<CGServerClient*>& vecSocket = pThis->GetSocketVec();

	while (pThis->m_running == true)
	{
		try
		{
			fd_set fdRead;
			FD_ZERO(&fdRead);

			// 将socket加入数组
			for (size_t i = uMinIndex; i < uMaxIndex && i < vecSocket.size(); i++)
			{
				CGServerClient* pGServerClient = vecSocket[i];
				if (pGServerClient && pGServerClient->IsConnected())
				{
					FD_SET(pGServerClient->GetSocket(), &fdRead);
				}
			}

			// 没有任何socket
			if (fdRead.fd_count == 0)
			{
				Sleep(500);
				continue;
			}

			int ret = select(0, &fdRead, NULL, NULL, NULL);
			if (ret == SOCKET_ERROR)
			{
				//输出错误消息
				ERROR_LOG("##### CGServerConnect::ThreadRSSocket select error,thread Exit.WSAGetLastError =%d #####", WSAGetLastError());
				continue;
			}

			if (ret == 0)
			{
				// 正常
				continue;
			}

			// 读写数据
			for (size_t i = uMinIndex; i < uMaxIndex && i < vecSocket.size(); i++)
			{
				CGServerClient* pGServerClient = vecSocket[i];
				if (!pGServerClient)
				{
					continue;
				}

				if (FD_ISSET(pGServerClient->GetSocket(), &fdRead))
				{
					pGServerClient->OnRead();
				}
			}
		}
		catch (...)
		{
			ERROR_LOG("CATCH:%s with %s\n", __FILE__, __FUNCTION__);
		}
	}

	INFO_LOG("CGServerConnect::ThreadRSSocket thread Exit...");

	return 0;
}