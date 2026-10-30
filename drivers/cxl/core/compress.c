// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026. Transparent-compression CXL Type-3 vendor mailbox. */
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <cxl/event.h>

#include "cxlmem.h"
#include "cxl.h"
#include "core.h"

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

void cxl_compress_handle_event(struct cxl_memdev *cxlmd, const uuid_t *uuid,
			       union cxl_event *evt)
{
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);
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

	if (level == CXL_COMPRESS_WM_LEVEL_LOW)
		dev_warn(&cxlmd->dev,
			 "compress capacity low watermark: used %#llx / %#llx\n",
			 phys_used, phys_total);
	else if (level == CXL_COMPRESS_WM_LEVEL_HIGH)
		dev_info(&cxlmd->dev,
			 "compress capacity high watermark: used %#llx / %#llx\n",
			 phys_used, phys_total);
	else
		dev_info(&cxlmd->dev,
			 "compress capacity event level=%u used %#llx / %#llx\n",
			 level, phys_used, phys_total);
}
EXPORT_SYMBOL_NS_GPL(cxl_compress_handle_event, "CXL");

bool cxl_region_compress_skip_kmem(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_memdev *cxlmd;
	struct cxl_memdev_state *mds;
	u64 dpa_start;

	if (!p->nr_targets)
		return false;

	/*
	 * Hybrid layout: a region whose DPA starts at or beyond the
	 * guaranteed 1:1 segment is the oversubscribed compression
	 * segment and must not be onlined as System-RAM.
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

	dev_info(mds->cxlds.dev,
		 "transparent compression: logical %#llx physical %#llx guaranteed %#llx\n",
		 mds->compress.logical_size, mds->compress.phys_total,
		 mds->compress.guaranteed_size);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_compress_probe, "CXL");
