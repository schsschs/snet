/**
@file  callbacks.h
@brief SNet callbacks
*/
#ifndef __SNET_CALLBACKS_H__
#define __SNET_CALLBACKS_H__

#include <stdlib.h>

typedef struct _SNetCallbacks
{
	void * (SNET_CALLBACK * malloc) (size_t size);
	void (SNET_CALLBACK * free) (void * memory);
	void (SNET_CALLBACK * no_memory) (void);
} SNetCallbacks;

/** @defgroup callbacks SNet internal callbacks
@{
@ingroup private
*/
extern void * snet_malloc(size_t);
extern void   snet_free(void *);

/** @} */

#endif /* __SNET_CALLBACKS_H__ */