#ifndef INCLUDE_GUARD_time_1364388461
#define INCLUDE_GUARD_time_1364388461

#include "common.h"

int gettimeofday(struct timeval *__p, void *__tz);
struct tm *localtime_r(const time_t *timep, struct tm *result);

#endif
