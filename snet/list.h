/**
@file  list.h
@brief SNet list management
*/
#ifndef __SNET_LIST_H__
#define __SNET_LIST_H__

#include <stdlib.h>

typedef struct _SNetListNode
{
	struct _SNetListNode * next;
	struct _SNetListNode * previous;
} SNetListNode;

typedef SNetListNode * SNetListIterator;

typedef struct _SNetList
{
	SNetListNode sentinel;
} SNetList;

extern void snet_list_clear(SNetList *);

extern SNetListIterator snet_list_insert(SNetListIterator, void *);
extern void * snet_list_remove(SNetListIterator);
extern SNetListIterator snet_list_move(SNetListIterator, void *, void *);

extern size_t snet_list_size(SNetList *);

#define snet_list_begin(list) ((list) -> sentinel.next)
#define snet_list_end(list) (& (list) -> sentinel)

#define snet_list_empty(list) (snet_list_begin (list) == snet_list_end (list))

#define snet_list_next(iterator) ((iterator) -> next)
#define snet_list_previous(iterator) ((iterator) -> previous)

#define snet_list_front(list) ((void *) (list) -> sentinel.next)
#define snet_list_back(list) ((void *) (list) -> sentinel.previous)

#endif /* __SNET_LIST_H__ */

