.. SPDX-License-Identifier: GPL-2.0

=============================================
Transparent-compression CXL Type 3 (Hybrid)
=============================================

This out-of-tree support pairs with the QEMU CompressCXL Type-3 model.
A device that implements vendor mailbox set ``0xC0`` advertises a logical
capacity larger than its physical backend. Linux consumes that interface
as follows.

Vendor mailbox
==============

=========== ==============================================
Opcode      Meaning
=========== ==============================================
``C000h``   Get Cap: phys/logical sizes and watermarks
``C001h``   Set Watermarks
``C002h``   Get Page Acct
``4402h``   Media Operations Zero (discard reclaim)
=========== ==============================================

Telemetry appears under ``/sys/bus/cxl/devices/memX/compression/``.
Capacity watermark events use UUID
``6f3c9a21-8b44-4e1d-9c55-a1b2c3d4e5f6``.

Hybrid layout
=============

* ``[0, guaranteed_size)`` remains ordinary System-RAM via ``dax_kmem``.
* ``[guaranteed_size, logical_size)`` is kept in device-dax mode (no
  ``IORESOURCE_DAX_KMEM``) so it is never onlined as ``N_MEMORY``.
* Module ``cxl_cswap`` (``CONFIG_CXL_COMPRESS_SWAP``) binds that
  oversubscribed segment as ``/dev/cxlcswap0``. Writes that would exceed
  physical capacity return ``BLK_STS_NOSPC``. Discard issues ``4402h``.

This path does **not** implement Private Memory Nodes or CRAM-style
read-only demotion mappings.

Laboratory recipe
=================

QEMU (CompressCXL P0)::

  -device cxl-type3,...,volatile-memdev=vmem0,id=cxl-vmem0 \
      ,logical-size=512M,guaranteed-size=256M \
      ,low-wm-pct=80,high-wm-pct=60

Guest::

  # Build two regions (userspace cxl create-region), online only the
  # guaranteed segment via dax_kmem, then:
  modprobe cxl_cswap device=mem0
  mkswap /dev/cxlcswap0
  swapon -p 32767 --discard=pages /dev/cxlcswap0
