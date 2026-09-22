#include "time_utils.h"
#include <sys/time.h>
#include <stddef.h> 

int64_t getCurrentTimeUs()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

