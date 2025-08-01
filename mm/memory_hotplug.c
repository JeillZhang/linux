// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/memory_hotplug.c
 *
 *  Copyright (C)
 */

#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/swap.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/writeback.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/cpu.h>
#include <linux/memory.h>
#include <linux/memremap.h>
#include <linux/memory_hotplug.h>
#include <linux/vmalloc.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/migrate.h>
#include <linux/page-isolation.h>
#include <linux/pfn.h>
#include <linux/suspend.h>
#include <linux/mm_inline.h>
#include <linux/firmware-map.h>
#include <linux/stop_machine.h>
#include <linux/hugetlb.h>
#include <linux/memblock.h>
#include <linux/compaction.h>
#include <linux/rmap.h>
#include <linux/module.h>
#include <linux/node.h>

#include <asm/tlbflush.h>

#include "internal.h"
#include "shuffle.h"

enum {
	MEMMAP_ON_MEMORY_DISABLE = 0,
	MEMMAP_ON_MEMORY_ENABLE,
	MEMMAP_ON_MEMORY_FORCE,
};

static int memmap_mode __read_mostly = MEMMAP_ON_MEMORY_DISABLE;

static inline unsigned long memory_block_memmap_size(void)
{
	return PHYS_PFN(memory_block_size_bytes()) * sizeof(struct page);
}

static inline unsigned long memory_block_memmap_on_memory_pages(void)
{
	unsigned long nr_pages = PFN_UP(memory_block_memmap_size());

	/*
	 * In "forced" memmap_on_memory mode, we add extra pages to align the
	 * vmemmap size to cover full pageblocks. That way, we can add memory
	 * even if the vmemmap size is not properly aligned, however, we might waste
	 * memory.
	 */
	if (memmap_mode == MEMMAP_ON_MEMORY_FORCE)
		return pageblock_align(nr_pages);
	return nr_pages;
}

#ifdef CONFIG_MHP_MEMMAP_ON_MEMORY
/*
 * memory_hotplug.memmap_on_memory parameter
 */
static int set_memmap_mode(const char *val, const struct kernel_param *kp)
{
	int ret, mode;
	bool enabled;

	if (sysfs_streq(val, "force") ||  sysfs_streq(val, "FORCE")) {
		mode = MEMMAP_ON_MEMORY_FORCE;
	} else {
		ret = kstrtobool(val, &enabled);
		if (ret < 0)
			return ret;
		if (enabled)
			mode = MEMMAP_ON_MEMORY_ENABLE;
		else
			mode = MEMMAP_ON_MEMORY_DISABLE;
	}
	*((int *)kp->arg) = mode;
	if (mode == MEMMAP_ON_MEMORY_FORCE) {
		unsigned long memmap_pages = memory_block_memmap_on_memory_pages();

		pr_info_once("Memory hotplug will waste %ld pages in each memory block\n",
			     memmap_pages - PFN_UP(memory_block_memmap_size()));
	}
	return 0;
}

static int get_memmap_mode(char *buffer, const struct kernel_param *kp)
{
	int mode = *((int *)kp->arg);

	if (mode == MEMMAP_ON_MEMORY_FORCE)
		return sprintf(buffer, "force\n");
	return sprintf(buffer, "%c\n", mode ? 'Y' : 'N');
}

static const struct kernel_param_ops memmap_mode_ops = {
	.set = set_memmap_mode,
	.get = get_memmap_mode,
};
module_param_cb(memmap_on_memory, &memmap_mode_ops, &memmap_mode, 0444);
MODULE_PARM_DESC(memmap_on_memory, "Enable memmap on memory for memory hotplug\n"
		 "With value \"force\" it could result in memory wastage due "
		 "to memmap size limitations (Y/N/force)");

static inline bool mhp_memmap_on_memory(void)
{
	return memmap_mode != MEMMAP_ON_MEMORY_DISABLE;
}
#else
static inline bool mhp_memmap_on_memory(void)
{
	return false;
}
#endif

enum {
	ONLINE_POLICY_CONTIG_ZONES = 0,
	ONLINE_POLICY_AUTO_MOVABLE,
};

static const char * const online_policy_to_str[] = {
	[ONLINE_POLICY_CONTIG_ZONES] = "contig-zones",
	[ONLINE_POLICY_AUTO_MOVABLE] = "auto-movable",
};

static int set_online_policy(const char *val, const struct kernel_param *kp)
{
	int ret = sysfs_match_string(online_policy_to_str, val);

	if (ret < 0)
		return ret;
	*((int *)kp->arg) = ret;
	return 0;
}

static int get_online_policy(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "%s\n", online_policy_to_str[*((int *)kp->arg)]);
}

/*
 * memory_hotplug.online_policy: configure online behavior when onlining without
 * specifying a zone (MMOP_ONLINE)
 *
 * "contig-zones": keep zone contiguous
 * "auto-movable": online memory to ZONE_MOVABLE if the configuration
 *                 (auto_movable_ratio, auto_movable_numa_aware) allows for it
 */
static int online_policy __read_mostly = ONLINE_POLICY_CONTIG_ZONES;
static const struct kernel_param_ops online_policy_ops = {
	.set = set_online_policy,
	.get = get_online_policy,
};
module_param_cb(online_policy, &online_policy_ops, &online_policy, 0644);
MODULE_PARM_DESC(online_policy,
		"Set the online policy (\"contig-zones\", \"auto-movable\") "
		"Default: \"contig-zones\"");

/*
 * memory_hotplug.auto_movable_ratio: specify maximum MOVABLE:KERNEL ratio
 *
 * The ratio represent an upper limit and the kernel might decide to not
 * online some memory to ZONE_MOVABLE -- e.g., because hotplugged KERNEL memory
 * doesn't allow for more MOVABLE memory.
 */
static unsigned int auto_movable_ratio __read_mostly = 301;
module_param(auto_movable_ratio, uint, 0644);
MODULE_PARM_DESC(auto_movable_ratio,
		"Set the maximum ratio of MOVABLE:KERNEL memory in the system "
		"in percent for \"auto-movable\" online policy. Default: 301");

/*
 * memory_hotplug.auto_movable_numa_aware: consider numa node stats
 */
#ifdef CONFIG_NUMA
static bool auto_movable_numa_aware __read_mostly = true;
module_param(auto_movable_numa_aware, bool, 0644);
MODULE_PARM_DESC(auto_movable_numa_aware,
		"Consider numa node stats in addition to global stats in "
		"\"auto-movable\" online policy. Default: true");
#endif /* CONFIG_NUMA */

/*
 * online_page_callback contains pointer to current page onlining function.
 * Initially it is generic_online_page(). If it is required it could be
 * changed by calling set_online_page_callback() for callback registration
 * and restore_online_page_callback() for generic callback restore.
 */

static online_page_callback_t online_page_callback = generic_online_page;
static DEFINE_MUTEX(online_page_callback_lock);

DEFINE_STATIC_PERCPU_RWSEM(mem_hotplug_lock);

void get_online_mems(void)
{
	percpu_down_read(&mem_hotplug_lock);
}

void put_online_mems(void)
{
	percpu_up_read(&mem_hotplug_lock);
}

bool movable_node_enabled = false;

static int mhp_default_online_type = -1;
int mhp_get_default_online_type(void)
{
	if (mhp_default_online_type >= 0)
		return mhp_default_online_type;

	if (IS_ENABLED(CONFIG_MHP_DEFAULT_ONLINE_TYPE_OFFLINE))
		mhp_default_online_type = MMOP_OFFLINE;
	else if (IS_ENABLED(CONFIG_MHP_DEFAULT_ONLINE_TYPE_ONLINE_AUTO))
		mhp_default_online_type = MMOP_ONLINE;
	else if (IS_ENABLED(CONFIG_MHP_DEFAULT_ONLINE_TYPE_ONLINE_KERNEL))
		mhp_default_online_type = MMOP_ONLINE_KERNEL;
	else if (IS_ENABLED(CONFIG_MHP_DEFAULT_ONLINE_TYPE_ONLINE_MOVABLE))
		mhp_default_online_type = MMOP_ONLINE_MOVABLE;
	else
		mhp_default_online_type = MMOP_OFFLINE;

	return mhp_default_online_type;
}

void mhp_set_default_online_type(int online_type)
{
	mhp_default_online_type = online_type;
}

static int __init setup_memhp_default_state(char *str)
{
	const int online_type = mhp_online_type_from_str(str);

	if (online_type >= 0)
		mhp_default_online_type = online_type;

	return 1;
}
__setup("memhp_default_state=", setup_memhp_default_state);

void mem_hotplug_begin(void)
{
	cpus_read_lock();
	percpu_down_write(&mem_hotplug_lock);
}

void mem_hotplug_done(void)
{
	percpu_up_write(&mem_hotplug_lock);
	cpus_read_unlock();
}

u64 max_mem_size = U64_MAX;

/* add this memory to iomem resource */
static struct resource *register_memory_resource(u64 start, u64 size,
						 const char *resource_name)
{
	struct resource *res;
	unsigned long flags =  IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;

	if (strcmp(resource_name, "System RAM"))
		flags |= IORESOURCE_SYSRAM_DRIVER_MANAGED;

	if (!mhp_range_allowed(start, size, true))
		return ERR_PTR(-E2BIG);

	/*
	 * Make sure value parsed from 'mem=' only restricts memory adding
	 * while booting, so that memory hotplug won't be impacted. Please
	 * refer to document of 'mem=' in kernel-parameters.txt for more
	 * details.
	 */
	if (start + size > max_mem_size && system_state < SYSTEM_RUNNING)
		return ERR_PTR(-E2BIG);

	/*
	 * Request ownership of the new memory range.  This might be
	 * a child of an existing resource that was present but
	 * not marked as busy.
	 */
	res = __request_region(&iomem_resource, start, size,
			       resource_name, flags);

	if (!res) {
		pr_debug("Unable to reserve System RAM region: %016llx->%016llx\n",
				start, start + size);
		return ERR_PTR(-EEXIST);
	}
	return res;
}

static void release_memory_resource(struct resource *res)
{
	if (!res)
		return;
	release_resource(res);
	kfree(res);
}

static int check_pfn_span(unsigned long pfn, unsigned long nr_pages)
{
	/*
	 * Disallow all operations smaller than a sub-section and only
	 * allow operations smaller than a section for
	 * SPARSEMEM_VMEMMAP. Note that check_hotplug_memory_range()
	 * enforces a larger memory_block_size_bytes() granularity for
	 * memory that will be marked online, so this check should only
	 * fire for direct arch_{add,remove}_memory() users outside of
	 * add_memory_resource().
	 */
	unsigned long min_align;

	if (IS_ENABLED(CONFIG_SPARSEMEM_VMEMMAP))
		min_align = PAGES_PER_SUBSECTION;
	else
		min_align = PAGES_PER_SECTION;
	if (!IS_ALIGNED(pfn | nr_pages, min_align))
		return -EINVAL;
	return 0;
}

/*
 * Return page for the valid pfn only if the page is online. All pfn
 * walkers which rely on the fully initialized page->flags and others
 * should use this rather than pfn_valid && pfn_to_page
 */
struct page *pfn_to_online_page(unsigned long pfn)
{
	unsigned long nr = pfn_to_section_nr(pfn);
	struct dev_pagemap *pgmap;
	struct mem_section *ms;

	if (nr >= NR_MEM_SECTIONS)
		return NULL;

	ms = __nr_to_section(nr);
	if (!online_section(ms))
		return NULL;

	/*
	 * Save some code text when online_section() +
	 * pfn_section_valid() are sufficient.
	 */
	if (IS_ENABLED(CONFIG_HAVE_ARCH_PFN_VALID) && !pfn_valid(pfn))
		return NULL;

	if (!pfn_section_valid(ms, pfn))
		return NULL;

	if (!online_device_section(ms))
		return pfn_to_page(pfn);

	/*
	 * Slowpath: when ZONE_DEVICE collides with
	 * ZONE_{NORMAL,MOVABLE} within the same section some pfns in
	 * the section may be 'offline' but 'valid'. Only
	 * get_dev_pagemap() can determine sub-section online status.
	 */
	pgmap = get_dev_pagemap(pfn, NULL);
	put_dev_pagemap(pgmap);

	/* The presence of a pgmap indicates ZONE_DEVICE offline pfn */
	if (pgmap)
		return NULL;

	return pfn_to_page(pfn);
}
EXPORT_SYMBOL_GPL(pfn_to_online_page);

int __add_pages(int nid, unsigned long pfn, unsigned long nr_pages,
		struct mhp_params *params)
{
	const unsigned long end_pfn = pfn + nr_pages;
	unsigned long cur_nr_pages;
	int err;
	struct vmem_altmap *altmap = params->altmap;

	if (WARN_ON_ONCE(!pgprot_val(params->pgprot)))
		return -EINVAL;

	VM_BUG_ON(!mhp_range_allowed(PFN_PHYS(pfn), nr_pages * PAGE_SIZE, false));

	if (altmap) {
		/*
		 * Validate altmap is within bounds of the total request
		 */
		if (altmap->base_pfn != pfn
				|| vmem_altmap_offset(altmap) > nr_pages) {
			pr_warn_once("memory add fail, invalid altmap\n");
			return -EINVAL;
		}
		altmap->alloc = 0;
	}

	if (check_pfn_span(pfn, nr_pages)) {
		WARN(1, "Misaligned %s start: %#lx end: %#lx\n", __func__, pfn, pfn + nr_pages - 1);
		return -EINVAL;
	}

	for (; pfn < end_pfn; pfn += cur_nr_pages) {
		/* Select all remaining pages up to the next section boundary */
		cur_nr_pages = min(end_pfn - pfn,
				   SECTION_ALIGN_UP(pfn + 1) - pfn);
		err = sparse_add_section(nid, pfn, cur_nr_pages, altmap,
					 params->pgmap);
		if (err)
			break;
		cond_resched();
	}
	vmemmap_populate_print_last();
	return err;
}

/* find the smallest valid pfn in the range [start_pfn, end_pfn) */
static unsigned long find_smallest_section_pfn(int nid, struct zone *zone,
				     unsigned long start_pfn,
				     unsigned long end_pfn)
{
	for (; start_pfn < end_pfn; start_pfn += PAGES_PER_SUBSECTION) {
		if (unlikely(!pfn_to_online_page(start_pfn)))
			continue;

		if (unlikely(pfn_to_nid(start_pfn) != nid))
			continue;

		if (zone != page_zone(pfn_to_page(start_pfn)))
			continue;

		return start_pfn;
	}

	return 0;
}

/* find the biggest valid pfn in the range [start_pfn, end_pfn). */
static unsigned long find_biggest_section_pfn(int nid, struct zone *zone,
				    unsigned long start_pfn,
				    unsigned long end_pfn)
{
	unsigned long pfn;

	/* pfn is the end pfn of a memory section. */
	pfn = end_pfn - 1;
	for (; pfn >= start_pfn; pfn -= PAGES_PER_SUBSECTION) {
		if (unlikely(!pfn_to_online_page(pfn)))
			continue;

		if (unlikely(pfn_to_nid(pfn) != nid))
			continue;

		if (zone != page_zone(pfn_to_page(pfn)))
			continue;

		return pfn;
	}

	return 0;
}

static void shrink_zone_span(struct zone *zone, unsigned long start_pfn,
			     unsigned long end_pfn)
{
	unsigned long pfn;
	int nid = zone_to_nid(zone);

	if (zone->zone_start_pfn == start_pfn) {
		/*
		 * If the section is smallest section in the zone, it need
		 * shrink zone->zone_start_pfn and zone->zone_spanned_pages.
		 * In this case, we find second smallest valid mem_section
		 * for shrinking zone.
		 */
		pfn = find_smallest_section_pfn(nid, zone, end_pfn,
						zone_end_pfn(zone));
		if (pfn) {
			zone->spanned_pages = zone_end_pfn(zone) - pfn;
			zone->zone_start_pfn = pfn;
		} else {
			zone->zone_start_pfn = 0;
			zone->spanned_pages = 0;
		}
	} else if (zone_end_pfn(zone) == end_pfn) {
		/*
		 * If the section is biggest section in the zone, it need
		 * shrink zone->spanned_pages.
		 * In this case, we find second biggest valid mem_section for
		 * shrinking zone.
		 */
		pfn = find_biggest_section_pfn(nid, zone, zone->zone_start_pfn,
					       start_pfn);
		if (pfn)
			zone->spanned_pages = pfn - zone->zone_start_pfn + 1;
		else {
			zone->zone_start_pfn = 0;
			zone->spanned_pages = 0;
		}
	}
}

static void update_pgdat_span(struct pglist_data *pgdat)
{
	unsigned long node_start_pfn = 0, node_end_pfn = 0;
	struct zone *zone;

	for (zone = pgdat->node_zones;
	     zone < pgdat->node_zones + MAX_NR_ZONES; zone++) {
		unsigned long end_pfn = zone_end_pfn(zone);

		/* No need to lock the zones, they can't change. */
		if (!zone->spanned_pages)
			continue;
		if (!node_end_pfn) {
			node_start_pfn = zone->zone_start_pfn;
			node_end_pfn = end_pfn;
			continue;
		}

		if (end_pfn > node_end_pfn)
			node_end_pfn = end_pfn;
		if (zone->zone_start_pfn < node_start_pfn)
			node_start_pfn = zone->zone_start_pfn;
	}

	pgdat->node_start_pfn = node_start_pfn;
	pgdat->node_spanned_pages = node_end_pfn - node_start_pfn;
}

void remove_pfn_range_from_zone(struct zone *zone,
				      unsigned long start_pfn,
				      unsigned long nr_pages)
{
	const unsigned long end_pfn = start_pfn + nr_pages;
	struct pglist_data *pgdat = zone->zone_pgdat;
	unsigned long pfn, cur_nr_pages;

	/* Poison struct pages because they are now uninitialized again. */
	for (pfn = start_pfn; pfn < end_pfn; pfn += cur_nr_pages) {
		cond_resched();

		/* Select all remaining pages up to the next section boundary */
		cur_nr_pages =
			min(end_pfn - pfn, SECTION_ALIGN_UP(pfn + 1) - pfn);
		page_init_poison(pfn_to_page(pfn),
				 sizeof(struct page) * cur_nr_pages);
	}

	/*
	 * Zone shrinking code cannot properly deal with ZONE_DEVICE. So
	 * we will not try to shrink the zones - which is okay as
	 * set_zone_contiguous() cannot deal with ZONE_DEVICE either way.
	 */
	if (zone_is_zone_device(zone))
		return;

	clear_zone_contiguous(zone);

	shrink_zone_span(zone, start_pfn, start_pfn + nr_pages);
	update_pgdat_span(pgdat);

	set_zone_contiguous(zone);
}

/**
 * __remove_pages() - remove sections of pages
 * @pfn: starting pageframe (must be aligned to start of a section)
 * @nr_pages: number of pages to remove (must be multiple of section size)
 * @altmap: alternative device page map or %NULL if default memmap is used
 *
 * Generic helper function to remove section mappings and sysfs entries
 * for the section of the memory we are removing. Caller needs to make
 * sure that pages are marked reserved and zones are adjust properly by
 * calling offline_pages().
 */
void __remove_pages(unsigned long pfn, unsigned long nr_pages,
		    struct vmem_altmap *altmap)
{
	const unsigned long end_pfn = pfn + nr_pages;
	unsigned long cur_nr_pages;

	if (check_pfn_span(pfn, nr_pages)) {
		WARN(1, "Misaligned %s start: %#lx end: %#lx\n", __func__, pfn, pfn + nr_pages - 1);
		return;
	}

	for (; pfn < end_pfn; pfn += cur_nr_pages) {
		cond_resched();
		/* Select all remaining pages up to the next section boundary */
		cur_nr_pages = min(end_pfn - pfn,
				   SECTION_ALIGN_UP(pfn + 1) - pfn);
		sparse_remove_section(pfn, cur_nr_pages, altmap);
	}
}

int set_online_page_callback(online_page_callback_t callback)
{
	int rc = -EINVAL;

	get_online_mems();
	mutex_lock(&online_page_callback_lock);

	if (online_page_callback == generic_online_page) {
		online_page_callback = callback;
		rc = 0;
	}

	mutex_unlock(&online_page_callback_lock);
	put_online_mems();

	return rc;
}
EXPORT_SYMBOL_GPL(set_online_page_callback);

int restore_online_page_callback(online_page_callback_t callback)
{
	int rc = -EINVAL;

	get_online_mems();
	mutex_lock(&online_page_callback_lock);

	if (online_page_callback == callback) {
		online_page_callback = generic_online_page;
		rc = 0;
	}

	mutex_unlock(&online_page_callback_lock);
	put_online_mems();

	return rc;
}
EXPORT_SYMBOL_GPL(restore_online_page_callback);

/* we are OK calling __meminit stuff here - we have CONFIG_MEMORY_HOTPLUG */
void generic_online_page(struct page *page, unsigned int order)
{
	__free_pages_core(page, order, MEMINIT_HOTPLUG);
}
EXPORT_SYMBOL_GPL(generic_online_page);

static void online_pages_range(unsigned long start_pfn, unsigned long nr_pages)
{
	const unsigned long end_pfn = start_pfn + nr_pages;
	unsigned long pfn;

	/*
	 * Online the pages in MAX_PAGE_ORDER aligned chunks. The callback might
	 * decide to not expose all pages to the buddy (e.g., expose them
	 * later). We account all pages as being online and belonging to this
	 * zone ("present").
	 * When using memmap_on_memory, the range might not be aligned to
	 * MAX_ORDER_NR_PAGES - 1, but pageblock aligned. __ffs() will detect
	 * this and the first chunk to online will be pageblock_nr_pages.
	 */
	for (pfn = start_pfn; pfn < end_pfn;) {
		struct page *page = pfn_to_page(pfn);
		int order;

		/*
		 * Free to online pages in the largest chunks alignment allows.
		 *
		 * __ffs() behaviour is undefined for 0. start == 0 is
		 * MAX_PAGE_ORDER-aligned, Set order to MAX_PAGE_ORDER for
		 * the case.
		 */
		if (pfn)
			order = min_t(int, MAX_PAGE_ORDER, __ffs(pfn));
		else
			order = MAX_PAGE_ORDER;

		/*
		 * Exposing the page to the buddy by freeing can cause
		 * issues with debug_pagealloc enabled: some archs don't
		 * like double-unmappings. So treat them like any pages that
		 * were allocated from the buddy.
		 */
		debug_pagealloc_map_pages(page, 1 << order);
		(*online_page_callback)(page, order);
		pfn += (1UL << order);
	}

	/* mark all involved sections as online */
	online_mem_sections(start_pfn, end_pfn);
}

static void __meminit resize_zone_range(struct zone *zone, unsigned long start_pfn,
		unsigned long nr_pages)
{
	unsigned long old_end_pfn = zone_end_pfn(zone);

	if (zone_is_empty(zone) || start_pfn < zone->zone_start_pfn)
		zone->zone_start_pfn = start_pfn;

	zone->spanned_pages = max(start_pfn + nr_pages, old_end_pfn) - zone->zone_start_pfn;
}

static void __meminit resize_pgdat_range(struct pglist_data *pgdat, unsigned long start_pfn,
                                     unsigned long nr_pages)
{
	unsigned long old_end_pfn = pgdat_end_pfn(pgdat);

	if (!pgdat->node_spanned_pages || start_pfn < pgdat->node_start_pfn)
		pgdat->node_start_pfn = start_pfn;

	pgdat->node_spanned_pages = max(start_pfn + nr_pages, old_end_pfn) - pgdat->node_start_pfn;

}

#ifdef CONFIG_ZONE_DEVICE
static void section_taint_zone_device(unsigned long pfn)
{
	struct mem_section *ms = __pfn_to_section(pfn);

	ms->section_mem_map |= SECTION_TAINT_ZONE_DEVICE;
}
#else
static inline void section_taint_zone_device(unsigned long pfn)
{
}
#endif

/*
 * Associate the pfn range with the given zone, initializing the memmaps
 * and resizing the pgdat/zone data to span the added pages. After this
 * call, all affected pages are PageOffline().
 *
 * All aligned pageblocks are initialized to the specified migratetype
 * (usually MIGRATE_MOVABLE). Besides setting the migratetype, no related
 * zone stats (e.g., nr_isolate_pageblock) are touched.
 */
void move_pfn_range_to_zone(struct zone *zone, unsigned long start_pfn,
				  unsigned long nr_pages,
				  struct vmem_altmap *altmap, int migratetype,
				  bool isolate_pageblock)
{
	struct pglist_data *pgdat = zone->zone_pgdat;
	int nid = pgdat->node_id;

	clear_zone_contiguous(zone);

	if (zone_is_empty(zone))
		init_currently_empty_zone(zone, start_pfn, nr_pages);
	resize_zone_range(zone, start_pfn, nr_pages);
	resize_pgdat_range(pgdat, start_pfn, nr_pages);

	/*
	 * Subsection population requires care in pfn_to_online_page().
	 * Set the taint to enable the slow path detection of
	 * ZONE_DEVICE pages in an otherwise  ZONE_{NORMAL,MOVABLE}
	 * section.
	 */
	if (zone_is_zone_device(zone)) {
		if (!IS_ALIGNED(start_pfn, PAGES_PER_SECTION))
			section_taint_zone_device(start_pfn);
		if (!IS_ALIGNED(start_pfn + nr_pages, PAGES_PER_SECTION))
			section_taint_zone_device(start_pfn + nr_pages);
	}

	/*
	 * TODO now we have a visible range of pages which are not associated
	 * with their zone properly. Not nice but set_pfnblock_migratetype()
	 * expects the zone spans the pfn range. All the pages in the range
	 * are reserved so nobody should be touching them so we should be safe
	 */
	memmap_init_range(nr_pages, nid, zone_idx(zone), start_pfn, 0,
			 MEMINIT_HOTPLUG, altmap, migratetype,
			 isolate_pageblock);

	set_zone_contiguous(zone);
}

struct auto_movable_stats {
	unsigned long kernel_early_pages;
	unsigned long movable_pages;
};

static void auto_movable_stats_account_zone(struct auto_movable_stats *stats,
					    struct zone *zone)
{
	if (zone_idx(zone) == ZONE_MOVABLE) {
		stats->movable_pages += zone->present_pages;
	} else {
		stats->kernel_early_pages += zone->present_early_pages;
#ifdef CONFIG_CMA
		/*
		 * CMA pages (never on hotplugged memory) behave like
		 * ZONE_MOVABLE.
		 */
		stats->movable_pages += zone->cma_pages;
		stats->kernel_early_pages -= zone->cma_pages;
#endif /* CONFIG_CMA */
	}
}
struct auto_movable_group_stats {
	unsigned long movable_pages;
	unsigned long req_kernel_early_pages;
};

static int auto_movable_stats_account_group(struct memory_group *group,
					   void *arg)
{
	const int ratio = READ_ONCE(auto_movable_ratio);
	struct auto_movable_group_stats *stats = arg;
	long pages;

	/*
	 * We don't support modifying the config while the auto-movable online
	 * policy is already enabled. Just avoid the division by zero below.
	 */
	if (!ratio)
		return 0;

	/*
	 * Calculate how many early kernel pages this group requires to
	 * satisfy the configured zone ratio.
	 */
	pages = group->present_movable_pages * 100 / ratio;
	pages -= group->present_kernel_pages;

	if (pages > 0)
		stats->req_kernel_early_pages += pages;
	stats->movable_pages += group->present_movable_pages;
	return 0;
}

static bool auto_movable_can_online_movable(int nid, struct memory_group *group,
					    unsigned long nr_pages)
{
	unsigned long kernel_early_pages, movable_pages;
	struct auto_movable_group_stats group_stats = {};
	struct auto_movable_stats stats = {};
	struct zone *zone;
	int i;

	/* Walk all relevant zones and collect MOVABLE vs. KERNEL stats. */
	if (nid == NUMA_NO_NODE) {
		/* TODO: cache values */
		for_each_populated_zone(zone)
			auto_movable_stats_account_zone(&stats, zone);
	} else {
		for (i = 0; i < MAX_NR_ZONES; i++) {
			pg_data_t *pgdat = NODE_DATA(nid);

			zone = pgdat->node_zones + i;
			if (populated_zone(zone))
				auto_movable_stats_account_zone(&stats, zone);
		}
	}

	kernel_early_pages = stats.kernel_early_pages;
	movable_pages = stats.movable_pages;

	/*
	 * Kernel memory inside dynamic memory group allows for more MOVABLE
	 * memory within the same group. Remove the effect of all but the
	 * current group from the stats.
	 */
	walk_dynamic_memory_groups(nid, auto_movable_stats_account_group,
				   group, &group_stats);
	if (kernel_early_pages <= group_stats.req_kernel_early_pages)
		return false;
	kernel_early_pages -= group_stats.req_kernel_early_pages;
	movable_pages -= group_stats.movable_pages;

	if (group && group->is_dynamic)
		kernel_early_pages += group->present_kernel_pages;

	/*
	 * Test if we could online the given number of pages to ZONE_MOVABLE
	 * and still stay in the configured ratio.
	 */
	movable_pages += nr_pages;
	return movable_pages <= (auto_movable_ratio * kernel_early_pages) / 100;
}

/*
 * Returns a default kernel memory zone for the given pfn range.
 * If no kernel zone covers this pfn range it will automatically go
 * to the ZONE_NORMAL.
 */
static struct zone *default_kernel_zone_for_pfn(int nid, unsigned long start_pfn,
		unsigned long nr_pages)
{
	struct pglist_data *pgdat = NODE_DATA(nid);
	int zid;

	for (zid = 0; zid < ZONE_NORMAL; zid++) {
		struct zone *zone = &pgdat->node_zones[zid];

		if (zone_intersects(zone, start_pfn, nr_pages))
			return zone;
	}

	return &pgdat->node_zones[ZONE_NORMAL];
}

/*
 * Determine to which zone to online memory dynamically based on user
 * configuration and system stats. We care about the following ratio:
 *
 *   MOVABLE : KERNEL
 *
 * Whereby MOVABLE is memory in ZONE_MOVABLE and KERNEL is memory in
 * one of the kernel zones. CMA pages inside one of the kernel zones really
 * behaves like ZONE_MOVABLE, so we treat them accordingly.
 *
 * We don't allow for hotplugged memory in a KERNEL zone to increase the
 * amount of MOVABLE memory we can have, so we end up with:
 *
 *   MOVABLE : KERNEL_EARLY
 *
 * Whereby KERNEL_EARLY is memory in one of the kernel zones, available sinze
 * boot. We base our calculation on KERNEL_EARLY internally, because:
 *
 * a) Hotplugged memory in one of the kernel zones can sometimes still get
 *    hotunplugged, especially when hot(un)plugging individual memory blocks.
 *    There is no coordination across memory devices, therefore "automatic"
 *    hotunplugging, as implemented in hypervisors, could result in zone
 *    imbalances.
 * b) Early/boot memory in one of the kernel zones can usually not get
 *    hotunplugged again (e.g., no firmware interface to unplug, fragmented
 *    with unmovable allocations). While there are corner cases where it might
 *    still work, it is barely relevant in practice.
 *
 * Exceptions are dynamic memory groups, which allow for more MOVABLE
 * memory within the same memory group -- because in that case, there is
 * coordination within the single memory device managed by a single driver.
 *
 * We rely on "present pages" instead of "managed pages", as the latter is
 * highly unreliable and dynamic in virtualized environments, and does not
 * consider boot time allocations. For example, memory ballooning adjusts the
 * managed pages when inflating/deflating the balloon, and balloon compaction
 * can even migrate inflated pages between zones.
 *
 * Using "present pages" is better but some things to keep in mind are:
 *
 * a) Some memblock allocations, such as for the crashkernel area, are
 *    effectively unused by the kernel, yet they account to "present pages".
 *    Fortunately, these allocations are comparatively small in relevant setups
 *    (e.g., fraction of system memory).
 * b) Some hotplugged memory blocks in virtualized environments, esecially
 *    hotplugged by virtio-mem, look like they are completely present, however,
 *    only parts of the memory block are actually currently usable.
 *    "present pages" is an upper limit that can get reached at runtime. As
 *    we base our calculations on KERNEL_EARLY, this is not an issue.
 */
static struct zone *auto_movable_zone_for_pfn(int nid,
					      struct memory_group *group,
					      unsigned long pfn,
					      unsigned long nr_pages)
{
	unsigned long online_pages = 0, max_pages, end_pfn;
	struct page *page;

	if (!auto_movable_ratio)
		goto kernel_zone;

	if (group && !group->is_dynamic) {
		max_pages = group->s.max_pages;
		online_pages = group->present_movable_pages;

		/* If anything is !MOVABLE online the rest !MOVABLE. */
		if (group->present_kernel_pages)
			goto kernel_zone;
	} else if (!group || group->d.unit_pages == nr_pages) {
		max_pages = nr_pages;
	} else {
		max_pages = group->d.unit_pages;
		/*
		 * Take a look at all online sections in the current unit.
		 * We can safely assume that all pages within a section belong
		 * to the same zone, because dynamic memory groups only deal
		 * with hotplugged memory.
		 */
		pfn = ALIGN_DOWN(pfn, group->d.unit_pages);
		end_pfn = pfn + group->d.unit_pages;
		for (; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
			page = pfn_to_online_page(pfn);
			if (!page)
				continue;
			/* If anything is !MOVABLE online the rest !MOVABLE. */
			if (!is_zone_movable_page(page))
				goto kernel_zone;
			online_pages += PAGES_PER_SECTION;
		}
	}

	/*
	 * Online MOVABLE if we could *currently* online all remaining parts
	 * MOVABLE. We expect to (add+) online them immediately next, so if
	 * nobody interferes, all will be MOVABLE if possible.
	 */
	nr_pages = max_pages - online_pages;
	if (!auto_movable_can_online_movable(NUMA_NO_NODE, group, nr_pages))
		goto kernel_zone;

#ifdef CONFIG_NUMA
	if (auto_movable_numa_aware &&
	    !auto_movable_can_online_movable(nid, group, nr_pages))
		goto kernel_zone;
#endif /* CONFIG_NUMA */

	return &NODE_DATA(nid)->node_zones[ZONE_MOVABLE];
kernel_zone:
	return default_kernel_zone_for_pfn(nid, pfn, nr_pages);
}

static inline struct zone *default_zone_for_pfn(int nid, unsigned long start_pfn,
		unsigned long nr_pages)
{
	struct zone *kernel_zone = default_kernel_zone_for_pfn(nid, start_pfn,
			nr_pages);
	struct zone *movable_zone = &NODE_DATA(nid)->node_zones[ZONE_MOVABLE];
	bool in_kernel = zone_intersects(kernel_zone, start_pfn, nr_pages);
	bool in_movable = zone_intersects(movable_zone, start_pfn, nr_pages);

	/*
	 * We inherit the existing zone in a simple case where zones do not
	 * overlap in the given range
	 */
	if (in_kernel ^ in_movable)
		return (in_kernel) ? kernel_zone : movable_zone;

	/*
	 * If the range doesn't belong to any zone or two zones overlap in the
	 * given range then we use movable zone only if movable_node is
	 * enabled because we always online to a kernel zone by default.
	 */
	return movable_node_enabled ? movable_zone : kernel_zone;
}

struct zone *zone_for_pfn_range(int online_type, int nid,
		struct memory_group *group, unsigned long start_pfn,
		unsigned long nr_pages)
{
	if (online_type == MMOP_ONLINE_KERNEL)
		return default_kernel_zone_for_pfn(nid, start_pfn, nr_pages);

	if (online_type == MMOP_ONLINE_MOVABLE)
		return &NODE_DATA(nid)->node_zones[ZONE_MOVABLE];

	if (online_policy == ONLINE_POLICY_AUTO_MOVABLE)
		return auto_movable_zone_for_pfn(nid, group, start_pfn, nr_pages);

	return default_zone_for_pfn(nid, start_pfn, nr_pages);
}

/*
 * This function should only be called by memory_block_{online,offline},
 * and {online,offline}_pages.
 */
void adjust_present_page_count(struct page *page, struct memory_group *group,
			       long nr_pages)
{
	struct zone *zone = page_zone(page);
	const bool movable = zone_idx(zone) == ZONE_MOVABLE;

	/*
	 * We only support onlining/offlining/adding/removing of complete
	 * memory blocks; therefore, either all is either early or hotplugged.
	 */
	if (early_section(__pfn_to_section(page_to_pfn(page))))
		zone->present_early_pages += nr_pages;
	zone->present_pages += nr_pages;
	zone->zone_pgdat->node_present_pages += nr_pages;

	if (group && movable)
		group->present_movable_pages += nr_pages;
	else if (group && !movable)
		group->present_kernel_pages += nr_pages;
}

int mhp_init_memmap_on_memory(unsigned long pfn, unsigned long nr_pages,
			      struct zone *zone, bool mhp_off_inaccessible)
{
	unsigned long end_pfn = pfn + nr_pages;
	int ret, i;

	ret = kasan_add_zero_shadow(__va(PFN_PHYS(pfn)), PFN_PHYS(nr_pages));
	if (ret)
		return ret;

	/*
	 * Memory block is accessible at this stage and hence poison the struct
	 * pages now.  If the memory block is accessible during memory hotplug
	 * addition phase, then page poisining is already performed in
	 * sparse_add_section().
	 */
	if (mhp_off_inaccessible)
		page_init_poison(pfn_to_page(pfn), sizeof(struct page) * nr_pages);

	move_pfn_range_to_zone(zone, pfn, nr_pages, NULL, MIGRATE_UNMOVABLE,
			       false);

	for (i = 0; i < nr_pages; i++) {
		struct page *page = pfn_to_page(pfn + i);

		__ClearPageOffline(page);
		SetPageVmemmapSelfHosted(page);
	}

	/*
	 * It might be that the vmemmap_pages fully span sections. If that is
	 * the case, mark those sections online here as otherwise they will be
	 * left offline.
	 */
	if (nr_pages >= PAGES_PER_SECTION)
	        online_mem_sections(pfn, ALIGN_DOWN(end_pfn, PAGES_PER_SECTION));

	return ret;
}

void mhp_deinit_memmap_on_memory(unsigned long pfn, unsigned long nr_pages)
{
	unsigned long end_pfn = pfn + nr_pages;

	/*
	 * It might be that the vmemmap_pages fully span sections. If that is
	 * the case, mark those sections offline here as otherwise they will be
	 * left online.
	 */
	if (nr_pages >= PAGES_PER_SECTION)
		offline_mem_sections(pfn, ALIGN_DOWN(end_pfn, PAGES_PER_SECTION));

        /*
	 * The pages associated with this vmemmap have been offlined, so
	 * we can reset its state here.
	 */
	remove_pfn_range_from_zone(page_zone(pfn_to_page(pfn)), pfn, nr_pages);
	kasan_remove_zero_shadow(__va(PFN_PHYS(pfn)), PFN_PHYS(nr_pages));
}

/*
 * Must be called with mem_hotplug_lock in write mode.
 */
int online_pages(unsigned long pfn, unsigned long nr_pages,
		       struct zone *zone, struct memory_group *group)
{
	struct memory_notify mem_arg = {
		.start_pfn = pfn,
		.nr_pages = nr_pages,
	};
	struct node_notify node_arg = {
		.nid = NUMA_NO_NODE,
	};
	const int nid = zone_to_nid(zone);
	int need_zonelists_rebuild = 0;
	unsigned long flags;
	int ret;

	/*
	 * {on,off}lining is constrained to full memory sections (or more
	 * precisely to memory blocks from the user space POV).
	 * memmap_on_memory is an exception because it reserves initial part
	 * of the physical memory space for vmemmaps. That space is pageblock
	 * aligned.
	 */
	if (WARN_ON_ONCE(!nr_pages || !pageblock_aligned(pfn) ||
			 !IS_ALIGNED(pfn + nr_pages, PAGES_PER_SECTION)))
		return -EINVAL;


	/* associate pfn range with the zone */
	move_pfn_range_to_zone(zone, pfn, nr_pages, NULL, MIGRATE_MOVABLE,
			       true);

	if (!node_state(nid, N_MEMORY)) {
		/* Adding memory to the node for the first time */
		node_arg.nid = nid;
		ret = node_notify(NODE_ADDING_FIRST_MEMORY, &node_arg);
		ret = notifier_to_errno(ret);
		if (ret)
			goto failed_addition;
	}

	ret = memory_notify(MEM_GOING_ONLINE, &mem_arg);
	ret = notifier_to_errno(ret);
	if (ret)
		goto failed_addition;

	/*
	 * Fixup the number of isolated pageblocks before marking the sections
	 * onlining, such that undo_isolate_page_range() works correctly.
	 */
	spin_lock_irqsave(&zone->lock, flags);
	zone->nr_isolate_pageblock += nr_pages / pageblock_nr_pages;
	spin_unlock_irqrestore(&zone->lock, flags);

	/*
	 * If this zone is not populated, then it is not in zonelist.
	 * This means the page allocator ignores this zone.
	 * So, zonelist must be updated after online.
	 */
	if (!populated_zone(zone)) {
		need_zonelists_rebuild = 1;
		setup_zone_pageset(zone);
	}

	online_pages_range(pfn, nr_pages);
	adjust_present_page_count(pfn_to_page(pfn), group, nr_pages);

	if (node_arg.nid >= 0)
		node_set_state(nid, N_MEMORY);
	if (need_zonelists_rebuild)
		build_all_zonelists(NULL);

	/* Basic onlining is complete, allow allocation of onlined pages. */
	undo_isolate_page_range(pfn, pfn + nr_pages);

	/*
	 * Freshly onlined pages aren't shuffled (e.g., all pages are placed to
	 * the tail of the freelist when undoing isolation). Shuffle the whole
	 * zone to make sure the just onlined pages are properly distributed
	 * across the whole freelist - to create an initial shuffle.
	 */
	shuffle_zone(zone);

	/* reinitialise watermarks and update pcp limits */
	init_per_zone_wmark_min();

	kswapd_run(nid);
	kcompactd_run(nid);

	if (node_arg.nid >= 0)
		/* First memory added successfully. Notify consumers. */
		node_notify(NODE_ADDED_FIRST_MEMORY, &node_arg);

	writeback_set_ratelimit();

	memory_notify(MEM_ONLINE, &mem_arg);
	return 0;

failed_addition:
	pr_debug("online_pages [mem %#010llx-%#010llx] failed\n",
		 (unsigned long long) pfn << PAGE_SHIFT,
		 (((unsigned long long) pfn + nr_pages) << PAGE_SHIFT) - 1);
	memory_notify(MEM_CANCEL_ONLINE, &mem_arg);
	if (node_arg.nid != NUMA_NO_NODE)
		node_notify(NODE_CANCEL_ADDING_FIRST_MEMORY, &node_arg);
	remove_pfn_range_from_zone(zone, pfn, nr_pages);
	return ret;
}

/* we are OK calling __meminit stuff here - we have CONFIG_MEMORY_HOTPLUG */
static pg_data_t *hotadd_init_pgdat(int nid)
{
	struct pglist_data *pgdat;

	/*
	 * NODE_DATA is preallocated (free_area_init) but its internal
	 * state is not allocated completely. Add missing pieces.
	 * Completely offline nodes stay around and they just need
	 * reintialization.
	 */
	pgdat = NODE_DATA(nid);

	/* init node's zones as empty zones, we don't have any present pages.*/
	free_area_init_core_hotplug(pgdat);

	/*
	 * The node we allocated has no zone fallback lists. For avoiding
	 * to access not-initialized zonelist, build here.
	 */
	build_all_zonelists(pgdat);

	return pgdat;
}

/*
 * __try_online_node - online a node if offlined
 * @nid: the node ID
 * @set_node_online: Whether we want to online the node
 * called by cpu_up() to online a node without onlined memory.
 *
 * Returns:
 * 1 -> a new node has been allocated
 * 0 -> the node is already online
 * -ENOMEM -> the node could not be allocated
 */
static int __try_online_node(int nid, bool set_node_online)
{
	pg_data_t *pgdat;
	int ret = 1;

	if (node_online(nid))
		return 0;

	pgdat = hotadd_init_pgdat(nid);
	if (!pgdat) {
		pr_err("Cannot online node %d due to NULL pgdat\n", nid);
		ret = -ENOMEM;
		goto out;
	}

	if (set_node_online) {
		node_set_online(nid);
		ret = register_one_node(nid);
		BUG_ON(ret);
	}
out:
	return ret;
}

/*
 * Users of this function always want to online/register the node
 */
int try_online_node(int nid)
{
	int ret;

	mem_hotplug_begin();
	ret =  __try_online_node(nid, true);
	mem_hotplug_done();
	return ret;
}

static int check_hotplug_memory_range(u64 start, u64 size)
{
	/* memory range must be block size aligned */
	if (!size || !IS_ALIGNED(start, memory_block_size_bytes()) ||
	    !IS_ALIGNED(size, memory_block_size_bytes())) {
		pr_err("Block size [%#lx] unaligned hotplug range: start %#llx, size %#llx",
		       memory_block_size_bytes(), start, size);
		return -EINVAL;
	}

	return 0;
}

static int online_memory_block(struct memory_block *mem, void *arg)
{
	mem->online_type = mhp_get_default_online_type();
	return device_online(&mem->dev);
}

#ifndef arch_supports_memmap_on_memory
static inline bool arch_supports_memmap_on_memory(unsigned long vmemmap_size)
{
	/*
	 * As default, we want the vmemmap to span a complete PMD such that we
	 * can map the vmemmap using a single PMD if supported by the
	 * architecture.
	 */
	return IS_ALIGNED(vmemmap_size, PMD_SIZE);
}
#endif

bool mhp_supports_memmap_on_memory(void)
{
	unsigned long vmemmap_size = memory_block_memmap_size();
	unsigned long memmap_pages = memory_block_memmap_on_memory_pages();

	/*
	 * Besides having arch support and the feature enabled at runtime, we
	 * need a few more assumptions to hold true:
	 *
	 * a) The vmemmap pages span complete PMDs: We don't want vmemmap code
	 *    to populate memory from the altmap for unrelated parts (i.e.,
	 *    other memory blocks)
	 *
	 * b) The vmemmap pages (and thereby the pages that will be exposed to
	 *    the buddy) have to cover full pageblocks: memory onlining/offlining
	 *    code requires applicable ranges to be page-aligned, for example, to
	 *    set the migratetypes properly.
	 *
	 * TODO: Although we have a check here to make sure that vmemmap pages
	 *       fully populate a PMD, it is not the right place to check for
	 *       this. A much better solution involves improving vmemmap code
	 *       to fallback to base pages when trying to populate vmemmap using
	 *       altmap as an alternative source of memory, and we do not exactly
	 *       populate a single PMD.
	 */
	if (!mhp_memmap_on_memory())
		return false;

	/*
	 * Make sure the vmemmap allocation is fully contained
	 * so that we always allocate vmemmap memory from altmap area.
	 */
	if (!IS_ALIGNED(vmemmap_size, PAGE_SIZE))
		return false;

	/*
	 * start pfn should be pageblock_nr_pages aligned for correctly
	 * setting migrate types
	 */
	if (!pageblock_aligned(memmap_pages))
		return false;

	if (memmap_pages == PHYS_PFN(memory_block_size_bytes()))
		/* No effective hotplugged memory doesn't make sense. */
		return false;

	return arch_supports_memmap_on_memory(vmemmap_size);
}
EXPORT_SYMBOL_GPL(mhp_supports_memmap_on_memory);

static void remove_memory_blocks_and_altmaps(u64 start, u64 size)
{
	unsigned long memblock_size = memory_block_size_bytes();
	u64 cur_start;

	/*
	 * For memmap_on_memory, the altmaps were added on a per-memblock
	 * basis; we have to process each individual memory block.
	 */
	for (cur_start = start; cur_start < start + size;
	     cur_start += memblock_size) {
		struct vmem_altmap *altmap = NULL;
		struct memory_block *mem;

		mem = find_memory_block(pfn_to_section_nr(PFN_DOWN(cur_start)));
		if (WARN_ON_ONCE(!mem))
			continue;

		altmap = mem->altmap;
		mem->altmap = NULL;

		remove_memory_block_devices(cur_start, memblock_size);

		arch_remove_memory(cur_start, memblock_size, altmap);

		/* Verify that all vmemmap pages have actually been freed. */
		WARN(altmap->alloc, "Altmap not fully unmapped");
		kfree(altmap);
	}
}

static int create_altmaps_and_memory_blocks(int nid, struct memory_group *group,
					    u64 start, u64 size, mhp_t mhp_flags)
{
	unsigned long memblock_size = memory_block_size_bytes();
	u64 cur_start;
	int ret;

	for (cur_start = start; cur_start < start + size;
	     cur_start += memblock_size) {
		struct mhp_params params = { .pgprot =
						     pgprot_mhp(PAGE_KERNEL) };
		struct vmem_altmap mhp_altmap = {
			.base_pfn = PHYS_PFN(cur_start),
			.end_pfn = PHYS_PFN(cur_start + memblock_size - 1),
		};

		mhp_altmap.free = memory_block_memmap_on_memory_pages();
		if (mhp_flags & MHP_OFFLINE_INACCESSIBLE)
			mhp_altmap.inaccessible = true;
		params.altmap = kmemdup(&mhp_altmap, sizeof(struct vmem_altmap),
					GFP_KERNEL);
		if (!params.altmap) {
			ret = -ENOMEM;
			goto out;
		}

		/* call arch's memory hotadd */
		ret = arch_add_memory(nid, cur_start, memblock_size, &params);
		if (ret < 0) {
			kfree(params.altmap);
			goto out;
		}

		/* create memory block devices after memory was added */
		ret = create_memory_block_devices(cur_start, memblock_size,
						  params.altmap, group);
		if (ret) {
			arch_remove_memory(cur_start, memblock_size, NULL);
			kfree(params.altmap);
			goto out;
		}
	}

	return 0;
out:
	if (ret && cur_start != start)
		remove_memory_blocks_and_altmaps(start, cur_start - start);
	return ret;
}

/*
 * NOTE: The caller must call lock_device_hotplug() to serialize hotplug
 * and online/offline operations (triggered e.g. by sysfs).
 *
 * we are OK calling __meminit stuff here - we have CONFIG_MEMORY_HOTPLUG
 */
int add_memory_resource(int nid, struct resource *res, mhp_t mhp_flags)
{
	struct mhp_params params = { .pgprot = pgprot_mhp(PAGE_KERNEL) };
	enum memblock_flags memblock_flags = MEMBLOCK_NONE;
	struct memory_group *group = NULL;
	u64 start, size;
	bool new_node = false;
	int ret;

	start = res->start;
	size = resource_size(res);

	ret = check_hotplug_memory_range(start, size);
	if (ret)
		return ret;

	if (mhp_flags & MHP_NID_IS_MGID) {
		group = memory_group_find_by_id(nid);
		if (!group)
			return -EINVAL;
		nid = group->nid;
	}

	if (!node_possible(nid)) {
		WARN(1, "node %d was absent from the node_possible_map\n", nid);
		return -EINVAL;
	}

	mem_hotplug_begin();

	if (IS_ENABLED(CONFIG_ARCH_KEEP_MEMBLOCK)) {
		if (res->flags & IORESOURCE_SYSRAM_DRIVER_MANAGED)
			memblock_flags = MEMBLOCK_DRIVER_MANAGED;
		ret = memblock_add_node(start, size, nid, memblock_flags);
		if (ret)
			goto error_mem_hotplug_end;
	}

	ret = __try_online_node(nid, false);
	if (ret < 0)
		goto error;
	new_node = ret;

	/*
	 * Self hosted memmap array
	 */
	if ((mhp_flags & MHP_MEMMAP_ON_MEMORY) &&
	    mhp_supports_memmap_on_memory()) {
		ret = create_altmaps_and_memory_blocks(nid, group, start, size, mhp_flags);
		if (ret)
			goto error;
	} else {
		ret = arch_add_memory(nid, start, size, &params);
		if (ret < 0)
			goto error;

		/* create memory block devices after memory was added */
		ret = create_memory_block_devices(start, size, NULL, group);
		if (ret) {
			arch_remove_memory(start, size, params.altmap);
			goto error;
		}
	}

	if (new_node) {
		/* If sysfs file of new node can't be created, cpu on the node
		 * can't be hot-added. There is no rollback way now.
		 * So, check by BUG_ON() to catch it reluctantly..
		 * We online node here. We can't roll back from here.
		 */
		node_set_online(nid);
		ret = register_one_node(nid);
		BUG_ON(ret);
	}

	register_memory_blocks_under_node_hotplug(nid, PFN_DOWN(start),
					  PFN_UP(start + size - 1));

	/* create new memmap entry */
	if (!strcmp(res->name, "System RAM"))
		firmware_map_add_hotplug(start, start + size, "System RAM");

	/* device_online() will take the lock when calling online_pages() */
	mem_hotplug_done();

	/*
	 * In case we're allowed to merge the resource, flag it and trigger
	 * merging now that adding succeeded.
	 */
	if (mhp_flags & MHP_MERGE_RESOURCE)
		merge_system_ram_resource(res);

	/* online pages if requested */
	if (mhp_get_default_online_type() != MMOP_OFFLINE)
		walk_memory_blocks(start, size, NULL, online_memory_block);

	return ret;
error:
	if (IS_ENABLED(CONFIG_ARCH_KEEP_MEMBLOCK))
		memblock_remove(start, size);
error_mem_hotplug_end:
	mem_hotplug_done();
	return ret;
}

/* requires device_hotplug_lock, see add_memory_resource() */
int __add_memory(int nid, u64 start, u64 size, mhp_t mhp_flags)
{
	struct resource *res;
	int ret;

	res = register_memory_resource(start, size, "System RAM");
	if (IS_ERR(res))
		return PTR_ERR(res);

	ret = add_memory_resource(nid, res, mhp_flags);
	if (ret < 0)
		release_memory_resource(res);
	return ret;
}

int add_memory(int nid, u64 start, u64 size, mhp_t mhp_flags)
{
	int rc;

	lock_device_hotplug();
	rc = __add_memory(nid, start, size, mhp_flags);
	unlock_device_hotplug();

	return rc;
}
EXPORT_SYMBOL_GPL(add_memory);

/*
 * Add special, driver-managed memory to the system as system RAM. Such
 * memory is not exposed via the raw firmware-provided memmap as system
 * RAM, instead, it is detected and added by a driver - during cold boot,
 * after a reboot, and after kexec.
 *
 * Reasons why this memory should not be used for the initial memmap of a
 * kexec kernel or for placing kexec images:
 * - The booting kernel is in charge of determining how this memory will be
 *   used (e.g., use persistent memory as system RAM)
 * - Coordination with a hypervisor is required before this memory
 *   can be used (e.g., inaccessible parts).
 *
 * For this memory, no entries in /sys/firmware/memmap ("raw firmware-provided
 * memory map") are created. Also, the created memory resource is flagged
 * with IORESOURCE_SYSRAM_DRIVER_MANAGED, so in-kernel users can special-case
 * this memory as well (esp., not place kexec images onto it).
 *
 * The resource_name (visible via /proc/iomem) has to have the format
 * "System RAM ($DRIVER)".
 */
int add_memory_driver_managed(int nid, u64 start, u64 size,
			      const char *resource_name, mhp_t mhp_flags)
{
	struct resource *res;
	int rc;

	if (!resource_name ||
	    strstr(resource_name, "System RAM (") != resource_name ||
	    resource_name[strlen(resource_name) - 1] != ')')
		return -EINVAL;

	lock_device_hotplug();

	res = register_memory_resource(start, size, resource_name);
	if (IS_ERR(res)) {
		rc = PTR_ERR(res);
		goto out_unlock;
	}

	rc = add_memory_resource(nid, res, mhp_flags);
	if (rc < 0)
		release_memory_resource(res);

out_unlock:
	unlock_device_hotplug();
	return rc;
}
EXPORT_SYMBOL_GPL(add_memory_driver_managed);

/*
 * Platforms should define arch_get_mappable_range() that provides
 * maximum possible addressable physical memory range for which the
 * linear mapping could be created. The platform returned address
 * range must adhere to these following semantics.
 *
 * - range.start <= range.end
 * - Range includes both end points [range.start..range.end]
 *
 * There is also a fallback definition provided here, allowing the
 * entire possible physical address range in case any platform does
 * not define arch_get_mappable_range().
 */
struct range __weak arch_get_mappable_range(void)
{
	struct range mhp_range = {
		.start = 0UL,
		.end = -1ULL,
	};
	return mhp_range;
}

struct range mhp_get_pluggable_range(bool need_mapping)
{
	const u64 max_phys = DIRECT_MAP_PHYSMEM_END;
	struct range mhp_range;

	if (need_mapping) {
		mhp_range = arch_get_mappable_range();
		if (mhp_range.start > max_phys) {
			mhp_range.start = 0;
			mhp_range.end = 0;
		}
		mhp_range.end = min_t(u64, mhp_range.end, max_phys);
	} else {
		mhp_range.start = 0;
		mhp_range.end = max_phys;
	}
	return mhp_range;
}
EXPORT_SYMBOL_GPL(mhp_get_pluggable_range);

bool mhp_range_allowed(u64 start, u64 size, bool need_mapping)
{
	struct range mhp_range = mhp_get_pluggable_range(need_mapping);
	u64 end = start + size;

	if (start < end && start >= mhp_range.start && (end - 1) <= mhp_range.end)
		return true;

	pr_warn("Hotplug memory [%#llx-%#llx] exceeds maximum addressable range [%#llx-%#llx]\n",
		start, end, mhp_range.start, mhp_range.end);
	return false;
}

#ifdef CONFIG_MEMORY_HOTREMOVE
/*
 * Scan pfn range [start,end) to find movable/migratable pages (LRU and
 * hugetlb folio, movable_ops pages). Will skip over most unmovable
 * pages (esp., pages that can be skipped when offlining), but bail out on
 * definitely unmovable pages.
 *
 * Returns:
 *	0 in case a movable page is found and movable_pfn was updated.
 *	-ENOENT in case no movable page was found.
 *	-EBUSY in case a definitely unmovable page was found.
 */
static int scan_movable_pages(unsigned long start, unsigned long end,
			      unsigned long *movable_pfn)
{
	unsigned long pfn;

	for_each_valid_pfn(pfn, start, end) {
		struct page *page;
		struct folio *folio;

		page = pfn_to_page(pfn);
		if (PageLRU(page) || page_has_movable_ops(page))
			goto found;

		/*
		 * PageOffline() pages that do not have movable_ops and
		 * have a reference count > 0 (after MEM_GOING_OFFLINE) are
		 * definitely unmovable. If their reference count would be 0,
		 * they could at least be skipped when offlining memory.
		 */
		if (PageOffline(page) && page_count(page))
			return -EBUSY;

		if (!PageHuge(page))
			continue;
		folio = page_folio(page);
		/*
		 * This test is racy as we hold no reference or lock.  The
		 * hugetlb page could have been free'ed and head is no longer
		 * a hugetlb page before the following check.  In such unlikely
		 * cases false positives and negatives are possible.  Calling
		 * code must deal with these scenarios.
		 */
		if (folio_test_hugetlb_migratable(folio))
			goto found;
		pfn |= folio_nr_pages(folio) - 1;
	}
	return -ENOENT;
found:
	*movable_pfn = pfn;
	return 0;
}

static void do_migrate_range(unsigned long start_pfn, unsigned long end_pfn)
{
	struct folio *folio;
	unsigned long pfn;
	LIST_HEAD(source);
	static DEFINE_RATELIMIT_STATE(migrate_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	for_each_valid_pfn(pfn, start_pfn, end_pfn) {
		struct page *page;

		page = pfn_to_page(pfn);
		folio = page_folio(page);

		if (!folio_try_get(folio))
			continue;

		if (unlikely(page_folio(page) != folio))
			goto put_folio;

		if (folio_test_large(folio))
			pfn = folio_pfn(folio) + folio_nr_pages(folio) - 1;

		if (folio_contain_hwpoisoned_page(folio)) {
			if (WARN_ON(folio_test_lru(folio)))
				folio_isolate_lru(folio);
			if (folio_mapped(folio)) {
				folio_lock(folio);
				unmap_poisoned_folio(folio, pfn, false);
				folio_unlock(folio);
			}

			goto put_folio;
		}

		if (!isolate_folio_to_list(folio, &source)) {
			if (__ratelimit(&migrate_rs)) {
				pr_warn("failed to isolate pfn %lx\n",
					page_to_pfn(page));
				dump_page(page, "isolation failed");
			}
		}
put_folio:
		folio_put(folio);
	}
	if (!list_empty(&source)) {
		nodemask_t nmask = node_states[N_MEMORY];
		struct migration_target_control mtc = {
			.nmask = &nmask,
			.gfp_mask = GFP_KERNEL | __GFP_MOVABLE | __GFP_RETRY_MAYFAIL,
			.reason = MR_MEMORY_HOTPLUG,
		};
		int ret;

		/*
		 * We have checked that migration range is on a single zone so
		 * we can use the nid of the first page to all the others.
		 */
		mtc.nid = folio_nid(list_first_entry(&source, struct folio, lru));

		/*
		 * try to allocate from a different node but reuse this node
		 * if there are no other online nodes to be used (e.g. we are
		 * offlining a part of the only existing node)
		 */
		node_clear(mtc.nid, nmask);
		if (nodes_empty(nmask))
			node_set(mtc.nid, nmask);
		ret = migrate_pages(&source, alloc_migration_target, NULL,
			(unsigned long)&mtc, MIGRATE_SYNC, MR_MEMORY_HOTPLUG, NULL);
		if (ret) {
			list_for_each_entry(folio, &source, lru) {
				if (__ratelimit(&migrate_rs)) {
					pr_warn("migrating pfn %lx failed ret:%d\n",
						folio_pfn(folio), ret);
					dump_page(&folio->page,
						  "migration failure");
				}
			}
			putback_movable_pages(&source);
		}
	}
}

static int __init cmdline_parse_movable_node(char *p)
{
	movable_node_enabled = true;
	return 0;
}
early_param("movable_node", cmdline_parse_movable_node);

static int count_system_ram_pages_cb(unsigned long start_pfn,
				     unsigned long nr_pages, void *data)
{
	unsigned long *nr_system_ram_pages = data;

	*nr_system_ram_pages += nr_pages;
	return 0;
}

/*
 * Must be called with mem_hotplug_lock in write mode.
 */
int offline_pages(unsigned long start_pfn, unsigned long nr_pages,
			struct zone *zone, struct memory_group *group)
{
	unsigned long pfn, managed_pages, system_ram_pages = 0;
	const unsigned long end_pfn = start_pfn + nr_pages;
	struct pglist_data *pgdat = zone->zone_pgdat;
	const int node = zone_to_nid(zone);
	struct memory_notify mem_arg = {
		.start_pfn = start_pfn,
		.nr_pages = nr_pages,
	};
	struct node_notify node_arg = {
		.nid = NUMA_NO_NODE,
	};
	unsigned long flags;
	char *reason;
	int ret;

	/*
	 * {on,off}lining is constrained to full memory sections (or more
	 * precisely to memory blocks from the user space POV).
	 * memmap_on_memory is an exception because it reserves initial part
	 * of the physical memory space for vmemmaps. That space is pageblock
	 * aligned.
	 */
	if (WARN_ON_ONCE(!nr_pages || !pageblock_aligned(start_pfn) ||
			 !IS_ALIGNED(start_pfn + nr_pages, PAGES_PER_SECTION)))
		return -EINVAL;

	/*
	 * Don't allow to offline memory blocks that contain holes.
	 * Consequently, memory blocks with holes can never get onlined
	 * via the hotplug path - online_pages() - as hotplugged memory has
	 * no holes. This way, we don't have to worry about memory holes,
	 * don't need pfn_valid() checks, and can avoid using
	 * walk_system_ram_range() later.
	 */
	walk_system_ram_range(start_pfn, nr_pages, &system_ram_pages,
			      count_system_ram_pages_cb);
	if (system_ram_pages != nr_pages) {
		ret = -EINVAL;
		reason = "memory holes";
		goto failed_removal;
	}

	/*
	 * We only support offlining of memory blocks managed by a single zone,
	 * checked by calling code. This is just a sanity check that we might
	 * want to remove in the future.
	 */
	if (WARN_ON_ONCE(page_zone(pfn_to_page(start_pfn)) != zone ||
			 page_zone(pfn_to_page(end_pfn - 1)) != zone)) {
		ret = -EINVAL;
		reason = "multizone range";
		goto failed_removal;
	}

	/*
	 * Disable pcplists so that page isolation cannot race with freeing
	 * in a way that pages from isolated pageblock are left on pcplists.
	 */
	zone_pcp_disable(zone);
	lru_cache_disable();

	/* set above range as isolated */
	ret = start_isolate_page_range(start_pfn, end_pfn,
				       PB_ISOLATE_MODE_MEM_OFFLINE);
	if (ret) {
		reason = "failure to isolate range";
		goto failed_removal_pcplists_disabled;
	}

	/*
	 * Check whether the node will have no present pages after we offline
	 * 'nr_pages' more. If so, we know that the node will become empty, and
	 * so we will clear N_MEMORY for it.
	 */
	if (nr_pages >= pgdat->node_present_pages) {
		node_arg.nid = node;
		ret = node_notify(NODE_REMOVING_LAST_MEMORY, &node_arg);
		ret = notifier_to_errno(ret);
		if (ret) {
			reason = "node notifier failure";
			goto failed_removal_isolated;
		}
	}

	ret = memory_notify(MEM_GOING_OFFLINE, &mem_arg);
	ret = notifier_to_errno(ret);
	if (ret) {
		reason = "notifier failure";
		goto failed_removal_isolated;
	}

	do {
		pfn = start_pfn;
		do {
			/*
			 * Historically we always checked for any signal and
			 * can't limit it to fatal signals without eventually
			 * breaking user space.
			 */
			if (signal_pending(current)) {
				ret = -EINTR;
				reason = "signal backoff";
				goto failed_removal_isolated;
			}

			cond_resched();

			ret = scan_movable_pages(pfn, end_pfn, &pfn);
			if (!ret) {
				/*
				 * TODO: fatal migration failures should bail
				 * out
				 */
				do_migrate_range(pfn, end_pfn);
			}
		} while (!ret);

		if (ret != -ENOENT) {
			reason = "unmovable page";
			goto failed_removal_isolated;
		}

		/*
		 * Dissolve free hugetlb folios in the memory block before doing
		 * offlining actually in order to make hugetlbfs's object
		 * counting consistent.
		 */
		ret = dissolve_free_hugetlb_folios(start_pfn, end_pfn);
		if (ret) {
			reason = "failure to dissolve huge pages";
			goto failed_removal_isolated;
		}

		ret = test_pages_isolated(start_pfn, end_pfn,
					  PB_ISOLATE_MODE_MEM_OFFLINE);

	} while (ret);

	/* Mark all sections offline and remove free pages from the buddy. */
	managed_pages = __offline_isolated_pages(start_pfn, end_pfn);
	pr_debug("Offlined Pages %ld\n", nr_pages);

	/*
	 * The memory sections are marked offline, and the pageblock flags
	 * effectively stale; nobody should be touching them. Fixup the number
	 * of isolated pageblocks, memory onlining will properly revert this.
	 */
	spin_lock_irqsave(&zone->lock, flags);
	zone->nr_isolate_pageblock -= nr_pages / pageblock_nr_pages;
	spin_unlock_irqrestore(&zone->lock, flags);

	lru_cache_enable();
	zone_pcp_enable(zone);

	/* removal success */
	adjust_managed_page_count(pfn_to_page(start_pfn), -managed_pages);
	adjust_present_page_count(pfn_to_page(start_pfn), group, -nr_pages);

	/* reinitialise watermarks and update pcp limits */
	init_per_zone_wmark_min();

	/*
	 * Make sure to mark the node as memory-less before rebuilding the zone
	 * list. Otherwise this node would still appear in the fallback lists.
	 */
	if (node_arg.nid >= 0)
		node_clear_state(node, N_MEMORY);
	if (!populated_zone(zone)) {
		zone_pcp_reset(zone);
		build_all_zonelists(NULL);
	}

	if (node_arg.nid >= 0) {
		kcompactd_stop(node);
		kswapd_stop(node);
		/* Node went memoryless. Notify consumers */
		node_notify(NODE_REMOVED_LAST_MEMORY, &node_arg);
	}

	writeback_set_ratelimit();

	memory_notify(MEM_OFFLINE, &mem_arg);
	remove_pfn_range_from_zone(zone, start_pfn, nr_pages);
	return 0;

failed_removal_isolated:
	/* pushback to free area */
	undo_isolate_page_range(start_pfn, end_pfn);
	memory_notify(MEM_CANCEL_OFFLINE, &mem_arg);
	if (node_arg.nid != NUMA_NO_NODE)
		node_notify(NODE_CANCEL_REMOVING_LAST_MEMORY, &node_arg);
failed_removal_pcplists_disabled:
	lru_cache_enable();
	zone_pcp_enable(zone);
failed_removal:
	pr_debug("memory offlining [mem %#010llx-%#010llx] failed due to %s\n",
		 (unsigned long long) start_pfn << PAGE_SHIFT,
		 ((unsigned long long) end_pfn << PAGE_SHIFT) - 1,
		 reason);
	return ret;
}

static int check_memblock_offlined_cb(struct memory_block *mem, void *arg)
{
	int *nid = arg;

	*nid = mem->nid;
	if (unlikely(mem->state != MEM_OFFLINE)) {
		phys_addr_t beginpa, endpa;

		beginpa = PFN_PHYS(section_nr_to_pfn(mem->start_section_nr));
		endpa = beginpa + memory_block_size_bytes() - 1;
		pr_warn("removing memory fails, because memory [%pa-%pa] is onlined\n",
			&beginpa, &endpa);

		return -EBUSY;
	}
	return 0;
}

static int count_memory_range_altmaps_cb(struct memory_block *mem, void *arg)
{
	u64 *num_altmaps = (u64 *)arg;

	if (mem->altmap)
		*num_altmaps += 1;

	return 0;
}

static int check_cpu_on_node(int nid)
{
	int cpu;

	for_each_present_cpu(cpu) {
		if (cpu_to_node(cpu) == nid)
			/*
			 * the cpu on this node isn't removed, and we can't
			 * offline this node.
			 */
			return -EBUSY;
	}

	return 0;
}

static int check_no_memblock_for_node_cb(struct memory_block *mem, void *arg)
{
	int nid = *(int *)arg;

	/*
	 * If a memory block belongs to multiple nodes, the stored nid is not
	 * reliable. However, such blocks are always online (e.g., cannot get
	 * offlined) and, therefore, are still spanned by the node.
	 */
	return mem->nid == nid ? -EEXIST : 0;
}

/**
 * try_offline_node
 * @nid: the node ID
 *
 * Offline a node if all memory sections and cpus of the node are removed.
 *
 * NOTE: The caller must call lock_device_hotplug() to serialize hotplug
 * and online/offline operations before this call.
 */
void try_offline_node(int nid)
{
	int rc;

	/*
	 * If the node still spans pages (especially ZONE_DEVICE), don't
	 * offline it. A node spans memory after move_pfn_range_to_zone(),
	 * e.g., after the memory block was onlined.
	 */
	if (node_spanned_pages(nid))
		return;

	/*
	 * Especially offline memory blocks might not be spanned by the
	 * node. They will get spanned by the node once they get onlined.
	 * However, they link to the node in sysfs and can get onlined later.
	 */
	rc = for_each_memory_block(&nid, check_no_memblock_for_node_cb);
	if (rc)
		return;

	if (check_cpu_on_node(nid))
		return;

	/*
	 * all memory/cpu of this node are removed, we can offline this
	 * node now.
	 */
	node_set_offline(nid);
	unregister_one_node(nid);
}
EXPORT_SYMBOL(try_offline_node);

static int memory_blocks_have_altmaps(u64 start, u64 size)
{
	u64 num_memblocks = size / memory_block_size_bytes();
	u64 num_altmaps = 0;

	if (!mhp_memmap_on_memory())
		return 0;

	walk_memory_blocks(start, size, &num_altmaps,
			   count_memory_range_altmaps_cb);

	if (num_altmaps == 0)
		return 0;

	if (WARN_ON_ONCE(num_memblocks != num_altmaps))
		return -EINVAL;

	return 1;
}

static int try_remove_memory(u64 start, u64 size)
{
	int rc, nid = NUMA_NO_NODE;

	BUG_ON(check_hotplug_memory_range(start, size));

	/*
	 * All memory blocks must be offlined before removing memory.  Check
	 * whether all memory blocks in question are offline and return error
	 * if this is not the case.
	 *
	 * While at it, determine the nid. Note that if we'd have mixed nodes,
	 * we'd only try to offline the last determined one -- which is good
	 * enough for the cases we care about.
	 */
	rc = walk_memory_blocks(start, size, &nid, check_memblock_offlined_cb);
	if (rc)
		return rc;

	/* remove memmap entry */
	firmware_map_remove(start, start + size, "System RAM");

	mem_hotplug_begin();

	rc = memory_blocks_have_altmaps(start, size);
	if (rc < 0) {
		mem_hotplug_done();
		return rc;
	} else if (!rc) {
		/*
		 * Memory block device removal under the device_hotplug_lock is
		 * a barrier against racing online attempts.
		 * No altmaps present, do the removal directly
		 */
		remove_memory_block_devices(start, size);
		arch_remove_memory(start, size, NULL);
	} else {
		/* all memblocks in the range have altmaps */
		remove_memory_blocks_and_altmaps(start, size);
	}

	if (IS_ENABLED(CONFIG_ARCH_KEEP_MEMBLOCK))
		memblock_remove(start, size);

	release_mem_region_adjustable(start, size);

	if (nid != NUMA_NO_NODE)
		try_offline_node(nid);

	mem_hotplug_done();
	return 0;
}

/**
 * __remove_memory - Remove memory if every memory block is offline
 * @start: physical address of the region to remove
 * @size: size of the region to remove
 *
 * NOTE: The caller must call lock_device_hotplug() to serialize hotplug
 * and online/offline operations before this call, as required by
 * try_offline_node().
 */
void __remove_memory(u64 start, u64 size)
{

	/*
	 * trigger BUG() if some memory is not offlined prior to calling this
	 * function
	 */
	if (try_remove_memory(start, size))
		BUG();
}

/*
 * Remove memory if every memory block is offline, otherwise return -EBUSY is
 * some memory is not offline
 */
int remove_memory(u64 start, u64 size)
{
	int rc;

	lock_device_hotplug();
	rc = try_remove_memory(start, size);
	unlock_device_hotplug();

	return rc;
}
EXPORT_SYMBOL_GPL(remove_memory);

static int try_offline_memory_block(struct memory_block *mem, void *arg)
{
	uint8_t online_type = MMOP_ONLINE_KERNEL;
	uint8_t **online_types = arg;
	struct page *page;
	int rc;

	/*
	 * Sense the online_type via the zone of the memory block. Offlining
	 * with multiple zones within one memory block will be rejected
	 * by offlining code ... so we don't care about that.
	 */
	page = pfn_to_online_page(section_nr_to_pfn(mem->start_section_nr));
	if (page && zone_idx(page_zone(page)) == ZONE_MOVABLE)
		online_type = MMOP_ONLINE_MOVABLE;

	rc = device_offline(&mem->dev);
	/*
	 * Default is MMOP_OFFLINE - change it only if offlining succeeded,
	 * so try_reonline_memory_block() can do the right thing.
	 */
	if (!rc)
		**online_types = online_type;

	(*online_types)++;
	/* Ignore if already offline. */
	return rc < 0 ? rc : 0;
}

static int try_reonline_memory_block(struct memory_block *mem, void *arg)
{
	uint8_t **online_types = arg;
	int rc;

	if (**online_types != MMOP_OFFLINE) {
		mem->online_type = **online_types;
		rc = device_online(&mem->dev);
		if (rc < 0)
			pr_warn("%s: Failed to re-online memory: %d",
				__func__, rc);
	}

	/* Continue processing all remaining memory blocks. */
	(*online_types)++;
	return 0;
}

/*
 * Try to offline and remove memory. Might take a long time to finish in case
 * memory is still in use. Primarily useful for memory devices that logically
 * unplugged all memory (so it's no longer in use) and want to offline + remove
 * that memory.
 */
int offline_and_remove_memory(u64 start, u64 size)
{
	const unsigned long mb_count = size / memory_block_size_bytes();
	uint8_t *online_types, *tmp;
	int rc;

	if (!IS_ALIGNED(start, memory_block_size_bytes()) ||
	    !IS_ALIGNED(size, memory_block_size_bytes()) || !size)
		return -EINVAL;

	/*
	 * We'll remember the old online type of each memory block, so we can
	 * try to revert whatever we did when offlining one memory block fails
	 * after offlining some others succeeded.
	 */
	online_types = kmalloc_array(mb_count, sizeof(*online_types),
				     GFP_KERNEL);
	if (!online_types)
		return -ENOMEM;
	/*
	 * Initialize all states to MMOP_OFFLINE, so when we abort processing in
	 * try_offline_memory_block(), we'll skip all unprocessed blocks in
	 * try_reonline_memory_block().
	 */
	memset(online_types, MMOP_OFFLINE, mb_count);

	lock_device_hotplug();

	tmp = online_types;
	rc = walk_memory_blocks(start, size, &tmp, try_offline_memory_block);

	/*
	 * In case we succeeded to offline all memory, remove it.
	 * This cannot fail as it cannot get onlined in the meantime.
	 */
	if (!rc) {
		rc = try_remove_memory(start, size);
		if (rc)
			pr_err("%s: Failed to remove memory: %d", __func__, rc);
	}

	/*
	 * Rollback what we did. While memory onlining might theoretically fail
	 * (nacked by a notifier), it barely ever happens.
	 */
	if (rc) {
		tmp = online_types;
		walk_memory_blocks(start, size, &tmp,
				   try_reonline_memory_block);
	}
	unlock_device_hotplug();

	kfree(online_types);
	return rc;
}
EXPORT_SYMBOL_GPL(offline_and_remove_memory);
#endif /* CONFIG_MEMORY_HOTREMOVE */
