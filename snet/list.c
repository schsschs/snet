/**
@file list.c
@brief SNet linked list functions
*/
#define SNET_BUILDING_LIB 1
#include "snet/snet.h"

/**
@defgroup list SNet linked list utility functions
@ingroup private
@{
*/
void
snet_list_clear(SNetList * list)
{
	list->sentinel.next = &list->sentinel;
	list->sentinel.previous = &list->sentinel;
}

SNetListIterator
snet_list_insert(SNetListIterator position, void * data)
{
	SNetListIterator result = (SNetListIterator)data;

	result->previous = position->previous;
	result->next = position;

	result->previous->next = result;
	position->previous = result;

	return result;
}

void *
snet_list_remove(SNetListIterator position)
{
	position->previous->next = position->next;
	position->next->previous = position->previous;

	return position;
}

SNetListIterator
snet_list_move(SNetListIterator position, void * dataFirst, void * dataLast)
{
	SNetListIterator first = (SNetListIterator)dataFirst,
		last = (SNetListIterator)dataLast;

	first->previous->next = last->next;
	last->next->previous = first->previous;

	first->previous = position->previous;
	last->next = position;

	first->previous->next = first;
	position->previous = last;

	return first;
}

size_t
snet_list_size(SNetList * list)
{
	size_t size = 0;

	for (SNetListIterator position = snet_list_begin(list);
		position != snet_list_end(list);
		position = snet_list_next(position))
		++size;

	return size;
}

/** @} */
