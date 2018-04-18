/**
@file  packet.c
@brief SNet packet management functions
*/
#include <string.h>
#define SNET_BUILDING_LIB 1
#include "snet/snet.h"

/** @defgroup Packet SNet packet functions
@{
*/

/** Creates a packet that may be sent to a peer.
@param data         initial contents of the packet's data; the packet's data will remain uninitialized if data is NULL.
@param dataLength   size of the data allocated for this packet
@param flags        flags for this packet as described for the SNetPacket structure.
@returns the packet on success, NULL on failure
*/
SNetPacket *
snet_packet_create(const void * data, size_t dataLength, snet_uint32 flags)
{
	SNetPacket * packet = (SNetPacket *)snet_malloc(sizeof(SNetPacket));
	if (packet == NULL)
		return NULL;

	if (flags & SNET_PACKET_FLAG_NO_ALLOCATE)
		packet->data = (snet_uint8 *)data;
	else
		if (dataLength <= 0)
			packet->data = NULL;
		else
		{
			packet->data = (snet_uint8 *)snet_malloc(dataLength);
			if (packet->data == NULL)
			{
				snet_free(packet);
				return NULL;
			}

			if (data != NULL)
				memcpy(packet->data, data, dataLength);
		}

	packet->referenceCount = 0;
	packet->flags = flags;
	packet->dataLength = dataLength;
	packet->freeCallback = NULL;
	packet->userData = NULL;

	return packet;
}

/** Destroys the packet and deallocates its data.
@param packet packet to be destroyed
*/
void
snet_packet_destroy(SNetPacket * packet)
{
	if (packet == NULL)
		return;

	if (packet->freeCallback != NULL)
		(*packet->freeCallback) (packet);
	if (!(packet->flags & SNET_PACKET_FLAG_NO_ALLOCATE) &&
		packet->data != NULL)
		snet_free(packet->data);
	snet_free(packet);
}

/** Attempts to resize the data in the packet to length specified in the
dataLength parameter
@param packet packet to resize
@param dataLength new size for the packet data
@returns 0 on success, < 0 on failure
*/
int
snet_packet_resize(SNetPacket * packet, size_t dataLength)
{
	snet_uint8 * newData;

	if (dataLength <= packet->dataLength || (packet->flags & SNET_PACKET_FLAG_NO_ALLOCATE))
	{
		packet->dataLength = dataLength;

		return 0;
	}

	newData = (snet_uint8 *)snet_malloc(dataLength);
	if (newData == NULL)
		return -1;

	memcpy(newData, packet->data, packet->dataLength);
	snet_free(packet->data);

	packet->data = newData;
	packet->dataLength = dataLength;

	return 0;
}

static int initializedCRC32 = 0;
static snet_uint32 crcTable[256];

static snet_uint32
reflect_crc(int val, int bits)
{
	int result = 0, bit;

	for (bit = 0; bit < bits; bit++)
	{
		if (val & 1) result |= 1 << (bits - 1 - bit);
		val >>= 1;
	}

	return result;
}

static void
initialize_crc32(void)
{
	int byte;

	for (byte = 0; byte < 256; ++byte)
	{
		snet_uint32 crc = reflect_crc(byte, 8) << 24;
		int offset;

		for (offset = 0; offset < 8; ++offset)
		{
			if (crc & 0x80000000)
				crc = (crc << 1) ^ 0x04c11db7;
			else
				crc <<= 1;
		}

		crcTable[byte] = reflect_crc(crc, 32);
	}

	initializedCRC32 = 1;
}

snet_uint32
snet_crc32(const SNetBuffer * buffers, size_t bufferCount)
{
	snet_uint32 crc = 0xFFFFFFFF;

	if (!initializedCRC32) initialize_crc32();

	while (bufferCount-- > 0)
	{
		const snet_uint8 * data = (const snet_uint8 *)buffers->data,
			*dataEnd = &data[buffers->dataLength];

		while (data < dataEnd)
		{
			crc = (crc >> 8) ^ crcTable[(crc & 0xFF) ^ *data++];
		}

		++buffers;
	}

	return SNET_HOST_TO_NET_32(~crc);
}

/** @} */
