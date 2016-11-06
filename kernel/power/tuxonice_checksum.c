/*
 * kernel/power/tuxonice_checksum.c
 *
 * Copyright (C) 2006-2015 Nigel Cunningham (nigel at nigelcunningham com au)
 *
 * This file is released under the GPLv2.
 *
 * This file contains data checksum routines for TuxOnIce,
 * using cryptoapi. They are used to locate any modifications
 * made to pageset 2 while we're saving it.
 */

#include <crypto/hash.h>
#include <linux/suspend.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/scatterlist.h>

#include "tuxonice.h"
#include "tuxonice_modules.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_io.h"
#include "tuxonice_pageflags.h"
#include "tuxonice_checksum.h"
#include "tuxonice_pagedir.h"
#include "tuxonice_alloc.h"
#include "tuxonice_ui.h"

static struct toi_module_ops toi_checksum_ops;

/* Constant at the mo, but I might allow tuning later */
static char toi_checksum_name[32] = "md4";
/* Bytes per checksum */
#define CHECKSUM_SIZE (16)

#define CHECKSUMS_PER_PAGE ((PAGE_SIZE - sizeof(void *)) / CHECKSUM_SIZE)

struct cpu_context {
        struct shash_desc *desc;
        char *buf;
};

static DEFINE_PER_CPU(struct cpu_context, contexts);
static int pages_allocated;
static unsigned long page_list;

static int toi_num_resaved;

static unsigned long this_checksum, next_page;
static int checksum_count;

static inline int checksum_pages_needed(void)
{
        return DIV_ROUND_UP(pagedir2.size, CHECKSUMS_PER_PAGE);
}

/* ---- Local buffer management ---- */

/*
 * toi_checksum_cleanup
 *
 * Frees memory allocated for our labours.
 */
static void toi_checksum_cleanup(int ending_cycle)
{
        int cpu;

        if (ending_cycle) {
                for_each_online_cpu(cpu) {
                        struct cpu_context *this = &per_cpu(contexts, cpu);
                        if (this->desc) {
                                size_t size = sizeof(*this->desc) +
                                        crypto_shash_descsize(this->desc->tfm);
                                crypto_free_shash(this->desc->tfm);
                                toi_kfree(26, this->desc, size);
                                this->desc = NULL;
                        }

                        if (this->buf) {
                                toi_free_page(27, (unsigned long) this->buf);
                                this->buf = NULL;
                        }
                }
        }
}

/*
 * toi_crypto_initialise
 *
 * Prepare to do some work by allocating buffers and transforms.
 * Returns: Int: Zero. Even if we can't set up checksum, we still
 * seek to hibernate.
 */
static int toi_checksum_initialise(int starting_cycle)
{
        int cpu;

        if (!(starting_cycle & SYSFS_HIBERNATE) || !toi_checksum_ops.enabled)
                return 0;

        if (!*toi_checksum_name) {
                printk(KERN_INFO "TuxOnIce: No checksum algorithm name set.\n");
                return 1;
        }

        for_each_online_cpu(cpu) {
                struct cpu_context *this = &per_cpu(contexts, cpu);
                struct page *page;
                struct crypto_shash *tfm;
                struct shash_desc *tdesc;


                tfm = crypto_alloc_shash(toi_checksum_name, 0, 0);
                if (IS_ERR(tfm)) {
                        printk(KERN_INFO "TuxOnIce: Failed to initialise the "
                                "%s checksum algorithm: %ld.\n",
                                toi_checksum_name, (long) tfm);
                        return 1;
                }

                tdesc = toi_kzalloc(26, sizeof(*this->desc) + crypto_shash_descsize(tfm),
                        GFP_KERNEL);

                if (!tdesc) {
                        printk(KERN_INFO "TuxOnIce: Failed to allocate memory "
                                "in checksum initialisation.\n");
                        return 1;
                }
                tdesc->tfm = tfm;
                this->desc = tdesc;

                page = toi_alloc_page(27, GFP_KERNEL);
                if (!page)
                        return 1;
                this->buf = page_address(page);
        }
        return 0;
}

/*
 * toi_checksum_print_debug_stats
 * @buffer: Pointer to a buffer into which the debug info will be printed.
 * @size: Size of the buffer.
 *
 * Print information to be recorded for debugging purposes into a buffer.
 * Returns: Number of characters written to the buffer.
 */

static int toi_checksum_print_debug_stats(char *buffer, int size)
{
        int len;

        if (!toi_checksum_ops.enabled)
                return scnprintf(buffer, size,
                        "- Checksumming disabled.\n");

        len = scnprintf(buffer, size, "- Checksum method is '%s'.\n",
                        toi_checksum_name);
        len += scnprintf(buffer + len, size - len,
                "  %d pages resaved in atomic copy.\n", toi_num_resaved);
        return len;
}

static int toi_checksum_memory_needed(void)
{
        return toi_checksum_ops.enabled ?
                checksum_pages_needed() << PAGE_SHIFT : 0;
}

static int toi_checksum_storage_needed(void)
{
        if (toi_checksum_ops.enabled)
                return strlen(toi_checksum_name) + sizeof(int) + 1;
        else
                return 0;
}

/*
 * toi_checksum_save_config_info
 * @buffer: Pointer to a buffer of size PAGE_SIZE.
 *
 * Save informaton needed when reloading the image at resume time.
 * Returns: Number of bytes used for saving our data.
 */
static int toi_checksum_save_config_info(char *buffer)
{
        int namelen = strlen(toi_checksum_name) + 1;
        int total_len;

        *((unsigned int *) buffer) = namelen;
        strncpy(buffer + sizeof(unsigned int), toi_checksum_name, namelen);
        total_len = sizeof(unsigned int) + namelen;
        return total_len;
}

/* toi_checksum_load_config_info
 * @buffer: Pointer to the start of the data.
 * @size: Number of bytes that were saved.
 *
 * Description:        Reload information needed for dechecksuming the image at
 * resume time.
 */
static void toi_checksum_load_config_info(char *buffer, int size)
{
        int namelen;

        namelen = *((unsigned int *) (buffer));
        strncpy(toi_checksum_name, buffer + sizeof(unsigned int),
                        namelen);
        return;
}

/*
 * Free Checksum Memory
 */

void free_checksum_pages(void)
{
        while (pages_allocated) {
                unsigned long next = *((unsigned long *) page_list);
                ClearPageNosave(virt_to_page(page_list));
                toi_free_page(15, (unsigned long) page_list);
                page_list = next;
                pages_allocated--;
        }
}

/*
 * Allocate Checksum Memory
 */

int allocate_checksum_pages(void)
{
        int pages_needed = checksum_pages_needed();

        if (!toi_checksum_ops.enabled)
                return 0;

        while (pages_allocated < pages_needed) {
                unsigned long *new_page =
                  (unsigned long *) toi_get_zeroed_page(15, TOI_ATOMIC_GFP);
                if (!new_page) {
                        printk(KERN_ERR "Unable to allocate checksum pages.\n");
                        return -ENOMEM;
                }
                SetPageNosave(virt_to_page(new_page));
                (*new_page) = page_list;
                page_list = (unsigned long) new_page;
                pages_allocated++;
        }

        next_page = (unsigned long) page_list;
        checksum_count = 0;

        return 0;
}

char *tuxonice_get_next_checksum(void)
{
        if (!toi_checksum_ops.enabled)
                return NULL;

        if (checksum_count % CHECKSUMS_PER_PAGE)
                this_checksum += CHECKSUM_SIZE;
        else {
                this_checksum = next_page + sizeof(void *);
                next_page = *((unsigned long *) next_page);
        }

        checksum_count++;
        return (char *) this_checksum;
}

int tuxonice_calc_checksum(struct page *page, char *checksum_locn)
{
        char *pa;
        int result, cpu = smp_processor_id();
        struct cpu_context *ctx = &per_cpu(contexts, cpu);

        if (!toi_checksum_ops.enabled)
                return 0;

        pa = kmap(page);
        memcpy(ctx->buf, pa, PAGE_SIZE);
        kunmap(page);
        result = crypto_shash_digest(ctx->desc, ctx->buf, PAGE_SIZE,
                                                checksum_locn);
        if (result)
                printk(KERN_ERR "TuxOnIce checksumming: crypto_shash_digest "
                                "returned %d.\n", result);
        return result;
}
/*
 * Calculate checksums
 */

void check_checksums(void)
{
        int index = 0, cpu = smp_processor_id();
        char current_checksum[CHECKSUM_SIZE];
        struct cpu_context *ctx = &per_cpu(contexts, cpu);
        unsigned long pfn;

        if (!toi_checksum_ops.enabled) {
                toi_message(TOI_IO, TOI_VERBOSE, 0, "Checksumming disabled.");
                return;
        }

        next_page = (unsigned long) page_list;

        toi_num_resaved = 0;
        this_checksum = 0;

        toi_trace_index++;

        toi_message(TOI_IO, TOI_VERBOSE, 0, "Verifying checksums.");
        memory_bm_position_reset(pageset2_map);
        for (pfn = memory_bm_next_pfn(pageset2_map, 0); pfn != BM_END_OF_MAP;
                        pfn = memory_bm_next_pfn(pageset2_map, 0)) {
                int ret, resave_needed = false;
                char *pa;
                struct page *page = pfn_to_page(pfn);

                if (index < checksum_count) {
                    if (index % CHECKSUMS_PER_PAGE) {
                        this_checksum += CHECKSUM_SIZE;
                    } else {
                        this_checksum = next_page + sizeof(void *);
                        next_page = *((unsigned long *) next_page);
                    }

                    /* Done when IRQs disabled so must be atomic */
                    pa = kmap_atomic(page);
                    memcpy(ctx->buf, pa, PAGE_SIZE);
                    kunmap_atomic(pa);
                    ret = crypto_shash_digest(ctx->desc, ctx->buf, PAGE_SIZE,
                            current_checksum);

                    if (ret) {
                        printk(KERN_INFO "Digest failed. Returned %d.\n", ret);
                        return;
                    }

                    resave_needed = memcmp(current_checksum, (char *) this_checksum,
                            CHECKSUM_SIZE);
                } else {
                    resave_needed = true;
                }

                if (resave_needed) {
                        TOI_TRACE_DEBUG(pfn, "_Resaving %d", resave_needed);
                        SetPageResave(pfn_to_page(pfn));
                        toi_num_resaved++;
                        if (test_action_state(TOI_ABORT_ON_RESAVE_NEEDED))
                                set_abort_result(TOI_RESAVE_NEEDED);
                }

                index++;
        }
        toi_message(TOI_IO, TOI_VERBOSE, 0, "Checksum verification complete.");
}

static struct toi_sysfs_data sysfs_params[] = {
        SYSFS_INT("enabled", SYSFS_RW, &toi_checksum_ops.enabled, 0, 1, 0,
                        NULL),
        SYSFS_BIT("abort_if_resave_needed", SYSFS_RW, &toi_bkd.toi_action,
                        TOI_ABORT_ON_RESAVE_NEEDED, 0)
};

/*
 * Ops structure.
 */
static struct toi_module_ops toi_checksum_ops = {
        .type                        = MISC_MODULE,
        .name                        = "checksumming",
        .directory                = "checksum",
        .module                        = THIS_MODULE,
        .initialise                = toi_checksum_initialise,
        .cleanup                = toi_checksum_cleanup,
        .print_debug_info        = toi_checksum_print_debug_stats,
        .save_config_info        = toi_checksum_save_config_info,
        .load_config_info        = toi_checksum_load_config_info,
        .memory_needed                = toi_checksum_memory_needed,
        .storage_needed                = toi_checksum_storage_needed,

        .sysfs_data                = sysfs_params,
        .num_sysfs_entries        = sizeof(sysfs_params) /
                sizeof(struct toi_sysfs_data),
};

/* ---- Registration ---- */
int toi_checksum_init(void)
{
        int result = toi_register_module(&toi_checksum_ops);
        return result;
}

void toi_checksum_exit(void)
{
        toi_unregister_module(&toi_checksum_ops);
}
