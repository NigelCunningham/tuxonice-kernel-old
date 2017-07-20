/*
 * include/linux/tuxonice.h
 *
 * Copyright (C) 2015 Nigel Cunningham (nigel at tuxonice net)
 *
 * This file is released under the GPLv2.
 */

#ifndef INCLUDE_LINUX_TUXONICE_H
#define INCLUDE_LINUX_TUXONICE_H
#ifdef CONFIG_TOI_INCREMENTAL
extern void toi_set_logbuf_untracked(void);
extern int toi_make_writable(pgd_t *pgd, unsigned long address);

static inline int toi_incremental_support(void)
{
    return 1;
}

/* Copy Before Write */
struct toi_cbw {
    unsigned long pfn;
    void *virt;
    struct toi_cbw *next;
};

struct toi_cbw_state {
    bool active;            /* Is a fault handler running? */
    bool enabled;           /* Are we doing copy before write? */
    int size;               /* The number of pages allocated */
    struct toi_cbw *first, *next, *last;  /* Pointers to the data structure */
};

#define CBWS_PER_PAGE (PAGE_SIZE / sizeof(struct toi_cbw))
DECLARE_PER_CPU(struct toi_cbw_state, toi_cbw_states);
#else
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
#endif
