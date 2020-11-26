#ifndef MS_RTC_PRODUCER_HPP
#define MS_RTC_PRODUCER_HPP

#include "common.hpp"
#include "Channel/Request.hpp"
#include "handles/Timer.hpp"
#include "RTC/AbstractProducer.hpp"
#include "RTC/KeyFrameRequestManager.hpp"
#include "RTC/RTCP/CompoundPacket.hpp"
#include "RTC/RTCP/Packet.hpp"
#include "RTC/RTCP/SenderReport.hpp"
#include "RTC/RTCP/XrDelaySinceLastRr.hpp"
#include "RTC/RtpHeaderExtensionIds.hpp"
#include <json.hpp>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace RTC
{
	class Producer : public AbstractProducer, 
					 public RTC::RtpStreamRecv::Listener, 
					 public RTC::KeyFrameRequestManager::Listener,
					 public Timer::Listener
	{
	public:
		enum TranslateMode
		{
			direct           = 0,
			unpackAndProduce = 1,
			decodeAndEncode  = 2
		};

	private:
		struct RtpEncodingMapping
		{
			std::string rid;
			uint32_t ssrc{ 0 };
			uint32_t mappedSsrc{ 0 };
		};

	private:
		struct RtpMapping
		{
			std::map<uint8_t, uint8_t> codecs;
			std::vector<RtpEncodingMapping> encodings;
		};

	private:
		struct VideoOrientation
		{
			bool camera{ false };
			bool flip{ false };
			uint16_t rotation{ 0 };
		};

	private:
		struct TraceEventTypes
		{
			bool rtp{ false };
			bool keyframe{ false };
			bool nack{ false };
			bool pli{ false };
			bool fir{ false };
		};

	public:
		Producer(const std::string& id, RTC::AbstractProducer::Listener* listener, json& data);
		virtual ~Producer();

	public:
		//
		virtual void FillJson(json& jsonObject) const;

		//
		virtual void FillJsonStats(json& jsonArray) const;

		//
		virtual void HandleRequest(Channel::Request* request);

		//
		virtual ReceiveRtpPacketResult ReceiveRtpPacket(RTC::RtpPacket* packet);
		ReceiveRtpPacketResult ReceiveRtpMedia(RTC::RtpPacket* packet);
		ReceiveRtpPacketResult ReceiveRtpRtx(RTC::RtpPacket* packet);
		ReceiveRtpPacketResult DispatchRtpPacket(RTC::RtpPacket* packet);

		//
		void setMaster(Producer * master);

		// 
		void onClosedSlave(Producer * slave);

		//
		AVFramePtr getLastFrame() const;

	protected:
		RTC::RtpStreamRecv* GetRtpStream(RTC::RtpPacket* packet);
		RTC::RtpStreamRecv* GetRtpStream(const uint32_t ssrc, const uint8_t payloadType, const std::string & rid);

	private:
		RTC::RtpStreamRecv* CreateRtpStream(
		  const uint32_t ssrc, const RTC::RtpCodecParameters& mediaCodec, size_t encodingIdx);
		void NotifyNewRtpStream(RTC::RtpStreamRecv* rtpStream);
		void PreProcessRtpPacket(RTC::RtpPacket* packet);
		bool MangleRtpPacket(RTC::RtpPacket* packet, RTC::RtpStreamRecv* rtpStream) const;
		void PostProcessRtpPacket(RTC::RtpPacket* packet);
		void EmitScore() const;
		void EmitTraceEventRtpAndKeyFrameTypes(RTC::RtpPacket* packet, bool isRtx = false) const;
		void EmitTraceEventKeyFrameType(RTC::RtpPacket* packet, bool isRtx = false) const;
		void EmitTraceEventPliType(uint32_t ssrc) const;
		void EmitTraceEventFirType(uint32_t ssrc) const;
		void EmitTraceEventNackType() const;

		/* Pure virtual methods inherited from RTC::RtpStreamRecv::Listener. */
	public:
		void OnRtpStreamScore(RTC::RtpStream* rtpStream, uint8_t score, uint8_t previousScore) override;
		void OnRtpStreamSendRtcpPacket(RTC::RtpStreamRecv* rtpStream, RTC::RTCP::Packet* packet) override;
		void OnRtpStreamNeedWorstRemoteFractionLost(
		  RTC::RtpStreamRecv* rtpStream, uint8_t& worstRemoteFractionLost) override;
		void OnRtpStreamResendPackets(
			  RTC::RtpStreamRecv* rtpStream, const std::vector<uint16_t> & seqNumbers) override;


		/* Pure virtual methods inherited from RTC::KeyFrameRequestManager::Listener. */
	public:
		void OnKeyFrameNeeded(RTC::KeyFrameRequestManager* keyFrameRequestManager, uint32_t ssrc) override;

	private:
	    void OnTimer(Timer * timer); 


	private:
		// Others.
		struct RtpMapping rtpMapping;
		std::vector<RTC::RtpStreamRecv*> rtpStreamByEncodingIdx;
		// Video orientation.
		bool videoOrientationDetected{ false };
		struct VideoOrientation videoOrientation;
		struct TraceEventTypes traceEventTypes;

		TranslateMode translateMode { decodeAndEncode };

		// 40 - 25 fps
		// 67 - 15 fps
		const uint32_t m_timerDelay { 40 };
		Timer m_timer;

		// TODO need remove when slave closed
		Producer                * m_master { nullptr };
		std::vector<Producer *>   m_slaves;
	};
} // namespace RTC

#endif
