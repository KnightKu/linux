/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CXL_COMPRESS_ALERT_H__
#define __CXL_COMPRESS_ALERT_H__

#include <linux/types.h>

struct cxl_memdev;
struct cxl_memdev_state;

struct cxl_compress_hpa_range {
	struct list_head list;
	struct cxl_memdev *cxlmd;
	u64 hpa_start;
	u64 length;
	u64 dpa_base; /* DPA corresponding to hpa_start (lab: 0) */
	int nid;
};

#endif
