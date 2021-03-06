/**
@file  peer.c
@brief SNet peer management functions
*/
#include <string.h>
#define SNET_BUILDING_LIB 1
#include "snet/snet.h"

/** @defgroup peer SNet peer functions
@{
*/

/** Configures throttle parameter for a peer.

Unreliable packets are dropped by SNet in response to the varying conditions
of the Internet connection to the peer.  The throttle represents a probability
that an unreliable packet should not be dropped and thus sent by SNet to the peer.
The lowest mean round trip time from the sending of a reliable packet to the
receipt of its acknowledgement is measured over an amount of time specified by
the interval parameter in milliseconds.  If a measured round trip time happens to
be significantly less than the mean round trip time measured over the interval,
then the throttle probability is increased to allow more traffic by an amount
specified in the acceleration parameter, which is a ratio to the SNET_PEER_PACKET_THROTTLE_SCALE
constant.  If a measured round trip time happens to be significantly greater than
the mean round trip time measured over the interval, then the throttle probability
is decreased to limit traffic by an amount specified in the deceleration parameter, which
is a ratio to the SNET_PEER_PACKET_THROTTLE_SCALE constant.  When the throttle has
a value of SNET_PEER_PACKET_THROTTLE_SCALE, no unreliable packets are dropped by
SNet, and so 100% of all unreliable packets will be sent.  When the throttle has a
value of 0, all unreliable packets are dropped by SNet, and so 0% of all unreliable
packets will be sent.  Intermediate values for the throttle represent intermediate
probabilities between 0% and 100% of unreliable packets being sent.  The bandwidth
limits of the local and foreign hosts are taken into account to determine a
sensible limit for the throttle probability above which it should not raise even in
the best of conditions.

@param peer peer to configure
@param interval interval, in milliseconds, over which to measure lowest mean RTT; the default value is SNET_PEER_PACKET_THROTTLE_INTERVAL.
@param acceleration rate at which to increase the throttle probability as mean RTT declines
@param deceleration rate at which to decrease the throttle probability as mean RTT increases
*/
void
snet_peer_throttle_configure(SNetPeer * peer, snet_uint32 interval, snet_uint32 acceleration, snet_uint32 deceleration)
{
	SNetProtocol command;

	peer->packetThrottleInterval = interval;
	peer->packetThrottleAcceleration = acceleration;
	peer->packetThrottleDeceleration = deceleration;

	command.header.command = SNET_PROTOCOL_COMMAND_THROTTLE_CONFIGURE | SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
	command.header.channelID = 0xFF;

	command.throttleConfigure.packetThrottleInterval = SNET_HOST_TO_NET_32(interval);
	command.throttleConfigure.packetThrottleAcceleration = SNET_HOST_TO_NET_32(acceleration);
	command.throttleConfigure.packetThrottleDeceleration = SNET_HOST_TO_NET_32(deceleration);

	snet_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);
}

int
snet_peer_throttle(SNetPeer * peer, snet_uint32 rtt)
{
	if (peer->lastRoundTripTime <= peer->lastRoundTripTimeVariance)
	{
		peer->packetThrottle = peer->packetThrottleLimit;
	}
	else
		if (rtt < peer->lastRoundTripTime)
		{
			peer->packetThrottle += peer->packetThrottleAcceleration;

			if (peer->packetThrottle > peer->packetThrottleLimit)
				peer->packetThrottle = peer->packetThrottleLimit;

			return 1;
		}
		else
			if (rtt > peer->lastRoundTripTime + 2 * peer->lastRoundTripTimeVariance)
			{
				if (peer->packetThrottle > peer->packetThrottleDeceleration)
					peer->packetThrottle -= peer->packetThrottleDeceleration;
				else
					peer->packetThrottle = 0;

				return -1;
			}

	return 0;
}

/** Queues a packet to be sent.
@param peer destination for the packet
@param channelID channel on which to send
@param packet packet to send
@retval 0 on success
@retval < 0 on failure
*/
int
snet_peer_send(SNetPeer * peer, snet_uint8 channelID, SNetPacket * packet)
{
	SNetChannel * channel = &peer->channels[channelID];
	SNetProtocol command;
	size_t fragmentLength;

	if (peer->state != SNET_PEER_STATE_CONNECTED ||
		channelID >= peer->channelCount ||
		packet->dataLength > peer->host->maximumPacketSize)
		return -1;

	fragmentLength = peer->mtu - sizeof(SNetProtocolHeader) - sizeof(SNetProtocolSendFragment);
	if (peer->host->checksum != NULL)
		fragmentLength -= sizeof(snet_uint32);

	if (packet->dataLength > fragmentLength)
	{
		snet_uint32 fragmentCount = (packet->dataLength + fragmentLength - 1) / fragmentLength,
			fragmentNumber,
			fragmentOffset;
		snet_uint8 commandNumber;
		snet_uint16 startSequenceNumber;
		SNetList fragments;
		SNetOutgoingCommand * fragment;

		if (fragmentCount > SNET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
			return -1;

		if ((packet->flags & (SNET_PACKET_FLAG_RELIABLE | SNET_PACKET_FLAG_UNRELIABLE_FRAGMENT)) == SNET_PACKET_FLAG_UNRELIABLE_FRAGMENT &&
			channel->outgoingUnreliableSequenceNumber < 0xFFFF)
		{
			commandNumber = SNET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT;
			startSequenceNumber = SNET_HOST_TO_NET_16(channel->outgoingUnreliableSequenceNumber + 1);
		}
		else
		{
			commandNumber = SNET_PROTOCOL_COMMAND_SEND_FRAGMENT | SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
			startSequenceNumber = SNET_HOST_TO_NET_16(channel->outgoingReliableSequenceNumber + 1);
		}

		snet_list_clear(&fragments);

		for (fragmentNumber = 0,
			fragmentOffset = 0;
			fragmentOffset < packet->dataLength;
			++fragmentNumber,
			fragmentOffset += fragmentLength)
		{
			if (packet->dataLength - fragmentOffset < fragmentLength)
				fragmentLength = packet->dataLength - fragmentOffset;

			fragment = (SNetOutgoingCommand *)snet_malloc(sizeof(SNetOutgoingCommand));
			if (fragment == NULL)
			{
				while (!snet_list_empty(&fragments))
				{
					fragment = (SNetOutgoingCommand *)snet_list_remove(snet_list_begin(&fragments));

					snet_free(fragment);
				}

				return -1;
			}

			fragment->fragmentOffset = fragmentOffset;
			fragment->fragmentLength = fragmentLength;
			fragment->packet = packet;
			fragment->command.header.command = commandNumber;
			fragment->command.header.channelID = channelID;
			fragment->command.sendFragment.startSequenceNumber = startSequenceNumber;
			fragment->command.sendFragment.dataLength = SNET_HOST_TO_NET_16(fragmentLength);
			fragment->command.sendFragment.fragmentCount = SNET_HOST_TO_NET_32(fragmentCount);
			fragment->command.sendFragment.fragmentNumber = SNET_HOST_TO_NET_32(fragmentNumber);
			fragment->command.sendFragment.totalLength = SNET_HOST_TO_NET_32(packet->dataLength);
			fragment->command.sendFragment.fragmentOffset = SNET_NET_TO_HOST_32(fragmentOffset);

			snet_list_insert(snet_list_end(&fragments), fragment);
		}

		packet->referenceCount += fragmentNumber;

		while (!snet_list_empty(&fragments))
		{
			fragment = (SNetOutgoingCommand *)snet_list_remove(snet_list_begin(&fragments));

			snet_peer_setup_outgoing_command(peer, fragment);
		}

		return 0;
	}

	command.header.channelID = channelID;

	if ((packet->flags & (SNET_PACKET_FLAG_RELIABLE | SNET_PACKET_FLAG_UNSEQUENCED)) == SNET_PACKET_FLAG_UNSEQUENCED)
	{
		command.header.command = SNET_PROTOCOL_COMMAND_SEND_UNSEQUENCED | SNET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
		command.sendUnsequenced.dataLength = SNET_HOST_TO_NET_16(packet->dataLength);
	}
	else
		if (packet->flags & SNET_PACKET_FLAG_RELIABLE || channel->outgoingUnreliableSequenceNumber >= 0xFFFF)
		{
			command.header.command = SNET_PROTOCOL_COMMAND_SEND_RELIABLE | SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
			command.sendReliable.dataLength = SNET_HOST_TO_NET_16(packet->dataLength);
		}
		else
		{
			command.header.command = SNET_PROTOCOL_COMMAND_SEND_UNRELIABLE;
			command.sendUnreliable.dataLength = SNET_HOST_TO_NET_16(packet->dataLength);
		}

	if (snet_peer_queue_outgoing_command(peer, &command, packet, 0, packet->dataLength) == NULL)
		return -1;

	return 0;
}

/** Attempts to dequeue any incoming queued packet.
@param peer peer to dequeue packets from
@param channelID holds the channel ID of the channel the packet was received on success
@returns a pointer to the packet, or NULL if there are no available incoming queued packets
*/
SNetPacket *
snet_peer_receive(SNetPeer * peer, snet_uint8 * channelID)
{
	SNetIncomingCommand * incomingCommand;
	SNetPacket * packet;

	if (snet_list_empty(&peer->dispatchedCommands))
		return NULL;

	incomingCommand = (SNetIncomingCommand *)snet_list_remove(snet_list_begin(&peer->dispatchedCommands));

	if (channelID != NULL)
		* channelID = incomingCommand->command.header.channelID;

	packet = incomingCommand->packet;

	--packet->referenceCount;

	if (incomingCommand->fragments != NULL)
		snet_free(incomingCommand->fragments);

	snet_free(incomingCommand);

	peer->totalWaitingData -= packet->dataLength;

	return packet;
}

static void
snet_peer_reset_outgoing_commands(SNetList * queue)
{
	SNetOutgoingCommand * outgoingCommand;

	while (!snet_list_empty(queue))
	{
		outgoingCommand = (SNetOutgoingCommand *)snet_list_remove(snet_list_begin(queue));

		if (outgoingCommand->packet != NULL)
		{
			--outgoingCommand->packet->referenceCount;

			if (outgoingCommand->packet->referenceCount == 0)
				snet_packet_destroy(outgoingCommand->packet);
		}

		snet_free(outgoingCommand);
	}
}

static void
snet_peer_remove_incoming_commands(SNetList * queue, SNetListIterator startCommand, SNetListIterator endCommand)
{
	SNetListIterator currentCommand;

	for (currentCommand = startCommand; currentCommand != endCommand; )
	{
		SNetIncomingCommand * incomingCommand = (SNetIncomingCommand *)currentCommand;

		currentCommand = snet_list_next(currentCommand);

		snet_list_remove(&incomingCommand->incomingCommandList);

		if (incomingCommand->packet != NULL)
		{
			--incomingCommand->packet->referenceCount;

			if (incomingCommand->packet->referenceCount == 0)
				snet_packet_destroy(incomingCommand->packet);
		}

		if (incomingCommand->fragments != NULL)
			snet_free(incomingCommand->fragments);

		snet_free(incomingCommand);
	}
}

static void
snet_peer_reset_incoming_commands(SNetList * queue)
{
	snet_peer_remove_incoming_commands(queue, snet_list_begin(queue), snet_list_end(queue));
}

void
snet_peer_reset_queues(SNetPeer * peer)
{
	SNetChannel * channel;

	if (peer->needsDispatch)
	{
		snet_list_remove(&peer->dispatchList);

		peer->needsDispatch = 0;
	}

	while (!snet_list_empty(&peer->acknowledgements))
		snet_free(snet_list_remove(snet_list_begin(&peer->acknowledgements)));

	snet_peer_reset_outgoing_commands(&peer->sentReliableCommands);
	snet_peer_reset_outgoing_commands(&peer->sentUnreliableCommands);
	snet_peer_reset_outgoing_commands(&peer->outgoingReliableCommands);
	snet_peer_reset_outgoing_commands(&peer->outgoingUnreliableCommands);
	snet_peer_reset_incoming_commands(&peer->dispatchedCommands);

	if (peer->channels != NULL && peer->channelCount > 0)
	{
		for (channel = peer->channels;
			channel < &peer->channels[peer->channelCount];
			++channel)
		{
			snet_peer_reset_incoming_commands(&channel->incomingReliableCommands);
			snet_peer_reset_incoming_commands(&channel->incomingUnreliableCommands);
		}

		snet_free(peer->channels);
	}

	peer->channels = NULL;
	peer->channelCount = 0;
}

void
snet_peer_on_connect(SNetPeer * peer)
{
	if (peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER)
	{
		if (peer->incomingBandwidth != 0)
			++peer->host->bandwidthLimitedPeers;

		++peer->host->connectedPeers;
	}
}

void
snet_peer_on_disconnect(SNetPeer * peer)
{
	if (peer->state == SNET_PEER_STATE_CONNECTED || peer->state == SNET_PEER_STATE_DISCONNECT_LATER)
	{
		if (peer->incomingBandwidth != 0)
			--peer->host->bandwidthLimitedPeers;

		--peer->host->connectedPeers;
	}
}

/** Forcefully disconnects a peer.
@param peer peer to forcefully disconnect
@remarks The foreign host represented by the peer is not notified of the disconnection and will timeout
on its connection to the local host.
*/
void
snet_peer_reset(SNetPeer * peer)
{
	snet_peer_on_disconnect(peer);

	peer->outgoingPeerID = SNET_PROTOCOL_MAXIMUM_PEER_ID;
	peer->connectID = 0;

	peer->state = SNET_PEER_STATE_DISCONNECTED;

	peer->incomingBandwidth = 0;
	peer->outgoingBandwidth = 0;
	peer->incomingBandwidthThrottleEpoch = 0;
	peer->outgoingBandwidthThrottleEpoch = 0;
	peer->incomingDataTotal = 0;
	peer->outgoingDataTotal = 0;
	peer->lastSendTime = 0;
	peer->lastReceiveTime = 0;
	peer->nextTimeout = 0;
	peer->earliestTimeout = 0;
	peer->packetLossEpoch = 0;
	peer->packetsSent = 0;
	peer->packetsLost = 0;
	peer->packetLoss = 0;
	peer->packetLossVariance = 0;
	peer->packetThrottle = SNET_PEER_DEFAULT_PACKET_THROTTLE;
	peer->packetThrottleLimit = SNET_PEER_PACKET_THROTTLE_SCALE;
	peer->packetThrottleCounter = 0;
	peer->packetThrottleEpoch = 0;
	peer->packetThrottleAcceleration = SNET_PEER_PACKET_THROTTLE_ACCELERATION;
	peer->packetThrottleDeceleration = SNET_PEER_PACKET_THROTTLE_DECELERATION;
	peer->packetThrottleInterval = SNET_PEER_PACKET_THROTTLE_INTERVAL;
	peer->pingInterval = SNET_PEER_PING_INTERVAL;
	peer->timeoutLimit = SNET_PEER_TIMEOUT_LIMIT;
	peer->timeoutMinimum = SNET_PEER_TIMEOUT_MINIMUM;
	peer->timeoutMaximum = SNET_PEER_TIMEOUT_MAXIMUM;
	peer->lastRoundTripTime = SNET_PEER_DEFAULT_ROUND_TRIP_TIME;
	peer->lowestRoundTripTime = SNET_PEER_DEFAULT_ROUND_TRIP_TIME;
	peer->lastRoundTripTimeVariance = 0;
	peer->highestRoundTripTimeVariance = 0;
	peer->roundTripTime = SNET_PEER_DEFAULT_ROUND_TRIP_TIME;
	peer->roundTripTimeVariance = 0;
	peer->mtu = peer->host->mtu;
	peer->reliableDataInTransit = 0;
	peer->outgoingReliableSequenceNumber = 0;
	peer->windowSize = SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
	peer->incomingUnsequencedGroup = 0;
	peer->outgoingUnsequencedGroup = 0;
	peer->eventData = 0;
	peer->totalWaitingData = 0;

	memset(peer->unsequencedWindow, 0, sizeof(peer->unsequencedWindow));

	snet_peer_reset_queues(peer);
}

/** Sends a ping request to a peer.
@param peer destination for the ping request
@remarks ping requests factor into the mean round trip time as designated by the
roundTripTime field in the SNetPeer structure.  SNet automatically pings all connected
peers at regular intervals, however, this function may be called to ensure more
frequent ping requests.
*/
void
snet_peer_ping(SNetPeer * peer)
{
	SNetProtocol command;

	if (peer->state != SNET_PEER_STATE_CONNECTED)
		return;

	command.header.command = SNET_PROTOCOL_COMMAND_PING | SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
	command.header.channelID = 0xFF;

	snet_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);
}

/** Sets the interval at which pings will be sent to a peer.

Pings are used both to monitor the liveness of the connection and also to dynamically
adjust the throttle during periods of low traffic so that the throttle has reasonable
responsiveness during traffic spikes.

@param peer the peer to adjust
@param pingInterval the interval at which to send pings; defaults to SNET_PEER_PING_INTERVAL if 0
*/
void
snet_peer_ping_interval(SNetPeer * peer, snet_uint32 pingInterval)
{
	peer->pingInterval = pingInterval ? pingInterval : SNET_PEER_PING_INTERVAL;
}

/** Sets the timeout parameters for a peer.

The timeout parameter control how and when a peer will timeout from a failure to acknowledge
reliable traffic. Timeout values use an exponential backoff mechanism, where if a reliable
packet is not acknowledge within some multiple of the average RTT plus a variance tolerance,
the timeout will be doubled until it reaches a set limit. If the timeout is thus at this
limit and reliable packets have been sent but not acknowledged within a certain minimum time
period, the peer will be disconnected. Alternatively, if reliable packets have been sent
but not acknowledged for a certain maximum time period, the peer will be disconnected regardless
of the current timeout limit value.

@param peer the peer to adjust
@param timeoutLimit the timeout limit; defaults to SNET_PEER_TIMEOUT_LIMIT if 0
@param timeoutMinimum the timeout minimum; defaults to SNET_PEER_TIMEOUT_MINIMUM if 0
@param timeoutMaximum the timeout maximum; defaults to SNET_PEER_TIMEOUT_MAXIMUM if 0
*/

void
snet_peer_timeout(SNetPeer * peer, snet_uint32 timeoutLimit, snet_uint32 timeoutMinimum, snet_uint32 timeoutMaximum)
{
	peer->timeoutLimit = timeoutLimit ? timeoutLimit : SNET_PEER_TIMEOUT_LIMIT;
	peer->timeoutMinimum = timeoutMinimum ? timeoutMinimum : SNET_PEER_TIMEOUT_MINIMUM;
	peer->timeoutMaximum = timeoutMaximum ? timeoutMaximum : SNET_PEER_TIMEOUT_MAXIMUM;
}

/** Force an immediate disconnection from a peer.
@param peer peer to disconnect
@param data data describing the disconnection
@remarks No SNET_EVENT_DISCONNECT event will be generated. The foreign peer is not
guaranteed to receive the disconnect notification, and is reset immediately upon
return from this function.
*/
void
snet_peer_disconnect_now(SNetPeer * peer, snet_uint32 data)
{
	SNetProtocol command;

	if (peer->state == SNET_PEER_STATE_DISCONNECTED)
		return;

	if (peer->state != SNET_PEER_STATE_ZOMBIE &&
		peer->state != SNET_PEER_STATE_DISCONNECTING)
	{
		snet_peer_reset_queues(peer);

		command.header.command = SNET_PROTOCOL_COMMAND_DISCONNECT | SNET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
		command.header.channelID = 0xFF;
		command.disconnect.data = SNET_HOST_TO_NET_32(data);

		snet_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);

		snet_host_flush(peer->host);
	}

	snet_peer_reset(peer);
}

/** Request a disconnection from a peer.
@param peer peer to request a disconnection
@param data data describing the disconnection
@remarks An SNET_EVENT_DISCONNECT event will be generated by snet_host_service()
once the disconnection is complete.
*/
void
snet_peer_disconnect(SNetPeer * peer, snet_uint32 data)
{
	SNetProtocol command;

	if (peer->state == SNET_PEER_STATE_DISCONNECTING ||
		peer->state == SNET_PEER_STATE_DISCONNECTED ||
		peer->state == SNET_PEER_STATE_ACKNOWLEDGING_DISCONNECT ||
		peer->state == SNET_PEER_STATE_ZOMBIE)
		return;

	snet_peer_reset_queues(peer);

	command.header.command = SNET_PROTOCOL_COMMAND_DISCONNECT;
	command.header.channelID = 0xFF;
	command.disconnect.data = SNET_HOST_TO_NET_32(data);

	if (peer->state == SNET_PEER_STATE_CONNECTED || peer->state == SNET_PEER_STATE_DISCONNECT_LATER)
		command.header.command |= SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
	else
		command.header.command |= SNET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;

	snet_peer_queue_outgoing_command(peer, &command, NULL, 0, 0);

	if (peer->state == SNET_PEER_STATE_CONNECTED || peer->state == SNET_PEER_STATE_DISCONNECT_LATER)
	{
		snet_peer_on_disconnect(peer);

		peer->state = SNET_PEER_STATE_DISCONNECTING;
	}
	else
	{
		snet_host_flush(peer->host);
		snet_peer_reset(peer);
	}
}

/** Request a disconnection from a peer, but only after all queued outgoing packets are sent.
@param peer peer to request a disconnection
@param data data describing the disconnection
@remarks An SNET_EVENT_DISCONNECT event will be generated by snet_host_service()
once the disconnection is complete.
*/
void
snet_peer_disconnect_later(SNetPeer * peer, snet_uint32 data)
{
	if ((peer->state == SNET_PEER_STATE_CONNECTED || peer->state == SNET_PEER_STATE_DISCONNECT_LATER) &&
		!(snet_list_empty(&peer->outgoingReliableCommands) &&
			snet_list_empty(&peer->outgoingUnreliableCommands) &&
			snet_list_empty(&peer->sentReliableCommands)))
	{
		peer->state = SNET_PEER_STATE_DISCONNECT_LATER;
		peer->eventData = data;
	}
	else
		snet_peer_disconnect(peer, data);
}

SNetAcknowledgement *
snet_peer_queue_acknowledgement(SNetPeer * peer, const SNetProtocol * command, snet_uint16 sentTime)
{
	SNetAcknowledgement * acknowledgement;

	if (command->header.channelID < peer->channelCount)
	{
		SNetChannel * channel = &peer->channels[command->header.channelID];
		snet_uint16 reliableWindow = command->header.reliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE,
			currentWindow = channel->incomingReliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE;

		if (command->header.reliableSequenceNumber < channel->incomingReliableSequenceNumber)
			reliableWindow += SNET_PEER_RELIABLE_WINDOWS;

		if (reliableWindow >= currentWindow + SNET_PEER_FREE_RELIABLE_WINDOWS - 1 && reliableWindow <= currentWindow + SNET_PEER_FREE_RELIABLE_WINDOWS)
			return NULL;
	}

	acknowledgement = (SNetAcknowledgement *)snet_malloc(sizeof(SNetAcknowledgement));
	if (acknowledgement == NULL)
		return NULL;

	peer->outgoingDataTotal += sizeof(SNetProtocolAcknowledge);

	acknowledgement->sentTime = sentTime;
	acknowledgement->command = *command;

	snet_list_insert(snet_list_end(&peer->acknowledgements), acknowledgement);

	return acknowledgement;
}

void
snet_peer_setup_outgoing_command(SNetPeer * peer, SNetOutgoingCommand * outgoingCommand)
{
	SNetChannel * channel = &peer->channels[outgoingCommand->command.header.channelID];

	peer->outgoingDataTotal += snet_protocol_command_size(outgoingCommand->command.header.command) + outgoingCommand->fragmentLength;

	if (outgoingCommand->command.header.channelID == 0xFF)
	{
		++peer->outgoingReliableSequenceNumber;

		outgoingCommand->reliableSequenceNumber = peer->outgoingReliableSequenceNumber;
		outgoingCommand->unreliableSequenceNumber = 0;
	}
	else
		if (outgoingCommand->command.header.command & SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
		{
			++channel->outgoingReliableSequenceNumber;
			channel->outgoingUnreliableSequenceNumber = 0;

			outgoingCommand->reliableSequenceNumber = channel->outgoingReliableSequenceNumber;
			outgoingCommand->unreliableSequenceNumber = 0;
		}
		else
			if (outgoingCommand->command.header.command & SNET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED)
			{
				++peer->outgoingUnsequencedGroup;

				outgoingCommand->reliableSequenceNumber = 0;
				outgoingCommand->unreliableSequenceNumber = 0;
			}
			else
			{
				if (outgoingCommand->fragmentOffset == 0)
					++channel->outgoingUnreliableSequenceNumber;

				outgoingCommand->reliableSequenceNumber = channel->outgoingReliableSequenceNumber;
				outgoingCommand->unreliableSequenceNumber = channel->outgoingUnreliableSequenceNumber;
			}

	outgoingCommand->sendAttempts = 0;
	outgoingCommand->sentTime = 0;
	outgoingCommand->roundTripTimeout = 0;
	outgoingCommand->roundTripTimeoutLimit = 0;
	outgoingCommand->command.header.reliableSequenceNumber = SNET_HOST_TO_NET_16(outgoingCommand->reliableSequenceNumber);

	switch (outgoingCommand->command.header.command & SNET_PROTOCOL_COMMAND_MASK)
	{
	case SNET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
		outgoingCommand->command.sendUnreliable.unreliableSequenceNumber = SNET_HOST_TO_NET_16(outgoingCommand->unreliableSequenceNumber);
		break;

	case SNET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
		outgoingCommand->command.sendUnsequenced.unsequencedGroup = SNET_HOST_TO_NET_16(peer->outgoingUnsequencedGroup);
		break;

	default:
		break;
	}

	if (outgoingCommand->command.header.command & SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
		snet_list_insert(snet_list_end(&peer->outgoingReliableCommands), outgoingCommand);
	else
		snet_list_insert(snet_list_end(&peer->outgoingUnreliableCommands), outgoingCommand);
}

SNetOutgoingCommand *
snet_peer_queue_outgoing_command(SNetPeer * peer, const SNetProtocol * command, SNetPacket * packet, snet_uint32 offset, snet_uint16 length)
{
	SNetOutgoingCommand * outgoingCommand = (SNetOutgoingCommand *)snet_malloc(sizeof(SNetOutgoingCommand));
	if (outgoingCommand == NULL)
		return NULL;

	outgoingCommand->command = *command;
	outgoingCommand->fragmentOffset = offset;
	outgoingCommand->fragmentLength = length;
	outgoingCommand->packet = packet;
	if (packet != NULL)
		++packet->referenceCount;

	snet_peer_setup_outgoing_command(peer, outgoingCommand);

	return outgoingCommand;
}

void
snet_peer_dispatch_incoming_unreliable_commands(SNetPeer * peer, SNetChannel * channel)
{
	SNetListIterator droppedCommand, startCommand, currentCommand;

	for (droppedCommand = startCommand = currentCommand = snet_list_begin(&channel->incomingUnreliableCommands);
		currentCommand != snet_list_end(&channel->incomingUnreliableCommands);
		currentCommand = snet_list_next(currentCommand))
	{
		SNetIncomingCommand * incomingCommand = (SNetIncomingCommand *)currentCommand;

		if ((incomingCommand->command.header.command & SNET_PROTOCOL_COMMAND_MASK) == SNET_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
			continue;

		if (incomingCommand->reliableSequenceNumber == channel->incomingReliableSequenceNumber)
		{
			if (incomingCommand->fragmentsRemaining <= 0)
			{
				channel->incomingUnreliableSequenceNumber = incomingCommand->unreliableSequenceNumber;
				continue;
			}

			if (startCommand != currentCommand)
			{
				snet_list_move(snet_list_end(&peer->dispatchedCommands), startCommand, snet_list_previous(currentCommand));

				if (!peer->needsDispatch)
				{
					snet_list_insert(snet_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

					peer->needsDispatch = 1;
				}

				droppedCommand = currentCommand;
			}
			else
				if (droppedCommand != currentCommand)
					droppedCommand = snet_list_previous(currentCommand);
		}
		else
		{
			snet_uint16 reliableWindow = incomingCommand->reliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE,
				currentWindow = channel->incomingReliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE;
			if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
				reliableWindow += SNET_PEER_RELIABLE_WINDOWS;
			if (reliableWindow >= currentWindow && reliableWindow < currentWindow + SNET_PEER_FREE_RELIABLE_WINDOWS - 1)
				break;

			droppedCommand = snet_list_next(currentCommand);

			if (startCommand != currentCommand)
			{
				snet_list_move(snet_list_end(&peer->dispatchedCommands), startCommand, snet_list_previous(currentCommand));

				if (!peer->needsDispatch)
				{
					snet_list_insert(snet_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

					peer->needsDispatch = 1;
				}
			}
		}

		startCommand = snet_list_next(currentCommand);
	}

	if (startCommand != currentCommand)
	{
		snet_list_move(snet_list_end(&peer->dispatchedCommands), startCommand, snet_list_previous(currentCommand));

		if (!peer->needsDispatch)
		{
			snet_list_insert(snet_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

			peer->needsDispatch = 1;
		}

		droppedCommand = currentCommand;
	}

	snet_peer_remove_incoming_commands(&channel->incomingUnreliableCommands, snet_list_begin(&channel->incomingUnreliableCommands), droppedCommand);
}

void
snet_peer_dispatch_incoming_reliable_commands(SNetPeer * peer, SNetChannel * channel)
{
	SNetListIterator currentCommand;

	for (currentCommand = snet_list_begin(&channel->incomingReliableCommands);
		currentCommand != snet_list_end(&channel->incomingReliableCommands);
		currentCommand = snet_list_next(currentCommand))
	{
		SNetIncomingCommand * incomingCommand = (SNetIncomingCommand *)currentCommand;

		if (incomingCommand->fragmentsRemaining > 0 ||
			incomingCommand->reliableSequenceNumber != (snet_uint16)(channel->incomingReliableSequenceNumber + 1))
			break;

		channel->incomingReliableSequenceNumber = incomingCommand->reliableSequenceNumber;

		if (incomingCommand->fragmentCount > 0)
			channel->incomingReliableSequenceNumber += incomingCommand->fragmentCount - 1;
	}

	if (currentCommand == snet_list_begin(&channel->incomingReliableCommands))
		return;

	channel->incomingUnreliableSequenceNumber = 0;

	snet_list_move(snet_list_end(&peer->dispatchedCommands), snet_list_begin(&channel->incomingReliableCommands), snet_list_previous(currentCommand));

	if (!peer->needsDispatch)
	{
		snet_list_insert(snet_list_end(&peer->host->dispatchQueue), &peer->dispatchList);

		peer->needsDispatch = 1;
	}

	if (!snet_list_empty(&channel->incomingUnreliableCommands))
		snet_peer_dispatch_incoming_unreliable_commands(peer, channel);
}

SNetIncomingCommand *
snet_peer_queue_incoming_command(SNetPeer * peer, const SNetProtocol * command, const void * data, size_t dataLength, snet_uint32 flags, snet_uint32 fragmentCount)
{
	static SNetIncomingCommand dummyCommand;

	SNetChannel * channel = &peer->channels[command->header.channelID];
	snet_uint32 unreliableSequenceNumber = 0, reliableSequenceNumber = 0;
	snet_uint16 reliableWindow, currentWindow;
	SNetIncomingCommand * incomingCommand;
	SNetListIterator currentCommand;
	SNetPacket * packet = NULL;

	if (peer->state == SNET_PEER_STATE_DISCONNECT_LATER)
		goto discardCommand;

	if ((command->header.command & SNET_PROTOCOL_COMMAND_MASK) != SNET_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
	{
		reliableSequenceNumber = command->header.reliableSequenceNumber;
		reliableWindow = reliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE;
		currentWindow = channel->incomingReliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE;

		if (reliableSequenceNumber < channel->incomingReliableSequenceNumber)
			reliableWindow += SNET_PEER_RELIABLE_WINDOWS;

		if (reliableWindow < currentWindow || reliableWindow >= currentWindow + SNET_PEER_FREE_RELIABLE_WINDOWS - 1)
			goto discardCommand;
	}

	switch (command->header.command & SNET_PROTOCOL_COMMAND_MASK)
	{
	case SNET_PROTOCOL_COMMAND_SEND_FRAGMENT:
	case SNET_PROTOCOL_COMMAND_SEND_RELIABLE:
		if (reliableSequenceNumber == channel->incomingReliableSequenceNumber)
			goto discardCommand;

		for (currentCommand = snet_list_previous(snet_list_end(&channel->incomingReliableCommands));
			currentCommand != snet_list_end(&channel->incomingReliableCommands);
			currentCommand = snet_list_previous(currentCommand))
		{
			incomingCommand = (SNetIncomingCommand *)currentCommand;

			if (reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
			{
				if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
					continue;
			}
			else
				if (incomingCommand->reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
					break;

			if (incomingCommand->reliableSequenceNumber <= reliableSequenceNumber)
			{
				if (incomingCommand->reliableSequenceNumber < reliableSequenceNumber)
					break;

				goto discardCommand;
			}
		}
		break;

	case SNET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
	case SNET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
		unreliableSequenceNumber = SNET_NET_TO_HOST_16(command->sendUnreliable.unreliableSequenceNumber);

		if (reliableSequenceNumber == channel->incomingReliableSequenceNumber &&
			unreliableSequenceNumber <= channel->incomingUnreliableSequenceNumber)
			goto discardCommand;

		for (currentCommand = snet_list_previous(snet_list_end(&channel->incomingUnreliableCommands));
			currentCommand != snet_list_end(&channel->incomingUnreliableCommands);
			currentCommand = snet_list_previous(currentCommand))
		{
			incomingCommand = (SNetIncomingCommand *)currentCommand;

			if ((command->header.command & SNET_PROTOCOL_COMMAND_MASK) == SNET_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
				continue;

			if (reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
			{
				if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
					continue;
			}
			else
				if (incomingCommand->reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
					break;

			if (incomingCommand->reliableSequenceNumber < reliableSequenceNumber)
				break;

			if (incomingCommand->reliableSequenceNumber > reliableSequenceNumber)
				continue;

			if (incomingCommand->unreliableSequenceNumber <= unreliableSequenceNumber)
			{
				if (incomingCommand->unreliableSequenceNumber < unreliableSequenceNumber)
					break;

				goto discardCommand;
			}
		}
		break;

	case SNET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
		currentCommand = snet_list_end(&channel->incomingUnreliableCommands);
		break;

	default:
		goto discardCommand;
	}

	if (peer->totalWaitingData >= peer->host->maximumWaitingData)
		goto notifyError;

	packet = snet_packet_create(data, dataLength, flags);
	if (packet == NULL)
		goto notifyError;

	incomingCommand = (SNetIncomingCommand *)snet_malloc(sizeof(SNetIncomingCommand));
	if (incomingCommand == NULL)
		goto notifyError;

	incomingCommand->reliableSequenceNumber = command->header.reliableSequenceNumber;
	incomingCommand->unreliableSequenceNumber = unreliableSequenceNumber & 0xFFFF;
	incomingCommand->command = *command;
	incomingCommand->fragmentCount = fragmentCount;
	incomingCommand->fragmentsRemaining = fragmentCount;
	incomingCommand->packet = packet;
	incomingCommand->fragments = NULL;

	if (fragmentCount > 0)
	{
		if (fragmentCount <= SNET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
			incomingCommand->fragments = (snet_uint32 *)snet_malloc((fragmentCount + 31) / 32 * sizeof(snet_uint32));
		if (incomingCommand->fragments == NULL)
		{
			snet_free(incomingCommand);

			goto notifyError;
		}
		memset(incomingCommand->fragments, 0, (fragmentCount + 31) / 32 * sizeof(snet_uint32));
	}

	if (packet != NULL)
	{
		++packet->referenceCount;

		peer->totalWaitingData += packet->dataLength;
	}

	snet_list_insert(snet_list_next(currentCommand), incomingCommand);

	switch (command->header.command & SNET_PROTOCOL_COMMAND_MASK)
	{
	case SNET_PROTOCOL_COMMAND_SEND_FRAGMENT:
	case SNET_PROTOCOL_COMMAND_SEND_RELIABLE:
		snet_peer_dispatch_incoming_reliable_commands(peer, channel);
		break;

	default:
		snet_peer_dispatch_incoming_unreliable_commands(peer, channel);
		break;
	}

	return incomingCommand;

discardCommand:
	if (fragmentCount > 0)
		goto notifyError;

	if (packet != NULL && packet->referenceCount == 0)
		snet_packet_destroy(packet);

	return &dummyCommand;

notifyError:
	if (packet != NULL && packet->referenceCount == 0)
		snet_packet_destroy(packet);

	return NULL;
}

/** @} */
