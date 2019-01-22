//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "stun_message.h"

#include "../ice/ice_private.h"
#include "../ice/ice.h"
#include "attributes/stun_fingerprint_attribute.h"

#include <base/ovlibrary/ovlibrary.h>
#include <base/ovcrypto/ovcrypto.h>

static const uint32_t StunTypeMask = 0x0110;
static const size_t StunAttributeHeaderSize = 4;

// STUN Message Integrity HMAC length.
static const size_t StunMessageIntegritySize = 20;

StunMessage::StunMessage()
	: _parsed(false),

	  _type(0),
	  _message_length(0),
	  _magic_cookie(OV_STUN_MAGIC_COOKIE)
{
	::memset(_transaction_id, 0, sizeof(_transaction_id));
}

StunMessage::~StunMessage()
{
}

bool StunMessage::Parse(ov::ByteStream &stream)
{
	_parsed = true;

	// Fingerprint를 처리하는 과정에서 stream의 offset이 변경되므로, 별도의 stream 생성
	ov::ByteStream copied_stream(stream);

	// 헤더 길이 정보 분석 및 헤더를 먼저 읽음
	_parsed = _parsed && ParseHeader(stream);

	// STUN 패킷인지를 빠르게 판단하기 위해, finger print를 먼저 계산해봄 (CRC는 stream의 뒷 부분을 탐색하여 계산하기 때문에, 복사해놓음 stream을 사용함)
	_parsed = _parsed && ValidateFingerprint(copied_stream);

	// 나머지 attribute들 파싱
	_parsed = _parsed && ParseAttributes(stream);

	return _parsed;
}

bool StunMessage::ParseHeader(ov::ByteStream &stream)
{
	logtd("Trying to check STUN header length...");

	uint8_t fb;
	stream.Peek(&fb);

	/* First byte of STUN message is always 0x00 or 0x01. */
	if (fb != 0x00 && fb != 0x01)
	{
		// This is not a stun packet
		return false;
	}

	/*
	 * The message length MUST contain the size, in bytes, of the message
	 * not including the 20-byte STUN header. Since all STUN attributes are
	 * padded to a multiple of 4 bytes, the last 2 bits of this field are
	 * always zero. This provides another way to distinguish STUN packets
	 * from packets of other protocols. (RFC 5389)
	 */
	if ((stream.Remained() & OV_STUN_LENGTH_AND_VALUE) != 0)
	{
		// This is not a stun packet
		return false;
	}

	if((stream.Remained() >= DefaultHeaderLength()) == false)
	{
		// 데이터 부족 - STUN 메시지가 아님
		return false;
	}

	logtd("Trying to parse STUN header...");

	// RFC5389, section 6
	// All STUN messages MUST start with a 20-byte header followed by zero
	// or more Attributes.  The STUN header contains a STUN message type,
	// 		magic cookie, transaction ID, and message length.
	//
	// 0                   1                   2                   3
	// 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	// |0 0|     STUN Message Type     |         Message Length        |
	// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	// |                         Magic Cookie                          |
	// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	// |                                                               |
	// |                     Transaction ID (96 bits)                  |
	// |                                                               |
	// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	//
	// Figure 2: Format of STUN Message Header

	// byte ordering 때문에 하나씩 읽어서 구조체에 넣어줌
	SetType(stream.ReadBE16());

	// Message Length
	_message_length = stream.ReadBE16();
	// Magic Cookie
	_magic_cookie = stream.ReadBE32();

	if(_magic_cookie != OV_STUN_MAGIC_COOKIE)
	{
		// RFC5389에 의하면 magic cookie값은 항상 고정되어 있음

		// TODO: RFC3489 형태로 올 경우, magic cookie값이 일치하지 않을 수 있음. RFC3489는 나중에 구현 할 예정
		logtd("Invalid magic cookie found: %08X", _magic_cookie);
		return false;
	}

	// Transaction ID
	if(stream.Read<>(_transaction_id, OV_COUNTOF(_transaction_id)) != OV_COUNTOF(_transaction_id))
	{
		logtw("Could not read transaction ID");
		return false;
	}

	if(stream.Remained() < _message_length)
	{
		logtw("Message is too short: %d (expected: %d)", stream.Remained(), _message_length);
		return false;
	}

	logtd("STUN header is parsed successfully");
	return true;
}

bool StunMessage::ParseAttributes(ov::ByteStream &stream)
{
	// 정상적인 상황이라면 fingerprint attribute는 반드시 파싱되어 있어야 함
	OV_ASSERT2(_fingerprint_attribute != nullptr);

	// fingerprint attribute는 다른 attribute들 보다 먼저 계산되기 때문에 제외하고 계산
	int minimum_length = StunAttribute::DefaultHeaderSize() + _fingerprint_attribute->GetLength(true, false);

	// 파싱 가능한 최소한의 데이터가 발견되면 parsing 시작
	while(stream.Remained() >= minimum_length)
	{
		std::unique_ptr<StunAttribute> attribute = StunAttribute::CreateAttribute(stream);

		if(attribute == nullptr)
		{
			logtw("Could not create attribute");
			return false;
		}

		_attributes.push_back(std::move(attribute));
	}

	return true;
}

std::unique_ptr<StunAttribute> StunMessage::ParseFingerprintAttribute(ov::ByteStream &stream)
{
	// 최소한의 데이터가 있는지 확인
	// Fingerprint attribute header + CRC (4B)
	int fingerprint_attribute_size = StunAttribute::DefaultHeaderSize() + sizeof(uint32_t);

	if(stream.IsRemained(DefaultHeaderLength() + fingerprint_attribute_size) == false)
	{
		// stream에 충분한 데이터가 없음
		return nullptr;
	}

	// 맨 뒤에서 부터 fingerprint_attribute_size 지점에 fingerprint attribute가 있어야 함
	if(stream.SetOffset(stream.GetOffset() + stream.Remained() - fingerprint_attribute_size) == false)
	{
		return nullptr;
	}

	std::unique_ptr<StunAttribute> attribute = StunAttribute::CreateAttribute(stream);

	if((attribute != nullptr) && (attribute->GetType() != StunAttributeType::Fingerprint))
	{
		// 타입이 맞지 않음
		logtw("Last attribute IS NOT FINGER-PRINT attribute(type: %d)", attribute->GetType());

		return nullptr;
	}

	return std::move(attribute);
}

bool StunMessage::WriteHeader(ov::ByteStream &stream)
{
	logtd("Trying to write STUN header...");

	bool result = true;

	// Message Type
	result = result && stream.WriteBE16(_type);
	// Message Length
	result = result && WriteMessageLength(stream);
	// Magic Cookie
	result = result && stream.WriteBE32(OV_STUN_MAGIC_COOKIE);
	// Transaction ID
	result = result && stream.Write<>(_transaction_id, OV_COUNTOF(_transaction_id));

	if(result)
	{
		logtd("STUN header is written");
	}
	else
	{
		logtw("Could not write STUN header");
	}

	return result;
}

bool StunMessage::WriteMessageLength(ov::ByteStream &stream)
{
	// STUN 헤더 크기
	_message_length = 0;

	for(const auto &attribute : _attributes)
	{
		if(
			(attribute->GetType() == StunAttributeType::MessageIntegrity) ||
			(attribute->GetType() == StunAttributeType::Fingerprint)
			)
		{
			// 나중에 추가되는 attribute들
			continue;
		}

		int attribute_length = attribute->GetLength(true, true);
		_message_length += attribute_length;
	};

	stream.WriteBE16(_message_length);

	return true;
}

bool StunMessage::WriteAttributes(ov::ByteStream &stream)
{
	uint8_t padding[4] = { 0 };

	logtd("Trying to write attributes...");

	for(const auto &attribute : _attributes)
	{
		if(
			(attribute->GetType() == StunAttributeType::MessageIntegrity) ||
			(attribute->GetType() == StunAttributeType::Fingerprint)
			)
		{
			// 나중에 추가되는 attribute들
			continue;
		}

		if(attribute->Serialize(stream) == false)
		{
			return false;
		}

		ssize_t padding_count = 4 - ((attribute->GetLength(true, false)) % 4);

		if(padding_count < 4)
		{
			// 4의 배수가 아니므로, zero-padding
			stream.Write<uint8_t>(padding, padding_count);
		}
	}

	return true;
}

bool StunMessage::WriteMessageIntegrityAttribute(ov::ByteStream &stream, const ov::String &integrity_key)
{
	logtd("Trying to write message integrity attribute...");

	std::unique_ptr<StunMessageIntegrityAttribute> attribute = std::make_unique<StunMessageIntegrityAttribute>();

	// 지금까지의 데이터들을 모두 모아서 integrity hash 생성
	// RFC5389 - 15.4. MESSAGE-INTEGRITY
	// ...
	// The text used as input to HMAC is the STUN message,
	// including the header, up to and including the attribute preceding the
	// MESSAGE-INTEGRITY attribute.  With the exception of the FINGERPRINT
	// attribute, which appears after MESSAGE-INTEGRITY, agents MUST ignore
	// all other attributes that follow MESSAGE-INTEGRITY
	// ...

	// Hash를 계산할 땐, STUN 헤더의 length에 integrity attribute 길이가 포함되어 있어야 함
	uint8_t *buffer = (uint8_t *)(stream.GetData()->GetWritableData());
	uint16_t *length = (uint16_t *)(buffer + sizeof(uint16_t));
	int attribute_length = attribute->GetLength(true, true);

	_message_length += attribute_length;
	*length = ov::HostToNetwork16(_message_length);

	uint8_t hash[OV_STUN_HASH_LENGTH];

	bool result = true;

	logtd("Computing hmac: key: %s (current message length: %d)\n%s", integrity_key.CStr(), _message_length, stream.GetData()->Dump().CStr());

	result = result && ov::MessageDigest::ComputeHmac(ov::CryptoAlgorithm::Sha1,
	                                                  integrity_key.CStr(), integrity_key.GetLength(),
	                                                  stream.GetData()->GetData(), stream.GetData()->GetLength(),
	                                                  hash, OV_STUN_HASH_LENGTH);
	result = result && attribute->SetHash(hash);
	result = result && attribute->Serialize(stream);

	if(result)
	{
		_integrity_attribute = std::move(attribute);
	}

	return result;
}

bool StunMessage::WriteFingerprintAttribute(ov::ByteStream &stream)
{
	logtd("Trying to write fingerprint attribute...");

	std::unique_ptr<StunFingerprintAttribute> attribute = std::make_unique<StunFingerprintAttribute>();

	// Crc를 계산할 땐, STUN 헤더의 length에 fingerprint attribute 길이가 포함되어 있어야 함
	uint8_t *buffer = stream.GetData()->GetWritableDataAs<uint8_t>();
	uint16_t *length = reinterpret_cast<uint16_t *>(buffer + sizeof(uint16_t));
	int attribute_length = attribute->GetLength(true, true);

	_message_length += attribute_length;
	*length = ov::HostToNetwork16(_message_length);

	logtd("Calculating CRC for STUN message:\n%s", stream.GetData()->Dump().CStr());

	uint32_t crc = ov::Crc32::Calculate(stream.GetData());

	bool result = true;

	result = result && attribute->SetCrc(crc ^ OV_STUN_FINGERPRINT_XOR_VALUE);
	result = result && attribute->Serialize(stream);

	if(result)
	{
		_fingerprint_attribute = std::move(attribute);
	}

	return result;
}

bool StunMessage::CalculateFingerprint(ov::ByteStream &stream, ssize_t length, uint32_t *fingerprint) const
{
	uint32_t calculated = 0;
	const uint8_t *buffer = stream.Read<>();

	// 위에서 buffer 포인터에 현재 위치를 담아두었으므로, 나머지 데이터는 읽지 않게 skip
	if(stream.Skip(length) == false)
	{
		// 데이터가 부족함
		return false;
	}

	calculated = ov::Crc32::Calculate(buffer, length);

	// 계산한 CRC에 xoring
	calculated ^= OV_STUN_FINGERPRINT_XOR_VALUE;

	// 성공적으로 계산되었다면 CRC 반환을 위해 대입
	if(fingerprint != nullptr)
	{
		*fingerprint = calculated;
	}

	return true;
}

bool StunMessage::ValidateFingerprint(ov::ByteStream &stream)
{
	logtd("Trying to validate fingerprint...");

	// stream에서 fingerprint attribute를 얻어옴 (가장 마지막 attribute)
	ov::ByteStream fingerprint_stream(stream);
	_fingerprint_attribute = ParseFingerprintAttribute(fingerprint_stream);

	if(_fingerprint_attribute != nullptr)
	{
		// 성공적으로 fingerprint attribute를 얻어왔다면, CRC를 계산함

		// CRC 계산 범위:
		// sizeof(STUN header) + sum(sizeof(Attributes)) - sizeof(Fingerprint attribute)
		// 즉, stream에서 fingerprint attribute의 길이만큼 제외한 나머지 영역에 대해 CRC 계산
		uint32_t crc;
		ssize_t crc_length = stream.Remained() - (_fingerprint_attribute->GetLength(true, true));

#if STUN_LOG_DATA
		logtd("Trying to calculate crc for data:\n%s", stream.Dump(crc_length).CStr());
#else // STUN_LOG_DATA
		logtd("Trying to calculate crc...");
#endif // STUN_LOG_DATA

		if(CalculateFingerprint(stream, crc_length, &crc))
		{
			// stream 데이터에 대해 CRC 계산이 완료되었다면, fingerprint attribute의 CRC와 대조해봄
			uint32_t fingerprint = static_cast<const StunFingerprintAttribute *>(_fingerprint_attribute.get())->GetCrc();

			OV_ASSERT(crc == fingerprint, "Fingerprint DOES NOT MATCHED: Calculated CRC: %08X, Fingerprint attribute CRC: %08X", crc, fingerprint);

			if(crc == fingerprint)
			{
				logtd("Calculated CRC: %08X, Fingerprint attribute CRC: %08X", crc, fingerprint);
			}
			else
			{
				logtw("Fingerprint DOES NOT MATCHED: Calculated CRC: %08X, Fingerprint attribute CRC: %08X", crc, fingerprint);
			}

			return (crc == fingerprint);
		}
	}
	else
	{
		logtw("Could not find fingerprint attribute");
	}

	return false;
}

bool StunMessage::IsValid() const
{
	return _parsed && (_magic_cookie == OV_STUN_MAGIC_COOKIE);
}

bool StunMessage::CheckIntegrity(const ov::String &password) const
{
	// RFC 5389, section 15.4 참고
	return true;
}

bool StunMessage::GetUfrags(ov::String *first_ufrag, ov::String *second_ufrag) const
{
	const auto *user_name_attribute = GetAttribute<StunUserNameAttribute>(StunAttributeType::UserName);

	if(user_name_attribute == nullptr)
	{
		logtw("User name attribute not found");
		return false;
	}

	const ov::String &user_name = user_name_attribute->GetUserName();

	std::vector<ov::String> tokens = user_name.Split(":");

	if(tokens.size() != 2)
	{
		// token은 반드시 2개여야 함
		logtw("Invalid user name: %s", user_name.CStr());
		return false;
	}

	if(first_ufrag != nullptr)
	{
		*first_ufrag = tokens[0];
	}

	if(second_ufrag != nullptr)
	{
		*second_ufrag = tokens[1];
	}

	return true;
}

ov::String StunMessage::ToString() const
{
	ov::String dump;

	dump.AppendFormat("========== STUN Message (%p) ==========\n", this);

	dump.AppendFormat("Parsing status: %s\n", _parsed ? "Parsed" : "Not parsed");
	dump.AppendFormat("Message Type: 0x%04X\n", _type);
	dump.AppendFormat("    Class: %s (0x%02X)\n", GetClassString(), GetClass());
	dump.AppendFormat("    Method: %s (0x%04X)\n", GetMethodString(), GetMethod());
	dump.AppendFormat("Message Length: %d (0x%04X)\n", _message_length, _message_length);
	dump.AppendFormat("Magic Cookie: 0x%08X\n", _magic_cookie);
	dump.AppendFormat("Transaction ID: %s\n", ov::ToHexString(&(_transaction_id[0]), OV_COUNTOF(_transaction_id)).CStr());
	dump.AppendFormat("Attributes: %d items", _attributes.size());

	for(const auto &attribute : _attributes)
	{
		dump.AppendFormat("\n    %s", attribute->ToString().CStr());
	};

	if(_integrity_attribute != nullptr)
	{
		dump.AppendFormat("\n    %s", _integrity_attribute->ToString().CStr());
	}

	if(_fingerprint_attribute != nullptr)
	{
		dump.AppendFormat("\n    %s", _fingerprint_attribute->ToString().CStr());
	}

	return dump;
}

uint16_t StunMessage::GetType() const
{
	return _type;
}

void StunMessage::SetType(uint16_t type)
{
	// +--+--+--+--+--+--+--+--+--+--+--+--+--+--+
	// |13 12 11 10 09 08 07 06 05 04 03 02 01 00|
	// +--+--+--+--+--+--+--+--+--+--+--+--+--+--+
	// |M |M |M |M |M |C |M |M |M |C |M |M |M |M |
	// |11|10|9 |8 |7 |1 |6 |5 |4 |0 |3 |2 |1 |0 |
	// +--+--+--+--+--+--+--+--+--+--+--+--+--+--+
	// Figure 3: Format of STUN Message Type Field

	// Message Class
	_type = type;
}

StunClass StunMessage::GetClass() const
{
	// RFC5389 - A class of 0b00 is a request, a class of 0b01 is an indication, a class of 0b10 is a success response, and a class of 0b11 is an error response.
	return (StunClass)((OV_GET_BIT(_type, 8) << 1) | (OV_GET_BIT(_type, 4) << 0));
}

const char *StunMessage::GetClassString() const
{
	switch(GetClass())
	{
		case StunClass::Request:
			return "Request";

		case StunClass::Indication:
			return "Indication";

		case StunClass::SuccessResponse:
			return "SuccessResponse";

		case StunClass::ErrorResponse:
			return "ErrorResponse";
	}

	return "(Unknown)";
}

void StunMessage::SetClass(StunClass clazz)
{
	uint16_t class_value = ((OV_GET_BIT((uint16_t)clazz, 1) << 8) | (OV_GET_BIT((uint16_t)clazz, 0) << 4));

	// class 쪽 bit 초기화
	// STUN Message Type에서, M 부분만 남기기 위한 bit_mask = 0b11111011101111 (0x3EEF)
	_type = (_type & 0b11111011101111) | class_value;
}

StunMethod StunMessage::GetMethod() const
{
	return (StunMethod)(
		// 13~09 번 째 bit 모음
		((_type & 0b0011111000000000) >> 2) |
		// 07~05 번 째 bit 모음
		((_type & 0b0000000011100000) >> 1) |
		// 03~00 번 째 bit 모음
		((_type & 0b0000000000001111) >> 0)
	);
}

const char *StunMessage::GetMethodString() const
{
	switch(GetMethod())
	{
		case StunMethod::Binding:
			return "Binding";
	}

	return "(Unknown)";
}

void StunMessage::SetMethod(StunMethod method)
{
	//   BA9876543210 =>   DCBA9876543210
	// 0bXXXXXYYYZZZZ => 0bXXXXX0YYY0ZZZZ 로 변환해야 함
	uint16_t method_value =
		// BA987 => DCBA9
		(OV_GET_BITS(uint16_t, method, 7, 5) << 2) |
		// 654 => 765
		(OV_GET_BITS(uint16_t, method, 4, 3) << 1) |
		// 3210 => 3210
		OV_GET_BITS(uint16_t, method, 0, 3);

	// class 쪽 bit 초기화
	// STUN Message Type에서, C 부분만 남기기 위한 bit_mask = 0b00000100010000 (0x0110)
	_type = (_type & 0b00000100010000) | method_value;
}

uint16_t StunMessage::GetMessageLength() const
{
	return _message_length;
}

uint32_t StunMessage::GetMagicCookie() const
{
	return _magic_cookie;
}

// 길이: OV_STUN_TRANSACTION_ID_LENGTH
const uint8_t *StunMessage::GetTransactionId() const
{
	return _transaction_id;
}

void StunMessage::SetTransactionId(const uint8_t transaction_id[OV_STUN_TRANSACTION_ID_LENGTH])
{
	::memcpy(_transaction_id, transaction_id, sizeof(_transaction_id));
}

bool StunMessage::AddAttribute(std::unique_ptr<StunAttribute> attribute)
{
	if(_fingerprint_attribute != nullptr)
	{
		// Serialize 한 뒤에는 메시지를 수정할 수 없음
		OV_ASSERT(false, "Could not add attribute to serialized message");
		return false;
	}

	if(attribute->GetType() == StunAttributeType::Fingerprint)
	{
		// Fingerprint attribute는 가장 마지막에 계산 가능
		OV_ASSERT(false, "Fingerprint attribute will generate automatically");
		return false;
	}

	_attributes.push_back(std::move(attribute));

	return true;
}

std::shared_ptr<ov::Data> StunMessage::Serialize(const ov::String &integrity_key)
{
	std::shared_ptr<ov::Data> data = std::make_shared<ov::Data>();
	ov::ByteStream stream(data.get());

	bool result = true;

	// 헤더 기록
	result = result && WriteHeader(stream);

	// attribute들 기록
	result = result && WriteAttributes(stream);

	// Integrity attribute 기록
	result = result && WriteMessageIntegrityAttribute(stream, integrity_key);

	// Fingerprint attribute 기록
	result = result && WriteFingerprintAttribute(stream);

	return result ? data : nullptr;
}
