/**
@file host.c
@brief SNet host management functions
*/
#define SNET_BUILDING_LIB 1
#include <string.h>
#include "snet/snet.h"

/** @defgroup host SNet host functions
@{
*/

/** Creates a host for communicating to peers.

@param address   the address at which other peers may connect to this host.  If NULL, then no peers may connect to the host.
@param peerCount the maximum number of peers that should be allocated for the host.
@param channelLimit the maximum number of channels allowed; if 0, then this is equivalent to SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT
@param incomingBandwidth downstream bandwidth of the host in bytes/second; if 0, SNet will assume unlimited bandwidth.
@param outgoingBandwidth upstream bandwidth of the host in bytes/second; if 0, SNet will assume unlimited bandwidth.

@returns the host on success and NULL on failure

@remarks SNet will strategically drop packets on specific sides of a connection between hosts
to ensure the host's bandwidth is not overwhelmed.  The bandwidth parameters also determine
the window size of a connection which limits the amount of reliable packets that may be in transit
at any given time.
*/
SNetHost *
snet_host_create(const SNetAddress * address, size_t peerCount, size_t channelLimit, snet_uint32 incomingBandwidth, snet_uint32 outgoingBandwidth)
{
	SNetHost * host;
	SNetPeer * currentPeer;

	if (peerCount > SNET_PROTOCOL_MAXIMUM_PEER_ID)
		return NULL;

	host = (SNetHost *)snet_malloc(sizeof(SNetHost));
	if (host == NULL)
		return NULL;
	memset(host, 0, sizeof(SNetHost));

	host->peers = (SNetPeer *)snet_malloc(peerCount * sizeof(SNetPeer));
	if (host->peers == NULL)
	{
		snet_free(host);

		return NULL;
	}
	memset(host->peers, 0, peerCount * sizeof(SNetPeer));

	host->socket = snet_socket_create(SNET_SOCKET_TYPE_DATAGRAM);
	if (host->socket == SNET_SOCKET_NULL || (address != NULL && snet_socket_bind(host->socket, address) < 0))
	{
		if (host->socket != SNET_SOCKET_NULL)
			snet_socket_destroy(host->socket);

		snet_free(host->peers);
		snet_free(host);

		return NULL;
	}

	snet_socket_set_option(host->socket, SNET_SOCKOPT_NONBLOCK, 1);
	snet_socket_set_option(host->socket, SNET_SOCKOPT_BROADCAST, 1);
	snet_socket_set_option(host->socket, SNET_SOCKOPT_RCVBUF, SNET_HOST_RECEIVE_BUFFER_SIZE);
	snet_socket_set_option(host->socket, SNET_SOCKOPT_SNDBUF, SNET_HOST_SEND_BUFFER_SIZE);

	if (address != NULL && snet_socket_get_address(host->socket, &host->address) < 0)
		host->address = *address;

	if (!channelLimit || channelLimit > SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
		channelLimit = SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT;
	else
		if (channelLimit < SNET_PROTOCOL_MINIMUM_CHANNEL_COUNT)
			channelLimit = SNET_PROTOCOL_MINIMUM_CHANNEL_COUNT;

	host->randomSeed = (snet_uint32)(size_t)host;
	host->randomSeed += snet_host_random_seed();
	host->randomSeed = (host->randomSeed << 16) | (host->randomSeed >> 16);
	host->channelLimit = channelLimit;
	host->incomingBandwidth = incomingBandwidth;
	host->outgoingBandwidth = outgoingBandwidth;
	host->bandwidthThrottleEpoch = 0;
	host->recalculateBandwidthLimits = 0;
	host->mtu = SNET_HOST_DEFAULT_MTU;
	host->peerCount = peerCount;
	host->commandCount = 0;
	host->bufferCount = 0;
	host->checksum = NULL;
	host->receivedAddress.host = SNET_HOST_ANY;
	host->receivedAddress.port = 0;
	host->receivedData = NULL;
	host->receivedDataLength = 0;

	host->totalSentData = 0;
	host->totalSentPackets = 0;
	host->totalReceivedData = 0;
	host->totalReceivedPackets = 0;

	host->connectedPeers = 0;
	host->bandwidthLimitedPeers = 0;
	host->duplicatePeers = SNET_PROTOCOL_MAXIMUM_PEER_ID;
	host->maximumPacketSize = SNET_HOST_DEFAULT_MAXIMUM_PACKET_SIZE;
	host->maximumWaitingData = SNET_HOST_DEFAULT_MAXIMUM_WAITING_DATA;

	host->compressor.context = NULL;
	host->compressor.compress = NULL;
	host->compressor.decompress = NULL;
	host->compressor.destroy = NULL;

	host->intercept = NULL;

	snet_list_clear(&host->dispatchQueue);

	for (currentPeer = host->peers;
		currentPeer < &host->peers[host->peerCount];
		++currentPeer)
	{
		currentPeer->host = host;
		currentPeer->incomingPeerID = currentPeer - host->peers;
		currentPeer->outgoingSessionID = currentPeer->incomingSessionID = 0xFF;
		currentPeer->data = NULL;

		snet_list_clear(&currentPeer->acknowledgements);
		snet_list_clear(&currentPeer->sentReliableCommands);
		snet_list_clear(&currentPeer->sentUnreliableCommands);
		snet_list_clear(&currentPeer->outgoingReliableCommands);
		snet_list_clear(&currentPeer->outgoingUnreliableCommands);
		snet_list_clear(&currentPeer->dispatchedCommands);

		snet_peer_reset(currentPeer);
	}

	return host;
}

/** Destroys the host and all resources associated with it.
@param host pointer to the host to destroy
*/
void
snet_host_destroy(SNetHost * host)
{
	SNetPeer * currentPeer;

	if (host == NULL)
		return;

	snet_socket_destroy(host->socket);

	for (currentPeer = host->peers;
		currentPeer < &host->peers[host->peerCount];
		++currentPeer)
	{
		snet_peer_reset(currentPeer);
	}

	if (host->compressor.context != NULL && host->compressor.destroy)
		(*host->compressor.destroy) (host->compressor.context);

	snet_free(host->peers);
	snet_free(host);
}

/** Initiates a connection to a foreign host.
@param host host seeking the connection
@param address destination for the connection
@param channelCount number of channels to allocate
@param data user data supplied to the receiving host
@returns a peer representing the foreign host on success, NULL on failure
@remarks The peer returned will have not completed the connection until snet_host_service()
notifies of an SNET_EVENT_TYPE_CONNECT event for the peer.
*/
SNetPeer *
snet_host_connect(SNetHost * host, const SNetAddress * address, size_t channelCount, snet_uint32 data)
{
	SNetPeer * currentPeer;
	SNetChannel * channel;
	SNetProtocol command;

	if (channelCount < SNET_PROTOCOL_MINIMUM_CHANNEL_COUNT)
		channelCount = SNET_PROTOCOL_MINIMUM_CHANNEL_COUNT;
	else
		if (channelCount > SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
			channelCount = SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT;

	for (currentPeer = host->peers;
		currentPeer < &host->peers[host->peerCount];
		++currentPeer)
	{
		if (currentPeer->state == SNET_PEER_STATE_DISCONNECTED)
			break;
	}

	if (currentPeer >= &host->peers[host->peerCount])
		return NULL;

	currentPeer->channels = (SNetChannel *)snet_malloc(channelCount * sizeof(SNetChannel));
	if (currentPeer->channels == NULL)
		return NULL;
	currentPeer->channelCount = channelCount;
	currentPeer->state = SNET_PEER_STATE_CONNECTING;
	currentPeer->address = *address;
	currentPeer->connectID = ++host->randomSeed;

	if (host->outgoingBandwidth == 0)
		currentPeer->windowSize = SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
	else
		currentPeer->windowSize = (host->outgoingBandwidth /
			SNET_PEER_WINDOW_SIZE_SCALE) *
		SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (currentPeer->windowSize < SNET_PROTOCOL_MINIMUM_WINDOW_SIZE)
		currentPeer->windowSize = SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else
		if (currentPeer->windowSize > SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
			currentPeer->windowSize = SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	for (channel = currentPeer->channels;
		channel < &currentPeer->channels[channelCount];
		++channel)
	{
		channel->outgoingReliableSequenceNumber = 0;
		channel->outgoingUnreliableSequenceNumber = 0;
		channel->incomingReliableSequenceNumber = 0;
		channel->incomingUnreliableSequenceNumber = 0;

		snet_list_clear(&channel->incomingReliableCommands);
		snet_list_clear(&channel->incomingUnreliableCommands);

		channel->usedReliableWindows = 0;
		memset(channel->reliableWindows, 0, sizeof(channel->reliableWindows));
	}

	command.header.command = SNET_PROTOCOL_COMMAND_CONNECT | SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
	command.header.channelID = 0xFF;
	command.connect.outgoingPeerID = SNET_HOST_TO_NET_16(currentPeer->incomingPeerID);
	command.connect.incomingSessionID = currentPeer->incomingSessionID;
	command.connect.outgoingSessionID = currentPeer->outgoingSessionID;
	command.connect.mtu = SNET_HOST_TO_NET_32(currentPeer->mtu);
	command.connect.windowSize = SNET_HOST_TO_NET_32(currentPeer->windowSize);
	command.connect.channelCount = SNET_HOST_TO_NET_32(channelCount);
	command.connect.incomingBandwidth = SNET_HOST_TO_NET_32(host->incomingBandwidth);
	command.connect.outgoingBandwidth = SNET_HOST_TO_NET_32(host->outgoingBandwidth);
	command.connect.packetThrottleInterval = SNET_HOST_TO_NET_32(currentPeer->packetThrottleInterval);
	command.connect.packetThrottleAcceleration = SNET_HOST_TO_NET_32(currentPeer->packetThrottleAcceleration);
	command.connect.packetThrottleDeceleration = SNET_HOST_TO_NET_32(currentPeer->packetThrottleDeceleration);
	command.connect.connectID = currentPeer->connectID;
	command.connect.data = SNET_HOST_TO_NET_32(data);

	snet_peer_queue_outgoing_command(currentPeer, &command, NULL, 0, 0);

	return currentPeer;
}

/** Queues a packet to be sent to all peers associated with the host.
@param host host on which to broadcast the packet
@param channelID channel on which to broadcast
@param packet packet to broadcast
*/
void
snet_host_broadcast(SNetHost * host, snet_uint8 channelID, SNetPacket * packet)
{
	SNetPeer * currentPeer;

	for (currentPeer = host->peers;
		currentPeer < &host->peers[host->peerCount];
		++currentPeer)
	{
		if (currentPeer->state != SNET_PEER_STATE_CONNECTED)
			continue;

		snet_peer_send(currentPeer, channelID, packet);
	}

	if (packet->referenceCount == 0)
		snet_packet_destroy(packet);
}

/** Sets the packet compressor the host should use to compress and decompress packets.
@param host host to enable or disable compression for
@param compressor callbacks for for the packet compressor; if NULL, then compression is disabled
*/
void
snet_host_compress(SNetHost * host, const SNetCompressor * compressor)
{
	if (host->compressor.context != NULL && host->compressor.destroy)
		(*host->compressor.destroy) (host->compressor.context);

	if (compressor)
		host->compressor = *compressor;
	else
		host->compressor.context = NULL;
}

/** Limits the maximum allowed channels of future incoming connections.
@param host host to limit
@param channelLimit the maximum number of channels allowed; if 0, then this is equivalent to SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT
*/
void
snet_host_channel_limit(SNetHost * host, size_t channelLimit)
{
	if (!channelLimit || channelLimit > SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
		channelLimit = SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT;
	else
		if (channelLimit < SNET_PROTOCOL_MINIMUM_CHANNEL_COUNT)
			channelLimit = SNET_PROTOCOL_MINIMUM_CHANNEL_COUNT;

	host->channelLimit = channelLimit;
}


/** Adjusts the bandwidth limits of a host.
@param host host to adjust
@param incomingBandwidth new incoming bandwidth
@param outgoingBandwidth new outgoing bandwidth
@remarks the incoming and outgoing bandwidth parameters are identical in function to those
specified in snet_host_create().
*/
void
snet_host_bandwidth_limit(SNetHost * host, snet_uint32 incomingBandwidth, snet_uint32 outgoingBandwidth)
{
	host->incomingBandwidth = incomingBandwidth;
	host->outgoingBandwidth = outgoingBandwidth;
	host->recalculateBandwidthLimits = 1;
}

void
snet_host_bandwidth_throttle(SNetHost * host)
{
	snet_uint32 timeCurrent = snet_time_get(),
		elapsedTime = timeCurrent - host->bandwidthThrottleEpoch,
		peersRemaining = (snet_uint32)host->connectedPeers,
		dataTotal = ~0,
		bandwidth = ~0,
		throttle = 0,
		bandwidthLimit = 0;
	int needsAdjustment = host->bandwidthLimitedPeers > 0 ? 1 : 0;
	SNetPeer * peer;
	SNetProtocol command;

	if (elapsedTime < SNET_HOST_BANDWIDTH_THROTTLE_INTERVAL)
		return;

	host->bandwidthThrottleEpoch = timeCurrent;

	if (peersRemaining == 0)
		return;

	if (host->outgoingBandwidth != 0)
	{
		dataTotal = 0;
		bandwidth = (host->outgoingBandwidth * elapsedTime) / 1000;

		for (peer = host->peers;
			peer < &host->peers[host->peerCount];
			++peer)
		{
			if (peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER)
				continue;

			dataTotal += peer->outgoingDataTotal;
		}
	}

	while (peersRemaining > 0 && needsAdjustment != 0)
	{
		needsAdjustment = 0;

		if (dataTotal <= bandwidth)
			throttle = SNET_PEER_PACKET_THROTTLE_SCALE;
		else
			throttle = (bandwidth * SNET_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

		for (peer = host->peers;
			peer < &host->peers[host->peerCount];
			++peer)
		{
			snet_uint32 peerBandwidth;

			if ((peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER) ||
				peer->incomingBandwidth == 0 ||
				peer->outgoingBandwidthThrottleEpoch == timeCurrent)
				continue;

			peerBandwidth = (peer->incomingBandwidth * elapsedTime) / 1000;
			if ((throttle * peer->outgoingDataTotal) / SNET_PEER_PACKET_THROTTLE_SCALE <= peerBandwidth)
				continue;

			peer->packetThrottleLimit = (peerBandwidth *
				SNET_PEER_PACKET_THROTTLE_SCALE) / peer->outgoingDataTotal;

			if (peer->packetThrottleLimit == 0)
				peer->packetThrottleLimit = 1;

			if (peer->packetThrottle > peer->packetThrottleLimit)
				peer->packetThrottle = peer->packetThrottleLimit;

			peer->outgoingBandwidthThrottleEpoch = timeCurrent;

			peer->incomingDataTotal = 0;
			peer->outgoingDataTotal = 0;

			needsAdjustment = 1;
			--peersRemaining;
			bandwidth -= peerBandwidth;
			dataTotal -= peerBandwidth;
		}
	}

	if (peersRemaining > 0)
	{
		if (dataTotal <= bandwidth)
			throttle = SNET_PEER_PACKET_THROTTLE_SCALE;
		else
			throttle = (bandwidth * SNET_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

		for (peer = host->peers;
			peer < &host->peers[host->peerCount];
			++peer)
		{
			if ((peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER) ||
				peer->outgoingBandwidthThrottleEpoch == timeCurrent)
				continue;

			peer->packetThrottleLimit = throttle;

			if (peer->packetThrottle > peer->packetThrottleLimit)
				peer->packetThrottle = peer->packetThrottleLimit;

			peer->incomingDataTotal = 0;
			peer->outgoingDataTotal = 0;
		}
	}

	if (host->recalculateBandwidthLimits)
	{
		host->recalculateBandwidthLimits = 0;

		peersRemaining = (snet_uint32)host->connectedPeers;
		bandwidth = host->incomingBandwidth;
		needsAdjustment = 1;

		if (bandwidth == 0)
			bandwidthLimit = 0;
		else
			while (peersRemaining > 0 && needsAdjustment != 0)
			{
				needsAdjustment = 0;
				bandwidthLimit = bandwidth / peersRemaining;

				for (peer = host->peers;
					peer < &host->peers[host->peerCount];
					++peer)
				{
					if ((peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER) ||
						peer->incomingBandwidthThrottleEpoch == timeCurrent)
						continue;

					if (peer->outgoingBandwidth > 0 &&
						peer->outgoingBandwidth >= bandwidthLimit)
						continue;

					peer->incomingBandwidthThrottleEpoch = timeCurrent;

					needsAdjustment = 1;
					--peersRemaining;
					bandwidth -= peer->outgoingBandwidth;
				}
			}

		for (peer = host->peers;
			peer < &host->peers[host->peerCount];
			++peer)
		{
			if (peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER)
				continue;

			command.header.command = SNET_PROTOCOL_COMMAND_BANDWIDTH_LIMIT | SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
			command.header.channelID = 0xFF;
			command.bandwidthLimit.outgoingBandwidth = SNET_HOST_TO_NET_32(host->outgoingBandwidth);

			if (peer->incomingBandwidthThrottleEpoch == timeCurrent)
				command.bandwidthLimit.incomingBandwidth = SNET_HOST_TO_NET_32(peer->outgoingBandwidth);
			else
				command.bandwidthLimit.incomingBandwidth = SNET_HOST_TO_NET_32(bandwidthLimit);

			snet_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);
		}
	}
}

/** @} */
