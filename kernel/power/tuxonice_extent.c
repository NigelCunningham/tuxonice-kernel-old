/*
 * kernel/power/tuxonice_extent.c
 *
 * Copyright (C) 2003-2015 Nigel Cunningham (nigel at nigelcunningham com au)
 *
 * Distributed under GPLv2.
 *
 * These functions encapsulate the manipulation of storage metadata.
 */

#include <linux/suspend.h>
#include "tuxonice_modules.h"
#include "tuxonice_extent.h"
#include "tuxonice_alloc.h"
#include "tuxonice_ui.h"
#include "tuxonice.h"

/**
 * toi_get_extent - return a free extent
 *
 * May fail, returning NULL instead.
 **/
static struct hibernate_extent *toi_get_extent(void)
{
  return (struct hibernate_extent *) toi_kzalloc(2,
      sizeof(struct hibernate_extent), TOI_ATOMIC_GFP);
}

/**
 * toi_put_extent_chain - free a chain of extents starting from value 'from'
 * @chain:        Chain to free.
 *
 * Note that 'from' is an extent value, and may be part way through an extent.
 * In this case, the extent should be truncated (if necessary) and following
 * extents freed.
 **/
void toi_put_extent_chain_from(struct hibernate_extent_chain *chain, unsigned long from)
{
  struct hibernate_extent *this;

  this = chain->first;

  while (this) {
    struct hibernate_extent *next = this->next;

    // Delete the whole extent?
    if (this->start >= from) {
      chain->size -= (this->end - this->start + 1);
      if (chain->first == this)
        chain->first = next;
      if (chain->last_touched == this)
        chain->last_touched = NULL;
      if (chain->current_extent == this)
        chain->current_extent = NULL;
      toi_kfree(2, this, sizeof(*this));
      chain->num_extents--;
    } else if (this->end >= from) {
      // Delete part of the extent
      chain->size -= (this->end - from + 1);
      this->start = from;
    }
    this = next;
  }
}

/**
 * toi_put_extent_chain - free a whole chain of extents
 * @chain:        Chain to free.
 **/
void toi_put_extent_chain(struct hibernate_extent_chain *chain)
{
  toi_put_extent_chain_from(chain, 0);
}

/**
 * toi_add_to_extent_chain - add an extent to an existing chain
 * @chain:        Chain to which the extend should be added
 * @start:        Start of the extent (first physical block)
 * @end:        End of the extent (last physical block)
 *
 * The chain information is updated if the insertion is successful.
 **/
int toi_add_to_extent_chain(struct hibernate_extent_chain *chain,
    unsigned long start, unsigned long end)
{
  struct hibernate_extent *new_ext = NULL, *cur_ext = NULL;

  toi_message(TOI_IO, TOI_VERBOSE, 0,
      "Adding extent %lu-%lu to chain %p.\n", start, end, chain);

  /* Find the right place in the chain */
  if (chain->last_touched && chain->last_touched->start < start)
    cur_ext = chain->last_touched;
  else if (chain->first && chain->first->start < start)
    cur_ext = chain->first;

  if (cur_ext) {
    while (cur_ext->next && cur_ext->next->start < start)
      cur_ext = cur_ext->next;

    if (cur_ext->end == (start - 1)) {
      struct hibernate_extent *next_ext = cur_ext->next;
      cur_ext->end = end;

      /* Merge with the following one? */
      if (next_ext && cur_ext->end + 1 == next_ext->start) {
        cur_ext->end = next_ext->end;
        cur_ext->next = next_ext->next;
        toi_kfree(2, next_ext, sizeof(*next_ext));
        chain->num_extents--;
      }

      chain->last_touched = cur_ext;
      chain->size += (end - start + 1);

      return 0;
    }
  }

  new_ext = toi_get_extent();
  if (!new_ext) {
    printk(KERN_INFO "Error unable to append a new extent to the "
        "chain.\n");
    return -ENOMEM;
  }

  chain->num_extents++;
  chain->size += (end - start + 1);
  new_ext->start = start;
  new_ext->end = end;

  chain->last_touched = new_ext;

  if (cur_ext) {
    new_ext->next = cur_ext->next;
    cur_ext->next = new_ext;
  } else {
    if (chain->first)
      new_ext->next = chain->first;
    chain->first = new_ext;
  }

  return 0;
}
