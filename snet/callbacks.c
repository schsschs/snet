/**
@file callbacks.c
@brief SNet callback functions
*/
#define SNET_BUILDING_LIB 1
#include "snet/snet.h"

static SNetCallbacks callbacks = { malloc, free, abort };

int
snet_initialize_with_callbacks(SNetVersion version, const SNetCallbacks * inits)
{
	if (version < SNET_VERSION_CREATE(1, 3, 0))
		return -1;

	if (inits->malloc != NULL || inits->free != NULL)
	{
		if (inits->malloc == NULL || inits->free == NULL)
			return -1;

		callbacks.malloc = inits->malloc;
		callbacks.free = inits->free;
	}

	if (inits->no_memory != NULL)
		callbacks.no_memory = inits->no_memory;

	return snet_initialize();
}

SNetVersion
snet_linked_version(void)
{
	return SNET_VERSION;
}

void *
snet_malloc(size_t size)
{
	void * memory = callbacks.malloc(size);

	if (memory == NULL)
		callbacks.no_memory();

	return memory;
}

void
snet_free(void * memory)
{
	callbacks.free(memory);
}

