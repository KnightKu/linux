// SPDX-License-Identifier: GPL-2.0-only
/*
 * Full-online compress capacity emergency: SOFT reclaim / HARD wrprotect.
 */
#include <linux/cxl_compress_emergency.h>
#include <linux/migrate.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nodemask.h>
#include <linux/swap.h>
#include <linux/workqueue.h>

#include "cxlmem.h"
#include "cxl.h"
#include "compress_alert.h"

/* Owned by compress.c */
extern struct list_head cxl_compress_hpa_ranges;
extern struct mutex cxl_compress_hpa_lock;

enum ce_state {
	CE_NORMAL = 0,
	CE_SOFT,
	CE_HARD,
	CE_CRITICAL,
	CE_RECOVER,
};

static enum ce_state ce_state = CE_NORMAL;
static DEFINE_MUTEX(ce_lock);
static int ce_nid = NUMA_NO_NODE;
static struct cxl_memdev *ce_cxlmd;
static struct delayed_work ce_reclaim_work;
static bool ce_registered;

static const char *ce_state_name(enum ce_state s)
{
	switch (s) {
	case CE_NORMAL:
		return "normal";
	case CE_SOFT:
		return "soft";
	case CE_HARD:
		return "hard";
	case CE_CRITICAL:
		return "critical";
	case CE_RECOVER:
		return "recover";
	}
	return "?";
}

static void ce_boost_node(int nid, bool on)
{
	pg_data_t *pgdat;
	int z;

	if (nid < 0 || nid >= MAX_NUMNODES || !node_online(nid))
		return;
	pgdat = NODE_DATA(nid);
	for (z = 0; z < MAX_NR_ZONES; z++) {
		struct zone *zone = &pgdat->node_zones[z];

		if (!managed_zone(zone))
			continue;
		if (on) {
			unsigned long managed = zone_managed_pages(zone);

			WRITE_ONCE(zone->watermark_boost,
				   managed ? managed / 2 : 0);
			set_bit(ZONE_BOOSTED_WATERMARK, &zone->flags);
			wakeup_kswapd(zone, GFP_KERNEL, 0, ZONE_MOVABLE);
		} else {
			WRITE_ONCE(zone->watermark_boost, 0);
		}
	}
}

static void ce_wrprotect_tracked_ranges(void)
{
	struct cxl_compress_hpa_range *r;
	u64 protected = 0;

	mutex_lock(&cxl_compress_hpa_lock);
	list_for_each_entry(r, &cxl_compress_hpa_ranges, list) {
		u64 off;

		if (ce_cxlmd && r->cxlmd != ce_cxlmd)
			continue;
		for (off = 0; off < r->length; off += PAGE_SIZE) {
			unsigned long pfn = PHYS_PFN(r->hpa_start + off);
			struct page *page;
			struct folio *folio;

			if (!pfn_valid(pfn))
				continue;
			page = pfn_to_page(pfn);
			if (page_count(page) == 0)
				continue;
			folio = page_folio(page);
			if (cxl_compress_wrprotect_folio(folio) > 0)
				protected++;
		}
	}
	mutex_unlock(&cxl_compress_hpa_lock);

	pr_info("cxl_compress_emergency: wrprotected %llu page(s)\n",
		protected);
}

static void ce_enter(enum ce_state next)
{
	enum ce_state prev;

	mutex_lock(&ce_lock);
	prev = ce_state;
	if (prev == next) {
		mutex_unlock(&ce_lock);
		return;
	}
	ce_state = next;
	mutex_unlock(&ce_lock);

	pr_warn("cxl_compress_emergency: %s -> %s (nid=%d)\n",
		ce_state_name(prev), ce_state_name(next), ce_nid);

	switch (next) {
	case CE_SOFT:
		ce_boost_node(ce_nid, true);
		if (ce_cxlmd)
			cxl_compress_sync_free_pages(ce_cxlmd);
		mod_delayed_work(system_long_wq, &ce_reclaim_work, 0);
		break;
	case CE_HARD:
	case CE_CRITICAL:
		ce_boost_node(ce_nid, true);
		ce_wrprotect_tracked_ranges();
		if (ce_cxlmd)
			cxl_compress_sync_free_pages(ce_cxlmd);
		mod_delayed_work(system_long_wq, &ce_reclaim_work, 0);
		break;
	case CE_RECOVER:
	case CE_NORMAL:
		ce_boost_node(ce_nid, false);
		cancel_delayed_work_sync(&ce_reclaim_work);
		break;
	}
}

static void ce_reclaim_workfn(struct work_struct *work)
{
	if (ce_state == CE_SOFT || ce_state == CE_HARD ||
	    ce_state == CE_CRITICAL) {
		ce_boost_node(ce_nid, true);
		if (ce_cxlmd)
			cxl_compress_sync_free_pages(ce_cxlmd);
		schedule_delayed_work(&ce_reclaim_work, HZ);
	}
}

static int ce_pick_dest_nid(void)
{
	int dest;

	for_each_online_node(dest) {
		if (dest != ce_nid)
			return dest;
	}
	return NUMA_NO_NODE;
}

static int ce_wp_fault_fixed(struct folio *folio)
{
	unsigned long pfn = folio_pfn(folio);
	struct cxl_compress_hpa_range *r;
	bool in_range = false;
	int dest;
	int rc;

	if (ce_state < CE_HARD)
		return 0;

	mutex_lock(&cxl_compress_hpa_lock);
	list_for_each_entry(r, &cxl_compress_hpa_ranges, list) {
		u64 hpa = PFN_PHYS(pfn);

		if (hpa >= r->hpa_start && hpa < r->hpa_start + r->length) {
			in_range = true;
			break;
		}
	}
	mutex_unlock(&cxl_compress_hpa_lock);
	if (!in_range)
		return 0;

	dest = ce_pick_dest_nid();
	if (dest < 0)
		return -ENOMEM;

	folio_get(folio);
	rc = cxl_compress_migrate_folio_to_nid(folio, dest);
	folio_put(folio);
	return rc ? -ENOMEM : 1;
}

static int ce_alert_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct cxl_compress_alert *alert = data;
	u64 used, total;

	if (!alert)
		return NOTIFY_DONE;

	ce_cxlmd = alert->cxlmd;
	used = alert->phys_used;
	total = alert->phys_total ? alert->phys_total : 1;

	if (ce_nid == NUMA_NO_NODE) {
		struct cxl_compress_hpa_range *r;

		mutex_lock(&cxl_compress_hpa_lock);
		if (!list_empty(&cxl_compress_hpa_ranges)) {
			r = list_first_entry(&cxl_compress_hpa_ranges,
					     struct cxl_compress_hpa_range,
					     list);
			ce_nid = r->nid;
		}
		mutex_unlock(&cxl_compress_hpa_lock);
	}

	switch (alert->level) {
	case CXL_COMPRESS_WM_LEVEL_LOW:
		if (ce_state == CE_NORMAL || ce_state == CE_RECOVER)
			ce_enter(CE_SOFT);
		if (ce_state == CE_SOFT && used * 100 >= total * 90)
			ce_enter(CE_HARD);
		break;
	case CXL_COMPRESS_WM_LEVEL_CRITICAL:
		ce_enter(CE_CRITICAL);
		break;
	case CXL_COMPRESS_WM_LEVEL_HIGH:
		if (ce_state != CE_NORMAL) {
			ce_enter(CE_RECOVER);
			ce_enter(CE_NORMAL);
		}
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block ce_nb = {
	.notifier_call = ce_alert_notify,
};

void cxl_compress_emergency_init(void)
{
	INIT_DELAYED_WORK(&ce_reclaim_work, ce_reclaim_workfn);
	cxl_compress_register_alert(&ce_nb);
	WRITE_ONCE(cxl_compress_emergency_wp_fault_hook, ce_wp_fault_fixed);
	ce_registered = true;
	pr_info("cxl_compress_emergency: registered (full-online mode)\n");
}

void cxl_compress_emergency_exit(void)
{
	if (!ce_registered)
		return;
	WRITE_ONCE(cxl_compress_emergency_wp_fault_hook, NULL);
	cxl_compress_unregister_alert(&ce_nb);
	cancel_delayed_work_sync(&ce_reclaim_work);
	ce_boost_node(ce_nid, false);
	ce_registered = false;
}
