/*
 * kernel/power/tuxonice_pageflags.c
 *
 * Copyright (C) 2004-2015 Nigel Cunningham (nigel at nigelcunningham com au)
 *
 * This file is released under the GPLv2.
 *
 * Routines for serialising and relocating pageflags in which we
 * store our image metadata.
 */

#include "tuxonice_pageflags.h"
#include "power.h"

int toi_pageflags_space_needed(void)
{
  return memory_bm_space_needed(pageset1_map);
}
