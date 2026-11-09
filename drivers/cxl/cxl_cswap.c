// SPDX-License-Identifier: GPL-2.0-only
/*
 * CXL transparent-compression swap block device (Hybrid without private nodes).
 *
 * Binds the oversubscribed DPA segment [guaranteed_size, logical_size) of a
 * compress-capable Type-3 memdev as a synchronous block device. Writes are
 * gated by worst-case 4KiB physical accounting via vendor mailbox C000h.
 * Discard issues Media Operations Zero (4402h).
 *
 * Module parameter:
 *   device=<memdev name>   e.g. device=mem0
 */
#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/highmem.h>

#include "cxlmem.h"
#include "cxl.h"

static char *cswap_device;
module_param_named(device, cswap_device, charp, 0444);
MODULE_PARM_DESC(device, "CXL memdev name to bind (e.g. mem0)");

struct cxl_cswap {
	struct cxl_memdev *cxlmd;
	struct cxl_memdev_state *mds;
	struct gendisk *disk;
	void *virt;
	u64 hpa;
	u64 size;
	u64 dpa_base;
	u64 local_used; /* bytes accounted for by this driver */
	struct mutex lock;
};

static struct cxl_cswap *cswap_dev;

struct cswap_find_ctx {
	struct cxl_memdev_state *mds;
	struct cxl_endpoint_decoder *cxled;
};

static int cswap_match_decoder(struct device *dev, void *data)
{
	struct cswap_find_ctx *ctx = data;
	struct cxl_endpoint_decoder *cxled;
	u64 dpa_start;

	if (!is_endpoint_decoder(dev))
		return 0;

	cxled = to_cxl_endpoint_decoder(dev);
	if (!cxled->dpa_res)
		return 0;
	if (!(cxled->cxld.flags & CXL_DECODER_F_ENABLE))
		return 0;

	dpa_start = cxled->dpa_res->start;
	if (dpa_start < ctx->mds->compress.guaranteed_size)
		return 0;

	ctx->cxled = cxled;
	return 1;
}

static void cswap_do_discard(struct cxl_cswap *cs, sector_t sector, unsigned int size)
{
	u64 off = (u64)sector << SECTOR_SHIFT;
	u64 dpa = cs->dpa_base + off;
	int rc;

	rc = cxl_compress_media_zero(cs->mds, dpa, size);
	if (rc)
		return;

	mutex_lock(&cs->lock);
	if (cs->local_used >= size)
		cs->local_used -= size;
	else
		cs->local_used = 0;
	mutex_unlock(&cs->lock);
}

static blk_status_t cswap_rw_bvec(struct cxl_cswap *cs, struct bio *bio,
				  struct bio_vec *bvec, sector_t sector)
{
	u64 off = (u64)sector << SECTOR_SHIFT;
	unsigned int len = bvec->bv_len;
	void *page_addr, *dev_addr;
	int rc;

	if (off + len > cs->size)
		return BLK_STS_IOERR;

	dev_addr = cs->virt + off;

	if (op_is_write(bio_op(bio))) {
		u64 need = ALIGN(len, SZ_4K);

		mutex_lock(&cs->lock);
		rc = cxl_compress_refresh(cs->mds);
		if (rc) {
			mutex_unlock(&cs->lock);
			return BLK_STS_IOERR;
		}
		if (cs->mds->compress.phys_used + cs->local_used + need >
		    cs->mds->compress.phys_total) {
			mutex_unlock(&cs->lock);
			return BLK_STS_NOSPC;
		}
		cs->local_used += need;
		mutex_unlock(&cs->lock);

		page_addr = bvec_kmap_local(bvec);
		memcpy(dev_addr, page_addr, len);
		kunmap_local(page_addr);
	} else {
		page_addr = bvec_kmap_local(bvec);
		memcpy(page_addr, dev_addr, len);
		kunmap_local(page_addr);
	}

	return BLK_STS_OK;
}

static void cswap_submit_bio(struct bio *bio)
{
	struct cxl_cswap *cs = bio->bi_bdev->bd_disk->private_data;
	sector_t sector = bio->bi_iter.bi_sector;
	struct bio_vec bvec;
	struct bvec_iter iter;

	if (unlikely(op_is_discard(bio->bi_opf))) {
		cswap_do_discard(cs, sector, bio->bi_iter.bi_size);
		bio_endio(bio);
		return;
	}

	bio_for_each_segment(bvec, bio, iter) {
		blk_status_t st;

		st = cswap_rw_bvec(cs, bio, &bvec, sector);
		if (st != BLK_STS_OK) {
			bio->bi_status = st;
			break;
		}
		sector += bvec.bv_len >> SECTOR_SHIFT;
	}

	bio_endio(bio);
}

static const struct block_device_operations cswap_fops = {
	.owner		= THIS_MODULE,
	.submit_bio	= cswap_submit_bio,
};

static int __init cxl_cswap_init(void)
{
	struct queue_limits lim = {
		.physical_block_size	= PAGE_SIZE,
		.max_hw_discard_sectors	= UINT_MAX,
		.max_discard_segments	= 1,
		.discard_granularity	= PAGE_SIZE,
		.max_write_zeroes_sectors = UINT_MAX,
		.features		= BLK_FEAT_SYNCHRONOUS,
	};
	struct cxl_cswap *cs;
	struct device *dev;
	struct cxl_memdev *cxlmd;
	struct cxl_memdev_state *mds;
	struct cxl_port *endpoint;
	struct cswap_find_ctx ctx = {};
	struct cxl_endpoint_decoder *cxled;
	struct gendisk *disk;
	u64 size, hpa;
	int rc;

	if (!cswap_device || !cswap_device[0]) {
		pr_info("cxl_cswap: set device=<memdev> to bind\n");
		return 0;
	}

	dev = bus_find_device_by_name(&cxl_bus_type, NULL, cswap_device);
	if (!dev)
		return -ENODEV;
	if (!is_cxl_memdev(dev)) {
		put_device(dev);
		return -EINVAL;
	}

	cxlmd = to_cxl_memdev(dev);
	mds = to_cxl_memdev_state(cxlmd->cxlds);
	if (!mds || !mds->compress.enabled) {
		put_device(dev);
		return -ENODEV;
	}

	rc = cxl_compress_refresh(mds);
	if (rc) {
		put_device(dev);
		return rc;
	}

	endpoint = cxlmd->endpoint;
	if (!endpoint) {
		put_device(dev);
		return -ENODEV;
	}

	ctx.mds = mds;
	rc = device_for_each_child(&endpoint->dev, &ctx, cswap_match_decoder);
	if (!rc || !ctx.cxled) {
		put_device(dev);
		return -ENOENT;
	}
	cxled = ctx.cxled;

	if (cxled->dpa_res->start < mds->compress.guaranteed_size) {
		put_device(dev);
		return -EINVAL;
	}

	hpa = cxled->cxld.hpa_range.start;
	size = range_len(&cxled->cxld.hpa_range);
	if (!size) {
		put_device(dev);
		return -EINVAL;
	}

	cs = kzalloc(sizeof(*cs), GFP_KERNEL);
	if (!cs) {
		put_device(dev);
		return -ENOMEM;
	}
	mutex_init(&cs->lock);
	cs->cxlmd = cxlmd;
	cs->mds = mds;
	cs->hpa = hpa;
	cs->size = size;
	cs->dpa_base = cxled->dpa_res->start;

	cs->virt = memremap(hpa, size, MEMREMAP_WB);
	if (!cs->virt) {
		kfree(cs);
		put_device(dev);
		return -ENOMEM;
	}

	disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(disk)) {
		rc = PTR_ERR(disk);
		goto err_unmap;
	}

	strscpy(disk->disk_name, "cxlcswap0", DISK_NAME_LEN);
	disk->fops = &cswap_fops;
	disk->private_data = cs;
	set_capacity(disk, size >> SECTOR_SHIFT);

	rc = add_disk(disk);
	if (rc)
		goto err_disk;

	cs->disk = disk;
	cswap_dev = cs;
	pr_info("cxl_cswap: %s -> /dev/%s hpa=%#llx size=%#llx dpa=%#llx\n",
		cswap_device, disk->disk_name, hpa, size, cs->dpa_base);
	return 0;

err_disk:
	put_disk(disk);
err_unmap:
	memunmap(cs->virt);
	kfree(cs);
	put_device(dev);
	return rc;
}

static void __exit cxl_cswap_exit(void)
{
	struct cxl_cswap *cs = cswap_dev;

	if (!cs)
		return;

	del_gendisk(cs->disk);
	put_disk(cs->disk);
	memunmap(cs->virt);
	put_device(&cs->cxlmd->dev);
	kfree(cs);
	cswap_dev = NULL;
}

module_init(cxl_cswap_init);
module_exit(cxl_cswap_exit);

MODULE_DESCRIPTION("CXL compress Hybrid swap block device");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("CompressCXL");
MODULE_IMPORT_NS("CXL");
