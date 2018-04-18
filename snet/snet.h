/**
@file  snet.h
@brief SNet public header file
*/
#ifndef __SNET_SNET_H__
#define __SNET_SNET_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdlib.h>

#ifdef _WIN32
#include "snet/win32.h"
#else
#include "snet/unix.h"
#endif

#include "snet/types.h"
#include "snet/protocol.h"
#include "snet/list.h"
#include "snet/callbacks.h"

#define SNET_VERSION_MAJOR 0
#define SNET_VERSION_MINOR 0
#define SNET_VERSION_PATCH 1
#define SNET_VERSION_CREATE(major, minor, patch) (((major)<<16) | ((minor)<<8) | (patch))
#define SNET_VERSION_GET_MAJOR(version) (((version)>>16)&0xFF)
#define SNET_VERSION_GET_MINOR(version) (((version)>>8)&0xFF)
#define SNET_VERSION_GET_PATCH(version) ((version)&0xFF)
#define SNET_VERSION SNET_VERSION_CREATE(SNET_VERSION_MAJOR, SNET_VERSION_MINOR, SNET_VERSION_PATCH)

	typedef snet_uint32 SNetVersion;

	struct _SNetHost;
	struct _SNetEvent;
	struct _SNetPacket;

	typedef enum _SNetSocketType
	{
		SNET_SOCKET_TYPE_STREAM = 1,
		SNET_SOCKET_TYPE_DATAGRAM = 2
	} SNetSocketType;

	typedef enum _SNetSocketWait
	{
		SNET_SOCKET_WAIT_NONE = 0,
		SNET_SOCKET_WAIT_SEND = (1 << 0),
		SNET_SOCKET_WAIT_RECEIVE = (1 << 1),
		SNET_SOCKET_WAIT_INTERRUPT = (1 << 2)
	} SNetSocketWait;

	typedef enum _SNetSocketOption
	{
		SNET_SOCKOPT_NONBLOCK = 1,
		SNET_SOCKOPT_BROADCAST = 2,
		SNET_SOCKOPT_RCVBUF = 3,
		SNET_SOCKOPT_SNDBUF = 4,
		SNET_SOCKOPT_REUSEADDR = 5,
		SNET_SOCKOPT_RCVTIMEO = 6,
		SNET_SOCKOPT_SNDTIMEO = 7,
		SNET_SOCKOPT_ERROR = 8,
		SNET_SOCKOPT_NODELAY = 9
	} SNetSocketOption;

	typedef enum _SNetSocketShutdown
	{
		SNET_SOCKET_SHUTDOWN_READ = 0,
		SNET_SOCKET_SHUTDOWN_WRITE = 1,
		SNET_SOCKET_SHUTDOWN_READ_WRITE = 2
	} SNetSocketShutdown;

#define SNET_HOST_ANY       0
#define SNET_HOST_BROADCAST 0xFFFFFFFFU
#define SNET_PORT_ANY       0

	/**
	* Portable internet address structure.
	*
	* The host must be specified in network byte-order, and the port must be in host
	* byte-order. The constant SNET_HOST_ANY may be used to specify the default
	* server host. The constant SNET_HOST_BROADCAST may be used to specify the
	* broadcast address (255.255.255.255).  This makes sense for snet_host_connect,
	* but not for snet_host_create.  Once a server responds to a broadcast, the
	* address is updated from SNET_HOST_BROADCAST to the server's actual IP address.
	*/
	typedef struct _SNetAddress
	{
		snet_uint32 host;
		snet_uint16 port;
	} SNetAddress;

	/**
	* Packet flag bit constants.
	*
	* The host must be specified in network byte-order, and the port must be in
	* host byte-order. The constant SNET_HOST_ANY may be used to specify the
	* default server host.

	@sa SNetPacket
	*/
	typedef enum _SNetPacketFlag
	{
		/** packet must be received by the target peer and resend attempts should be
		* made until the packet is delivered */
		SNET_PACKET_FLAG_RELIABLE = (1 << 0),
		/** packet will not be sequenced with other packets
		* not supported for reliable packets
		*/
		SNET_PACKET_FLAG_UNSEQUENCED = (1 << 1),
		/** packet will not allocate data, and user must supply it instead */
		SNET_PACKET_FLAG_NO_ALLOCATE = (1 << 2),
		/** packet will be fragmented using unreliable (instead of reliable) sends
		* if it exceeds the MTU */
		SNET_PACKET_FLAG_UNRELIABLE_FRAGMENT = (1 << 3),

		/** whether the packet has been sent from all queues it has been entered into */
		SNET_PACKET_FLAG_SENT = (1 << 8)
	} SNetPacketFlag;

	typedef void (SNET_CALLBACK * SNetPacketFreeCallback) (struct _SNetPacket *);

	/**
	* SNet packet structure.
	*
	* An SNet data packet that may be sent to or received from a peer. The shown
	* fields should only be read and never modified. The data field contains the
	* allocated data for the packet. The dataLength fields specifies the length
	* of the allocated data.  The flags field is either 0 (specifying no flags),
	* or a bitwise-or of any combination of the following flags:
	*
	*    SNET_PACKET_FLAG_RELIABLE - packet must be received by the target peer
	*    and resend attempts should be made until the packet is delivered
	*
	*    SNET_PACKET_FLAG_UNSEQUENCED - packet will not be sequenced with other packets
	*    (not supported for reliable packets)
	*
	*    SNET_PACKET_FLAG_NO_ALLOCATE - packet will not allocate data, and user must supply it instead

	@sa SNetPacketFlag
	*/
	typedef struct _SNetPacket
	{
		size_t                   referenceCount;  /**< internal use only */
		snet_uint32              flags;           /**< bitwise-or of SNetPacketFlag constants */
		snet_uint8 *             data;            /**< allocated data for packet */
		size_t                   dataLength;      /**< length of data */
		SNetPacketFreeCallback   freeCallback;    /**< function to be called when the packet is no longer in use */
		void *                   userData;        /**< application private data, may be freely modified */
	} SNetPacket;

	typedef struct _SNetAcknowledgement
	{
		SNetListNode acknowledgementList;
		snet_uint32  sentTime;
		SNetProtocol command;
	} SNetAcknowledgement;

	typedef struct _SNetOutgoingCommand
	{
		SNetListNode outgoingCommandList;
		snet_uint16  reliableSequenceNumber;
		snet_uint16  unreliableSequenceNumber;
		snet_uint32  sentTime;
		snet_uint32  roundTripTimeout;
		snet_uint32  roundTripTimeoutLimit;
		snet_uint32  fragmentOffset;
		snet_uint16  fragmentLength;
		snet_uint16  sendAttempts;
		SNetProtocol command;
		SNetPacket * packet;
	} SNetOutgoingCommand;

	typedef struct _SNetIncomingCommand
	{
		SNetListNode     incomingCommandList;
		snet_uint16      reliableSequenceNumber;
		snet_uint16      unreliableSequenceNumber;
		SNetProtocol     command;
		snet_uint32      fragmentCount;
		snet_uint32      fragmentsRemaining;
		snet_uint32 *    fragments;
		SNetPacket *     packet;
	} SNetIncomingCommand;

	typedef enum _SNetPeerState
	{
		SNET_PEER_STATE_DISCONNECTED = 0,
		SNET_PEER_STATE_CONNECTING = 1,
		SNET_PEER_STATE_ACKNOWLEDGING_CONNECT = 2,
		SNET_PEER_STATE_CONNECTION_PENDING = 3,
		SNET_PEER_STATE_CONNECTION_SUCCEEDED = 4,
		SNET_PEER_STATE_CONNECTED = 5,
		SNET_PEER_STATE_DISCONNECT_LATER = 6,
		SNET_PEER_STATE_DISCONNECTING = 7,
		SNET_PEER_STATE_ACKNOWLEDGING_DISCONNECT = 8,
		SNET_PEER_STATE_ZOMBIE = 9
	} SNetPeerState;

#ifndef SNET_BUFFER_MAXIMUM
#define SNET_BUFFER_MAXIMUM (1 + 2 * SNET_PROTOCOL_MAXIMUM_PACKET_COMMANDS)
#endif

	enum
	{
		SNET_HOST_RECEIVE_BUFFER_SIZE = 256 * 1024,
		SNET_HOST_SEND_BUFFER_SIZE = 256 * 1024,
		SNET_HOST_BANDWIDTH_THROTTLE_INTERVAL = 1000,
		SNET_HOST_DEFAULT_MTU = 1400,
		SNET_HOST_DEFAULT_MAXIMUM_PACKET_SIZE = 32 * 1024 * 1024,
		SNET_HOST_DEFAULT_MAXIMUM_WAITING_DATA = 32 * 1024 * 1024,

		SNET_PEER_DEFAULT_ROUND_TRIP_TIME = 500,
		SNET_PEER_DEFAULT_PACKET_THROTTLE = 32,
		SNET_PEER_PACKET_THROTTLE_SCALE = 32,
		SNET_PEER_PACKET_THROTTLE_COUNTER = 7,
		SNET_PEER_PACKET_THROTTLE_ACCELERATION = 2,
		SNET_PEER_PACKET_THROTTLE_DECELERATION = 2,
		SNET_PEER_PACKET_THROTTLE_INTERVAL = 5000,
		SNET_PEER_PACKET_LOSS_SCALE = (1 << 16),
		SNET_PEER_PACKET_LOSS_INTERVAL = 10000,
		SNET_PEER_WINDOW_SIZE_SCALE = 64 * 1024,
		SNET_PEER_TIMEOUT_LIMIT = 32,
		SNET_PEER_TIMEOUT_MINIMUM = 5000,
		SNET_PEER_TIMEOUT_MAXIMUM = 30000,
		SNET_PEER_PING_INTERVAL = 500,
		SNET_PEER_UNSEQUENCED_WINDOWS = 64,
		SNET_PEER_UNSEQUENCED_WINDOW_SIZE = 1024,
		SNET_PEER_FREE_UNSEQUENCED_WINDOWS = 32,
		SNET_PEER_RELIABLE_WINDOWS = 16,
		SNET_PEER_RELIABLE_WINDOW_SIZE = 0x1000,
		SNET_PEER_FREE_RELIABLE_WINDOWS = 8
	};

	typedef struct _SNetChannel
	{
		snet_uint16  outgoingReliableSequenceNumber;
		snet_uint16  outgoingUnreliableSequenceNumber;
		snet_uint16  usedReliableWindows;
		snet_uint16  reliableWindows[SNET_PEER_RELIABLE_WINDOWS];
		snet_uint16  incomingReliableSequenceNumber;
		snet_uint16  incomingUnreliableSequenceNumber;
		SNetList     incomingReliableCommands;
		SNetList     incomingUnreliableCommands;
	} SNetChannel;

	/**
	* An SNet peer which data packets may be sent or received from.
	*
	* No fields should be modified unless otherwise specified.
	*/
	typedef struct _SNetPeer
	{
		SNetListNode  dispatchList;
		struct _SNetHost * host;
		snet_uint16   outgoingPeerID;
		snet_uint16   incomingPeerID;
		snet_uint32   connectID;
		snet_uint8    outgoingSessionID;
		snet_uint8    incomingSessionID;
		SNetAddress   address;            /**< Internet address of the peer */
		void *        data;               /**< Application private data, may be freely modified */
		SNetPeerState state;
		SNetChannel * channels;
		size_t        channelCount;       /**< Number of channels allocated for communication with peer */
		snet_uint32   incomingBandwidth;  /**< Downstream bandwidth of the client in bytes/second */
		snet_uint32   outgoingBandwidth;  /**< Upstream bandwidth of the client in bytes/second */
		snet_uint32   incomingBandwidthThrottleEpoch;
		snet_uint32   outgoingBandwidthThrottleEpoch;
		snet_uint32   incomingDataTotal;
		snet_uint32   outgoingDataTotal;
		snet_uint32   lastSendTime;
		snet_uint32   lastReceiveTime;
		snet_uint32   nextTimeout;
		snet_uint32   earliestTimeout;
		snet_uint32   packetLossEpoch;
		snet_uint32   packetsSent;
		snet_uint32   packetsLost;
		snet_uint32   packetLoss;          /**< mean packet loss of reliable packets as a ratio with respect to the constant SNET_PEER_PACKET_LOSS_SCALE */
		snet_uint32   packetLossVariance;
		snet_uint32   packetThrottle;
		snet_uint32   packetThrottleLimit;
		snet_uint32   packetThrottleCounter;
		snet_uint32   packetThrottleEpoch;
		snet_uint32   packetThrottleAcceleration;
		snet_uint32   packetThrottleDeceleration;
		snet_uint32   packetThrottleInterval;
		snet_uint32   pingInterval;
		snet_uint32   timeoutLimit;
		snet_uint32   timeoutMinimum;
		snet_uint32   timeoutMaximum;
		snet_uint32   lastRoundTripTime;
		snet_uint32   lowestRoundTripTime;
		snet_uint32   lastRoundTripTimeVariance;
		snet_uint32   highestRoundTripTimeVariance;
		snet_uint32   roundTripTime;            /**< mean round trip time (RTT), in milliseconds, between sending a reliable packet and receiving its acknowledgement */
		snet_uint32   roundTripTimeVariance;
		snet_uint32   mtu;
		snet_uint32   windowSize;
		snet_uint32   reliableDataInTransit;
		snet_uint16   outgoingReliableSequenceNumber;
		SNetList      acknowledgements;
		SNetList      sentReliableCommands;
		SNetList      sentUnreliableCommands;
		SNetList      outgoingReliableCommands;
		SNetList      outgoingUnreliableCommands;
		SNetList      dispatchedCommands;
		int           needsDispatch;
		snet_uint16   incomingUnsequencedGroup;
		snet_uint16   outgoingUnsequencedGroup;
		snet_uint32   unsequencedWindow[SNET_PEER_UNSEQUENCED_WINDOW_SIZE / 32];
		snet_uint32   eventData;
		size_t        totalWaitingData;
	} SNetPeer;

	/** An SNet packet compressor for compressing UDP packets before socket sends or receives.
	*/
	typedef struct _SNetCompressor
	{
		/** Context data for the compressor. Must be non-NULL. */
		void * context;
		/** Compresses from inBuffers[0:inBufferCount-1], containing inLimit bytes, to outData, outputting at most outLimit bytes. Should return 0 on failure. */
		size_t(SNET_CALLBACK * compress) (void * context, const SNetBuffer * inBuffers, size_t inBufferCount, size_t inLimit, snet_uint8 * outData, size_t outLimit);
		/** Decompresses from inData, containing inLimit bytes, to outData, outputting at most outLimit bytes. Should return 0 on failure. */
		size_t(SNET_CALLBACK * decompress) (void * context, const snet_uint8 * inData, size_t inLimit, snet_uint8 * outData, size_t outLimit);
		/** Destroys the context when compression is disabled or the host is destroyed. May be NULL. */
		void (SNET_CALLBACK * destroy) (void * context);
	} SNetCompressor;

	/** Callback that computes the checksum of the data held in buffers[0:bufferCount-1] */
	typedef snet_uint32(SNET_CALLBACK * SNetChecksumCallback) (const SNetBuffer * buffers, size_t bufferCount);

	/** Callback for intercepting received raw UDP packets. Should return 1 to intercept, 0 to ignore, or -1 to propagate an error. */
	typedef int (SNET_CALLBACK * SNetInterceptCallback) (struct _SNetHost * host, struct _SNetEvent * event);

	/** An SNet host for communicating with peers.
	*
	* No fields should be modified unless otherwise stated.

	@sa snet_host_create()
	@sa snet_host_destroy()
	@sa snet_host_connect()
	@sa snet_host_service()
	@sa snet_host_flush()
	@sa snet_host_broadcast()
	@sa snet_host_compress()
	@sa snet_host_compress_with_range_coder()
	@sa snet_host_channel_limit()
	@sa snet_host_bandwidth_limit()
	@sa snet_host_bandwidth_throttle()
	*/
	typedef struct _SNetHost
	{
		SNetSocket           socket;
		SNetAddress          address;                     /**< Internet address of the host */
		snet_uint32          incomingBandwidth;           /**< downstream bandwidth of the host */
		snet_uint32          outgoingBandwidth;           /**< upstream bandwidth of the host */
		snet_uint32          bandwidthThrottleEpoch;
		snet_uint32          mtu;
		snet_uint32          randomSeed;
		int                  recalculateBandwidthLimits;
		SNetPeer *           peers;                       /**< array of peers allocated for this host */
		size_t               peerCount;                   /**< number of peers allocated for this host */
		size_t               channelLimit;                /**< maximum number of channels allowed for connected peers */
		snet_uint32          serviceTime;
		SNetList             dispatchQueue;
		int                  continueSending;
		size_t               packetSize;
		snet_uint16          headerFlags;
		SNetProtocol         commands[SNET_PROTOCOL_MAXIMUM_PACKET_COMMANDS];
		size_t               commandCount;
		SNetBuffer           buffers[SNET_BUFFER_MAXIMUM];
		size_t               bufferCount;
		SNetChecksumCallback checksum;                    /**< callback the user can set to enable packet checksums for this host */
		SNetCompressor       compressor;
		snet_uint8           packetData[2][SNET_PROTOCOL_MAXIMUM_MTU];
		SNetAddress          receivedAddress;
		snet_uint8 *         receivedData;
		size_t               receivedDataLength;
		snet_uint32          totalSentData;               /**< total data sent, user should reset to 0 as needed to prevent overflow */
		snet_uint32          totalSentPackets;            /**< total UDP packets sent, user should reset to 0 as needed to prevent overflow */
		snet_uint32          totalReceivedData;           /**< total data received, user should reset to 0 as needed to prevent overflow */
		snet_uint32          totalReceivedPackets;        /**< total UDP packets received, user should reset to 0 as needed to prevent overflow */
		SNetInterceptCallback intercept;                  /**< callback the user can set to intercept received raw UDP packets */
		size_t               connectedPeers;
		size_t               bandwidthLimitedPeers;
		size_t               duplicatePeers;              /**< optional number of allowed peers from duplicate IPs, defaults to SNET_PROTOCOL_MAXIMUM_PEER_ID */
		size_t               maximumPacketSize;           /**< the maximum allowable packet size that may be sent or received on a peer */
		size_t               maximumWaitingData;          /**< the maximum aggregate amount of buffer space a peer may use waiting for packets to be delivered */
	} SNetHost;

	/**
	* An SNet event type, as specified in @ref SNetEvent.
	*/
	typedef enum _SNetEventType
	{
		/** no event occurred within the specified time limit */
		SNET_EVENT_TYPE_NONE = 0,

		/** a connection request initiated by snet_host_connect has completed.
		* The peer field contains the peer which successfully connected.
		*/
		SNET_EVENT_TYPE_CONNECT = 1,

		/** a peer has disconnected.  This event is generated on a successful
		* completion of a disconnect initiated by snet_pper_disconnect, if
		* a peer has timed out, or if a connection request intialized by
		* snet_host_connect has timed out.  The peer field contains the peer
		* which disconnected. The data field contains user supplied data
		* describing the disconnection, or 0, if none is available.
		*/
		SNET_EVENT_TYPE_DISCONNECT = 2,

		/** a packet has been received from a peer.  The peer field specifies the
		* peer which sent the packet.  The channelID field specifies the channel
		* number upon which the packet was received.  The packet field contains
		* the packet that was received; this packet must be destroyed with
		* snet_packet_destroy after use.
		*/
		SNET_EVENT_TYPE_RECEIVE = 3
	} SNetEventType;

	/**
	* An SNet event as returned by snet_host_service().

	@sa snet_host_service
	*/
	typedef struct _SNetEvent
	{
		SNetEventType        type;      /**< type of the event */
		SNetPeer *           peer;      /**< peer that generated a connect, disconnect or receive event */
		snet_uint8           channelID; /**< channel on the peer that generated the event, if appropriate */
		snet_uint32          data;      /**< data associated with the event, if appropriate */
		SNetPacket *         packet;    /**< packet associated with the event, if appropriate */
	} SNetEvent;

	/** @defgroup global SNet global functions
	@{
	*/

	/**
	Initializes SNet globally.  Must be called prior to using any functions in
	SNet.
	@returns 0 on success, < 0 on failure
	*/
	SNET_API int snet_initialize(void);

	/**
	Initializes SNet globally and supplies user-overridden callbacks. Must be called prior to using any functions in SNet. Do not use snet_initialize() if you use this variant. Make sure the SNetCallbacks structure is zeroed out so that any additional callbacks added in future versions will be properly ignored.

	@param version the constant SNET_VERSION should be supplied so SNet knows which version of SNetCallbacks struct to use
	@param inits user-overridden callbacks where any NULL callbacks will use SNet's defaults
	@returns 0 on success, < 0 on failure
	*/
	SNET_API int snet_initialize_with_callbacks(SNetVersion version, const SNetCallbacks * inits);

	/**
	Shuts down SNet globally.  Should be called when a program that has
	initialized SNet exits.
	*/
	SNET_API void snet_deinitialize(void);

	/**
	Gives the linked version of the SNet library.
	@returns the version number
	*/
	SNET_API SNetVersion snet_linked_version(void);

	/** @} */

	/** @defgroup private SNet private implementation functions */

	/**
	Returns the wall-time in milliseconds.  Its initial value is unspecified
	unless otherwise set.
	*/
	SNET_API snet_uint32 snet_time_get(void);
	/**
	Sets the current wall-time in milliseconds.
	*/
	SNET_API void snet_time_set(snet_uint32);

	/** @defgroup socket SNet socket functions
	@{
	*/
	SNET_API SNetSocket snet_socket_create(SNetSocketType);
	SNET_API int        snet_socket_bind(SNetSocket, const SNetAddress *);
	SNET_API int        snet_socket_get_address(SNetSocket, SNetAddress *);
	SNET_API int        snet_socket_listen(SNetSocket, int);
	SNET_API SNetSocket snet_socket_accept(SNetSocket, SNetAddress *);
	SNET_API int        snet_socket_connect(SNetSocket, const SNetAddress *);
	SNET_API int        snet_socket_send(SNetSocket, const SNetAddress *, const SNetBuffer *, size_t);
	SNET_API int        snet_socket_receive(SNetSocket, SNetAddress *, SNetBuffer *, size_t);
	SNET_API int        snet_socket_wait(SNetSocket, snet_uint32 *, snet_uint32);
	SNET_API int        snet_socket_set_option(SNetSocket, SNetSocketOption, int);
	SNET_API int        snet_socket_get_option(SNetSocket, SNetSocketOption, int *);
	SNET_API int        snet_socket_shutdown(SNetSocket, SNetSocketShutdown);
	SNET_API void       snet_socket_destroy(SNetSocket);
	SNET_API int        snet_socketset_select(SNetSocket, SNetSocketSet *, SNetSocketSet *, snet_uint32);

	/** @} */

	/** @defgroup Address SNet address functions
	@{
	*/
	/** Attempts to resolve the host named by the parameter hostName and sets
	the host field in the address parameter if successful.
	@param address destination to store resolved address
	@param hostName host name to lookup
	@retval 0 on success
	@retval < 0 on failure
	@returns the address of the given hostName in address on success
	*/
	SNET_API int snet_address_set_host(SNetAddress * address, const char * hostName);

	/** Gives the printable form of the IP address specified in the address parameter.
	@param address    address printed
	@param hostName   destination for name, must not be NULL
	@param nameLength maximum length of hostName.
	@returns the null-terminated name of the host in hostName on success
	@retval 0 on success
	@retval < 0 on failure
	*/
	SNET_API int snet_address_get_host_ip(const SNetAddress * address, char * hostName, size_t nameLength);

	/** Attempts to do a reverse lookup of the host field in the address parameter.
	@param address    address used for reverse lookup
	@param hostName   destination for name, must not be NULL
	@param nameLength maximum length of hostName.
	@returns the null-terminated name of the host in hostName on success
	@retval 0 on success
	@retval < 0 on failure
	*/
	SNET_API int snet_address_get_host(const SNetAddress * address, char * hostName, size_t nameLength);

	/** @} */

	SNET_API SNetPacket * snet_packet_create(const void *, size_t, snet_uint32);
	SNET_API void         snet_packet_destroy(SNetPacket *);
	SNET_API int          snet_packet_resize(SNetPacket *, size_t);
	SNET_API snet_uint32  snet_crc32(const SNetBuffer *, size_t);

	SNET_API SNetHost * snet_host_create(const SNetAddress *, size_t, size_t, snet_uint32, snet_uint32);
	SNET_API void       snet_host_destroy(SNetHost *);
	SNET_API SNetPeer * snet_host_connect(SNetHost *, const SNetAddress *, size_t, snet_uint32);
	SNET_API int        snet_host_check_events(SNetHost *, SNetEvent *);
	SNET_API int        snet_host_service(SNetHost *, SNetEvent *, snet_uint32);
	SNET_API void       snet_host_flush(SNetHost *);
	SNET_API void       snet_host_broadcast(SNetHost *, snet_uint8, SNetPacket *);
	SNET_API void       snet_host_compress(SNetHost *, const SNetCompressor *);
	SNET_API int        snet_host_compress_with_range_coder(SNetHost * host);
	SNET_API void       snet_host_channel_limit(SNetHost *, size_t);
	SNET_API void       snet_host_bandwidth_limit(SNetHost *, snet_uint32, snet_uint32);
	extern   void       snet_host_bandwidth_throttle(SNetHost *);
	extern  snet_uint32 snet_host_random_seed(void);

	SNET_API int                 snet_peer_send(SNetPeer *, snet_uint8, SNetPacket *);
	SNET_API SNetPacket *        snet_peer_receive(SNetPeer *, snet_uint8 * channelID);
	SNET_API void                snet_peer_ping(SNetPeer *);
	SNET_API void                snet_peer_ping_interval(SNetPeer *, snet_uint32);
	SNET_API void                snet_peer_timeout(SNetPeer *, snet_uint32, snet_uint32, snet_uint32);
	SNET_API void                snet_peer_reset(SNetPeer *);
	SNET_API void                snet_peer_disconnect(SNetPeer *, snet_uint32);
	SNET_API void                snet_peer_disconnect_now(SNetPeer *, snet_uint32);
	SNET_API void                snet_peer_disconnect_later(SNetPeer *, snet_uint32);
	SNET_API void                snet_peer_throttle_configure(SNetPeer *, snet_uint32, snet_uint32, snet_uint32);
	extern int                   snet_peer_throttle(SNetPeer *, snet_uint32);
	extern void                  snet_peer_reset_queues(SNetPeer *);
	extern void                  snet_peer_setup_outgoing_command(SNetPeer *, SNetOutgoingCommand *);
	extern SNetOutgoingCommand * snet_peer_queue_outgoing_command(SNetPeer *, const SNetProtocol *, SNetPacket *, snet_uint32, snet_uint16);
	extern SNetIncomingCommand * snet_peer_queue_incoming_command(SNetPeer *, const SNetProtocol *, const void *, size_t, snet_uint32, snet_uint32);
	extern SNetAcknowledgement * snet_peer_queue_acknowledgement(SNetPeer *, const SNetProtocol *, snet_uint16);
	extern void                  snet_peer_dispatch_incoming_unreliable_commands(SNetPeer *, SNetChannel *);
	extern void                  snet_peer_dispatch_incoming_reliable_commands(SNetPeer *, SNetChannel *);
	extern void                  snet_peer_on_connect(SNetPeer *);
	extern void                  snet_peer_on_disconnect(SNetPeer *);

	SNET_API void * snet_range_coder_create(void);
	SNET_API void   snet_range_coder_destroy(void *);
	SNET_API size_t snet_range_coder_compress(void *, const SNetBuffer *, size_t, size_t, snet_uint8 *, size_t);
	SNET_API size_t snet_range_coder_decompress(void *, const snet_uint8 *, size_t, snet_uint8 *, size_t);

	extern size_t snet_protocol_command_size(snet_uint8);

#ifdef __cplusplus
}
#endif

#endif /* __SNET_SNET_H__ */