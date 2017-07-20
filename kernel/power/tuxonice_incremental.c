/*
 * kernel/power/tuxonice_incremental.c
 *
 * Copyright (C) 2015 Nigel Cunningham (nigel at nigelcunningham com au)
 *
 * This file is released under the GPLv2.
 *
 * This file contains routines related to storing incremental images - that
 * is, retaining an image after an initial cycle and then storing incremental
 * changes on subsequent hibernations.
 *
 * Based in part on on...
 *
 * Debug helper to dump the current kernel pagetables of the system
 * so that we can see what the various memory ranges are set to.
 *
 * (C) Copyright 2008 Intel Corporation
 *
 * Author: Arjan van de Ven <arjan@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/mm.h>
#include <linux/tuxonice.h>
#include <linux/sched.h>
#include <asm/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/page.h>
#include "tuxonice_pageflags.h"
#include "tuxonice_builtin.h"
#include "power.h"

int toi_do_incremental_initcall;

extern void kdb_init(int level);
extern noinline void kgdb_breakpoint(void);

#undef pr_debug
#if 0
#define pr_debug(a, b...) do { printk(a, ##b); } while(0)
#else
#define pr_debug(a, b...) do { } while(0)
#endif

/* Multipliers for offsets within the PTEs */
#define PTE_LEVEL_MULT (PAGE_SIZE)
#define PMD_LEVEL_MULT (PTRS_PER_PTE * PTE_LEVEL_MULT)
#define PUD_LEVEL_MULT (PTRS_PER_PMD * PMD_LEVEL_MULT)
#define PGD_LEVEL_MULT (PTRS_PER_PUD * PUD_LEVEL_MULT)

/*
 * This function gets called on a break in a continuous series
 * of PTE entries; the next one is different so we need to
 * print what we collected so far.
 */
static void note_page(void *addr)
{
  static struct page *lastpage;
  struct page *page;

  page = virt_to_page(addr);

  if (page != lastpage) {
    unsigned int level;
    pte_t *pte = lookup_address((unsigned long) addr, &level);
    struct page *pt_page2 = pte_page(*pte);
    //debug("Note page %p (=> %p => %p|%ld).\n", addr, pte, pt_page2, page_to_pfn(pt_page2));
    SetPageTOI_Untracked(pt_page2);
    lastpage = page;
  }
}

static void walk_pte_level(pmd_t addr)
{
  int i;
  pte_t *start;

  start = (pte_t *) pmd_page_vaddr(addr);
  for (i = 0; i < PTRS_PER_PTE; i++) {
    note_page(start);
    start++;
  }
}

#if PTRS_PER_PMD > 1

static void walk_pmd_level(pud_t addr)
{
  int i;
  pmd_t *start;

  start = (pmd_t *) pud_page_vaddr(addr);
  for (i = 0; i < PTRS_PER_PMD; i++) {
    if (!pmd_none(*start)) {
      if (pmd_large(*start) || !pmd_present(*start))
        note_page(start);
      else
        walk_pte_level(*start);
    } else
      note_page(start);
    start++;
  }
}

#else
#define walk_pmd_level(a) walk_pte_level(__pmd(pud_val(a)))
#define pud_large(a) pmd_large(__pmd(pud_val(a)))
#define pud_none(a)  pmd_none(__pmd(pud_val(a)))
#endif

#if PTRS_PER_PUD > 1

static void walk_pud_level(pgd_t addr)
{
  int i;
  pud_t *start;

  start = (pud_t *) pgd_page_vaddr(addr);

  for (i = 0; i < PTRS_PER_PUD; i++) {
    if (!pud_none(*start)) {
      if (pud_large(*start) || !pud_present(*start))
        note_page(start);
      else
        walk_pmd_level(*start);
    } else
      note_page(start);

    start++;
  }
}

#else
#define walk_pud_level(a) walk_pmd_level(__pud(pgd_val(a)))
#define pgd_large(a) pud_large(__pud(pgd_val(a)))
#define pgd_none(a)  pud_none(__pud(pgd_val(a)))
#endif

/*
 * Not static in the original at the time of writing, so needs renaming here.
 */
static void toi_ptdump_walk_pgd_level(pgd_t *pgd)
{
#ifdef CONFIG_X86_64
  pgd_t *start = (pgd_t *) &init_level4_pgt;
#else
  pgd_t *start = swapper_pg_dir;
#endif
  int i;
  if (pgd) {
    start = pgd;
  }

  for (i = 0; i < PTRS_PER_PGD; i++) {
    if (!pgd_none(*start)) {
      if (pgd_large(*start) || !pgd_present(*start))
        note_page(start);
      else
        walk_pud_level(*start);
    } else
      note_page(start);

    start++;
  }

  /* Flush out the last page */
  note_page(start);
}

#ifdef CONFIG_PARAVIRT
extern struct pv_info pv_info;

static void toi_set_paravirt_ops_untracked(void) {
  int i;

  unsigned long pvpfn = page_to_pfn(virt_to_page(__parainstructions)),
                pvpfn_end = page_to_pfn(virt_to_page(__parainstructions_end));
  //debug(KERN_EMERG ".parainstructions goes from pfn %ld to %ld.\n", pvpfn, pvpfn_end);
  for (i = pvpfn; i <= pvpfn_end; i++) {
    SetPageTOI_Untracked(pfn_to_page(i));
  }
}
#else
#define toi_set_paravirt_ops_untracked() { do { } while(0) }
#endif

extern void toi_mark_per_cpus_pages_untracked(void);

void toi_untrack_stack(unsigned long *stack)
{
  int i;
  struct page *stack_page = virt_to_page(stack);

  for (i = 0; i < (1 << THREAD_SIZE_ORDER); i++) {
    pr_debug("Untrack stack page %p.\n", page_address(stack_page + i));
    SetPageTOI_Untracked(stack_page + i);
  }
}
void toi_untrack_process(struct task_struct *p)
{
  SetPageTOI_Untracked(virt_to_page(p));
  pr_debug("Untrack process %d page %p.\n", p->pid, page_address(virt_to_page(p)));

  toi_untrack_stack(p->stack);
}

void toi_generate_untracked_map(void)
{
  struct task_struct *p, *t;
  struct page *page;
  pte_t *pte;
  int i;
  unsigned int level;
  static int been_here = 0;

  if (been_here)
    return;

  been_here = 1;

  /* Pagetable pages */
  toi_ptdump_walk_pgd_level(NULL);

  /* Printk buffer - not normally needed but can be helpful for debugging. */
  //toi_set_logbuf_untracked();

  /* Paravirt ops */
  toi_set_paravirt_ops_untracked();

  /* Task structs and stacks */
  for_each_process_thread(p, t) {
    toi_untrack_process(p);
    //toi_untrack_stack((unsigned long *) t->thread.sp);
  }

  for (i = 0; i < NR_CPUS; i++) {
    struct task_struct *idle = idle_task(i);

    if (idle) {
      pr_debug("Untrack idle process for CPU %d.\n", i);
      toi_untrack_process(idle);
    }

    /* IRQ stack */
    pr_debug("Untrack IRQ stack for CPU %d.\n", i);
    toi_untrack_stack((unsigned long *)per_cpu(irq_stack_ptr, i));
  }

  /* Per CPU data */
  //pr_debug("Untracking per CPU variable pages.\n");
  toi_mark_per_cpus_pages_untracked();

  /* Init stack - for bringing up secondary CPUs */
  page = virt_to_page(init_stack);
  for (i = 0; i < DIV_ROUND_UP(sizeof(init_stack), PAGE_SIZE); i++) {
    SetPageTOI_Untracked(page + i);
  }

  pte = lookup_address((unsigned long) &mmu_cr4_features, &level);
  SetPageTOI_Untracked(pte_page(*pte));
  SetPageTOI_Untracked(virt_to_page(trampoline_cr4_features));
}

/**
 * toi_reset_dirtiness_one
 */

void toi_reset_dirtiness_one(unsigned long pfn, int verbose)
{
  struct page *page = pfn_to_page(pfn);

  /**
   * Don't worry about whether the Dirty flag is
   * already set. If this is our first call, it
   * won't be.
   */

  preempt_disable();

  ClearPageTOI_Dirty(page);
  SetPageTOI_RO(page);
  if (verbose)
    printk(KERN_EMERG "Making page %ld (%p|%p) read only.\n", pfn, page, page_address(page));

  set_memory_ro((unsigned long) page_address(page), 1);

  preempt_enable();
}

/**
 * TuxOnIce's incremental image support works by marking all memory apart from
 * the page tables read-only, then in the page-faults that result enabling
 * writing if appropriate and flagging the page as dirty. Free pages are also
 * marked as dirty and not protected so that if allocated, they will be included
 * in the image without further processing.
 *
 * toi_reset_dirtiness is called when and image exists and incremental images are
 * enabled, and each time we resume thereafter. It is not invoked on a fresh boot.
 *
 * This routine should be called from a single-cpu-running context to avoid races in setting
 * page dirty/read only flags.
 *
 * TODO: Make "it is not invoked on a fresh boot" true  when I've finished developing it!
 *
 * TODO: Consider Xen paravirt guest boot issues. See arch/x86/mm/pageattr.c.
 **/

int toi_reset_dirtiness(int verbose)
{
  struct zone *zone;
  unsigned long loop;
  int allocated_map = 0;

  toi_generate_untracked_map();

  if (!free_map) {
    if (!toi_alloc_bitmap(&free_map))
      return -ENOMEM;
    allocated_map = 1;
  }

  toi_generate_free_page_map();

  pr_debug(KERN_EMERG "Reset dirtiness.\n");
  for_each_populated_zone(zone) {
    // 64 bit only. No need to worry about highmem.
    for (loop = 0; loop < zone->spanned_pages; loop++) {
      unsigned long pfn = zone->zone_start_pfn + loop;
      struct page *page;
      int chunk_size;

      if (!pfn_valid(pfn)) {
        continue;
      }

      chunk_size = toi_size_of_free_region(zone, pfn);
      if (chunk_size) {
        loop += chunk_size - 1;
        continue;
      }

      page = pfn_to_page(pfn);

      if (PageNosave(page) || !saveable_page(zone, pfn)) {
        continue;
      }

      if (PageTOI_Untracked(page)) {
        continue;
      }

      /**
       * Do we need to (re)protect the page?
       * If it is already protected (PageTOI_RO), there is
       * nothing to do - skip the following.
       * If it is marked as dirty (PageTOI_Dirty), it was
       * either free and has been allocated or has been
       * written to and marked dirty. Reset the dirty flag
       * and (re)apply the protection.
       */
      if (!PageTOI_RO(page)) {
        toi_reset_dirtiness_one(pfn, verbose);
      }
    }
  }

  pr_debug(KERN_EMERG "Done resetting dirtiness.\n");

  if (allocated_map) {
    toi_free_bitmap(&free_map);
  }
  return 0;
}

static int toi_reset_dirtiness_initcall(void)
{
  if (toi_do_incremental_initcall) {
    pr_info("TuxOnIce: Enabling dirty page tracking.\n");
    toi_reset_dirtiness(0);
  }
  return 1;
}
extern void toi_generate_untracked_map(void);

// Leave early_initcall for pages to register untracked sections.
early_initcall(toi_reset_dirtiness_initcall);

static int __init toi_incremental_initcall_setup(char *str)
{
  int value;

  if (sscanf(str, "=%d", &value) && value)
    toi_do_incremental_initcall = value;

  return 1;
}
__setup("toi_incremental_initcall", toi_incremental_initcall_setup);
