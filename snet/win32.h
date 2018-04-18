/**
@file  win32.h
@brief SNet Win32 header
*/
#ifndef __SNET_WIN32_H__
#define __SNET_WIN32_H__

#ifdef _MSC_VER
#ifdef SNET_BUILDING_LIB
#pragma warning (disable: 4267) // size_t to int conversion
#pragma warning (disable: 4244) // 64bit to 32bit int
#pragma warning (disable: 4018) // signed/unsigned mismatch
#pragma warning (disable: 4146) // unary minus operator applied to unsigned type
#endif
#endif

#include <stdlib.h>
#include <winsock2.h>

typedef SOCKET SNetSocket;

#define SNET_SOCKET_NULL INVALID_SOCKET

#define SNET_HOST_TO_NET_16(value) (htons (value))
#define SNET_HOST_TO_NET_32(value) (htonl (value))

#define SNET_NET_TO_HOST_16(value) (ntohs (value))
#define SNET_NET_TO_HOST_32(value) (ntohl (value))

typedef struct
{
	size_t dataLength;
	void * data;
} SNetBuffer;

#define SNET_CALLBACK __cdecl

#ifdef SNET_DLL
#ifdef SNET_BUILDING_LIB
#define SNET_API __declspec( dllexport )
#else
#define SNET_API __declspec( dllimport )
#endif /* SNET_BUILDING_LIB */
#else /* !SNET_DLL */
#define SNET_API extern
#endif /* SNET_DLL */

typedef fd_set SNetSocketSet;

#define SNET_SOCKETSET_EMPTY(sockset)          FD_ZERO (& (sockset))
#define SNET_SOCKETSET_ADD(sockset, socket)    FD_SET (socket, & (sockset))
#define SNET_SOCKETSET_REMOVE(sockset, socket) FD_CLR (socket, & (sockset))
#define SNET_SOCKETSET_CHECK(sockset, socket)  FD_ISSET (socket, & (sockset))

#endif /* __SNET_WIN32_H__ */


