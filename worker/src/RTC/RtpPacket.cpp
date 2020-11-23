#define MS_CLASS "RTC::RtpPacket"
// #define MS_LOG_DEV_LEVEL 3

#include "RTC/RtpPacket.hpp"
#include "Logger.hpp"
#include <cstring>  // std::memcpy(), std::memmove(), std::memset()
#include <iterator> // std::ostream_iterator
#include <sstream>  // std::ostringstream

extern "C"
{
	#include <libavutil/opt.h>
}

namespace RTC
{
	/* Class methods. */

	RtpPacket* RtpPacket::Parse(const uint8_t* data, size_t len)
	{
		MS_TRACE();

		if (!RtpPacket::IsRtp(data, len))
			return nullptr;

		auto* ptr = const_cast<uint8_t*>(data);

		// Get the header.
		auto* header = reinterpret_cast<Header*>(ptr);

		// Inspect data after the minimum header size.
		ptr += sizeof(Header);

		// Check CSRC list.
		size_t csrcListSize{ 0u };

		if (header->csrcCount != 0u)
		{
			csrcListSize = header->csrcCount * sizeof(header->ssrc);

			// Packet size must be >= header size + CSRC list.
			if (len < (ptr - data) + csrcListSize)
			{
				MS_WARN_TAG(rtp, "not enough space for the announced CSRC list, packet discarded");

				return nullptr;
			}
			ptr += csrcListSize;
		}

		// Check header extension.
		HeaderExtension* headerExtension{ nullptr };
		size_t extensionValueSize{ 0u };

		if (header->extension == 1u)
		{
			// The header extension is at least 4 bytes.
			if (len < static_cast<size_t>(ptr - data) + 4)
			{
				MS_WARN_TAG(rtp, "not enough space for the announced header extension, packet discarded");

				return nullptr;
			}

			headerExtension = reinterpret_cast<HeaderExtension*>(ptr);

			// The header extension contains a 16-bit length field that counts the number of
			// 32-bit words in the extension, excluding the four-octet header extension.
			extensionValueSize = static_cast<size_t>(ntohs(headerExtension->length) * 4);

			// Packet size must be >= header size + CSRC list + header extension size.
			if (len < (ptr - data) + 4 + extensionValueSize)
			{
				MS_WARN_TAG(
				  rtp, "not enough space for the announced header extension value, packet discarded");

				return nullptr;
			}
			ptr += 4 + extensionValueSize;
		}

		// Get payload.
		uint8_t* payload     = ptr;
		size_t payloadLength = len - (ptr - data);
		uint8_t payloadPadding{ 0 };

		MS_ASSERT(len >= static_cast<size_t>(ptr - data), "payload has negative size");

		// Check padding field.
		if (header->padding != 0u)
		{
			// Must be at least a single payload byte.
			if (payloadLength == 0)
			{
				MS_WARN_TAG(rtp, "padding bit is set but no space for a padding byte, packet discarded");

				return nullptr;
			}

			payloadPadding = data[len - 1];
			if (payloadPadding == 0)
			{
				MS_WARN_TAG(rtp, "padding byte cannot be 0, packet discarded");

				return nullptr;
			}

			if (payloadLength < size_t{ payloadPadding })
			{
				MS_WARN_TAG(
				  rtp,
				  "number of padding octets is greater than available space for payload, packet "
				  "discarded");

				return nullptr;
			}
			payloadLength -= size_t{ payloadPadding };
		}

		MS_ASSERT(
		  len == sizeof(Header) + csrcListSize + (headerExtension ? 4 + extensionValueSize : 0) +
		           payloadLength + size_t{ payloadPadding },
		  "packet's computed size does not match received size");

		auto* packet =
		  new RtpPacket(header, headerExtension, payload, payloadLength, payloadPadding, len);

		return packet;
	}

	/* Instance methods. */

	RtpPacket::RtpPacket(
	  Header* header,
	  HeaderExtension* headerExtension,
	  const uint8_t* payload,
	  size_t payloadLength,
	  uint8_t payloadPadding,
	  size_t size)
	  : header(header), headerExtension(headerExtension), payload(const_cast<uint8_t*>(payload)),
	    payloadLength(payloadLength), payloadPadding(payloadPadding), size(size)
	{
		MS_TRACE();

		if (this->header->csrcCount != 0u)
			this->csrcList = reinterpret_cast<uint8_t*>(header) + sizeof(Header);

		// Parse RFC 5285 header extension.
		ParseExtensions();
	}

	RtpPacket::~RtpPacket()
	{
		MS_TRACE();

		if (buffer)
		{
			delete[] buffer;
		}
	}

	void RtpPacket::Dump() const
	{
		MS_TRACE();

		MS_DUMP("<RtpPacket>");
		MS_DUMP("  padding           : %s", this->header->padding ? "true" : "false");
		if (HasHeaderExtension())
		{
			MS_DUMP(
			  "  header extension  : id:%" PRIu16 ", length:%zu",
			  GetHeaderExtensionId(),
			  GetHeaderExtensionLength());
		}
		if (HasOneByteExtensions())
		{
			MS_DUMP("  RFC5285 ext style : One-Byte Header");
		}
		if (HasTwoBytesExtensions())
		{
			MS_DUMP("  RFC5285 ext style : Two-Bytes Header");
		}
		if (HasOneByteExtensions() || HasTwoBytesExtensions())
		{
			std::vector<std::string> extIds;
			std::ostringstream extIdsStream;

			if (HasOneByteExtensions())
			{
				extIds.reserve(this->mapOneByteExtensions.size());

				for (const auto& kv : this->mapOneByteExtensions)
				{
					extIds.push_back(std::to_string(kv.first));
				}
			}
			else
			{
				extIds.reserve(this->mapTwoBytesExtensions.size());

				for (const auto& kv : this->mapTwoBytesExtensions)
				{
					extIds.push_back(std::to_string(kv.first));
				}
			}

			if (!extIds.empty())
			{
				std::copy(
				  extIds.begin(), extIds.end() - 1, std::ostream_iterator<std::string>(extIdsStream, ","));
				extIdsStream << extIds.back();

				MS_DUMP("  RFC5285 ext ids   : %s", extIdsStream.str().c_str());
			}
		}
		if (this->midExtensionId != 0u)
		{
			std::string mid;

			if (ReadMid(mid))
			{
				MS_DUMP(
				  "  mid               : extId:%" PRIu8 ", value:'%s'", this->midExtensionId, mid.c_str());
			}
		}
		if (this->ridExtensionId != 0u)
		{
			std::string rid;

			if (ReadRid(rid))
			{
				MS_DUMP(
				  "  rid               : extId:%" PRIu8 ", value:'%s'", this->ridExtensionId, rid.c_str());
			}
		}
		if (this->rridExtensionId != 0u)
		{
			std::string rid;

			if (ReadRid(rid))
			{
				MS_DUMP(
				  "  rrid              : extId:%" PRIu8 ", value:'%s'", this->rridExtensionId, rid.c_str());
			}
		}
		if (this->absSendTimeExtensionId != 0u)
		{
			MS_DUMP("  absSendTime       : extId:%" PRIu8, this->absSendTimeExtensionId);
		}
		if (this->transportWideCc01ExtensionId != 0u)
		{
			uint16_t wideSeqNumber;

			if (ReadTransportWideCc01(wideSeqNumber))
			{
				MS_DUMP(
				  "  transportWideCc01 : extId:%" PRIu8 ", value:%" PRIu16,
				  this->transportWideCc01ExtensionId,
				  wideSeqNumber);
			}
		}
		// Remove once it becomes RFC.
		if (this->frameMarking07ExtensionId != 0u)
		{
			MS_DUMP("  frameMarking07    : extId:%" PRIu8, this->frameMarking07ExtensionId);
		}
		if (this->frameMarkingExtensionId != 0u)
		{
			MS_DUMP("  frameMarking      : extId:%" PRIu8, this->frameMarkingExtensionId);
		}
		if (this->ssrcAudioLevelExtensionId != 0u)
		{
			uint8_t volume;
			bool voice;

			if (ReadSsrcAudioLevel(volume, voice))
			{
				MS_DUMP(
				  "  ssrcAudioLevel    : extId:%" PRIu8 ", volume:%" PRIu8 ", voice:%s",
				  this->ssrcAudioLevelExtensionId,
				  volume,
				  voice ? "true" : "false");
			}
		}
		if (this->videoOrientationExtensionId != 0u)
		{
			bool camera;
			bool flip;
			uint16_t rotation;

			if (ReadVideoOrientation(camera, flip, rotation))
			{
				MS_DUMP(
				  "  videoOrientation  : extId:%" PRIu8 ", camera:%s, flip:%s, rotation:%" PRIu16,
				  this->videoOrientationExtensionId,
				  camera ? "true" : "false",
				  flip ? "true" : "false",
				  rotation);
			}
		}
		MS_DUMP("  csrc count        : %" PRIu8, this->header->csrcCount);
		MS_DUMP("  marker            : %s", HasMarker() ? "true" : "false");
		MS_DUMP("  payload type      : %" PRIu8, GetPayloadType());
		MS_DUMP("  sequence number   : %" PRIu16, GetSequenceNumber());
		MS_DUMP("  timestamp         : %" PRIu32, GetTimestamp());
		MS_DUMP("  ssrc              : %" PRIu32, GetSsrc());
		MS_DUMP("  payload size      : %zu bytes", GetPayloadLength());
		if (this->header->padding != 0u)
		{
			MS_DUMP("  padding size      : %" PRIu8 " bytes", this->payloadPadding);
		}
		MS_DUMP("  packet size       : %zu bytes", GetSize());
		MS_DUMP("  spatial layer     : %" PRIu8, GetSpatialLayer());
		MS_DUMP("  temporal layer    : %" PRIu8, GetTemporalLayer());
		MS_DUMP("</RtpPacket>");
	}

	void RtpPacket::FillJson(json& jsonObject) const
	{
		MS_TRACE();

		// Add payloadType.
		jsonObject["payloadType"] = GetPayloadType();

		// Add sequenceNumber.
		jsonObject["sequenceNumber"] = GetSequenceNumber();

		// Add timestamp.
		jsonObject["timestamp"] = GetTimestamp();

		// Add marker.
		jsonObject["marker"] = HasMarker() ? "true" : "false";

		// Add ssrc.
		jsonObject["ssrc"] = GetSsrc();

		// Add isKeyFrame.
		jsonObject["isKeyFrame"] = IsKeyFrame();

		// Add size.
		jsonObject["size"] = GetSize();

		// Add payloadSize.
		jsonObject["payloadSize"] = GetPayloadLength();

		// Add spatialLayer.
		jsonObject["spatialLayer"] = GetSpatialLayer();

		// Add temporalLayer.
		jsonObject["temporalLayer"] = GetTemporalLayer();

		// Add mid.
		std::string mid;

		if (this->midExtensionId != 0u && ReadMid(mid))
			jsonObject["mid"] = mid;

		// Add rid.
		std::string rid;

		if (this->ridExtensionId != 0u && ReadRid(rid))
			jsonObject["rid"] = rid;

		// Add rrid.
		std::string rrid;

		if (this->rridExtensionId != 0u && ReadRid(rrid))
			jsonObject["rrid"] = rrid;

		// Add wideSequenceNumber.
		uint16_t wideSequenceNumber;

		if (this->transportWideCc01ExtensionId != 0u && ReadTransportWideCc01(wideSequenceNumber))
			jsonObject["wideSequenceNumber"] = wideSequenceNumber;
	}

	void RtpPacket::SetExtensions(uint8_t type, const std::vector<GenericExtension>& extensions)
	{
		MS_ASSERT(type == 1u || type == 2u, "type must be 1 or 2");

		// Reset extension ids.
		this->midExtensionId               = 0u;
		this->ridExtensionId               = 0u;
		this->rridExtensionId              = 0u;
		this->absSendTimeExtensionId       = 0u;
		this->transportWideCc01ExtensionId = 0u;
		this->frameMarking07ExtensionId    = 0u;
		this->frameMarkingExtensionId      = 0u;
		this->ssrcAudioLevelExtensionId    = 0u;
		this->videoOrientationExtensionId  = 0u;

		// Clear the One-Byte and Two-Bytes extension elements maps.
		this->mapOneByteExtensions.clear();
		this->mapTwoBytesExtensions.clear();

		// If One-Byte is requested and the packet already has One-Byte extensions,
		// keep the header extension id.
		if (type == 1u && HasOneByteExtensions())
		{
			// Nothing to do.
		}
		// If Two-Bytes is requested and the packet already has Two-Bytes extensions,
		// keep the header extension id.
		else if (type == 2u && HasTwoBytesExtensions())
		{
			// Nothing to do.
		}
		// Otherwise, if there is header extension of non matching type, modify its id.
		else if (this->headerExtension)
		{
			if (type == 1u)
				this->headerExtension->id = uint16_t{ htons(0xBEDE) };
			else if (type == 2u)
				this->headerExtension->id = uint16_t{ htons(0b0001000000000000) };
		}

		// Calculate total size required for all extensions (with padding if needed).
		size_t extensionsTotalSize{ 0 };

		for (const auto& extension : extensions)
		{
			if (type == 1u)
			{
				if (extension.id == 0 || extension.id > 14 || extension.len == 0 || extension.len > 16)
					continue;

				extensionsTotalSize += (1 + extension.len);
			}
			else if (type == 2u)
			{
				if (extension.id == 0)
					continue;

				extensionsTotalSize += (2 + extension.len);
			}
		}

		auto paddedExtensionsTotalSize =
		  static_cast<size_t>(Utils::Byte::PadTo4Bytes(static_cast<uint16_t>(extensionsTotalSize)));
		size_t padding = paddedExtensionsTotalSize - extensionsTotalSize;

		extensionsTotalSize = paddedExtensionsTotalSize;

		// Calculate the number of bytes to shift (may be negative if the packet did
		// already have header extension).
		int16_t shift{ 0 };

		if (this->headerExtension)
		{
			shift = static_cast<int16_t>(extensionsTotalSize - GetHeaderExtensionLength());
		}
		else
		{
			shift = 4 + static_cast<int16_t>(extensionsTotalSize);
		}

		if (this->headerExtension && shift != 0)
		{
			// Shift the payload.
			std::memmove(this->payload + shift, this->payload, this->payloadLength + this->payloadPadding);
			this->payload += shift;

			// Update packet total size.
			this->size += shift;

			// Update the header extension length.
			this->headerExtension->length = htons(extensionsTotalSize / 4);
		}
		else if (!this->headerExtension)
		{
			// Set the header extension bit.
			this->header->extension = 1u;

			// Set the header extension pointing to the current payload.
			this->headerExtension = reinterpret_cast<HeaderExtension*>(this->payload);

			// Shift the payload.
			std::memmove(this->payload + shift, this->payload, this->payloadLength + this->payloadPadding);
			this->payload += shift;

			// Update packet total size.
			this->size += shift;

			// Set the header extension id.
			if (type == 1u)
				this->headerExtension->id = uint16_t{ htons(0xBEDE) };
			else if (type == 2u)
				this->headerExtension->id = uint16_t{ htons(0b0001000000000000) };

			// Set the header extension length.
			this->headerExtension->length = htons(extensionsTotalSize / 4);
		}

		// Write the new extensions into the header extension value.
		uint8_t* ptr = this->headerExtension->value;

		for (const auto& extension : extensions)
		{
			if (type == 1u)
			{
				if (extension.id == 0 || extension.id > 14 || extension.len == 0 || extension.len > 16)
					continue;

				// Store the One-Byte extension element in the map.
				this->mapOneByteExtensions[extension.id] = reinterpret_cast<OneByteExtension*>(ptr);

				*ptr = (extension.id << 4) | ((extension.len - 1) & 0x0F);
				++ptr;
				std::memmove(ptr, extension.value, extension.len);
				ptr += extension.len;
			}
			else if (type == 2u)
			{
				if (extension.id == 0)
					continue;

				// Store the Two-Bytes extension element in the map.
				this->mapTwoBytesExtensions[extension.id] = reinterpret_cast<TwoBytesExtension*>(ptr);

				*ptr = extension.id;
				++ptr;
				*ptr = extension.len;
				++ptr;
				std::memmove(ptr, extension.value, extension.len);
				ptr += extension.len;
			}
		}

		for (size_t i = 0; i < padding; ++i)
		{
			*ptr = 0u;
			++ptr;
		}

		MS_ASSERT(ptr == this->payload, "wrong ptr calculation");
	}

	bool RtpPacket::UpdateMid(const std::string& mid)
	{
		MS_TRACE();

		uint8_t extenLen;
		uint8_t* extenValue = GetExtension(this->midExtensionId, extenLen);

		if (!extenValue)
			return false;

		// Here we assume that there is MidMaxLength available bytes, even if now
		// they are padding bytes.
		if (mid.size() > RTC::MidMaxLength)
		{
			MS_ERROR(
			  "no enough space for MID value [MidMaxLength:%" PRIu8 ", mid:'%s']",
			  RTC::MidMaxLength,
			  mid.c_str());

			return false;
		}

		std::memcpy(extenValue, mid.c_str(), mid.size());

		SetExtensionLength(this->midExtensionId, mid.size());

		return true;
	}

	/**
	 * The caller is responsible of not setting a length higher than the
	 * available one (taking into account existing padding bytes).
	 */
	bool RtpPacket::SetExtensionLength(uint8_t id, uint8_t len)
	{
		MS_TRACE();

		if (len == 0u)
		{
			MS_ERROR("cannot set extension length to 0");

			return false;
		}

		if (id == 0u)
		{
			return false;
		}
		else if (HasOneByteExtensions())
		{
			auto it = this->mapOneByteExtensions.find(id);

			if (it == this->mapOneByteExtensions.end())
				return false;

			auto* extension = it->second;
			auto currentLen = extension->len + 1;

			// Fill with 0's if new length is minor.
			if (len < currentLen)
				std::memset(extension->value + len, 0, currentLen - len);

			// In One-Byte extensions value length 0 means 1.
			extension->len = len - 1;

			return true;
		}
		else if (HasTwoBytesExtensions())
		{
			auto it = this->mapTwoBytesExtensions.find(id);

			if (it == this->mapTwoBytesExtensions.end())
				return false;

			auto* extension = it->second;
			auto currentLen = extension->len;

			// Fill with 0's if new length is minor.
			if (len < currentLen)
				std::memset(extension->value + len, 0, currentLen - len);

			extension->len = len;

			return true;
		}
		else
		{
			return false;
		}
	}

	void RtpPacket::SetPayloadLength(size_t length)
	{
		MS_TRACE();

		// Pad desired length to 4 bytes.
		length = static_cast<size_t>(Utils::Byte::PadTo4Bytes(static_cast<uint16_t>(length)));

		this->size -= this->payloadLength;
		this->size -= size_t{ this->payloadPadding };
		this->payloadLength  = length;
		this->payloadPadding = 0u;
		this->size += length;

		SetPayloadPaddingFlag(false);
	}

	RtpPacket* RtpPacket::Clone(const uint8_t* buffer) const
	{
		MS_TRACE();

		auto* ptr = const_cast<uint8_t*>(buffer);
		size_t numBytes{ 0 };

		// Copy the minimum header.
		numBytes = sizeof(Header);
		std::memcpy(ptr, GetData(), numBytes);

		// Set header pointer.
		auto* newHeader = reinterpret_cast<Header*>(ptr);

		ptr += numBytes;

		// Copy CSRC list.
		if (this->csrcList != nullptr)
		{
			numBytes = this->header->csrcCount * sizeof(this->header->ssrc);
			std::memcpy(ptr, this->csrcList, numBytes);

			ptr += numBytes;
		}

		// Copy header extension.
		HeaderExtension* newHeaderExtension{ nullptr };

		if (this->headerExtension != nullptr)
		{
			numBytes = 4 + GetHeaderExtensionLength();
			std::memcpy(ptr, this->headerExtension, numBytes);

			// Set the header extension pointer.
			newHeaderExtension = reinterpret_cast<HeaderExtension*>(ptr);

			ptr += numBytes;
		}

		// Copy payload.
		uint8_t* newPayload{ ptr };

		if (this->payloadLength != 0u)
		{
			numBytes = this->payloadLength;
			std::memcpy(ptr, this->payload, numBytes);

			ptr += numBytes;
		}

		// Copy payload padding.
		if (this->payloadPadding != 0u)
		{
			*(ptr + static_cast<size_t>(this->payloadPadding) - 1) = this->payloadPadding;
			ptr += size_t{ this->payloadPadding };
		}

		MS_ASSERT(static_cast<size_t>(ptr - buffer) == this->size, "ptr - buffer == this->size");

		// Create the new RtpPacket instance and return it.
		auto* packet = new RtpPacket(
		  newHeader, newHeaderExtension, newPayload, this->payloadLength, this->payloadPadding, this->size);

		// Keep already set extension ids.
		packet->midExtensionId               = this->midExtensionId;
		packet->ridExtensionId               = this->ridExtensionId;
		packet->rridExtensionId              = this->rridExtensionId;
		packet->absSendTimeExtensionId       = this->absSendTimeExtensionId;
		packet->transportWideCc01ExtensionId = this->transportWideCc01ExtensionId;
		packet->frameMarking07ExtensionId    = this->frameMarking07ExtensionId; // Remove once RFC.
		packet->frameMarkingExtensionId      = this->frameMarkingExtensionId;
		packet->ssrcAudioLevelExtensionId    = this->ssrcAudioLevelExtensionId;
		packet->videoOrientationExtensionId  = this->videoOrientationExtensionId;

		return packet;
	}

	// NOTE: The caller must ensure that the buffer/memmory of the packet has
	// space enough for adding 2 extra bytes.
	void RtpPacket::RtxEncode(uint8_t payloadType, uint32_t ssrc, uint16_t seq)
	{
		MS_TRACE();

		// Rewrite the payload type.
		SetPayloadType(payloadType);

		// Rewrite the SSRC.
		SetSsrc(ssrc);

		// Write the original sequence number at the begining of the payload.
		std::memmove(this->payload + 2, this->payload, this->payloadLength);
		Utils::Byte::Set2Bytes(this->payload, 0, GetSequenceNumber());

		// Rewrite the sequence number.
		SetSequenceNumber(seq);

		// Fix the payload length.
		this->payloadLength += 2u;

		// Fix the packet size.
		this->size += 2u;

		// Remove padding if present.
		if (this->payloadPadding != 0u)
		{
			SetPayloadPaddingFlag(false);

			this->size -= size_t{ this->payloadPadding };
			this->payloadPadding = 0u;
		}
	}

	bool RtpPacket::RtxDecode(uint8_t payloadType, uint32_t ssrc)
	{
		MS_TRACE();

		// Chrome sends some RTX packets with no payload when the stream is started.
		// Just ignore them.
		if (this->payloadLength < 2u)
			return false;

		// Rewrite the payload type.
		SetPayloadType(payloadType);

		// Rewrite the sequence number.
		SetSequenceNumber(Utils::Byte::Get2Bytes(this->payload, 0));

		// Rewrite the SSRC.
		SetSsrc(ssrc);

		// Shift the payload to its original place.
		std::memmove(this->payload, this->payload + 2, this->payloadLength - 2);

		// Fix the payload length.
		this->payloadLength -= 2u;

		// Fix the packet size.
		this->size -= 2u;

		// Remove padding if present.
		if (this->payloadPadding != 0u)
		{
			SetPayloadPaddingFlag(false);

			this->size -= size_t{ this->payloadPadding };
			this->payloadPadding = 0u;
		}

		return true;
	}

	bool RtpPacket::ProcessPayload(RTC::Codecs::EncodingContext* context)
	{
		MS_TRACE();

		if (!this->payloadDescriptorHandler)
			return true;

		bool marker{ false };

		if (this->payloadDescriptorHandler->Process(context, this->payload, marker))
		{
			if (marker)
				SetMarker(true);

			return true;
		}
		else
		{
			return false;
		}
	}

	void RtpPacket::RestorePayload()
	{
		MS_TRACE();

		if (!this->payloadDescriptorHandler)
			return;

		this->payloadDescriptorHandler->Restore(this->payload);
	}

	void RtpPacket::ShiftPayload(size_t payloadOffset, size_t shift, bool expand)
	{
		MS_TRACE();

		if (shift == 0u)
			return;

		MS_ASSERT(payloadOffset < this->payloadLength, "payload offset bigger than payload size");

		if (!expand)
			MS_ASSERT(shift <= (this->payloadLength - payloadOffset), "shift too big");

		uint8_t* payloadOffsetPtr = this->payload + payloadOffset;
		size_t shiftedLen;

		if (expand)
		{
			shiftedLen = this->payloadLength + size_t{ this->payloadPadding } - payloadOffset;

			std::memmove(payloadOffsetPtr + shift, payloadOffsetPtr, shiftedLen);

			this->payloadLength += shift;
			this->size += shift;
		}
		else
		{
			shiftedLen = this->payloadLength + size_t{ this->payloadPadding } - payloadOffset - shift;

			std::memmove(payloadOffsetPtr, payloadOffsetPtr + shift, shiftedLen);

			this->payloadLength -= shift;
			this->size -= shift;
		}
	}

	void RtpPacket::ParseExtensions()
	{
		MS_TRACE();

		// Parse One-Byte header extension.
		if (HasOneByteExtensions())
		{
			// Clear the One-Byte extension elements map.
			this->mapOneByteExtensions.clear();

			uint8_t* extensionStart = reinterpret_cast<uint8_t*>(this->headerExtension) + 4;
			uint8_t* extensionEnd   = extensionStart + GetHeaderExtensionLength();
			uint8_t* ptr            = extensionStart;

			// One-Byte extensions cannot have length 0.
			while (ptr < extensionEnd)
			{
				uint8_t id = (*ptr & 0xF0) >> 4;
				size_t len = static_cast<size_t>(*ptr & 0x0F) + 1;

				// id=15 in One-Byte extensions means "stop parsing here".
				if (id == 15u)
					break;

				// Valid extension id.
				if (id != 0u)
				{
					if (ptr + 1 + len > extensionEnd)
					{
						MS_WARN_TAG(
						  rtp, "not enough space for the announced One-Byte header extension element value");

						break;
					}

					// Store the One-Byte extension element in the map.
					this->mapOneByteExtensions[id] = reinterpret_cast<OneByteExtension*>(ptr);

					ptr += (1 + len);
				}
				// id=0 means alignment.
				else
				{
					++ptr;
				}

				// Counting padding bytes.
				while ((ptr < extensionEnd) && (*ptr == 0))
				{
					++ptr;
				}
			}
		}
		// Parse Two-Bytes header extension.
		else if (HasTwoBytesExtensions())
		{
			// Clear the Two-Bytes extension elements map.
			this->mapTwoBytesExtensions.clear();

			uint8_t* extensionStart = reinterpret_cast<uint8_t*>(this->headerExtension) + 4;
			uint8_t* extensionEnd   = extensionStart + GetHeaderExtensionLength();
			uint8_t* ptr            = extensionStart;

			// ptr points to the ID field (1 byte).
			// ptr+1 points to the length field (1 byte, can have value 0).

			// Two-Byte extensions can have length 0.
			while (ptr + 1 < extensionEnd)
			{
				uint8_t id  = *ptr;
				uint8_t len = *(ptr + 1);

				// Valid extension id.
				if (id != 0u)
				{
					if (ptr + 2 + len > extensionEnd)
					{
						MS_WARN_TAG(
						  rtp, "not enough space for the announced Two-Bytes header extension element value");

						break;
					}

					// Store the Two-Bytes extension element in the map.
					this->mapTwoBytesExtensions[id] = reinterpret_cast<TwoBytesExtension*>(ptr);

					ptr += (2 + len);
				}
				// id=0 means alignment.
				else
				{
					++ptr;
				}

				// Counting padding bytes.
				while ((ptr < extensionEnd) && (*ptr == 0))
				{
					++ptr;
				}
			}
		}
	}

	void RtpPacket::SetBuffer(uint8_t * buf)
	{
		MS_TRACE();

		MS_ASSERT(buffer == nullptr, "beffer already set");
		buffer = buf;
	}

  //   int DecodeContext::updateDefaultFrame(const uint32_t width, const uint32_t height)
  //   {
  //   	MS_TRACE();

		// defaultFrame.reset(av_frame_alloc());
		// defaultFrame->format = AV_PIX_FMT_YUV420P;
		// defaultFrame->width  = width; // c->width;
		// defaultFrame->height = height; // c->height;

		// int result = av_frame_get_buffer(defaultFrame.get(), 0);
		// if (result < 0)
		// {
		// 	defaultFrame.reset();
		// 	return result;
		// }

		// result = av_frame_make_writable(defaultFrame.get());
		// if (result < 0)
		// {
		// 	defaultFrame.reset();
		// 	return result;
		// }

		// // int i = 0; // random32(0) % 32;

		// i = (++i) % 256;

  //       // prepare a dummy image
  //       // Y
  //       for (uint32_t y = 0; y < height; ++y) 
  //       {
  //           for (uint32_t x = 0; x < width; ++x) 
  //           {
  //               defaultFrame->data[0][y * defaultFrame->linesize[0] + x] = x + y + i * 3; // 149;
  //           }
  //       }
  //       // Cb and Cr
  //       for (uint32_t y = 0; y < height/2; ++y) 
  //       {
  //           for (uint32_t x = 0; x < width/2; ++x) 
  //           {
  //               defaultFrame->data[1][y * defaultFrame->linesize[1] + x] = 128 + y + i * 2; // 128 + 84;
  //               defaultFrame->data[2][y * defaultFrame->linesize[2] + x] = 64 + x + i * 5; // 128 + 106;
  //           }
  //       }

		// return 0;
  //   }

    int EncodeContext::initContext(const uint32_t width, const uint32_t height)
    {
    	MS_TRACE();

		MS_WARN_TAG(dead, "init context %" PRIu32 "x%" PRIu32, width, height);

    	frameWidth  = width;
    	frameHeight = height;

    	totalPts    = 0;

		// TODO codec id must be variable ( from stream ?)
		codec        = avcodec_find_encoder(AV_CODEC_ID_H264);
		MS_ASSERT(codec, "no codec");

		MS_WARN_TAG(dead, "found codec %s %s", codec->name, codec->long_name);

		codecContext.reset(avcodec_alloc_context3(codec));
		MS_ASSERT(codecContext, "alloc context failed");

	    codecContext->width        = width;
	    codecContext->height       = height;
		codecContext->time_base    = (AVRational){1, 25};
		codecContext->pix_fmt      = AV_PIX_FMT_YUV420P;

        MS_WARN_TAG(dead, "codec params %dx%d timebase %d-%d", 
        			codecContext->width, codecContext->height,
        			codecContext->time_base.num, codecContext->time_base.den);

		// MS_ASSERT(av_opt_set(c.codecContext->priv_data, "preset", "ultrafast", AV_OPT_SEARCH_CHILDREN) == 0, "preset");
		// av_opt_set(c.codecContext->priv_data, "preset", "medium", 0);
		// av_opt_set(c.codecContext->priv_data, "tune",   "zerolatency", 0);
		// c.codecContext->level = ;

		// c.codecContext->coder_type = 1;
		// c.codecContext->me_subpel_quality = 7;
		// c.codecContext->me_range = 16;
		// c.codecContext->keyint_min = 25; 
		// c.codecContext->i_quant_factor = 0.71;
		// c.codecContext->b_frame_strategy = 1;
		// c.codecContext->qcompress    = .6;
		codecContext->max_b_frames      = 0;
		codecContext->refs              = 3;
		codecContext->gop_size          = 25;
		codecContext->thread_count      = 1;
		codecContext->delay             = 0;
		codecContext->me_subpel_quality = 4; 

		// sliced-threads:
		// quantizer=15:no-mbtree:sync-lookahead=0:rc-lookahead=0
		// bad option 'speed-preset': '2'
		// bad option 'dct8x8': 'true'
		// bad value for 'pass': 'qual'
		// bad option 'quantizer': '15' 
		// bad option 'key-int-max': '60'
		MS_ASSERT(av_opt_set(codecContext->priv_data, "x264opts", "sliced-threads:no-mbtree:sync-lookahead=0:rc-lookahead=0", 0) == 0, "x264opts");

		// MS_ASSERT(av_opt_set(c.codecContext.get(), "rc-lookahead",   "0", AV_OPT_SEARCH_CHILDREN) == 0, "rc-lookahead");
		// MS_ASSERT(av_opt_set(c.codecContext.get(), "b-frames",        "0", AV_OPT_SEARCH_CHILDREN) == 0, "bframes");
		// MS_ASSERT(av_opt_set(c.codecContext.get(), "sync-lookahead", "0", AV_OPT_SEARCH_CHILDREN) == 0, "sync-lookahead");

        int result = avcodec_open2(codecContext.get(), codec, nullptr);
        if (result < 0)
        {
        	char errstr[80];
            MS_WARN_TAG(dead, "codec not opened %x %s", result, av_make_error_string(errstr, 80, result));
        }
        else
        {
            MS_WARN_TAG(dead, "codec OK");
            isOpened = true;
        }

        jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        jpegContext.reset(avcodec_alloc_context3(jpegCodec));

	    jpegContext->width        = width;
	    jpegContext->height       = height;
	    jpegContext->time_base    = (AVRational){1, 25}; 
        jpegContext->pix_fmt = AV_PIX_FMT_YUVJ420P;

		result = avcodec_open2(jpegContext.get(), jpegCodec, NULL);
        if (result < 0)
        {
        	char errstr[80];
            MS_WARN_TAG(dead, "jpeg codec not opened %x %s", result, av_make_error_string(errstr, 80, result));
        }
        else
        {
            MS_WARN_TAG(dead, "jpeg codec OK");
        }

	    return result;
    }

} // namespace RTC
