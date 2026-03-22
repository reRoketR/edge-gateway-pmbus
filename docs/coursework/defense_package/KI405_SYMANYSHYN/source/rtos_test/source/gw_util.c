/*******************************************************************************
 * File Name:   gw_util.c
 *
 * Description: Shared utility helpers – implementation.
 *
 ******************************************************************************/

#include "gw_util.h"

#include <stdio.h>

int fmt_u64(char *buf, size_t sz, uint64_t val)
{
    if (val == 0u)
    {
        return snprintf(buf, sz, "0");
    }

    char tmp[21]; /* max uint64 is 20 digits */
    int pos = (int)sizeof(tmp) - 1;
    tmp[pos] = '\0';

    while (val > 0u && pos > 0)
    {
        pos--;
        tmp[pos] = (char)('0' + (int)(val % 10u));
        val /= 10u;
    }

    return snprintf(buf, sz, "%s", &tmp[pos]);
}
