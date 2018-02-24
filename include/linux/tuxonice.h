/*
 * include/linux/tuxonice.h
 *
 * Copyright (C) 2015 Nigel Cunningham (nigel at tuxonice net)
 *
 * This file is released under the GPLv2.
 */

#ifndef INCLUDE_LINUX_TUXONICE_H
#define INCLUDE_LINUX_TUXONICE_H
#define toi_set_logbuf_untracked() do { } while(0)
static inline int toi_make_writable(pgd_t *pgd, unsigned long addr)
{
    return 0;
}

static inline int toi_incremental_support(void)
{
    return 0;
}
#endif
