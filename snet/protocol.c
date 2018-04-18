/**
@file  protocol.c
@brief SNet protocol functions
*/
#include <stdio.h>
#include <string.h>
#define SNET_BUILDING_LIB 1
#include "snet/utility.h"
#include "snet/time.h"
#include "snet/snet.h"

static size_t commandSizes[SNET_PROTOCOL_COMMAND_COUNT] =
{
	0,
	sizeof(SNetProtocolAcknowledge),
	sizeof(SNetProtocolConnect),
	sizeof(SNetProtocolVerifyConnect),
	sizeof(SNetProtocolDisconnect),
	sizeof(SNetProtocolPing),
	sizeof(SNetProtocolSendReliable),
	sizeof(SNetProtocolSendUnreliable),
	sizeof(SNetProtocolSendFragment),
	sizeof(SNetProtocolSendUnsequenced),
	sizeof(SNetProtocolBandwidthLimit),
	sizeof(SNetProtocolThrottleConfigure),
	sizeof(SNetProtocolSendFragment)
};

size_t
snet_protocol_command_size(snet_uint8 commandNumber)
{
	return commandSizes[commandNumber & SNET_PROTOCOL_COMMAND_MASK];
}

static void
snet_protocol_change_state(SNetHost * host, SNetPeer * peer, SNetPeerState state)
{
	if (state == SNET_PEER_STATE_CONNECTED || state == SNET_PEER_STATE_DISCONNECT_LATER)
		snet_peer_on_connect(peer);
	else
		snet_peer_on_disconnect(peer);

	peer->state = state;
}

static void
snet_protocol_dispatch_state(SNetHost * host, SNetPeer * peer, SNetPeerState state)
{
	snet_protocol_change_state(host, peer, state);

	if (!peer->needsDispatch)
	{
		snet_list_insert(snet_list_end(&host->dispatchQueue), &peer->dispatchList);

		peer->needsDispatch = 1;
	}
}

static int
snet_protocol_dispatch_incoming_commands(SNetHost * host, SNetEvent * event)
{
	while (!snet_list_empty(&host->dispatchQueue))
	{
		SNetPeer * peer = (SNetPeer *)snet_list_remove(snet_list_begin(&host->dispatchQueue));

		peer->needsDispatch = 0;

		switch (peer->state)
		{
		case SNET_PEER_STATE_CONNECTION_PENDING:
		case SNET_PEER_STATE_CONNECTION_SUCCEEDED:
			snet_protocol_change_state(host, peer, SNET_PEER_STATE_CONNECTED);

			event->type = SNET_EVENT_TYPE_CONNECT;
			event->peer = peer;
			event->data = peer->eventData;

			return 1;

		case SNET_PEER_STATE_ZOMBIE:
			host->recalculateBandwidthLimits = 1;

			event->type = SNET_EVENT_TYPE_DISCONNECT;
			event->peer = peer;
			event->data = peer->eventData;

			snet_peer_reset(peer);

			return 1;

		case SNET_PEER_STATE_CONNECTED:
			if (snet_list_empty(&peer->dispatchedCommands))
				continue;

			event->packet = snet_peer_receive(peer, &event->channelID);
			if (event->packet == NULL)
				continue;

			event->type = SNET_EVENT_TYPE_RECEIVE;
			event->peer = peer;

			if (!snet_list_empty(&peer->dispatchedCommands))
			{
				peer->needsDispatch = 1;

				snet_list_insert(snet_list_end(&host->dispatchQueue), &peer->dispatchList);
			}

			return 1;

		default:
			break;
		}
	}

	return 0;
}

static void
snet_protocol_notify_connect(SNetHost * host, SNetPeer * peer, SNetEvent * event)
{
	host->recalculateBandwidthLimits = 1;

	if (event != NULL)
	{
		snet_protocol_change_state(host, peer, SNET_PEER_STATE_CONNECTED);

		event->type = SNET_EVENT_TYPE_CONNECT;
		event->peer = peer;
		event->data = peer->eventData;
	}
	else
		snet_protocol_dispatch_state(host, peer, peer->state == SNET_PEER_STATE_CONNECTING ? SNET_PEER_STATE_CONNECTION_SUCCEEDED : SNET_PEER_STATE_CONNECTION_PENDING);
}

static void
snet_protocol_notify_disconnect(SNetHost * host, SNetPeer * peer, SNetEvent * event)
{
	if (peer->state >= SNET_PEER_STATE_CONNECTION_PENDING)
		host->recalculateBandwidthLimits = 1;

	if (peer->state != SNET_PEER_STATE_CONNECTING && peer->state < SNET_PEER_STATE_CONNECTION_SUCCEEDED)
		snet_peer_reset(peer);
	else
		if (event != NULL)
		{
			event->type = SNET_EVENT_TYPE_DISCONNECT;
			event->peer = peer;
			event->data = 0;

			snet_peer_reset(peer);
		}
		else
		{
			peer->eventData = 0;

			snet_protocol_dispatch_state(host, peer, SNET_PEER_STATE_ZOMBIE);
		}
}

static void
snet_protocol_remove_sent_unreliable_commands(SNetPeer * peer)
{
	SNetOutgoingCommand * outgoingCommand;

	while (!snet_list_empty(&peer->sentUnreliableCommands))
	{
		outgoingCommand = (SNetOutgoingCommand *)snet_list_front(&peer->sentUnreliableCommands);

		snet_list_remove(&outgoingCommand->outgoingCommandList);

		if (outgoingCommand->packet != NULL)
		{
			--outgoingCommand->packet->referenceCount;

			if (outgoingCommand->packet->referenceCount == 0)
			{
				outgoingCommand->packet->flags |= SNET_PACKET_FLAG_SENT;

				snet_packet_destroy(outgoingCommand->packet);
			}
		}

		snet_free(outgoingCommand);
	}
}

static SNetProtocolCommand
snet_protocol_remove_sent_reliable_command(SNetPeer * peer, snet_uint16 reliableSequenceNumber, snet_uint8 channelID)
{
	SNetOutgoingCommand * outgoingCommand = NULL;
	SNetListIterator currentCommand;
	SNetProtocolCommand commandNumber;
	int wasSent = 1;

	for (currentCommand = snet_list_begin(&peer->sentReliableCommands);
		currentCommand != snet_list_end(&peer->sentReliableCommands);
		currentCommand = snet_list_next(currentCommand))
	{
		outgoingCommand = (SNetOutgoingCommand *)currentCommand;

		if (outgoingCommand->reliableSequenceNumber == reliableSequenceNumber &&
			outgoingCommand->command.header.channelID == channelID)
			break;
	}

	if (currentCommand == snet_list_end(&peer->sentReliableCommands))
	{
		for (currentCommand = snet_list_begin(&peer->outgoingReliableCommands);
			currentCommand != snet_list_end(&peer->outgoingReliableCommands);
			currentCommand = snet_list_next(currentCommand))
		{
			outgoingCommand = (SNetOutgoingCommand *)currentCommand;

			if (outgoingCommand->sendAttempts < 1) return SNET_PROTOCOL_COMMAND_NONE;

			if (outgoingCommand->reliableSequenceNumber == reliableSequenceNumber &&
				outgoingCommand->command.header.channelID == channelID)
				break;
		}

		if (currentCommand == snet_list_end(&peer->outgoingReliableCommands))
			return SNET_PROTOCOL_COMMAND_NONE;

		wasSent = 0;
	}

	if (outgoingCommand == NULL)
		return SNET_PROTOCOL_COMMAND_NONE;

	if (channelID < peer->channelCount)
	{
		SNetChannel * channel = &peer->channels[channelID];
		snet_uint16 reliableWindow = reliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE;
		if (channel->reliableWindows[reliableWindow] > 0)
		{
			--channel->reliableWindows[reliableWindow];
			if (!channel->reliableWindows[reliableWindow])
				channel->usedReliableWindows &= ~(1 << reliableWindow);
		}
	}

	commandNumber = (SNetProtocolCommand)(outgoingCommand->command.header.command & SNET_PROTOCOL_COMMAND_MASK);

	snet_list_remove(&outgoingCommand->outgoingCommandList);

	if (outgoingCommand->packet != NULL)
	{
		if (wasSent)
			peer->reliableDataInTransit -= outgoingCommand->fragmentLength;

		--outgoingCommand->packet->referenceCount;

		if (outgoingCommand->packet->referenceCount == 0)
		{
			outgoingCommand->packet->flags |= SNET_PACKET_FLAG_SENT;

			snet_packet_destroy(outgoingCommand->packet);
		}
	}

	snet_free(outgoingCommand);

	if (snet_list_empty(&peer->sentReliableCommands))
		return commandNumber;

	outgoingCommand = (SNetOutgoingCommand *)snet_list_front(&peer->sentReliableCommands);

	peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;

	return commandNumber;
}

static SNetPeer *
snet_protocol_handle_connect(SNetHost * host, SNetProtocolHeader * header, SNetProtocol * command)
{
	snet_uint8 incomingSessionID, outgoingSessionID;
	snet_uint32 mtu, windowSize;
	SNetChannel * channel;
	size_t channelCount, duplicatePeers = 0;
	SNetPeer * currentPeer, *peer = NULL;
	SNetProtocol verifyCommand;

	channelCount = SNET_NET_TO_HOST_32(command->connect.channelCount);

	if (channelCount < SNET_PROTOCOL_MINIMUM_CHANNEL_COUNT ||
		channelCount > SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
		return NULL;

	for (currentPeer = host->peers;
		currentPeer < &host->peers[host->peerCount];
		++currentPeer)
	{
		if (currentPeer->state == SNET_PEER_STATE_DISCONNECTED)
		{
			if (peer == NULL)
				peer = currentPeer;
		}
		else
			if (currentPeer->state != SNET_PEER_STATE_CONNECTING &&
				currentPeer->address.host == host->receivedAddress.host)
			{
				if (currentPeer->address.port == host->receivedAddress.port &&
					currentPeer->connectID == command->connect.connectID)
					return NULL;

				++duplicatePeers;
			}
	}

	if (peer == NULL || duplicatePeers >= host->duplicatePeers)
		return NULL;

	if (channelCount > host->channelLimit)
		channelCount = host->channelLimit;
	peer->channels = (SNetChannel *)snet_malloc(channelCount * sizeof(SNetChannel));
	if (peer->channels == NULL)
		return NULL;
	peer->channelCount = channelCount;
	peer->state = SNET_PEER_STATE_ACKNOWLEDGING_CONNECT;
	peer->connectID = command->connect.connectID;
	peer->address = host->receivedAddress;
	peer->outgoingPeerID = SNET_NET_TO_HOST_16(command->connect.outgoingPeerID);
	peer->incomingBandwidth = SNET_NET_TO_HOST_32(command->connect.incomingBandwidth);
	peer->outgoingBandwidth = SNET_NET_TO_HOST_32(command->connect.outgoingBandwidth);
	peer->packetThrottleInterval = SNET_NET_TO_HOST_32(command->connect.packetThrottleInterval);
	peer->packetThrottleAcceleration = SNET_NET_TO_HOST_32(command->connect.packetThrottleAcceleration);
	peer->packetThrottleDeceleration = SNET_NET_TO_HOST_32(command->connect.packetThrottleDeceleration);
	peer->eventData = SNET_NET_TO_HOST_32(command->connect.data);

	incomingSessionID = command->connect.incomingSessionID == 0xFF ? peer->outgoingSessionID : command->connect.incomingSessionID;
	incomingSessionID = (incomingSessionID + 1) & (SNET_PROTOCOL_HEADER_SESSION_MASK >> SNET_PROTOCOL_HEADER_SESSION_SHIFT);
	if (incomingSessionID == peer->outgoingSessionID)
		incomingSessionID = (incomingSessionID + 1) & (SNET_PROTOCOL_HEADER_SESSION_MASK >> SNET_PROTOCOL_HEADER_SESSION_SHIFT);
	peer->outgoingSessionID = incomingSessionID;

	outgoingSessionID = command->connect.outgoingSessionID == 0xFF ? peer->incomingSessionID : command->connect.outgoingSessionID;
	outgoingSessionID = (outgoingSessionID + 1) & (SNET_PROTOCOL_HEADER_SESSION_MASK >> SNET_PROTOCOL_HEADER_SESSION_SHIFT);
	if (outgoingSessionID == peer->incomingSessionID)
		outgoingSessionID = (outgoingSessionID + 1) & (SNET_PROTOCOL_HEADER_SESSION_MASK >> SNET_PROTOCOL_HEADER_SESSION_SHIFT);
	peer->incomingSessionID = outgoingSessionID;

	for (channel = peer->channels;
		channel < &peer->channels[channelCount];
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

	mtu = SNET_NET_TO_HOST_32(command->connect.mtu);

	if (mtu < SNET_PROTOCOL_MINIMUM_MTU)
		mtu = SNET_PROTOCOL_MINIMUM_MTU;
	else
		if (mtu > SNET_PROTOCOL_MAXIMUM_MTU)
			mtu = SNET_PROTOCOL_MAXIMUM_MTU;

	peer->mtu = mtu;

	if (host->outgoingBandwidth == 0 &&
		peer->incomingBandwidth == 0)
		peer->windowSize = SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
	else
		if (host->outgoingBandwidth == 0 ||
			peer->incomingBandwidth == 0)
			peer->windowSize = (SNET_MAX(host->outgoingBandwidth, peer->incomingBandwidth) /
				SNET_PEER_WINDOW_SIZE_SCALE) *
			SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;
		else
			peer->windowSize = (SNET_MIN(host->outgoingBandwidth, peer->incomingBandwidth) /
				SNET_PEER_WINDOW_SIZE_SCALE) *
			SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (peer->windowSize < SNET_PROTOCOL_MINIMUM_WINDOW_SIZE)
		peer->windowSize = SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else
		if (peer->windowSize > SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
			peer->windowSize = SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	if (host->incomingBandwidth == 0)
		windowSize = SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
	else
		windowSize = (host->incomingBandwidth / SNET_PEER_WINDOW_SIZE_SCALE) *
		SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (windowSize > SNET_NET_TO_HOST_32(command->connect.windowSize))
		windowSize = SNET_NET_TO_HOST_32(command->connect.windowSize);

	if (windowSize < SNET_PROTOCOL_MINIMUM_WINDOW_SIZE)
		windowSize = SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else
		if (windowSize > SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
			windowSize = SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	verifyCommand.header.command = SNET_PROTOCOL_COMMAND_VERIFY_CONNECT | SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
	verifyCommand.header.channelID = 0xFF;
	verifyCommand.verifyConnect.outgoingPeerID = SNET_HOST_TO_NET_16(peer->incomingPeerID);
	verifyCommand.verifyConnect.incomingSessionID = incomingSessionID;
	verifyCommand.verifyConnect.outgoingSessionID = outgoingSessionID;
	verifyCommand.verifyConnect.mtu = SNET_HOST_TO_NET_32(peer->mtu);
	verifyCommand.verifyConnect.windowSize = SNET_HOST_TO_NET_32(windowSize);
	verifyCommand.verifyConnect.channelCount = SNET_HOST_TO_NET_32(channelCount);
	verifyCommand.verifyConnect.incomingBandwidth = SNET_HOST_TO_NET_32(host->incomingBandwidth);
	verifyCommand.verifyConnect.outgoingBandwidth = SNET_HOST_TO_NET_32(host->outgoingBandwidth);
	verifyCommand.verifyConnect.packetThrottleInterval = SNET_HOST_TO_NET_32(peer->packetThrottleInterval);
	verifyCommand.verifyConnect.packetThrottleAcceleration = SNET_HOST_TO_NET_32(peer->packetThrottleAcceleration);
	verifyCommand.verifyConnect.packetThrottleDeceleration = SNET_HOST_TO_NET_32(peer->packetThrottleDeceleration);
	verifyCommand.verifyConnect.connectID = peer->connectID;

	snet_peer_queue_outgoing_command(peer, &verifyCommand, NULL, 0, 0);

	return peer;
}

static int
snet_protocol_handle_send_reliable(SNetHost * host, SNetPeer * peer, const SNetProtocol * command, snet_uint8 ** currentData)
{
	size_t dataLength;

	if (command->header.channelID >= peer->channelCount ||
		(peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER))
		return -1;

	dataLength = SNET_NET_TO_HOST_16(command->sendReliable.dataLength);
	*currentData += dataLength;
	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	if (snet_peer_queue_incoming_command(peer, command, (const snet_uint8 *)command + sizeof(SNetProtocolSendReliable), dataLength, SNET_PACKET_FLAG_RELIABLE, 0) == NULL)
		return -1;

	return 0;
}

static int
snet_protocol_handle_send_unsequenced(SNetHost * host, SNetPeer * peer, const SNetProtocol * command, snet_uint8 ** currentData)
{
	snet_uint32 unsequencedGroup, index;
	size_t dataLength;

	if (command->header.channelID >= peer->channelCount ||
		(peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER))
		return -1;

	dataLength = SNET_NET_TO_HOST_16(command->sendUnsequenced.dataLength);
	*currentData += dataLength;
	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	unsequencedGroup = SNET_NET_TO_HOST_16(command->sendUnsequenced.unsequencedGroup);
	index = unsequencedGroup % SNET_PEER_UNSEQUENCED_WINDOW_SIZE;

	if (unsequencedGroup < peer->incomingUnsequencedGroup)
		unsequencedGroup += 0x10000;

	if (unsequencedGroup >= (snet_uint32)peer->incomingUnsequencedGroup + SNET_PEER_FREE_UNSEQUENCED_WINDOWS * SNET_PEER_UNSEQUENCED_WINDOW_SIZE)
		return 0;

	unsequencedGroup &= 0xFFFF;

	if (unsequencedGroup - index != peer->incomingUnsequencedGroup)
	{
		peer->incomingUnsequencedGroup = unsequencedGroup - index;

		memset(peer->unsequencedWindow, 0, sizeof(peer->unsequencedWindow));
	}
	else
		if (peer->unsequencedWindow[index / 32] & (1 << (index % 32)))
			return 0;

	if (snet_peer_queue_incoming_command(peer, command, (const snet_uint8 *)command + sizeof(SNetProtocolSendUnsequenced), dataLength, SNET_PACKET_FLAG_UNSEQUENCED, 0) == NULL)
		return -1;

	peer->unsequencedWindow[index / 32] |= 1 << (index % 32);

	return 0;
}

static int
snet_protocol_handle_send_unreliable(SNetHost * host, SNetPeer * peer, const SNetProtocol * command, snet_uint8 ** currentData)
{
	size_t dataLength;

	if (command->header.channelID >= peer->channelCount ||
		(peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER))
		return -1;

	dataLength = SNET_NET_TO_HOST_16(command->sendUnreliable.dataLength);
	*currentData += dataLength;
	if (dataLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	if (snet_peer_queue_incoming_command(peer, command, (const snet_uint8 *)command + sizeof(SNetProtocolSendUnreliable), dataLength, 0, 0) == NULL)
		return -1;

	return 0;
}

static int
snet_protocol_handle_send_fragment(SNetHost * host, SNetPeer * peer, const SNetProtocol * command, snet_uint8 ** currentData)
{
	snet_uint32 fragmentNumber,
		fragmentCount,
		fragmentOffset,
		fragmentLength,
		startSequenceNumber,
		totalLength;
	SNetChannel * channel;
	snet_uint16 startWindow, currentWindow;
	SNetListIterator currentCommand;
	SNetIncomingCommand * startCommand = NULL;

	if (command->header.channelID >= peer->channelCount ||
		(peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER))
		return -1;

	fragmentLength = SNET_NET_TO_HOST_16(command->sendFragment.dataLength);
	*currentData += fragmentLength;
	if (fragmentLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	channel = &peer->channels[command->header.channelID];
	startSequenceNumber = SNET_NET_TO_HOST_16(command->sendFragment.startSequenceNumber);
	startWindow = startSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE;
	currentWindow = channel->incomingReliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE;

	if (startSequenceNumber < channel->incomingReliableSequenceNumber)
		startWindow += SNET_PEER_RELIABLE_WINDOWS;

	if (startWindow < currentWindow || startWindow >= currentWindow + SNET_PEER_FREE_RELIABLE_WINDOWS - 1)
		return 0;

	fragmentNumber = SNET_NET_TO_HOST_32(command->sendFragment.fragmentNumber);
	fragmentCount = SNET_NET_TO_HOST_32(command->sendFragment.fragmentCount);
	fragmentOffset = SNET_NET_TO_HOST_32(command->sendFragment.fragmentOffset);
	totalLength = SNET_NET_TO_HOST_32(command->sendFragment.totalLength);

	if (fragmentCount > SNET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT ||
		fragmentNumber >= fragmentCount ||
		totalLength > host->maximumPacketSize ||
		fragmentOffset >= totalLength ||
		fragmentLength > totalLength - fragmentOffset)
		return -1;

	for (currentCommand = snet_list_previous(snet_list_end(&channel->incomingReliableCommands));
		currentCommand != snet_list_end(&channel->incomingReliableCommands);
		currentCommand = snet_list_previous(currentCommand))
	{
		SNetIncomingCommand * incomingCommand = (SNetIncomingCommand *)currentCommand;

		if (startSequenceNumber >= channel->incomingReliableSequenceNumber)
		{
			if (incomingCommand->reliableSequenceNumber < channel->incomingReliableSequenceNumber)
				continue;
		}
		else
			if (incomingCommand->reliableSequenceNumber >= channel->incomingReliableSequenceNumber)
				break;

		if (incomingCommand->reliableSequenceNumber <= startSequenceNumber)
		{
			if (incomingCommand->reliableSequenceNumber < startSequenceNumber)
				break;

			if ((incomingCommand->command.header.command & SNET_PROTOCOL_COMMAND_MASK) != SNET_PROTOCOL_COMMAND_SEND_FRAGMENT ||
				totalLength != incomingCommand->packet->dataLength ||
				fragmentCount != incomingCommand->fragmentCount)
				return -1;

			startCommand = incomingCommand;
			break;
		}
	}

	if (startCommand == NULL)
	{
		SNetProtocol hostCommand = *command;

		hostCommand.header.reliableSequenceNumber = startSequenceNumber;

		startCommand = snet_peer_queue_incoming_command(peer, &hostCommand, NULL, totalLength, SNET_PACKET_FLAG_RELIABLE, fragmentCount);
		if (startCommand == NULL)
			return -1;
	}

	if ((startCommand->fragments[fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0)
	{
		--startCommand->fragmentsRemaining;

		startCommand->fragments[fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

		if (fragmentOffset + fragmentLength > startCommand->packet->dataLength)
			fragmentLength = startCommand->packet->dataLength - fragmentOffset;

		memcpy(startCommand->packet->data + fragmentOffset,
			(snet_uint8 *)command + sizeof(SNetProtocolSendFragment),
			fragmentLength);

		if (startCommand->fragmentsRemaining <= 0)
			snet_peer_dispatch_incoming_reliable_commands(peer, channel);
	}

	return 0;
}

static int
snet_protocol_handle_send_unreliable_fragment(SNetHost * host, SNetPeer * peer, const SNetProtocol * command, snet_uint8 ** currentData)
{
	snet_uint32 fragmentNumber,
		fragmentCount,
		fragmentOffset,
		fragmentLength,
		reliableSequenceNumber,
		startSequenceNumber,
		totalLength;
	snet_uint16 reliableWindow, currentWindow;
	SNetChannel * channel;
	SNetListIterator currentCommand;
	SNetIncomingCommand * startCommand = NULL;

	if (command->header.channelID >= peer->channelCount ||
		(peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER))
		return -1;

	fragmentLength = SNET_NET_TO_HOST_16(command->sendFragment.dataLength);
	*currentData += fragmentLength;
	if (fragmentLength > host->maximumPacketSize ||
		*currentData < host->receivedData ||
		*currentData > & host->receivedData[host->receivedDataLength])
		return -1;

	channel = &peer->channels[command->header.channelID];
	reliableSequenceNumber = command->header.reliableSequenceNumber;
	startSequenceNumber = SNET_NET_TO_HOST_16(command->sendFragment.startSequenceNumber);

	reliableWindow = reliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE;
	currentWindow = channel->incomingReliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE;

	if (reliableSequenceNumber < channel->incomingReliableSequenceNumber)
		reliableWindow += SNET_PEER_RELIABLE_WINDOWS;

	if (reliableWindow < currentWindow || reliableWindow >= currentWindow + SNET_PEER_FREE_RELIABLE_WINDOWS - 1)
		return 0;

	if (reliableSequenceNumber == channel->incomingReliableSequenceNumber &&
		startSequenceNumber <= channel->incomingUnreliableSequenceNumber)
		return 0;

	fragmentNumber = SNET_NET_TO_HOST_32(command->sendFragment.fragmentNumber);
	fragmentCount = SNET_NET_TO_HOST_32(command->sendFragment.fragmentCount);
	fragmentOffset = SNET_NET_TO_HOST_32(command->sendFragment.fragmentOffset);
	totalLength = SNET_NET_TO_HOST_32(command->sendFragment.totalLength);

	if (fragmentCount > SNET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT ||
		fragmentNumber >= fragmentCount ||
		totalLength > host->maximumPacketSize ||
		fragmentOffset >= totalLength ||
		fragmentLength > totalLength - fragmentOffset)
		return -1;

	for (currentCommand = snet_list_previous(snet_list_end(&channel->incomingUnreliableCommands));
		currentCommand != snet_list_end(&channel->incomingUnreliableCommands);
		currentCommand = snet_list_previous(currentCommand))
	{
		SNetIncomingCommand * incomingCommand = (SNetIncomingCommand *)currentCommand;

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

		if (incomingCommand->unreliableSequenceNumber <= startSequenceNumber)
		{
			if (incomingCommand->unreliableSequenceNumber < startSequenceNumber)
				break;

			if ((incomingCommand->command.header.command & SNET_PROTOCOL_COMMAND_MASK) != SNET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT ||
				totalLength != incomingCommand->packet->dataLength ||
				fragmentCount != incomingCommand->fragmentCount)
				return -1;

			startCommand = incomingCommand;
			break;
		}
	}

	if (startCommand == NULL)
	{
		startCommand = snet_peer_queue_incoming_command(peer, command, NULL, totalLength, SNET_PACKET_FLAG_UNRELIABLE_FRAGMENT, fragmentCount);
		if (startCommand == NULL)
			return -1;
	}

	if ((startCommand->fragments[fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0)
	{
		--startCommand->fragmentsRemaining;

		startCommand->fragments[fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

		if (fragmentOffset + fragmentLength > startCommand->packet->dataLength)
			fragmentLength = startCommand->packet->dataLength - fragmentOffset;

		memcpy(startCommand->packet->data + fragmentOffset,
			(snet_uint8 *)command + sizeof(SNetProtocolSendFragment),
			fragmentLength);

		if (startCommand->fragmentsRemaining <= 0)
			snet_peer_dispatch_incoming_unreliable_commands(peer, channel);
	}

	return 0;
}

static int
snet_protocol_handle_ping(SNetHost * host, SNetPeer * peer, const SNetProtocol * command)
{
	if (peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER)
		return -1;

	return 0;
}

static int
snet_protocol_handle_bandwidth_limit(SNetHost * host, SNetPeer * peer, const SNetProtocol * command)
{
	if (peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER)
		return -1;

	if (peer->incomingBandwidth != 0)
		--host->bandwidthLimitedPeers;

	peer->incomingBandwidth = SNET_NET_TO_HOST_32(command->bandwidthLimit.incomingBandwidth);
	peer->outgoingBandwidth = SNET_NET_TO_HOST_32(command->bandwidthLimit.outgoingBandwidth);

	if (peer->incomingBandwidth != 0)
		++host->bandwidthLimitedPeers;

	if (peer->incomingBandwidth == 0 && host->outgoingBandwidth == 0)
		peer->windowSize = SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
	else
		if (peer->incomingBandwidth == 0 || host->outgoingBandwidth == 0)
			peer->windowSize = (SNET_MAX(peer->incomingBandwidth, host->outgoingBandwidth) /
				SNET_PEER_WINDOW_SIZE_SCALE) * SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;
		else
			peer->windowSize = (SNET_MIN(peer->incomingBandwidth, host->outgoingBandwidth) /
				SNET_PEER_WINDOW_SIZE_SCALE) * SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (peer->windowSize < SNET_PROTOCOL_MINIMUM_WINDOW_SIZE)
		peer->windowSize = SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;
	else
		if (peer->windowSize > SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
			peer->windowSize = SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	return 0;
}

static int
snet_protocol_handle_throttle_configure(SNetHost * host, SNetPeer * peer, const SNetProtocol * command)
{
	if (peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER)
		return -1;

	peer->packetThrottleInterval = SNET_NET_TO_HOST_32(command->throttleConfigure.packetThrottleInterval);
	peer->packetThrottleAcceleration = SNET_NET_TO_HOST_32(command->throttleConfigure.packetThrottleAcceleration);
	peer->packetThrottleDeceleration = SNET_NET_TO_HOST_32(command->throttleConfigure.packetThrottleDeceleration);

	return 0;
}

static int
snet_protocol_handle_disconnect(SNetHost * host, SNetPeer * peer, const SNetProtocol * command)
{
	if (peer->state == SNET_PEER_STATE_DISCONNECTED || peer->state == SNET_PEER_STATE_ZOMBIE || peer->state == SNET_PEER_STATE_ACKNOWLEDGING_DISCONNECT)
		return 0;

	snet_peer_reset_queues(peer);

	if (peer->state == SNET_PEER_STATE_CONNECTION_SUCCEEDED || peer->state == SNET_PEER_STATE_DISCONNECTING || peer->state == SNET_PEER_STATE_CONNECTING)
		snet_protocol_dispatch_state(host, peer, SNET_PEER_STATE_ZOMBIE);
	else
		if (peer->state != SNET_PEER_STATE_CONNECTED && peer->state != SNET_PEER_STATE_DISCONNECT_LATER)
		{
			if (peer->state == SNET_PEER_STATE_CONNECTION_PENDING) host->recalculateBandwidthLimits = 1;

			snet_peer_reset(peer);
		}
		else
			if (command->header.command & SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
				snet_protocol_change_state(host, peer, SNET_PEER_STATE_ACKNOWLEDGING_DISCONNECT);
			else
				snet_protocol_dispatch_state(host, peer, SNET_PEER_STATE_ZOMBIE);

	if (peer->state != SNET_PEER_STATE_DISCONNECTED)
		peer->eventData = SNET_NET_TO_HOST_32(command->disconnect.data);

	return 0;
}

static int
snet_protocol_handle_acknowledge(SNetHost * host, SNetEvent * event, SNetPeer * peer, const SNetProtocol * command)
{
	snet_uint32 roundTripTime,
		receivedSentTime,
		receivedReliableSequenceNumber;
	SNetProtocolCommand commandNumber;

	if (peer->state == SNET_PEER_STATE_DISCONNECTED || peer->state == SNET_PEER_STATE_ZOMBIE)
		return 0;

	receivedSentTime = SNET_NET_TO_HOST_16(command->acknowledge.receivedSentTime);
	receivedSentTime |= host->serviceTime & 0xFFFF0000;
	if ((receivedSentTime & 0x8000) > (host->serviceTime & 0x8000))
		receivedSentTime -= 0x10000;

	if (SNET_TIME_LESS(host->serviceTime, receivedSentTime))
		return 0;

	peer->lastReceiveTime = host->serviceTime;
	peer->earliestTimeout = 0;

	roundTripTime = SNET_TIME_DIFFERENCE(host->serviceTime, receivedSentTime);

	snet_peer_throttle(peer, roundTripTime);

	peer->roundTripTimeVariance -= peer->roundTripTimeVariance / 4;

	if (roundTripTime >= peer->roundTripTime)
	{
		peer->roundTripTime += (roundTripTime - peer->roundTripTime) / 8;
		peer->roundTripTimeVariance += (roundTripTime - peer->roundTripTime) / 4;
	}
	else
	{
		peer->roundTripTime -= (peer->roundTripTime - roundTripTime) / 8;
		peer->roundTripTimeVariance += (peer->roundTripTime - roundTripTime) / 4;
	}

	if (peer->roundTripTime < peer->lowestRoundTripTime)
		peer->lowestRoundTripTime = peer->roundTripTime;

	if (peer->roundTripTimeVariance > peer->highestRoundTripTimeVariance)
		peer->highestRoundTripTimeVariance = peer->roundTripTimeVariance;

	if (peer->packetThrottleEpoch == 0 ||
		SNET_TIME_DIFFERENCE(host->serviceTime, peer->packetThrottleEpoch) >= peer->packetThrottleInterval)
	{
		peer->lastRoundTripTime = peer->lowestRoundTripTime;
		peer->lastRoundTripTimeVariance = peer->highestRoundTripTimeVariance;
		peer->lowestRoundTripTime = peer->roundTripTime;
		peer->highestRoundTripTimeVariance = peer->roundTripTimeVariance;
		peer->packetThrottleEpoch = host->serviceTime;
	}

	receivedReliableSequenceNumber = SNET_NET_TO_HOST_16(command->acknowledge.receivedReliableSequenceNumber);

	commandNumber = snet_protocol_remove_sent_reliable_command(peer, receivedReliableSequenceNumber, command->header.channelID);

	switch (peer->state)
	{
	case SNET_PEER_STATE_ACKNOWLEDGING_CONNECT:
		if (commandNumber != SNET_PROTOCOL_COMMAND_VERIFY_CONNECT)
			return -1;

		snet_protocol_notify_connect(host, peer, event);
		break;

	case SNET_PEER_STATE_DISCONNECTING:
		if (commandNumber != SNET_PROTOCOL_COMMAND_DISCONNECT)
			return -1;

		snet_protocol_notify_disconnect(host, peer, event);
		break;

	case SNET_PEER_STATE_DISCONNECT_LATER:
		if (snet_list_empty(&peer->outgoingReliableCommands) &&
			snet_list_empty(&peer->outgoingUnreliableCommands) &&
			snet_list_empty(&peer->sentReliableCommands))
			snet_peer_disconnect(peer, peer->eventData);
		break;

	default:
		break;
	}

	return 0;
}

static int
snet_protocol_handle_verify_connect(SNetHost * host, SNetEvent * event, SNetPeer * peer, const SNetProtocol * command)
{
	snet_uint32 mtu, windowSize;
	size_t channelCount;

	if (peer->state != SNET_PEER_STATE_CONNECTING)
		return 0;

	channelCount = SNET_NET_TO_HOST_32(command->verifyConnect.channelCount);

	if (channelCount < SNET_PROTOCOL_MINIMUM_CHANNEL_COUNT || channelCount > SNET_PROTOCOL_MAXIMUM_CHANNEL_COUNT ||
		SNET_NET_TO_HOST_32(command->verifyConnect.packetThrottleInterval) != peer->packetThrottleInterval ||
		SNET_NET_TO_HOST_32(command->verifyConnect.packetThrottleAcceleration) != peer->packetThrottleAcceleration ||
		SNET_NET_TO_HOST_32(command->verifyConnect.packetThrottleDeceleration) != peer->packetThrottleDeceleration ||
		command->verifyConnect.connectID != peer->connectID)
	{
		peer->eventData = 0;

		snet_protocol_dispatch_state(host, peer, SNET_PEER_STATE_ZOMBIE);

		return -1;
	}

	snet_protocol_remove_sent_reliable_command(peer, 1, 0xFF);

	if (channelCount < peer->channelCount)
		peer->channelCount = channelCount;

	peer->outgoingPeerID = SNET_NET_TO_HOST_16(command->verifyConnect.outgoingPeerID);
	peer->incomingSessionID = command->verifyConnect.incomingSessionID;
	peer->outgoingSessionID = command->verifyConnect.outgoingSessionID;

	mtu = SNET_NET_TO_HOST_32(command->verifyConnect.mtu);

	if (mtu < SNET_PROTOCOL_MINIMUM_MTU)
		mtu = SNET_PROTOCOL_MINIMUM_MTU;
	else
		if (mtu > SNET_PROTOCOL_MAXIMUM_MTU)
			mtu = SNET_PROTOCOL_MAXIMUM_MTU;

	if (mtu < peer->mtu)
		peer->mtu = mtu;

	windowSize = SNET_NET_TO_HOST_32(command->verifyConnect.windowSize);

	if (windowSize < SNET_PROTOCOL_MINIMUM_WINDOW_SIZE)
		windowSize = SNET_PROTOCOL_MINIMUM_WINDOW_SIZE;

	if (windowSize > SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
		windowSize = SNET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

	if (windowSize < peer->windowSize)
		peer->windowSize = windowSize;

	peer->incomingBandwidth = SNET_NET_TO_HOST_32(command->verifyConnect.incomingBandwidth);
	peer->outgoingBandwidth = SNET_NET_TO_HOST_32(command->verifyConnect.outgoingBandwidth);

	snet_protocol_notify_connect(host, peer, event);
	return 0;
}

static int
snet_protocol_handle_incoming_commands(SNetHost * host, SNetEvent * event)
{
	SNetProtocolHeader * header;
	SNetProtocol * command;
	SNetPeer * peer;
	snet_uint8 * currentData;
	size_t headerSize;
	snet_uint16 peerID, flags;
	snet_uint8 sessionID;

	if (host->receivedDataLength < (size_t) & ((SNetProtocolHeader *)0)->sentTime)
		return 0;

	header = (SNetProtocolHeader *)host->receivedData;

	peerID = SNET_NET_TO_HOST_16(header->peerID);
	sessionID = (peerID & SNET_PROTOCOL_HEADER_SESSION_MASK) >> SNET_PROTOCOL_HEADER_SESSION_SHIFT;
	flags = peerID & SNET_PROTOCOL_HEADER_FLAG_MASK;
	peerID &= ~(SNET_PROTOCOL_HEADER_FLAG_MASK | SNET_PROTOCOL_HEADER_SESSION_MASK);

	headerSize = (flags & SNET_PROTOCOL_HEADER_FLAG_SENT_TIME ? sizeof(SNetProtocolHeader) : (size_t) & ((SNetProtocolHeader *)0)->sentTime);
	if (host->checksum != NULL)
		headerSize += sizeof(snet_uint32);

	if (peerID == SNET_PROTOCOL_MAXIMUM_PEER_ID)
		peer = NULL;
	else
		if (peerID >= host->peerCount)
			return 0;
		else
		{
			peer = &host->peers[peerID];

			if (peer->state == SNET_PEER_STATE_DISCONNECTED ||
				peer->state == SNET_PEER_STATE_ZOMBIE ||
				((host->receivedAddress.host != peer->address.host ||
					host->receivedAddress.port != peer->address.port) &&
					peer->address.host != SNET_HOST_BROADCAST) ||
					(peer->outgoingPeerID < SNET_PROTOCOL_MAXIMUM_PEER_ID &&
						sessionID != peer->incomingSessionID))
				return 0;
		}

	if (flags & SNET_PROTOCOL_HEADER_FLAG_COMPRESSED)
	{
		size_t originalSize;
		if (host->compressor.context == NULL || host->compressor.decompress == NULL)
			return 0;

		originalSize = host->compressor.decompress(host->compressor.context,
			host->receivedData + headerSize,
			host->receivedDataLength - headerSize,
			host->packetData[1] + headerSize,
			sizeof(host->packetData[1]) - headerSize);
		if (originalSize <= 0 || originalSize > sizeof(host->packetData[1]) - headerSize)
			return 0;

		memcpy(host->packetData[1], header, headerSize);
		host->receivedData = host->packetData[1];
		host->receivedDataLength = headerSize + originalSize;
	}

	if (host->checksum != NULL)
	{
		snet_uint32 * checksum = (snet_uint32 *)& host->receivedData[headerSize - sizeof(snet_uint32)],
			desiredChecksum = *checksum;
		SNetBuffer buffer;

		*checksum = peer != NULL ? peer->connectID : 0;

		buffer.data = host->receivedData;
		buffer.dataLength = host->receivedDataLength;

		if (host->checksum(&buffer, 1) != desiredChecksum)
			return 0;
	}

	if (peer != NULL)
	{
		peer->address.host = host->receivedAddress.host;
		peer->address.port = host->receivedAddress.port;
		peer->incomingDataTotal += host->receivedDataLength;
	}

	currentData = host->receivedData + headerSize;

	while (currentData < &host->receivedData[host->receivedDataLength])
	{
		snet_uint8 commandNumber;
		size_t commandSize;

		command = (SNetProtocol *)currentData;

		if (currentData + sizeof(SNetProtocolCommandHeader) > & host->receivedData[host->receivedDataLength])
			break;

		commandNumber = command->header.command & SNET_PROTOCOL_COMMAND_MASK;
		if (commandNumber >= SNET_PROTOCOL_COMMAND_COUNT)
			break;

		commandSize = commandSizes[commandNumber];
		if (commandSize == 0 || currentData + commandSize > & host->receivedData[host->receivedDataLength])
			break;

		currentData += commandSize;

		if (peer == NULL && commandNumber != SNET_PROTOCOL_COMMAND_CONNECT)
			break;

		command->header.reliableSequenceNumber = SNET_NET_TO_HOST_16(command->header.reliableSequenceNumber);

		switch (commandNumber)
		{
		case SNET_PROTOCOL_COMMAND_ACKNOWLEDGE:
			if (snet_protocol_handle_acknowledge(host, event, peer, command))
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_CONNECT:
			if (peer != NULL)
				goto commandError;
			peer = snet_protocol_handle_connect(host, header, command);
			if (peer == NULL)
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_VERIFY_CONNECT:
			if (snet_protocol_handle_verify_connect(host, event, peer, command))
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_DISCONNECT:
			if (snet_protocol_handle_disconnect(host, peer, command))
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_PING:
			if (snet_protocol_handle_ping(host, peer, command))
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_SEND_RELIABLE:
			if (snet_protocol_handle_send_reliable(host, peer, command, &currentData))
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
			if (snet_protocol_handle_send_unreliable(host, peer, command, &currentData))
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
			if (snet_protocol_handle_send_unsequenced(host, peer, command, &currentData))
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_SEND_FRAGMENT:
			if (snet_protocol_handle_send_fragment(host, peer, command, &currentData))
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_BANDWIDTH_LIMIT:
			if (snet_protocol_handle_bandwidth_limit(host, peer, command))
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_THROTTLE_CONFIGURE:
			if (snet_protocol_handle_throttle_configure(host, peer, command))
				goto commandError;
			break;

		case SNET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
			if (snet_protocol_handle_send_unreliable_fragment(host, peer, command, &currentData))
				goto commandError;
			break;

		default:
			goto commandError;
		}

		if (peer != NULL &&
			(command->header.command & SNET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0)
		{
			snet_uint16 sentTime;

			if (!(flags & SNET_PROTOCOL_HEADER_FLAG_SENT_TIME))
				break;

			sentTime = SNET_NET_TO_HOST_16(header->sentTime);

			switch (peer->state)
			{
			case SNET_PEER_STATE_DISCONNECTING:
			case SNET_PEER_STATE_ACKNOWLEDGING_CONNECT:
			case SNET_PEER_STATE_DISCONNECTED:
			case SNET_PEER_STATE_ZOMBIE:
				break;

			case SNET_PEER_STATE_ACKNOWLEDGING_DISCONNECT:
				if ((command->header.command & SNET_PROTOCOL_COMMAND_MASK) == SNET_PROTOCOL_COMMAND_DISCONNECT)
					snet_peer_queue_acknowledgement(peer, command, sentTime);
				break;

			default:
				snet_peer_queue_acknowledgement(peer, command, sentTime);
				break;
			}
		}
	}

commandError:
	if (event != NULL && event->type != SNET_EVENT_TYPE_NONE)
		return 1;

	return 0;
}

static int
snet_protocol_receive_incoming_commands(SNetHost * host, SNetEvent * event)
{
	int packets;

	for (packets = 0; packets < 256; ++packets)
	{
		int receivedLength;
		SNetBuffer buffer;

		buffer.data = host->packetData[0];
		buffer.dataLength = sizeof(host->packetData[0]);

		receivedLength = snet_socket_receive(host->socket,
			&host->receivedAddress,
			&buffer,
			1);

		if (receivedLength < 0)
			return -1;

		if (receivedLength == 0)
			return 0;

		host->receivedData = host->packetData[0];
		host->receivedDataLength = receivedLength;

		host->totalReceivedData += receivedLength;
		host->totalReceivedPackets++;

		if (host->intercept != NULL)
		{
			switch (host->intercept(host, event))
			{
			case 1:
				if (event != NULL && event->type != SNET_EVENT_TYPE_NONE)
					return 1;

				continue;

			case -1:
				return -1;

			default:
				break;
			}
		}

		switch (snet_protocol_handle_incoming_commands(host, event))
		{
		case 1:
			return 1;

		case -1:
			return -1;

		default:
			break;
		}
	}

	return -1;
}

static void
snet_protocol_send_acknowledgements(SNetHost * host, SNetPeer * peer)
{
	SNetProtocol * command = &host->commands[host->commandCount];
	SNetBuffer * buffer = &host->buffers[host->bufferCount];
	SNetAcknowledgement * acknowledgement;
	SNetListIterator currentAcknowledgement;
	snet_uint16 reliableSequenceNumber;

	currentAcknowledgement = snet_list_begin(&peer->acknowledgements);

	while (currentAcknowledgement != snet_list_end(&peer->acknowledgements))
	{
		if (command >= &host->commands[sizeof(host->commands) / sizeof(SNetProtocol)] ||
			buffer >= &host->buffers[sizeof(host->buffers) / sizeof(SNetBuffer)] ||
			peer->mtu - host->packetSize < sizeof(SNetProtocolAcknowledge))
		{
			host->continueSending = 1;

			break;
		}

		acknowledgement = (SNetAcknowledgement *)currentAcknowledgement;

		currentAcknowledgement = snet_list_next(currentAcknowledgement);

		buffer->data = command;
		buffer->dataLength = sizeof(SNetProtocolAcknowledge);

		host->packetSize += buffer->dataLength;

		reliableSequenceNumber = SNET_HOST_TO_NET_16(acknowledgement->command.header.reliableSequenceNumber);

		command->header.command = SNET_PROTOCOL_COMMAND_ACKNOWLEDGE;
		command->header.channelID = acknowledgement->command.header.channelID;
		command->header.reliableSequenceNumber = reliableSequenceNumber;
		command->acknowledge.receivedReliableSequenceNumber = reliableSequenceNumber;
		command->acknowledge.receivedSentTime = SNET_HOST_TO_NET_16(acknowledgement->sentTime);

		if ((acknowledgement->command.header.command & SNET_PROTOCOL_COMMAND_MASK) == SNET_PROTOCOL_COMMAND_DISCONNECT)
			snet_protocol_dispatch_state(host, peer, SNET_PEER_STATE_ZOMBIE);

		snet_list_remove(&acknowledgement->acknowledgementList);
		snet_free(acknowledgement);

		++command;
		++buffer;
	}

	host->commandCount = command - host->commands;
	host->bufferCount = buffer - host->buffers;
}

static void
snet_protocol_send_unreliable_outgoing_commands(SNetHost * host, SNetPeer * peer)
{
	SNetProtocol * command = &host->commands[host->commandCount];
	SNetBuffer * buffer = &host->buffers[host->bufferCount];
	SNetOutgoingCommand * outgoingCommand;
	SNetListIterator currentCommand;

	currentCommand = snet_list_begin(&peer->outgoingUnreliableCommands);

	while (currentCommand != snet_list_end(&peer->outgoingUnreliableCommands))
	{
		size_t commandSize;

		outgoingCommand = (SNetOutgoingCommand *)currentCommand;
		commandSize = commandSizes[outgoingCommand->command.header.command & SNET_PROTOCOL_COMMAND_MASK];

		if (command >= &host->commands[sizeof(host->commands) / sizeof(SNetProtocol)] ||
			buffer + 1 >= &host->buffers[sizeof(host->buffers) / sizeof(SNetBuffer)] ||
			peer->mtu - host->packetSize < commandSize ||
			(outgoingCommand->packet != NULL &&
				peer->mtu - host->packetSize < commandSize + outgoingCommand->fragmentLength))
		{
			host->continueSending = 1;

			break;
		}

		currentCommand = snet_list_next(currentCommand);

		if (outgoingCommand->packet != NULL && outgoingCommand->fragmentOffset == 0)
		{
			peer->packetThrottleCounter += SNET_PEER_PACKET_THROTTLE_COUNTER;
			peer->packetThrottleCounter %= SNET_PEER_PACKET_THROTTLE_SCALE;

			if (peer->packetThrottleCounter > peer->packetThrottle)
			{
				snet_uint16 reliableSequenceNumber = outgoingCommand->reliableSequenceNumber,
					unreliableSequenceNumber = outgoingCommand->unreliableSequenceNumber;
				for (;;)
				{
					--outgoingCommand->packet->referenceCount;

					if (outgoingCommand->packet->referenceCount == 0)
						snet_packet_destroy(outgoingCommand->packet);

					snet_list_remove(&outgoingCommand->outgoingCommandList);
					snet_free(outgoingCommand);

					if (currentCommand == snet_list_end(&peer->outgoingUnreliableCommands))
						break;

					outgoingCommand = (SNetOutgoingCommand *)currentCommand;
					if (outgoingCommand->reliableSequenceNumber != reliableSequenceNumber ||
						outgoingCommand->unreliableSequenceNumber != unreliableSequenceNumber)
						break;

					currentCommand = snet_list_next(currentCommand);
				}

				continue;
			}
		}

		buffer->data = command;
		buffer->dataLength = commandSize;

		host->packetSize += buffer->dataLength;

		*command = outgoingCommand->command;

		snet_list_remove(&outgoingCommand->outgoingCommandList);

		if (outgoingCommand->packet != NULL)
		{
			++buffer;

			buffer->data = outgoingCommand->packet->data + outgoingCommand->fragmentOffset;
			buffer->dataLength = outgoingCommand->fragmentLength;

			host->packetSize += buffer->dataLength;

			snet_list_insert(snet_list_end(&peer->sentUnreliableCommands), outgoingCommand);
		}
		else
			snet_free(outgoingCommand);

		++command;
		++buffer;
	}

	host->commandCount = command - host->commands;
	host->bufferCount = buffer - host->buffers;

	if (peer->state == SNET_PEER_STATE_DISCONNECT_LATER &&
		snet_list_empty(&peer->outgoingReliableCommands) &&
		snet_list_empty(&peer->outgoingUnreliableCommands) &&
		snet_list_empty(&peer->sentReliableCommands))
		snet_peer_disconnect(peer, peer->eventData);
}

static int
snet_protocol_check_timeouts(SNetHost * host, SNetPeer * peer, SNetEvent * event)
{
	SNetOutgoingCommand * outgoingCommand;
	SNetListIterator currentCommand, insertPosition;

	currentCommand = snet_list_begin(&peer->sentReliableCommands);
	insertPosition = snet_list_begin(&peer->outgoingReliableCommands);

	while (currentCommand != snet_list_end(&peer->sentReliableCommands))
	{
		outgoingCommand = (SNetOutgoingCommand *)currentCommand;

		currentCommand = snet_list_next(currentCommand);

		if (SNET_TIME_DIFFERENCE(host->serviceTime, outgoingCommand->sentTime) < outgoingCommand->roundTripTimeout)
			continue;

		if (peer->earliestTimeout == 0 ||
			SNET_TIME_LESS(outgoingCommand->sentTime, peer->earliestTimeout))
			peer->earliestTimeout = outgoingCommand->sentTime;

		if (peer->earliestTimeout != 0 &&
			(SNET_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMaximum ||
			(outgoingCommand->roundTripTimeout >= outgoingCommand->roundTripTimeoutLimit &&
				SNET_TIME_DIFFERENCE(host->serviceTime, peer->earliestTimeout) >= peer->timeoutMinimum)))
		{
			snet_protocol_notify_disconnect(host, peer, event);

			return 1;
		}

		if (outgoingCommand->packet != NULL)
			peer->reliableDataInTransit -= outgoingCommand->fragmentLength;

		++peer->packetsLost;

		outgoingCommand->roundTripTimeout *= 2;

		snet_list_insert(insertPosition, snet_list_remove(&outgoingCommand->outgoingCommandList));

		if (currentCommand == snet_list_begin(&peer->sentReliableCommands) &&
			!snet_list_empty(&peer->sentReliableCommands))
		{
			outgoingCommand = (SNetOutgoingCommand *)currentCommand;

			peer->nextTimeout = outgoingCommand->sentTime + outgoingCommand->roundTripTimeout;
		}
	}

	return 0;
}

static int
snet_protocol_send_reliable_outgoing_commands(SNetHost * host, SNetPeer * peer)
{
	SNetProtocol * command = &host->commands[host->commandCount];
	SNetBuffer * buffer = &host->buffers[host->bufferCount];
	SNetOutgoingCommand * outgoingCommand;
	SNetListIterator currentCommand;
	SNetChannel *channel;
	snet_uint16 reliableWindow;
	size_t commandSize;
	int windowExceeded = 0, windowWrap = 0, canPing = 1;

	currentCommand = snet_list_begin(&peer->outgoingReliableCommands);

	while (currentCommand != snet_list_end(&peer->outgoingReliableCommands))
	{
		outgoingCommand = (SNetOutgoingCommand *)currentCommand;

		channel = outgoingCommand->command.header.channelID < peer->channelCount ? &peer->channels[outgoingCommand->command.header.channelID] : NULL;
		reliableWindow = outgoingCommand->reliableSequenceNumber / SNET_PEER_RELIABLE_WINDOW_SIZE;
		if (channel != NULL)
		{
			if (!windowWrap &&
				outgoingCommand->sendAttempts < 1 &&
				!(outgoingCommand->reliableSequenceNumber % SNET_PEER_RELIABLE_WINDOW_SIZE) &&
				(channel->reliableWindows[(reliableWindow + SNET_PEER_RELIABLE_WINDOWS - 1) % SNET_PEER_RELIABLE_WINDOWS] >= SNET_PEER_RELIABLE_WINDOW_SIZE ||
					channel->usedReliableWindows & ((((1 << SNET_PEER_FREE_RELIABLE_WINDOWS) - 1) << reliableWindow) |
					(((1 << SNET_PEER_FREE_RELIABLE_WINDOWS) - 1) >> (SNET_PEER_RELIABLE_WINDOWS - reliableWindow)))))
				windowWrap = 1;
			if (windowWrap)
			{
				currentCommand = snet_list_next(currentCommand);

				continue;
			}
		}

		if (outgoingCommand->packet != NULL)
		{
			if (!windowExceeded)
			{
				snet_uint32 windowSize = (peer->packetThrottle * peer->windowSize) / SNET_PEER_PACKET_THROTTLE_SCALE;

				if (peer->reliableDataInTransit + outgoingCommand->fragmentLength > SNET_MAX(windowSize, peer->mtu))
					windowExceeded = 1;
			}
			if (windowExceeded)
			{
				currentCommand = snet_list_next(currentCommand);

				continue;
			}
		}

		canPing = 0;

		commandSize = commandSizes[outgoingCommand->command.header.command & SNET_PROTOCOL_COMMAND_MASK];
		if (command >= &host->commands[sizeof(host->commands) / sizeof(SNetProtocol)] ||
			buffer + 1 >= &host->buffers[sizeof(host->buffers) / sizeof(SNetBuffer)] ||
			peer->mtu - host->packetSize < commandSize ||
			(outgoingCommand->packet != NULL &&
			(snet_uint16)(peer->mtu - host->packetSize) < (snet_uint16)(commandSize + outgoingCommand->fragmentLength)))
		{
			host->continueSending = 1;

			break;
		}

		currentCommand = snet_list_next(currentCommand);

		if (channel != NULL && outgoingCommand->sendAttempts < 1)
		{
			channel->usedReliableWindows |= 1 << reliableWindow;
			++channel->reliableWindows[reliableWindow];
		}

		++outgoingCommand->sendAttempts;

		if (outgoingCommand->roundTripTimeout == 0)
		{
			outgoingCommand->roundTripTimeout = peer->roundTripTime + 4 * peer->roundTripTimeVariance;
			outgoingCommand->roundTripTimeoutLimit = peer->timeoutLimit * outgoingCommand->roundTripTimeout;
		}

		if (snet_list_empty(&peer->sentReliableCommands))
			peer->nextTimeout = host->serviceTime + outgoingCommand->roundTripTimeout;

		snet_list_insert(snet_list_end(&peer->sentReliableCommands),
			snet_list_remove(&outgoingCommand->outgoingCommandList));

		outgoingCommand->sentTime = host->serviceTime;

		buffer->data = command;
		buffer->dataLength = commandSize;

		host->packetSize += buffer->dataLength;
		host->headerFlags |= SNET_PROTOCOL_HEADER_FLAG_SENT_TIME;

		*command = outgoingCommand->command;

		if (outgoingCommand->packet != NULL)
		{
			++buffer;

			buffer->data = outgoingCommand->packet->data + outgoingCommand->fragmentOffset;
			buffer->dataLength = outgoingCommand->fragmentLength;

			host->packetSize += outgoingCommand->fragmentLength;

			peer->reliableDataInTransit += outgoingCommand->fragmentLength;
		}

		++peer->packetsSent;

		++command;
		++buffer;
	}

	host->commandCount = command - host->commands;
	host->bufferCount = buffer - host->buffers;

	return canPing;
}

static int
snet_protocol_send_outgoing_commands(SNetHost * host, SNetEvent * event, int checkForTimeouts)
{
	snet_uint8 headerData[sizeof(SNetProtocolHeader) + sizeof(snet_uint32)];
	SNetProtocolHeader * header = (SNetProtocolHeader *)headerData;
	SNetPeer * currentPeer;
	int sentLength;
	size_t shouldCompress = 0;

	host->continueSending = 1;

	while (host->continueSending)
		for (host->continueSending = 0,
			currentPeer = host->peers;
			currentPeer < &host->peers[host->peerCount];
			++currentPeer)
		{
			if (currentPeer->state == SNET_PEER_STATE_DISCONNECTED ||
				currentPeer->state == SNET_PEER_STATE_ZOMBIE)
				continue;

			host->headerFlags = 0;
			host->commandCount = 0;
			host->bufferCount = 1;
			host->packetSize = sizeof(SNetProtocolHeader);

			if (!snet_list_empty(&currentPeer->acknowledgements))
				snet_protocol_send_acknowledgements(host, currentPeer);

			if (checkForTimeouts != 0 &&
				!snet_list_empty(&currentPeer->sentReliableCommands) &&
				SNET_TIME_GREATER_EQUAL(host->serviceTime, currentPeer->nextTimeout) &&
				snet_protocol_check_timeouts(host, currentPeer, event) == 1)
			{
				if (event != NULL && event->type != SNET_EVENT_TYPE_NONE)
					return 1;
				else
					continue;
			}

			if ((snet_list_empty(&currentPeer->outgoingReliableCommands) ||
				snet_protocol_send_reliable_outgoing_commands(host, currentPeer)) &&
				snet_list_empty(&currentPeer->sentReliableCommands) &&
				SNET_TIME_DIFFERENCE(host->serviceTime, currentPeer->lastReceiveTime) >= currentPeer->pingInterval &&
				currentPeer->mtu - host->packetSize >= sizeof(SNetProtocolPing))
			{
				snet_peer_ping(currentPeer);
				snet_protocol_send_reliable_outgoing_commands(host, currentPeer);
			}

			if (!snet_list_empty(&currentPeer->outgoingUnreliableCommands))
				snet_protocol_send_unreliable_outgoing_commands(host, currentPeer);

			if (host->commandCount == 0)
				continue;

			if (currentPeer->packetLossEpoch == 0)
				currentPeer->packetLossEpoch = host->serviceTime;
			else
				if (SNET_TIME_DIFFERENCE(host->serviceTime, currentPeer->packetLossEpoch) >= SNET_PEER_PACKET_LOSS_INTERVAL &&
					currentPeer->packetsSent > 0)
				{
					snet_uint32 packetLoss = currentPeer->packetsLost * SNET_PEER_PACKET_LOSS_SCALE / currentPeer->packetsSent;

#ifdef SNET_DEBUG
					printf("peer %u: %f%%+-%f%% packet loss, %u+-%u ms round trip time, %f%% throttle, %u/%u outgoing, %u/%u incoming\n", currentPeer->incomingPeerID, currentPeer->packetLoss / (float)SNET_PEER_PACKET_LOSS_SCALE, currentPeer->packetLossVariance / (float)SNET_PEER_PACKET_LOSS_SCALE, currentPeer->roundTripTime, currentPeer->roundTripTimeVariance, currentPeer->packetThrottle / (float)SNET_PEER_PACKET_THROTTLE_SCALE, snet_list_size(&currentPeer->outgoingReliableCommands), snet_list_size(&currentPeer->outgoingUnreliableCommands), currentPeer->channels != NULL ? snet_list_size(&currentPeer->channels->incomingReliableCommands) : 0, currentPeer->channels != NULL ? snet_list_size(&currentPeer->channels->incomingUnreliableCommands) : 0);
#endif

					currentPeer->packetLossVariance -= currentPeer->packetLossVariance / 4;

					if (packetLoss >= currentPeer->packetLoss)
					{
						currentPeer->packetLoss += (packetLoss - currentPeer->packetLoss) / 8;
						currentPeer->packetLossVariance += (packetLoss - currentPeer->packetLoss) / 4;
					}
					else
					{
						currentPeer->packetLoss -= (currentPeer->packetLoss - packetLoss) / 8;
						currentPeer->packetLossVariance += (currentPeer->packetLoss - packetLoss) / 4;
					}

					currentPeer->packetLossEpoch = host->serviceTime;
					currentPeer->packetsSent = 0;
					currentPeer->packetsLost = 0;
				}

			host->buffers->data = headerData;
			if (host->headerFlags & SNET_PROTOCOL_HEADER_FLAG_SENT_TIME)
			{
				header->sentTime = SNET_HOST_TO_NET_16(host->serviceTime & 0xFFFF);

				host->buffers->dataLength = sizeof(SNetProtocolHeader);
			}
			else
				host->buffers->dataLength = (size_t) & ((SNetProtocolHeader *)0)->sentTime;

			shouldCompress = 0;
			if (host->compressor.context != NULL && host->compressor.compress != NULL)
			{
				size_t originalSize = host->packetSize - sizeof(SNetProtocolHeader),
					compressedSize = host->compressor.compress(host->compressor.context,
						&host->buffers[1], host->bufferCount - 1,
						originalSize,
						host->packetData[1],
						originalSize);
				if (compressedSize > 0 && compressedSize < originalSize)
				{
					host->headerFlags |= SNET_PROTOCOL_HEADER_FLAG_COMPRESSED;
					shouldCompress = compressedSize;
#ifdef SNET_DEBUG_COMPRESS
					printf("peer %u: compressed %u -> %u (%u%%)\n", currentPeer->incomingPeerID, originalSize, compressedSize, (compressedSize * 100) / originalSize);
#endif
				}
			}

			if (currentPeer->outgoingPeerID < SNET_PROTOCOL_MAXIMUM_PEER_ID)
				host->headerFlags |= currentPeer->outgoingSessionID << SNET_PROTOCOL_HEADER_SESSION_SHIFT;
			header->peerID = SNET_HOST_TO_NET_16(currentPeer->outgoingPeerID | host->headerFlags);
			if (host->checksum != NULL)
			{
				snet_uint32 * checksum = (snet_uint32 *)& headerData[host->buffers->dataLength];
				*checksum = currentPeer->outgoingPeerID < SNET_PROTOCOL_MAXIMUM_PEER_ID ? currentPeer->connectID : 0;
				host->buffers->dataLength += sizeof(snet_uint32);
				*checksum = host->checksum(host->buffers, host->bufferCount);
			}

			if (shouldCompress > 0)
			{
				host->buffers[1].data = host->packetData[1];
				host->buffers[1].dataLength = shouldCompress;
				host->bufferCount = 2;
			}

			currentPeer->lastSendTime = host->serviceTime;

			sentLength = snet_socket_send(host->socket, &currentPeer->address, host->buffers, host->bufferCount);

			snet_protocol_remove_sent_unreliable_commands(currentPeer);

			if (sentLength < 0)
				return -1;

			host->totalSentData += sentLength;
			host->totalSentPackets++;
		}

	return 0;
}

/** Sends any queued packets on the host specified to its designated peers.

@param host   host to flush
@remarks this function need only be used in circumstances where one wishes to send queued packets earlier than in a call to snet_host_service().
@ingroup host
*/
void
snet_host_flush(SNetHost * host)
{
	host->serviceTime = snet_time_get();

	snet_protocol_send_outgoing_commands(host, NULL, 0);
}

/** Checks for any queued events on the host and dispatches one if available.

@param host    host to check for events
@param event   an event structure where event details will be placed if available
@retval > 0 if an event was dispatched
@retval 0 if no events are available
@retval < 0 on failure
@ingroup host
*/
int
snet_host_check_events(SNetHost * host, SNetEvent * event)
{
	if (event == NULL) return -1;

	event->type = SNET_EVENT_TYPE_NONE;
	event->peer = NULL;
	event->packet = NULL;

	return snet_protocol_dispatch_incoming_commands(host, event);
}

/** Waits for events on the host specified and shuttles packets between
the host and its peers.

@param host    host to service
@param event   an event structure where event details will be placed if one occurs
if event == NULL then no events will be delivered
@param timeout number of milliseconds that SNet should wait for events
@retval > 0 if an event occurred within the specified time limit
@retval 0 if no event occurred
@retval < 0 on failure
@remarks snet_host_service should be called fairly regularly for adequate performance
@ingroup host
*/
int
snet_host_service(SNetHost * host, SNetEvent * event, snet_uint32 timeout)
{
	snet_uint32 waitCondition;

	if (event != NULL)
	{
		event->type = SNET_EVENT_TYPE_NONE;
		event->peer = NULL;
		event->packet = NULL;

		switch (snet_protocol_dispatch_incoming_commands(host, event))
		{
		case 1:
			return 1;

		case -1:
#ifdef SNET_DEBUG
			perror("Error dispatching incoming packets");
#endif

			return -1;

		default:
			break;
		}
	}

	host->serviceTime = snet_time_get();

	timeout += host->serviceTime;

	do
	{
		if (SNET_TIME_DIFFERENCE(host->serviceTime, host->bandwidthThrottleEpoch) >= SNET_HOST_BANDWIDTH_THROTTLE_INTERVAL)
			snet_host_bandwidth_throttle(host);

		switch (snet_protocol_send_outgoing_commands(host, event, 1))
		{
		case 1:
			return 1;

		case -1:
#ifdef SNET_DEBUG
			perror("Error sending outgoing packets");
#endif

			return -1;

		default:
			break;
		}

		switch (snet_protocol_receive_incoming_commands(host, event))
		{
		case 1:
			return 1;

		case -1:
#ifdef SNET_DEBUG
			perror("Error receiving incoming packets");
#endif

			return -1;

		default:
			break;
		}

		switch (snet_protocol_send_outgoing_commands(host, event, 1))
		{
		case 1:
			return 1;

		case -1:
#ifdef SNET_DEBUG
			perror("Error sending outgoing packets");
#endif

			return -1;

		default:
			break;
		}

		if (event != NULL)
		{
			switch (snet_protocol_dispatch_incoming_commands(host, event))
			{
			case 1:
				return 1;

			case -1:
#ifdef SNET_DEBUG
				perror("Error dispatching incoming packets");
#endif

				return -1;

			default:
				break;
			}
		}

		if (SNET_TIME_GREATER_EQUAL(host->serviceTime, timeout))
			return 0;

		do
		{
			host->serviceTime = snet_time_get();

			if (SNET_TIME_GREATER_EQUAL(host->serviceTime, timeout))
				return 0;

			waitCondition = SNET_SOCKET_WAIT_RECEIVE | SNET_SOCKET_WAIT_INTERRUPT;

			if (snet_socket_wait(host->socket, &waitCondition, SNET_TIME_DIFFERENCE(timeout, host->serviceTime)) != 0)
				return -1;
		} while (waitCondition & SNET_SOCKET_WAIT_INTERRUPT);

		host->serviceTime = snet_time_get();
	} while (waitCondition & SNET_SOCKET_WAIT_RECEIVE);

	return 0;
}

