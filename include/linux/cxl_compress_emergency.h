/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_CXL_COMPRESS_EMERGENCY_H
#define _LINUX_CXL_COMPRESS_EMERGENCY_H

#include <linux/types.h>

struct folio;

/*
 * Optional write-fault hook installed by the CXL compress emergency
 * path (drivers/cxl). Returns:
 *   >0  handled (caller should VM_FAULT_RETRY)
 *   <0  fatal (caller should SIGBUS)
 *    0  not applicable; continue normal do_wp_page
 */
typedef int (*cxl_ce_wp_fault_fn)(struct folio *folio);

extern cxl_ce_wp_fault_fn cxl_compress_emergency_wp_fault_hook;

int cxl_compress_wrprotect_folio(struct folio *folio);

#ifdef CONFIG_MIGRATION
int cxl_compress_migrate_folio_to_nid(struct folio *folio, int dest_nid);
#else
static inline int cxl_compress_migrate_folio_to_nid(struct folio *folio,
						    int dest_nid)
{
	return -ENOSYS;
}
#endif

#endif /* _LINUX_CXL_COMPRESS_EMERGENCY_H */
