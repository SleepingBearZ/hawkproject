#include "HawkGateway.h"
#include "HawkGateSecurity.h"
#include "event.h"
#include "zmq.h"

namespace Hawk
{
	HawkGateway::HawkGateway()
	{
		m_iThread		= 0;
		m_iTurnIdx		= 0;
		m_iBaseSid		= 0;
		m_ppThread		= 0;
		m_pCrossDomain	= 0;
		m_pProfiler		= 0;
		m_pThreadZmq	= 0;
		m_pServerZmq	= 0;
		m_pMonitorZmq	= 0;
		m_pOctets		= 0;
		m_iTimeout		= 0;	
		m_bIdle			= true;
		m_bBlocking		= true;
		m_bWaitIdle		= false;
		m_bRunning		= false;	
		m_bSecurity		= false;
		m_iBufSize		= PAGE_SIZE * 2;
		m_iGateMode		= GATEMODE_SINGLE_SERVICE;
	}

	HawkGateway::~HawkGateway()
	{
		//�ͷ��߳�
		for (Int32 i=0;m_ppThread && i<m_iThread;i++)
		{
			HAWK_RELEASE(m_ppThread[i]);
		}
		HAWK_DELETE_ARRAY(m_ppThread);

		//�رռ����׽���
		m_sSocket.Close();

		//�ͷ���Ϣ����
		HAWK_RELEASE(m_pOctets);
		
		//�ͷ����ܼ�����
		HAWK_RELEASE(m_pProfiler);

		//�ر���Ϣ����
		P_ZmqManager->CloseZmq(m_pMonitorZmq);
		P_ZmqManager->CloseZmq(m_pServerZmq);
		P_ZmqManager->CloseZmq(m_pThreadZmq);

		//�ͷſ����߳�
		HAWK_RELEASE(m_pCrossDomain);
	}

	void HawkGateway::SetBufSize(Int32 iBufSize)
	{
		m_iBufSize = iBufSize;
	}

	Int32 HawkGateway::GetBufSize() const
	{
		return m_iBufSize;
	}

	Bool HawkGateway::SetWaitIdle(Bool bWaitIdle)
	{
		m_bWaitIdle = bWaitIdle;
		return true;
	}

	Bool HawkGateway::IsWaitIdle() const
	{
		return m_bWaitIdle;
	}

	Bool HawkGateway::IsRunning() const
	{
		return m_bRunning;
	}

	void HawkGateway::SetSessionTimeout(Int32 iTimeout)
	{
		m_iTimeout = iTimeout;
	}

	Int32 HawkGateway::GetSessionTimeout() const
	{
		return m_iTimeout;
	}

	Int32  HawkGateway::GetThreadNum() const
	{
		return m_iThread;
	}

	Int32  HawkGateway::GetThreadId(Int32 iIdx) const
	{
		if (iIdx >= 0 && iIdx < m_iThread && m_ppThread && m_ppThread[iIdx])
			return m_ppThread[iIdx]->GetThreadId();
		
		return 0;
	}

	Bool HawkGateway::SetBlocking(Bool bBlocking)
	{
		m_bBlocking = bBlocking;
		return true;
	}

	Bool HawkGateway::CreateGateThread(HawkGateThread*& pThread)
	{
		pThread = new HawkGateThread(this);
		return pThread != 0;
	}

	Bool HawkGateway::CreateISecurity(HawkSecurity*& pSecurity)
	{
		pSecurity = 0;
		if (m_bSecurity)
			pSecurity = new HawkGateSecurity(HawkGateSecurity::SECURITY_DECODE);

		return true;
	}

	Bool HawkGateway::CreateOSecurity(HawkSecurity*& pSecurity)
	{
		pSecurity = 0;
		if (m_bSecurity)
			pSecurity = new HawkGateSecurity(HawkGateSecurity::SECURITY_ENCODE);

		return true;
	}

	Bool HawkGateway::Init(const AString& sFrontend, const AString& sBackend, Int32 iThread, Int32 iGateMode)
	{
		HawkAssert(sFrontend.size() && sBackend.size() && iThread > 0);

		if (!sFrontend.size() || !sBackend.size() || iThread <= 0)
			return false;

		//������ֵ
		m_iThread   = iThread;
		m_iGateMode = iGateMode;

		//����ͨ�û���
		if (!m_pOctets)
			m_pOctets = new OctetsStream(m_iBufSize);

		//�����������׽���
		if (!m_sSocket.IsValid())
		{
			if (!m_sSocket.InitTcpServer(sFrontend))
			{
				HawkFmtPrint("Init TcpServer Error, Addr: %s, ErrCode: %d", sFrontend.c_str(), m_sSocket.GetSocketErr());
				return false;
			}
		}

		//���������߳�ͨ�Ŷ���
		if (!m_pThreadZmq)
		{
			m_pThreadZmq = P_ZmqManager->CreateZmq(HawkZmq::HZMQ_ROUTER);
			if (!m_pThreadZmq->Bind(GetThreadZmqAddr()))
			{
				HawkFmtPrint("ThreadZmq Bind Error, Addr: %s", GetThreadZmqAddr().c_str());
				return false;
			}
		}

		//������˷���ͨ�Ŷ���
		if (!m_pServerZmq)
		{
			AString sAddr = sBackend;
			if (sAddr.find("tcp://") == AString::npos)
				sAddr = "tcp://" + sBackend;

			m_pServerZmq = P_ZmqManager->CreateZmq(HawkZmq::HZMQ_ROUTER);

			//����ָ���¼�������
			m_pServerZmq->StartMonitor(HawkZmq::HZMQ_EVENT_ACCEPTED | HawkZmq::HZMQ_EVENT_DISCONNECTED, GetServerZmqAddr());
			//�󶨵���ַ
			if (!m_pServerZmq->Bind(sAddr))
			{
				HawkFmtPrint("ServerZmq Bind Error, Addr: %s", sAddr.c_str());
				return false;
			}			
		}
		
		//�������������
		if (!m_pMonitorZmq && m_pServerZmq)
		{
			m_pMonitorZmq = P_ZmqManager->CreateZmq(HawkZmq::HZMQ_PAIR);
			//���ӵ�������
			if (!m_pMonitorZmq->Connect(GetServerZmqAddr()))
			{
				HawkFmtPrint("MonitorZmq Connect Error, Addr: %s", GetServerZmqAddr().c_str());
				return false;
			}
		}

		//���������߳�
		m_ppThread = new HawkGateThread*[m_iThread];
		memset(m_ppThread, 0, sizeof(HawkGateThread*) * m_iThread);

		tm xTM = HawkOSOperator::GetSysClock();
		//ǿ���趨��8λ, �����������»ỰID�ظ�������Э�鵼�����
		m_iBaseSid = (xTM.tm_sec << 24) & 0xFF000000;

		for (UInt32 i=0;i<(UInt32)m_iThread;i++)
		{
			//���������߳�
			if (!CreateGateThread(m_ppThread[i]))
			{
				HawkPrint("Create GateThread Failed.");
				return false;
			}

			//��ʼ�������߳�
			if (!m_ppThread[i]->Init(i+1, m_iBaseSid, (UInt32)m_iThread))
			{
				HawkPrint("Init GateThread Failed.");
				return false;
			}

			//���������߳�
			if (!m_ppThread[i]->Start())
			{
				HawkPrint("Start GateThread Failed.");
				return false;
			}
		}		
		return true;
	}

	Bool HawkGateway::Stop()
	{
		//֪ͨ�˳�ѭ��
		m_bRunning = false;

		//�ȴ����ؽ���
		if (!m_bBlocking)
			OnGatewayClose();

		return true;
	}

	AString HawkGateway::GetThreadZmqAddr() const
	{
		return "inproc://hawk-gateway-thread";
	}

	AString HawkGateway::GetServerZmqAddr() const
	{
		return "inproc://hawk-server-monitor";
	}

	HawkProfiler* HawkGateway::GetProfiler()
	{
		return m_pProfiler;
	}

	Bool HawkGateway::TurnGateSecurity()
	{
		m_bSecurity = true;
		return true;
	}

	Bool HawkGateway::TurnOnProfiler(const AString& sAddr)
	{
		if (!m_pProfiler)
		{
			m_pProfiler = new HawkProfiler(sAddr);			
		}
		return true;
	}

	Bool HawkGateway::Run()
	{
		if (!m_bRunning)
		{
			m_bRunning = true;
			
			if (m_pCrossDomain)
				m_pCrossDomain->Start(this);

			if (m_pProfiler)
				m_pProfiler->Start();
		}
		
		while (m_bRunning)
		{
			//����Ĭ�Ͽ���״̬
			m_bIdle = true;

			//��������������
			OnSessionAccept();

			//���������¼�
			OnGateMonitorEvent();

			//��������߳��¼�
			OnGateThreadEvent();

			//����˷����¼�
			OnGateServerEvent();

			//���θ����¼�
			if (!m_bBlocking)
				return true;

			//���ؿ��д���
			OnGateIdleEvent();
		}

		//�����ȴ�����
		OnGatewayClose();

		return true;
	}		

	Bool HawkGateway::RecvThreadMsg(UInt32& iThread, ThreadMsgHeader& sHeader, OctetsStream* pOctets)
	{
		if (m_pThreadZmq && pOctets)
		{
			//��ȡ�߳�ID��ʶ��Ϣ
			pOctets->Clear();			
			Size_t iSize = (Size_t)pOctets->Capacity();
			if (!m_pThreadZmq->Recv(pOctets->Begin(), iSize))
				return false;

			UInt64 iThreadZmqId = 0;			
			iThreadZmqId	= *((UInt64*)pOctets->Begin());
			iThread			= HIGH_U32(iThreadZmqId);
			Bool bRecvMore = m_pThreadZmq->IsWaitRecv();
			HawkAssert(iSize == sizeof(iThreadZmqId) && bRecvMore);
			if (iSize != sizeof(iThreadZmqId) || !bRecvMore)
				return false;
		
			//��ȡ��Ϣͷ��Ϣ
			pOctets->Clear();
			iSize = (Size_t)pOctets->Capacity();
			if (!m_pThreadZmq->Recv(pOctets->Begin(), iSize))
				return false;
			
			sHeader	   = *((ThreadMsgHeader*)pOctets->Begin());
			bRecvMore = m_pThreadZmq->IsWaitRecv();
			HawkAssert(iSize == sizeof(sHeader) && bRecvMore);
			if (iSize != sizeof(sHeader) || !bRecvMore)
				return false;

			//��ȡ��Ϣ������
			pOctets->Clear();
			iSize = (Size_t)pOctets->Capacity();
			if (!m_pThreadZmq->Recv(pOctets->Begin(), iSize))
				return false;

			pOctets->Resize((UInt32)iSize);
			return true;
		}
		return false;
	}

	Bool HawkGateway::SendThreadMsg(UInt32 iThread, const GateMsgHeader& sHeader, void* pData, Size_t iSize, const OctetsStream* pExtData)
	{
		if (m_pThreadZmq && iThread)
		{
			//�߳�Ŀ��
			UInt64 iThreadZmqId = MAKE_UINT64(iThread, 1);
			if (!m_pThreadZmq->Send(&iThreadZmqId, sizeof(iThreadZmqId), HawkZmq::HZMQ_SNDMORE))
				return false;

			//��Ϣͷ��Ϣ
			if (!m_pThreadZmq->Send((void*)&sHeader, sizeof(sHeader), HawkZmq::HZMQ_SNDMORE))
				return false;

			//��������
			if (!m_pThreadZmq->Send(pData, iSize, pExtData ? HawkZmq::HZMQ_SNDMORE : 0))
				return false;

			//��������
			if (pExtData && !m_pThreadZmq->Send((void*)pExtData->Begin(), pExtData->Size()))
				return false;

			return true;
		}
		return false;
	}

	Bool HawkGateway::RecvServerMsg(UInt32& iSvrId, SvrMsgHeader& sHeader, OctetsStream* pOctets)
	{
		if (m_pServerZmq && pOctets)
		{
			//������ID��ʶ��Ϣ
			pOctets->Clear();
			Size_t iSize = (Size_t)pOctets->Capacity();
			if (!m_pServerZmq->Recv(pOctets->Begin(), iSize))
				return false;

			UInt64 iSvrZmqId	= *((UInt64*)pOctets->Begin());
			iSvrId				= HIGH_U32(iSvrZmqId);
			Bool bRecvMore		= m_pServerZmq->IsWaitRecv();
			HawkAssert(iSize == sizeof(iSvrZmqId) && bRecvMore);			
			if (iSize != sizeof(iSvrZmqId) || !bRecvMore)
				return false;

			//��ȡ��Ϣͷ��Ϣ
			pOctets->Clear();
			iSize = (Size_t)pOctets->Capacity();
			if (!m_pServerZmq->Recv(pOctets->Begin(), iSize))
				return false;

			sHeader	   = *((SvrMsgHeader*)pOctets->Begin());
			bRecvMore = m_pServerZmq->IsWaitRecv();
			HawkAssert(iSize == sizeof(sHeader) && bRecvMore);
			if (iSize != sizeof(sHeader) || !bRecvMore)
				return false;

			//��Ϣ������
			pOctets->Clear();
			iSize = (Size_t)pOctets->Capacity();
			if (!m_pServerZmq->Recv(pOctets->Begin(), iSize))
				return false;

			pOctets->Resize((UInt32)iSize);
			return true;
		}
		return false;
	}

	Bool HawkGateway::RecvServerExtData(OctetsStream* pOctets)
	{
		if (m_pServerZmq && pOctets)
		{
			//��Ϣ״̬У��
			Bool bRecvMore = m_pServerZmq->IsWaitRecv();
			HawkAssert(bRecvMore);
			if (!bRecvMore)
				return false;

			//��Ϣ������
			pOctets->Clear();
			Size_t iSize = (Size_t)pOctets->Capacity();
			if (!m_pServerZmq->Recv(pOctets->Begin(), iSize))
				return false;

			pOctets->Resize((UInt32)iSize);
			return true;
		}
		return false;
	}

	Bool HawkGateway::SendServerMsg(UInt32 iSvrId, const GateMsgHeader& sHeader, void* pData, Size_t iSize)
	{
		if (m_pServerZmq && iSvrId)
		{
			//Ŀ��
			UInt64 iSvrZmqId = MAKE_UINT64(iSvrId, 1);
			if (!m_pServerZmq->Send(&iSvrZmqId, sizeof(iSvrZmqId), HawkZmq::HZMQ_SNDMORE))
				return false;

			//��Ϣͷ
			if (!m_pServerZmq->Send((void*)&sHeader, sizeof(sHeader), HawkZmq::HZMQ_SNDMORE))
				return false;

			//����
			if (!m_pServerZmq->Send(pData, iSize))
				return false;

			return true;
		}
		return false;
	}

	Bool HawkGateway::TrustSession(void* pSession, OctetsStream* pIBuffer)
	{
		return true;
	}

	Bool HawkGateway::CloseSession(SID iSid, UInt32 iSvrId)
	{
		if (!iSvrId)
		{
			SidBindMap::const_iterator it = m_mSidBind.find(iSid);
			if (it != m_mSidBind.end())
				iSvrId = it->second;
		}

		if (iSvrId && iSid > m_iBaseSid)
		{
			//�����Ự��Ӧ������������
			OnSessionDetachServer(iSid, iSvrId);

			//֪ͨ�����̹߳رջỰ
			GateNotify sNotify(GateNotify::NOTIFY_SESSION_CLOSE);
			sNotify.eClose.Sid   = iSid;
			sNotify.eClose.SvrId = iSvrId;
			UInt32 iIdx = (iSid-m_iBaseSid-1) % m_iThread + 1;
			SendThreadMsg(iIdx, GateMsgHeader(iSid), &sNotify, sizeof(sNotify));

			return true;
		}
		return false;
	}

	Bool HawkGateway::OnSessionAccept()
	{
		if (!m_sSocket.UpdateEvent(HEVENT_READ))
			return true;

		//����������
		HawkSocket sSocket;
		SocketAddr sAddr;
		while (m_sSocket.Accept(sSocket, sAddr))
		{
			//���÷ǿ���
			m_bIdle = false;

			//ע����������Ϣ
			UInt32 iSvrId	 = 0;
			Int32  iErrCode = 0;
			if (!RegConnection(sAddr, iSvrId, iErrCode))
				return OnGateRefuseConn(iErrCode, sSocket, &sAddr);

			//Ͷ������֪ͨ
			GateNotify sNotify(GateNotify::NOTIFY_SESSION_CONNECT);
			sNotify.eConnect.SvrId		= iSvrId;
			sNotify.eConnect.AddrLen	= sAddr.CopyAddr(sNotify.eConnect.Address);
			sNotify.eConnect.Handle		= sSocket.Handle();
			UInt32 iIdx = m_iTurnIdx++ % m_iThread + 1;
			SendThreadMsg(iIdx, GateMsgHeader(0), &sNotify, sizeof(sNotify));

			//���δ���ƽ������ʱ��
			if (!m_bWaitIdle) break;
		}
		return true;
	}

	Bool HawkGateway::RegConnection(const SocketAddr& sAddr, UInt32& iSvrId, Int32& iErrCode)
	{
		//����ģʽ
		if (m_iGateMode == GATEMODE_ECHO)
		{
			iSvrId = 0;
			return true;
		}
		
		//��ǰ�޹��ط�����
		if (!m_vService.size())
		{
			iErrCode = SysProtocol::ERR_SERVICE_INVALID;
			return false;
		}

		//��һ������ģʽ, ��һ��������
		if (m_iGateMode == GATEMODE_SINGLE_SERVICE)
		{			
			if (m_vService[0].ConnCap && m_vService[0].CurCap >= m_vService[0].ConnCap)
			{
				iErrCode = SysProtocol::ERR_REFUSE_CONN;
				return false;
			}

			iSvrId = m_vService[0].SvrId;
			return true;
		}
		//�������ģʽ, ˳������ԭ��
		else if (m_iGateMode == GATEMODE_MULTI_SERVICE_STOCK)
		{
			for (Size_t i=0;i<m_vService.size();i++)
			{
				if (!m_vService[i].ConnCap || m_vService[i].CurCap < m_vService[i].ConnCap)
				{
					iSvrId = m_vService[i].SvrId;
					return true;
				}
			}

			iErrCode = SysProtocol::ERR_REFUSE_CONN;
			return false;
		}
		//�������ģʽ, ���ؾ���ԭ��
		else if (m_iGateMode == GATEMODE_MULTI_SERVICE_BALANCE)
		{
			Int32 iSvrIdx = -1;
			Int32 iSvrCap = -1;
			for (Size_t i=0;i<m_vService.size();i++)
			{
				if (!m_vService[i].ConnCap || m_vService[i].CurCap < m_vService[i].ConnCap)
				{
					if (iSvrCap == -1 || iSvrCap > (Int32)m_vService[i].CurCap)
					{
						iSvrIdx = i;
						iSvrCap = m_vService[i].CurCap;
					}
				}
			}

			//������Ч������
			if (iSvrIdx >= 0)
			{
				iSvrId = m_vService[iSvrIdx].SvrId;
				return true;
			}

			iErrCode = SysProtocol::ERR_REFUSE_CONN;
			return false;
		}

		return false;
	}

	Bool HawkGateway::OnGateRefuseConn(Int32 iReason, HawkSocket& sSocket, const SocketAddr* pAddr)
	{
		//�ȷ��;ܾ�����Э���ٹر�		
		SysProtocol::Sys_RefuseConn sProto(iReason);		

		m_pOctets->Clear();
		sProto.Encode(*m_pOctets);
		Size_t iSize = (Size_t)m_pOctets->Size();
		sSocket.Send(m_pOctets->Begin(), iSize);
		sSocket.Close();

		return true;
	}

	Bool HawkGateway::OnSessionAttachServer(SID iSid, UInt32 iSvrId)
	{
		//SID��ʱδʹ��, �ʴ˴���������SID��һ����ȷ
		for (Size_t i=0;i<m_vService.size();i++)
		{
			if (m_vService[i].SvrId == iSvrId)
			{
				m_vService[i].CurCap ++;				
				m_mSidBind[iSid] = iSvrId;
				return true;
			}
		}
		return false;
	}

	Bool HawkGateway::OnSessionDetachServer(SID iSid, UInt32 iSvrId)
	{
		//SID��ʱδʹ��, �ʴ˴���������SID��һ����ȷ
		for (Size_t i=0;i<m_vService.size();i++)
		{
			if (m_vService[i].SvrId == iSvrId)
			{
				if (m_vService[i].CurCap)
					m_vService[i].CurCap --;

				SidBindMap::iterator it = m_mSidBind.find(iSid);
				if (it != m_mSidBind.end())
					m_mSidBind.erase(it);

				return true;
			}
		}
		return false;
	}

	Bool HawkGateway::OnGateThreadEvent()
	{
		while (m_pThreadZmq && m_pThreadZmq->PollEvent(HEVENT_READ, 0))
		{
			//���÷ǿ���
			m_bIdle = false;

			//���������߳���Ϣ
			UInt32	iThread = 0;
			ThreadMsgHeader sHeader;
			if (!RecvThreadMsg(iThread, sHeader, m_pOctets))
				return false;
			
			//���ݰ�����, �޶��⸽������
			if (m_pThreadZmq->IsWaitRecv())
				return false;

			//�������û�Э������У��
			if (sHeader.Sid)
			{
				if (!OnSessionProtocol(sHeader.Sid, m_pOctets))
				{
					CloseSession(sHeader.SvrId, sHeader.Sid);
					return true;
				}
			}
			
			//Echoģʽֱ�ӻظ�
			if (m_iGateMode == GATEMODE_ECHO)
			{
				//�û�Э��ֱ�ӷ��ͻ�ȥ
				if (sHeader.Args > 0 && sHeader.Sid)
					SendThreadMsg(iThread, GateMsgHeader(sHeader.Sid), m_pOctets->Begin(), m_pOctets->Size());
			}
			//���͸���˷���
			else
			{
				//�Ự���غ�ж���ж�
				if (sHeader.Args == SysProtocol::SYS_SESSION_START)
				{
					//���ӻỰ��Ӧ������������
					OnSessionAttachServer(sHeader.Sid, sHeader.SvrId);

					//������ϢͷЯ���ĻỰID, ������˷��������������Э��(���û�Э��)
					sHeader.Sid = 0;
				}
				else if (sHeader.Args == SysProtocol::SYS_SESSION_BREAK)
				{
					//�����Ự��Ӧ������������
					OnSessionDetachServer(sHeader.Sid, sHeader.SvrId);

					//������ϢͷЯ���ĻỰID, ������˷��������������Э��(���û�Э��)
					sHeader.Sid = 0;
				}

				//ת������˷�����
				if (sHeader.SvrId)
					SendServerMsg(sHeader.SvrId, GateMsgHeader(sHeader.Sid), m_pOctets->Begin(), m_pOctets->Size());
			}

			//���δ���ƽ������ʱ��
			if (!m_bWaitIdle) break;
		}
		return true;
	}

	Bool HawkGateway::OnSessionProtocol(SID iSid, const OctetsStream* pOctets)
	{
		if (iSid && pOctets)			
			return true;

		return false;
	}

	Bool HawkGateway::OnGateServerEvent()
	{
		while (m_pServerZmq && m_pServerZmq->PollEvent(HEVENT_READ, 0))
		{
			//���÷ǿ���
			m_bIdle = false;
			
			//���պ�˷�����Ϣ
			UInt32 iSvrId = 0;
			SvrMsgHeader sHeader;
			if (!RecvServerMsg(iSvrId, sHeader, m_pOctets))
				return false;

			//�ỰЭ��ת��
			if (sHeader.Sid)
			{
				//���ݰ�����, �޶��⸽������
				if (m_pServerZmq->IsWaitRecv())
					return false;

				UInt32 iIdx = (sHeader.Sid-m_iBaseSid-1) % m_iThread + 1;
				SendThreadMsg(iIdx, GateMsgHeader(sHeader.Sid), m_pOctets->Begin(), m_pOctets->Size());
			}
			//ϵͳ��Ϣ֪ͨ
			else
			{
				HawkAssert(m_pOctets->Size() == sizeof(GateNotify));
				GateNotify* pNotify = (GateNotify*)m_pOctets->Begin();
				if (m_pOctets->Size() != sizeof(GateNotify))
					return false;
				
				//֪ͨ�����̹߳رջỰ����
				if (pNotify->Type == GateNotify::NOTIFY_SESSION_CLOSE)
				{
					//���ݰ�����, �޶��⸽������
					if (m_pServerZmq->IsWaitRecv())
						return false;

					//����Ự��
					OnSessionDetachServer(pNotify->eClose.Sid, pNotify->eClose.SvrId);

					//ת���������̴߳���
					UInt32 iIdx = (pNotify->eClose.Sid-m_iBaseSid-1) % m_iThread + 1;
					SendThreadMsg(iIdx, GateMsgHeader(0), pNotify, sizeof(*pNotify));
				}
				//Э��ע��
				else if (pNotify->Type == GateNotify::NOTIFY_PROTO_REGISTER)
				{
					//֪ͨ��ϢЯ������
					if (RecvServerExtData(m_pOctets))
					{
						GateNotify sNotify(GateNotify::NOTIFY_PROTO_REGISTER);
						for (Int32 i=0; i<m_iThread; i++)
						{
							UInt32 iIdx = i + 1;
							SendThreadMsg(iIdx, GateMsgHeader(0), &sNotify, sizeof(sNotify), m_pOctets);
						}
					}
				}
				//Э��㲥
				else if (pNotify->Type == GateNotify::NOTIFY_PROTO_BROADCAST)
				{
					//֪ͨ��ϢЯ������
					if (RecvServerExtData(m_pOctets))
					{
						vector<SID> vSid;
						*m_pOctets >> vSid;

						for (Size_t i=0; i<vSid.size(); i++)
						{
							UInt32 iIdx = (vSid[i]-m_iBaseSid-1) % m_iThread + 1;
							SendThreadMsg(iIdx, GateMsgHeader(vSid[i]), m_pOctets->AvailableData(), m_pOctets->AvailableSize());						
						}
					}
				}
			}

			//���δ���ƽ������ʱ��
			if (!m_bWaitIdle) break;
		}
		return true;
	}

	Bool HawkGateway::OnGateMonitorEvent()
	{
		while (m_pMonitorZmq && m_pMonitorZmq->PollEvent(HEVENT_READ, 0))
		{
			//���÷ǿ���
			m_bIdle = false;

			//����ͷ��Ϣ
			m_pOctets->Clear();
			Size_t iSize = (Size_t)m_pOctets->Capacity();
			if (!m_pMonitorZmq->Recv(m_pOctets->Begin(), iSize))
				return false;

			//����У��
			HawkAssert(iSize == sizeof(zmq_event_t));
			zmq_event_t sZmqEvent = *((zmq_event_t*)m_pOctets->Begin());
			if (iSize != sizeof(zmq_event_t))
				return false;

			//���÷���ID�Ѿ������б�
			if (sZmqEvent.event & HawkZmq::HZMQ_EVENT_ACCEPTED)
			{
				HawkFmtPrint("Service Connect Successful, Socket: %d", sZmqEvent.data.accepted.fd);

				if (!OnServerConnected(sZmqEvent.data.accepted.fd))
					return false;
			}
			else if (sZmqEvent.event & HawkZmq::HZMQ_EVENT_DISCONNECTED)
			{
				HawkFmtPrint("Service Disconnect Exception, Socket: %d", sZmqEvent.data.accepted.fd);

				if (!OnServerDisConnect(sZmqEvent.data.disconnected.fd))
					return false;				
			}
			else
			{
				HawkFmtPrint("Gateway Unhandle Event, Event: %d", sZmqEvent.event);
			}

			//���δ���ƽ������ʱ��
			if (!m_bWaitIdle) break;
		}
		return true;
	}

	Bool HawkGateway::OnGateIdleEvent()
	{
		//���д���
		if (m_bIdle)
		{
			HawkSleep(DEFAULT_SLEEP);
		}		
		return true;
	}

	Bool HawkGateway::OnGatewayClose()
	{
		//֪ͨ�����߳��˳�
		GateNotify sNotify(GateNotify::NOTIFY_SERVICE_EXIT);
		for (UInt32 j=0;m_pThreadZmq && j<(UInt32)m_iThread;j++)
		{
			SendThreadMsg(j+1, GateMsgHeader(0), &sNotify, sizeof(sNotify));
		}

		//�ȴ��߳̽���
		for (UInt32 i=0;m_ppThread && i<(UInt32)m_iThread;i++)
		{
			if (m_ppThread[i])
				m_ppThread[i]->Close();
		}

		//�ر����ܼ�����
		if (m_pProfiler)
			m_pProfiler->Stop();

		//��������״̬
		m_bRunning = false;

		return true;
	}

	Bool HawkGateway::OnServerConnected(SOCKET hSocket)
	{
		if (hSocket != INVALID_SOCKET)
		{
			if (m_pServerZmq->PollEvent(HEVENT_READ, DEFAULT_TIMEOUT))
			{				
				//��������֪ͨ
				UInt32 iSvrId = 0;
				SvrMsgHeader sHeader;
				if (!RecvServerMsg(iSvrId, sHeader, m_pOctets))
					return false;

				//���ݰ�����, �޶��⸽������
				if (m_pServerZmq->IsWaitRecv())
					return false;

				//��������У��(���͹��ط��������һ����)
				HawkAssert(sHeader.Sid == 0 && iSvrId && m_pOctets->Size() == sizeof(GateNotify));
				GateNotify* pNotify = (GateNotify*)m_pOctets->Begin();
				if (sHeader.Sid || !iSvrId || iSvrId != pNotify->eAttach.SvrId || m_pOctets->Size() != sizeof(GateNotify))
					return false;

				//�������֪ͨ
				if (pNotify->Type == GateNotify::NOTIFY_SERVICE_ATTACH)
				{
					//�жϷ���ID�ظ�
					Size_t i = 0;
					for (;i<m_vService.size();i++)
					{
						if (iSvrId == m_vService[i].SvrId)
							break;
					}

					//��ӵ��б�
					if (i == m_vService.size())
					{						
						m_vService.push_back(Service(iSvrId, hSocket, pNotify->eAttach.ConnLimit));
						HawkFmtPrint("Service Attach, SvrId: %u", iSvrId);
					}

					return true;
				}
			}		
		}
		return false;
	}

	Bool HawkGateway::OnServerDisConnect(SOCKET hSocket)
	{
		if (hSocket != INVALID_SOCKET)
		{
			ServiceVec::iterator it = m_vService.begin();
			for (;it!=m_vService.end();it++)
			{				
				if (it->SvrFd == hSocket)
				{	
					//֪ͨ�����̷߳���ж��
					GateNotify sNotify(GateNotify::NOTIFY_SERVICE_DETACH);
					sNotify.eDetach.SvrId = it->SvrId;

					for (UInt32 j=0;m_pThreadZmq && j<(UInt32)m_iThread;j++)
					{
						SendThreadMsg(j+1, GateMsgHeader(0), &sNotify, sizeof(sNotify));
					}

					//ɾ�����ϵĻỰID
					SidBindMap::iterator bit = m_mSidBind.begin();
					for (;bit != m_mSidBind.end();)
					{
						if (bit->second == it->SvrId)
						{
							m_mSidBind.erase(bit++);
							continue;
						}
						bit ++;
					}

					//�ӷ����б�ɾ��
					HawkFmtPrint("Service Detach, SvrId: %u", it->SvrId);
					m_vService.erase(it);

					break;
				}
			}
		}
		return false;
	}
}
