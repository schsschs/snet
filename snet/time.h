/**
@file  time.h
@brief SNet time constants and macros
*/
#ifndef __SNET_TIME_H__
#define __SNET_TIME_H__

#define SNET_TIME_OVERFLOW 86400000

#define SNET_TIME_LESS(a, b) ((a) - (b) >= SNET_TIME_OVERFLOW)
#define SNET_TIME_GREATER(a, b) ((b) - (a) >= SNET_TIME_OVERFLOW)
#define SNET_TIME_LESS_EQUAL(a, b) (! SNET_TIME_GREATER (a, b))
#define SNET_TIME_GREATER_EQUAL(a, b) (! SNET_TIME_LESS (a, b))

#define SNET_TIME_DIFFERENCE(a, b) ((a) - (b) >= SNET_TIME_OVERFLOW ? (b) - (a) : (a) - (b))

#endif /* __SNET_TIME_H__ */