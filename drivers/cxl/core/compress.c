// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026. Transparent-compression CXL Type-3 vendor mailbox. */
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/memory.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <cxl/event.h>

#include "cxlmem.h"
#include "cxl.h"
#include "core.h"
#include "compress_alert.h"

/*
 * full_online=Y (default): never skip dax_kmem for compress regions so the
 * entire logical capacity can be System-RAM. Hybrid skip-kmem is opt-in.
 */
static bool full_online = true;
module_param(full_online, bool, 0644);
MODULE_PARM_DESC(full_online,
		 "Online full compress logical capacity as System-RAM (default Y)");

static BLOCKING_NOTIFIER_HEAD(cxl_compress_alert_chain);
LIST_HEAD(cxl_compress_hpa_ranges);
DEFINE_MUTEX(cxl_compress_hpa_lock);
static struct workqueue_struct *cxl_compress_wq;

struct cxl_compress_alert_work {
	struct work_struct work;
	struct cxl_memdev *cxlmd;
	u8 level;
	u64 phys_used;
	u64 phys_total;
};

/*
 * Vendor mailbox ABI shared with QEMU CompressCXL P0
 * (opcode set 0xC0, event UUID CXL_EVENT_COMPRESS_WM_UUID).
 */

struct cxl_mbox_compress_cap {
	__le64 phys_total;
	__le64 phys_used;
	__le64 guaranteed_size;
	__le64 logical_size;
	__le64 low_wm;
	__le64 high_wm;
} __packed;

struct cxl_mbox_compress_set_wm {
	__le64 low_wm;
	__le64 high_wm;
} __packed;

struct cxl_mbox_compress_acct {
	__le64 allocated_pages;
	__le64 poison_pages;
	__le64 free_slots;
} __packed;

struct cxl_mbox_media_op_range {
	__le64 starting_dpa;
	__le64 length;
} __packed;

struct cxl_mbox_media_op_zero {
	u8 media_operation_class;
	u8 media_operation_subclass;
	u8 rsvd[2];
	__le32 dpa_range_count;
	struct cxl_mbox_media_op_range dpa_range_list[];
} __packed;

static void cxl_compress_apply_cap(struct cxl_compress_state *cs,
				   const struct cxl_mbox_compress_cap *cap)
{
	cs->phys_total = le64_to_cpu(cap->phys_total);
	cs->phys_used = le64_to_cpu(cap->phys_used);
	cs->guaranteed_size = le64_to_cpu(cap->guaranteed_size);
	cs->logical_size = le64_to_cpu(cap->logical_size);
	cs->low_wm = le64_to_cpu(cap->low_wm);
	cs->high_wm = le64_to_cpu(cap->high_wm);
}

static void cxl_compress_apply_acct(struct cxl_compress_state *cs,
				    const struct cxl_mbox_compress_acct *acct)
{
	cs->allocated_pages = le64_to_cpu(acct->allocated_pages);
	cs->poison_pages = le64_to_cpu(acct->poison_pages);
	cs->free_slots = le64_to_cpu(acct->free_slots);
}

int cxl_compress_refresh(struct cxl_memdev_state *mds)
{
	struct cxl_mailbox *cxl_mbox = &mds->cxlds.cxl_mbox;
	struct cxl_mbox_compress_cap cap;
	struct cxl_mbox_compress_acct acct;
	struct cxl_mbox_cmd mbox_cmd;
	int rc;

	if (!mds->compress.enabled)
		return -ENODEV;

	mbox_cmd = (struct cxl_mbox_cmd) {
		.opcode = CXL_MBOX_OP_COMPRESS_GET_CAP,
		.size_out = sizeof(cap),
		.payload_out = &cap,
	};
	rc = cxl_internal_send_cmd(cxl_mbox, &mbox_cmd);
	if (rc)
		return rc;

	mbox_cmd = (struct cxl_mbox_cmd) {
		.opcode = CXL_MBOX_OP_COMPRESS_GET_ACCT,
		.size_out = sizeof(acct),
		.payload_out = &acct,
	};
	rc = cxl_internal_send_cmd(cxl_mbox, &mbox_cmd);
	if (rc)
		return rc;

	guard(mutex)(&mds->compress.lock);
	cxl_compress_apply_cap(&mds->compress, &cap);
	cxl_compress_apply_acct(&mds->compress, &acct);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_compress_refresh, "CXL");

static int cxl_compress_set_watermarks(struct cxl_memdev_state *mds,
				       u64 low_wm, u64 high_wm)
{
	struct cxl_mailbox *cxl_mbox = &mds->cxlds.cxl_mbox;
	struct cxl_mbox_compress_set_wm in = {
		.low_wm = cpu_to_le64(low_wm),
		.high_wm = cpu_to_le64(high_wm),
	};
	struct cxl_mbox_cmd mbox_cmd = {
		.opcode = CXL_MBOX_OP_COMPRESS_SET_WM,
		.size_in = sizeof(in),
		.payload_in = &in,
	};
	int rc;

	rc = cxl_internal_send_cmd(cxl_mbox, &mbox_cmd);
	if (rc)
		return rc;

	return cxl_compress_refresh(mds);
}

int cxl_compress_media_zero(struct cxl_memdev_state *mds, u64 dpa, u64 length)
{
	struct cxl_mailbox *cxl_mbox = &mds->cxlds.cxl_mbox;
	struct cxl_mbox_media_op_zero *in;
	size_t in_size = struct_size(in, dpa_range_list, 1);
	struct cxl_mbox_cmd mbox_cmd;
	int rc;

	if (!mds->compress.enabled)
		return -ENODEV;
	if (!length || !IS_ALIGNED(dpa, SZ_4K) || !IS_ALIGNED(length, SZ_4K))
		return -EINVAL;

	in = kzalloc(in_size, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	in->media_operation_class = 0x1; /* Sanitize */
	in->media_operation_subclass = 0x1; /* Zero */
	in->dpa_range_count = cpu_to_le32(1);
	in->dpa_range_list[0].starting_dpa = cpu_to_le64(dpa);
	in->dpa_range_list[0].length = cpu_to_le64(length);

	mbox_cmd = (struct cxl_mbox_cmd) {
		.opcode = CXL_MBOX_OP_MEDIA_OPERATIONS,
		.size_in = in_size,
		.payload_in = in,
		.poll_interval_ms = 1000,
		.poll_count = 30,
	};
	rc = cxl_internal_send_cmd(cxl_mbox, &mbox_cmd);
	kfree(in);
	if (rc)
		return rc;

	return cxl_compress_refresh(mds);
}
EXPORT_SYMBOL_NS_GPL(cxl_compress_media_zero, "CXL");

int cxl_compress_register_alert(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&cxl_compress_alert_chain, nb);
}
EXPORT_SYMBOL_NS_GPL(cxl_compress_register_alert, "CXL");

int cxl_compress_unregister_alert(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&cxl_compress_alert_chain, nb);
}
EXPORT_SYMBOL_NS_GPL(cxl_compress_unregister_alert, "CXL");

int cxl_compress_register_hpa_range(struct cxl_memdev *cxlmd, u64 hpa,
				    u64 length, int nid)
{
	struct cxl_compress_hpa_range *r;

	if (!cxlmd || !length || !IS_ALIGNED(hpa, PAGE_SIZE) ||
	    !IS_ALIGNED(length, PAGE_SIZE))
		return -EINVAL;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	r->cxlmd = cxlmd;
	r->hpa_start = hpa;
	r->length = length;
	r->dpa_base = 0;
	r->nid = nid;

	mutex_lock(&cxl_compress_hpa_lock);
	list_add_tail(&r->list, &cxl_compress_hpa_ranges);
	mutex_unlock(&cxl_compress_hpa_lock);

	dev_info(&cxlmd->dev,
		 "compress track HPA %#llx len %#llx nid %d\n",
		 hpa, length, nid);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_compress_register_hpa_range, "CXL");

/*
 * Walk tracked HPA ranges and Zero device slots for pages that are free
 * in the buddy (swap-out / reclaim already dropped the host page).
 */
int cxl_compress_sync_free_pages(struct cxl_memdev *cxlmd)
{
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);
	struct cxl_compress_hpa_range *r;
	u64 zeroed = 0;
	int rc = 0;

	if (!mds || !mds->compress.enabled)
		return -ENODEV;

	mutex_lock(&cxl_compress_hpa_lock);
	list_for_each_entry(r, &cxl_compress_hpa_ranges, list) {
		u64 off, run_off = U64_MAX, run_len = 0;

		if (r->cxlmd != cxlmd)
			continue;
		for (off = 0; off <= r->length; off += PAGE_SIZE) {
			bool is_free = false;

			if (off < r->length) {
				unsigned long pfn = PHYS_PFN(r->hpa_start + off);
				struct page *page;

				if (pfn_valid(pfn)) {
					page = pfn_to_page(pfn);
					is_free = page_count(page) == 0;
				}
			}

			if (is_free) {
				if (run_off == U64_MAX)
					run_off = off;
				run_len += PAGE_SIZE;
				continue;
			}

			if (run_len) {
				int zrc = cxl_compress_media_zero(
					mds, r->dpa_base + run_off, run_len);

				if (!zrc)
					zeroed += run_len >> PAGE_SHIFT;
				else if (zrc != -EINVAL)
					rc = zrc;
				run_off = U64_MAX;
				run_len = 0;
				if (rc)
					break;
			}
		}
		if (rc)
			break;
	}
	mutex_unlock(&cxl_compress_hpa_lock);

	dev_info(&cxlmd->dev, "compress sync_free zeroed %llu pages rc=%d\n",
		 zeroed, rc);
	return rc;
}
EXPORT_SYMBOL_NS_GPL(cxl_compress_sync_free_pages, "CXL");

static void cxl_compress_alert_workfn(struct work_struct *work)
{
	struct cxl_compress_alert_work *aw =
		container_of(work, struct cxl_compress_alert_work, work);
	struct cxl_memdev *cxlmd = aw->cxlmd;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);
	struct cxl_compress_alert alert = {
		.cxlmd = cxlmd,
		.level = aw->level,
		.phys_used = aw->phys_used,
		.phys_total = aw->phys_total,
	};
	char *envp[3];
	char buf1[32], buf2[32];

	if (mds && mds->compress.enabled)
		cxl_compress_refresh(mds);

	if (aw->level == CXL_COMPRESS_WM_LEVEL_LOW)
		dev_warn(&cxlmd->dev,
			 "compress capacity low watermark: used %#llx / %#llx\n",
			 aw->phys_used, aw->phys_total);
	else if (aw->level == CXL_COMPRESS_WM_LEVEL_HIGH)
		dev_info(&cxlmd->dev,
			 "compress capacity high watermark: used %#llx / %#llx\n",
			 aw->phys_used, aw->phys_total);
	else if (aw->level == CXL_COMPRESS_WM_LEVEL_CRITICAL)
		dev_err(&cxlmd->dev,
			"compress capacity CRITICAL watermark: used %#llx / %#llx\n",
			aw->phys_used, aw->phys_total);
	else
		dev_info(&cxlmd->dev,
			 "compress capacity event level=%u used %#llx / %#llx\n",
			 aw->level, aw->phys_used, aw->phys_total);

	blocking_notifier_call_chain(&cxl_compress_alert_chain, aw->level,
				     &alert);

	snprintf(buf1, sizeof(buf1), "CXL_COMPRESS_ALERT=%u", aw->level);
	snprintf(buf2, sizeof(buf2), "CXL_COMPRESS_USED=%llu", aw->phys_used);
	envp[0] = buf1;
	envp[1] = buf2;
	envp[2] = NULL;
	kobject_uevent_env(&cxlmd->dev.kobj, KOBJ_CHANGE, envp);

	put_device(&cxlmd->dev);
	kfree(aw);
}

void cxl_compress_handle_event(struct cxl_memdev *cxlmd, const uuid_t *uuid,
			       union cxl_event *evt)
{
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);
	struct cxl_compress_alert_work *aw;
	u8 level;
	u64 phys_used, phys_total;

	if (!mds || !mds->compress.enabled)
		return;
	if (!uuid_equal(uuid, &CXL_EVENT_COMPRESS_WM_UUID))
		return;

	level = evt->generic.data[0];
	phys_used = get_unaligned_le64(&evt->generic.data[8]);
	phys_total = get_unaligned_le64(&evt->generic.data[16]);

	scoped_guard(mutex, &mds->compress.lock) {
		mds->compress.phys_used = phys_used;
		if (phys_total)
			mds->compress.phys_total = phys_total;
	}

	/* Never Zero/migrate here — only queue. */
	if (!cxl_compress_wq)
		return;

	aw = kzalloc(sizeof(*aw), GFP_ATOMIC);
	if (!aw)
		return;
	INIT_WORK(&aw->work, cxl_compress_alert_workfn);
	aw->cxlmd = cxlmd;
	aw->level = level;
	aw->phys_used = phys_used;
	aw->phys_total = phys_total;
	get_device(&cxlmd->dev);
	if (!queue_work(cxl_compress_wq, &aw->work)) {
		put_device(&cxlmd->dev);
		kfree(aw);
	}
}
EXPORT_SYMBOL_NS_GPL(cxl_compress_handle_event, "CXL");

bool cxl_region_compress_skip_kmem(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_memdev *cxlmd;
	struct cxl_memdev_state *mds;
	u64 dpa_start;

	if (full_online)
		return false;

	if (!p->nr_targets)
		return false;

	/*
	 * Hybrid layout (full_online=N): a region whose DPA starts at or
	 * beyond the guaranteed 1:1 segment skips dax_kmem.
	 */
	cxled = p->targets[0];
	if (!cxled || !cxled->dpa_res)
		return false;

	cxlmd = cxled_to_memdev(cxled);
	mds = to_cxl_memdev_state(cxlmd->cxlds);
	if (!mds || !mds->compress.enabled)
		return false;

	dpa_start = cxled->dpa_res->start;
	return dpa_start >= mds->compress.guaranteed_size;
}
EXPORT_SYMBOL_NS_GPL(cxl_region_compress_skip_kmem, "CXL");

#define CXL_COMPRESS_RO_ATTR(_name)					\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);			\
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);\
	u64 val;							\
									\
	if (!mds || !mds->compress.enabled)				\
		return -ENODEV;						\
	cxl_compress_refresh(mds);					\
	scoped_guard(mutex, &mds->compress.lock)			\
		val = mds->compress._name;				\
	return sysfs_emit(buf, "%#llx\n", val);				\
}									\
static DEVICE_ATTR_RO(_name)

CXL_COMPRESS_RO_ATTR(phys_total);
CXL_COMPRESS_RO_ATTR(phys_used);
CXL_COMPRESS_RO_ATTR(guaranteed_size);
CXL_COMPRESS_RO_ATTR(logical_size);
CXL_COMPRESS_RO_ATTR(allocated_pages);
CXL_COMPRESS_RO_ATTR(poison_pages);
CXL_COMPRESS_RO_ATTR(free_slots);

static ssize_t low_wm_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);
	u64 val;

	if (!mds || !mds->compress.enabled)
		return -ENODEV;
	scoped_guard(mutex, &mds->compress.lock)
		val = mds->compress.low_wm;
	return sysfs_emit(buf, "%#llx\n", val);
}

static ssize_t low_wm_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);
	u64 low_wm, high_wm;
	int rc;

	if (!mds || !mds->compress.enabled)
		return -ENODEV;
	rc = kstrtoull(buf, 0, &low_wm);
	if (rc)
		return rc;
	scoped_guard(mutex, &mds->compress.lock)
		high_wm = mds->compress.high_wm;
	rc = cxl_compress_set_watermarks(mds, low_wm, high_wm);
	return rc ? rc : len;
}
static DEVICE_ATTR_RW(low_wm);

static ssize_t high_wm_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);
	u64 val;

	if (!mds || !mds->compress.enabled)
		return -ENODEV;
	scoped_guard(mutex, &mds->compress.lock)
		val = mds->compress.high_wm;
	return sysfs_emit(buf, "%#llx\n", val);
}

static ssize_t high_wm_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t len)
{
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);
	u64 low_wm, high_wm;
	int rc;

	if (!mds || !mds->compress.enabled)
		return -ENODEV;
	rc = kstrtoull(buf, 0, &high_wm);
	if (rc)
		return rc;
	scoped_guard(mutex, &mds->compress.lock)
		low_wm = mds->compress.low_wm;
	rc = cxl_compress_set_watermarks(mds, low_wm, high_wm);
	return rc ? rc : len;
}
static DEVICE_ATTR_RW(high_wm);

static ssize_t track_range_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t len)
{
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	u64 hpa, length;
	int nid, rc;
	char *endp;

	/* "hpa length nid" — base-0 so region resource hex (0x…) is accepted */
	hpa = simple_strtoull(buf, &endp, 0);
	if (endp == buf)
		return -EINVAL;
	while (*endp == ' ' || *endp == '\t')
		endp++;
	length = simple_strtoull(endp, &endp, 0);
	if (*endp == '\0')
		return -EINVAL;
	while (*endp == ' ' || *endp == '\t')
		endp++;
	nid = simple_strtol(endp, &endp, 0);
	rc = cxl_compress_register_hpa_range(cxlmd, hpa, length, nid);
	return rc ? rc : len;
}
static DEVICE_ATTR_WO(track_range);

static ssize_t sync_free_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	unsigned int one;
	int rc;

	rc = kstrtouint(buf, 0, &one);
	if (rc)
		return rc;
	if (!one)
		return len;
	rc = cxl_compress_sync_free_pages(cxlmd);
	return rc ? rc : len;
}
static DEVICE_ATTR_WO(sync_free);

static struct attribute *cxl_memdev_compress_attributes[] = {
	&dev_attr_phys_total.attr,
	&dev_attr_phys_used.attr,
	&dev_attr_guaranteed_size.attr,
	&dev_attr_logical_size.attr,
	&dev_attr_low_wm.attr,
	&dev_attr_high_wm.attr,
	&dev_attr_allocated_pages.attr,
	&dev_attr_poison_pages.attr,
	&dev_attr_free_slots.attr,
	&dev_attr_track_range.attr,
	&dev_attr_sync_free.attr,
	NULL,
};

static umode_t cxl_memdev_compress_visible(struct kobject *kobj,
					   struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct cxl_memdev *cxlmd = to_cxl_memdev(dev);
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);

	if (!mds || !mds->compress.enabled)
		return 0;
	return a->mode;
}

struct attribute_group cxl_memdev_compress_attribute_group = {
	.name = "compression",
	.attrs = cxl_memdev_compress_attributes,
	.is_visible = cxl_memdev_compress_visible,
};
EXPORT_SYMBOL_NS_GPL(cxl_memdev_compress_attribute_group, "CXL");

/**
 * cxl_compress_probe() - Probe vendor compression capacity mailbox
 * @mds: memdev state
 *
 * Issues C000h. On success caches telemetry and enables the compression
 * sysfs group. Devices that return unsupported keep compress.enabled=false
 * and retain ordinary Type-3 behaviour.
 */
int cxl_compress_probe(struct cxl_memdev_state *mds)
{
	struct cxl_mailbox *cxl_mbox = &mds->cxlds.cxl_mbox;
	struct cxl_mbox_compress_cap cap;
	struct cxl_mbox_compress_acct acct;
	struct cxl_mbox_cmd mbox_cmd;
	int rc;

	mutex_init(&mds->compress.lock);
	mds->compress.enabled = false;

	mbox_cmd = (struct cxl_mbox_cmd) {
		.opcode = CXL_MBOX_OP_COMPRESS_GET_CAP,
		.size_out = sizeof(cap),
		.payload_out = &cap,
	};
	rc = cxl_internal_send_cmd(cxl_mbox, &mbox_cmd);
	if (rc) {
		dev_dbg(mds->cxlds.dev,
			"compress Get Cap unsupported/failed: %d\n", rc);
		return 0;
	}

	mbox_cmd = (struct cxl_mbox_cmd) {
		.opcode = CXL_MBOX_OP_COMPRESS_GET_ACCT,
		.size_out = sizeof(acct),
		.payload_out = &acct,
	};
	rc = cxl_internal_send_cmd(cxl_mbox, &mbox_cmd);
	if (rc) {
		dev_dbg(mds->cxlds.dev,
			"compress Get Acct failed after Get Cap: %d\n", rc);
		return 0;
	}

	guard(mutex)(&mds->compress.lock);
	cxl_compress_apply_cap(&mds->compress, &cap);
	cxl_compress_apply_acct(&mds->compress, &acct);
	mds->compress.enabled = true;

	if (!cxl_compress_wq) {
		cxl_compress_wq = alloc_workqueue("cxl_compress", WQ_UNBOUND, 0);
		if (!cxl_compress_wq)
			dev_warn(mds->cxlds.dev,
				 "compress alert workqueue alloc failed\n");
	}

	dev_info(mds->cxlds.dev,
		 "transparent compression: logical %#llx physical %#llx guaranteed %#llx full_online=%d\n",
		 mds->compress.logical_size, mds->compress.phys_total,
		 mds->compress.guaranteed_size, full_online);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_compress_probe, "CXL");
