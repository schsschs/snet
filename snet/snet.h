#ifndef __SNET_H__
#define __SNET_H__

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
#define SNET_VERSION_CREATE(major, minor, patch) (((major)<<16) | ((minor)<<8)|(patch))
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

	// TODO

#ifdef __cplusplus
}
#endif

#endif // !__SNET_H__
