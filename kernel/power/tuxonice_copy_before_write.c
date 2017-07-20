/*
 * kernel/power/tuxonice_copy_before_write.c
 *
 * Copyright (C) 2015 Nigel Cunningham (nigel at nigelcunningham com au)
 *
 * This file is released under the GPLv2.
 *
 * Routines (apart from the fault handling code) to deal with allocating memory
 * for copying pages before they are modified, restoring the contents and getting
 * the contents written to disk.
 */

#include <linux/percpu-defs.h>
#include <linux/sched.h>
#include <linux/tuxonice.h>
#include "tuxonice_alloc.h"
#include "tuxonice_modules.h"
#include "tuxonice_sysfs.h"
#include "tuxonice.h"

DEFINE_PER_CPU(struct toi_cbw_state, toi_cbw_states);
#define CBWS_PER_PAGE (PAGE_SIZE / sizeof(struct toi_cbw))
#define toi_cbw_pool_size 100

static void _toi_free_cbw_data(struct toi_cbw_state *state)
{
  struct toi_cbw *page_ptr, *ptr, *next;

  page_ptr = ptr = state->first;

  while(ptr) {
    next = ptr->next;

    if (ptr->virt) {
      toi__free_page(40, virt_to_page(ptr->virt));
    }
    if ((((unsigned long) ptr) & PAGE_MASK) != (unsigned long) page_ptr) {
      /* Must be on a new page - free the previous one. */
      toi__free_page(40, virt_to_page(page_ptr));
      page_ptr = ptr;
    }
    ptr = next;
  }

  if (page_ptr) {
    toi__free_page(40, virt_to_page(page_ptr));
  }

  state->first = state->next = state->last = NULL;
  state->size = 0;
}

void toi_free_cbw_data(void)
{
  int i;

  for_each_online_cpu(i) {
    struct toi_cbw_state *state = &per_cpu(toi_cbw_states, i);

    if (!state->first)
      continue;

    state->enabled = 0;

    while (state->active) {
      schedule();
    }

    _toi_free_cbw_data(state);
  }
}

static int _toi_allocate_cbw_data(struct toi_cbw_state *state)
{
  while(state->size < toi_cbw_pool_size) {
    int i;
    struct toi_cbw *ptr;

    ptr = (struct toi_cbw *) toi_get_zeroed_page(40, GFP_KERNEL);

    if (!ptr) {
      return -ENOMEM;
    }

    if (!state->first) {
      state->first = state->next = state->last = ptr;
    }

    for (i = 0; i < CBWS_PER_PAGE; i++) {
      struct toi_cbw *cbw = &ptr[i];

      cbw->virt = (char *) toi_get_zeroed_page(40, GFP_KERNEL);
      if (!cbw->virt) {
        state->size += i;
        printk("Out of memory allocating CBW pages.\n");
        return -ENOMEM;
      }

      if (cbw == state->first)
        continue;

      state->last->next = cbw;
      state->last = cbw;
    }

    state->size += CBWS_PER_PAGE;
  }

  state->enabled = 1;

  return 0;
}


int toi_allocate_cbw_data(void)
{
  int i, result;

  for_each_online_cpu(i) {
    struct toi_cbw_state *state = &per_cpu(toi_cbw_states, i);

    result = _toi_allocate_cbw_data(state);

    if (result)
      return result;
  }

  return 0;
}

void toi_cbw_restore(void)
{
  if (!toi_keeping_image)
    return;

}

void toi_cbw_write(void)
{
  if (!toi_keeping_image)
    return;

}

/**
 * toi_cbw_test_read - Test copy before write on one page
 *
 * Allocate copy before write buffers, then make one page only copy-before-write
 * and attempt to write to it. We should then be able to retrieve the original
 * version from the cbw buffer and the modified version from the page itself.
 */
static int toi_cbw_test_read(const char *buffer, int count)
{
  unsigned long virt = toi_get_zeroed_page(40, GFP_KERNEL);
  char *original = "Original contents";
  char *modified = "Modified material";
  struct page *page = virt_to_page(virt);
  int i, len = 0, found = 0, pfn = page_to_pfn(page);

  if (!page) {
    printk("toi_cbw_test_read: Unable to allocate a page for testing.\n");
    return -ENOMEM;
  }

  memcpy((char *) virt, original, strlen(original));

  if (toi_allocate_cbw_data()) {
    printk("toi_cbw_test_read: Unable to allocate cbw data.\n");
    return -ENOMEM;
  }

  toi_reset_dirtiness_one(pfn, 0);

  SetPageTOI_CBW(page);

  memcpy((char *) virt, modified, strlen(modified));

  if (strncmp((char *) virt, modified, strlen(modified))) {
    len += sprintf((char *) buffer + len, "Failed to write to page after protecting it.\n");
  }

  for_each_online_cpu(i) {
    struct toi_cbw_state *state = &per_cpu(toi_cbw_states, i);
    struct toi_cbw *ptr = state->first, *last_ptr = ptr;

    if (!found) {
      while (ptr) {
        if (ptr->pfn == pfn) {
          found = 1;
          if (strncmp(ptr->virt, original, strlen(original))) {
            len += sprintf((char *) buffer + len, "Contents of original buffer are not original.\n");
          } else {
            len += sprintf((char *) buffer + len, "Test passed. Buffer changed and original contents preserved.\n");
          }
          break;
        }

        last_ptr = ptr;
        ptr = ptr->next;
      }
    }

    if (!last_ptr)
      len += sprintf((char *) buffer + len, "All available CBW buffers on cpu %d used.\n", i);
  }

  if (!found)
    len += sprintf((char *) buffer + len, "Copy before write buffer not found.\n");

  toi_free_cbw_data();

  return len;
}

/*
 * This array contains entries that are automatically registered at
 * boot. Modules and the console code register their own entries separately.
 */
static struct toi_sysfs_data sysfs_params[] = {
  SYSFS_CUSTOM("test", SYSFS_RW, toi_cbw_test_read,
      NULL, SYSFS_NEEDS_SM_FOR_READ, NULL),
};

static struct toi_module_ops toi_cbw_ops = {
  .type                                        = MISC_HIDDEN_MODULE,
  .name                                        = "copy_before_write debugging",
  .directory                                = "cbw",
  .module                                        = THIS_MODULE,
  .early                                        = 1,

  .sysfs_data                = sysfs_params,
  .num_sysfs_entries        = sizeof(sysfs_params) /
    sizeof(struct toi_sysfs_data),
};

int toi_cbw_init(void)
{
  int result = toi_register_module(&toi_cbw_ops);
  return result;
}
