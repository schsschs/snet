/**
@file  protocol.h
@brief SNet protocol
*/
#ifndef __SNET_PROTOCOL_H__
#define __SNET_PROTOCOL_H__

#include "snet/types.h"

enum
{
	SNET_PROTOCOL_MINIMUM_MTU = 576,
	SNET_PROTOCOL_MAXIMUM_MTU = 4096,
	SNET_PROTOCOL_MAXIMUM_PACKET_COMMANDS = 32,
	SNET_PROTOCOL_MINIMUM_WINDOW_SIZE = 4096,
	SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE = 65536,
	SNET_PROTOCOL_MINIMUM_CHANNEL_COUNT = 1,
	SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT = 255,
	SNET_PROTOCOL_MAXIMUM_PEER_ID = 0xFFF,
	SNET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT = 1024 * 1024
};

typedef enum _SNetProtocolCommand
{
	SNET_PROTOCOL_COMMAND_NONE = 0,
	SNET_PROTOCOL_COMMAND_ACKNOWLEDGE = 1,
	SNET_PROTOCOL_COMMAND_CONNECT = 2,
	SNET_PROTOCOL_COMMAND_VERIFY_CONNECT = 3,
	SNET_PROTOCOL_COMMAND_DISCONNECT = 4,
	SNET_PROTOCOL_COMMAND_PING = 5,
	SNET_PROTOCOL_COMMAND_SEND_RELIABLE = 6,
	SNET_PROTOCOL_COMMAND_SEND_UNRELIABLE = 7,
	SNET_PROTOCOL_COMMAND_SEND_FRAGMENT = 8,
	SNET_PROTOCOL_COMMAND_SEND_UNSEQUENCED = 9,
	SNET_PROTOCOL_COMMAND_BANDWIDTH_LIMIT = 10,
	SNET_PROTOCOL_COMMAND_THROTTLE_CONFIGURE = 11,
	SNET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT = 12,
	SNET_PROTOCOL_COMMAND_COUNT = 13,

	SNET_PROTOCOL_COMMAND_MASK = 0x0F
} SNetProtocolCommand;

typedef enum _SNetProtocolFlag
{
	SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE = (1 << 7),
	SNET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED = (1 << 6),

	SNET_PROTOCOL_HEADER_FLAG_COMPRESSED = (1 << 14),
	SNET_PROTOCOL_HEADER_FLAG_SENT_TIME = (1 << 15),
	SNET_PROTOCOL_HEADER_FLAG_MASK = SNET_PROTOCOL_HEADER_FLAG_COMPRESSED | SNET_PROTOCOL_HEADER_FLAG_SENT_TIME,

	SNET_PROTOCOL_HEADER_SESSION_MASK = (3 << 12),
	SNET_PROTOCOL_HEADER_SESSION_SHIFT = 12
} SNetProtocolFlag;

#ifdef _MSC_VER
#pragma pack(push, 1)
#define SNET_PACKED
#elif defined(__GNUC__) || defined(__clang__)
#define SNET_PACKED __attribute__ ((packed))
#else
#define SNET_PACKED
#endif

typedef struct _SNetProtocolHeader
{
	snet_uint16 peerID;
	snet_uint16 sentTime;
} SNET_PACKED SNetProtocolHeader;

typedef struct _SNetProtocolCommandHeader
{
	snet_uint8 command;
	snet_uint8 channelID;
	snet_uint16 reliableSequenceNumber;
} SNET_PACKED SNetProtocolCommandHeader;

typedef struct _SNetProtocolAcknowledge
{
	SNetProtocolCommandHeader header;
	snet_uint16 receivedReliableSequenceNumber;
	snet_uint16 receivedSentTime;
} SNET_PACKED SNetProtocolAcknowledge;

typedef struct _SNetProtocolConnect
{
	SNetProtocolCommandHeader header;
	snet_uint16 outgoingPeerID;
	snet_uint8  incomingSessionID;
	snet_uint8  outgoingSessionID;
	snet_uint32 mtu;
	snet_uint32 windowSize;
	snet_uint32 channelCount;
	snet_uint32 incomingBandwidth;
	snet_uint32 outgoingBandwidth;
	snet_uint32 packetThrottleInterval;
	snet_uint32 packetThrottleAcceleration;
	snet_uint32 packetThrottleDeceleration;
	snet_uint32 connectID;
	snet_uint32 data;
} SNET_PACKED SNetProtocolConnect;

typedef struct _SNetProtocolVerifyConnect
{
	SNetProtocolCommandHeader header;
	snet_uint16 outgoingPeerID;
	snet_uint8  incomingSessionID;
	snet_uint8  outgoingSessionID;
	snet_uint32 mtu;
	snet_uint32 windowSize;
	snet_uint32 channelCount;
	snet_uint32 incomingBandwidth;
	snet_uint32 outgoingBandwidth;
	snet_uint32 packetThrottleInterval;
	snet_uint32 packetThrottleAcceleration;
	snet_uint32 packetThrottleDeceleration;
	snet_uint32 connectID;
} SNET_PACKED SNetProtocolVerifyConnect;

typedef struct _SNetProtocolBandwidthLimit
{
	SNetProtocolCommandHeader header;
	snet_uint32 incomingBandwidth;
	snet_uint32 outgoingBandwidth;
} SNET_PACKED SNetProtocolBandwidthLimit;

typedef struct _SNetProtocolThrottleConfigure
{
	SNetProtocolCommandHeader header;
	snet_uint32 packetThrottleInterval;
	snet_uint32 packetThrottleAcceleration;
	snet_uint32 packetThrottleDeceleration;
} SNET_PACKED SNetProtocolThrottleConfigure;

typedef struct _SNetProtocolDisconnect
{
	SNetProtocolCommandHeader header;
	snet_uint32 data;
} SNET_PACKED SNetProtocolDisconnect;

typedef struct _SNetProtocolPing
{
	SNetProtocolCommandHeader header;
} SNET_PACKED SNetProtocolPing;

typedef struct _SNetProtocolSendReliable
{
	SNetProtocolCommandHeader header;
	snet_uint16 dataLength;
} SNET_PACKED SNetProtocolSendReliable;

typedef struct _SNetProtocolSendUnreliable
{
	SNetProtocolCommandHeader header;
	snet_uint16 unreliableSequenceNumber;
	snet_uint16 dataLength;
} SNET_PACKED SNetProtocolSendUnreliable;

typedef struct _SNetProtocolSendUnsequenced
{
	SNetProtocolCommandHeader header;
	snet_uint16 unsequencedGroup;
	snet_uint16 dataLength;
} SNET_PACKED SNetProtocolSendUnsequenced;

typedef struct _SNetProtocolSendFragment
{
	SNetProtocolCommandHeader header;
	snet_uint16 startSequenceNumber;
	snet_uint16 dataLength;
	snet_uint32 fragmentCount;
	snet_uint32 fragmentNumber;
	snet_uint32 totalLength;
	snet_uint32 fragmentOffset;
} SNET_PACKED SNetProtocolSendFragment;

typedef union _SNetProtocol
{
	SNetProtocolCommandHeader header;
	SNetProtocolAcknowledge acknowledge;
	SNetProtocolConnect connect;
	SNetProtocolVerifyConnect verifyConnect;
	SNetProtocolDisconnect disconnect;
	SNetProtocolPing ping;
	SNetProtocolSendReliable sendReliable;
	SNetProtocolSendUnreliable sendUnreliable;
	SNetProtocolSendUnsequenced sendUnsequenced;
	SNetProtocolSendFragment sendFragment;
	SNetProtocolBandwidthLimit bandwidthLimit;
	SNetProtocolThrottleConfigure throttleConfigure;
} SNET_PACKED SNetProtocol;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

#endif /* __SNET_PROTOCOL_H__ */

