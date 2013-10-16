#ifndef HAWK_APPGATEWAY_H
#define HAWK_APPGATEWAY_H

#include "HawkGateway.h"

namespace Hawk
{
	class HawkAppFrame;
	/************************************************************************/
	/* �����ʹ����Ƕ���ط�����                                             */
	/************************************************************************/	
	class FRAME_API HawkAppGateway : public HawkGateway
	{
	public:
		//����
		HawkAppGateway(HawkAppFrame* pApp = 0);

		//����
		virtual ~HawkAppGateway();

		//֧������ƽ̨�Ķ���
		enum
		{
			GATE_PF_NONE = 0,
			GATE_PF_QQ,
		};

	public:
		//��ʼ��
		virtual Bool    Init();

		//ֹͣ
		virtual Bool	Stop();

		//�Ự����
		virtual Bool	TrustSession(void* pSession, OctetsStream* pIBuffer);

	protected:
		//App����
		HawkAppFrame*	m_pApp;
		//���ط������߳�
		HawkThread*		m_pThread;
		//����ƽ̨
		Int32			m_iPlatform;
	};
}
#endif
