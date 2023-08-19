// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2017-2018 Broadcom */

/**
 * DOC: Broadcom V3D MMU
 *
 * The V3D 3.x hardware (compared to VC4) now includes an MMU.  It has
 * a single level of page tables for the V3D's 4GB address space to
 * map to AXI bus addresses, thus it could need up to 4MB of
 * physically contiguous memory to store the PTEs.
 *
 * Because the 4MB of contiguous memory for page tables is precious,
 * and switching between them is expensive, we load all BOs into the
 * same 4GB address space.
 *
 * To protect clients from each other, we should use the GMP to
 * quickly mask out (at 128kb granularity) what pages are available to
 * each client.  This is not yet implemented.
 */

#include "v3d_drv.h"
#include "v3d_regs.h"

#define V3D_MMU_PAGE_SHIFT 12

/* Note: All PTEs for the 1MB superpage must be filled with the
 * superpage bit set.
 */
#define V3D_PTE_SUPERPAGE BIT(31)
#define V3D_PTE_WRITEABLE BIT(29)
#define V3D_PTE_VALID BIT(28)

static int v3d_mmu_flush_all(struct v3d_dev *v3d)
{
	int ret;

	/* Make sure that another flush isn't already running when we
	 * start this one.
	 */
	ret = wait_for(!(V3D_READ(V3D_MMU_CTL) &
			 V3D_MMU_CTL_TLB_CLEARING), 100);
	if (ret)
		dev_err(v3d->dev, "TLB clear wait idle pre-wait failed\n");

	V3D_WRITE(V3D_MMU_CTL, V3D_READ(V3D_MMU_CTL) |
		  V3D_MMU_CTL_TLB_CLEAR);

	V3D_WRITE(V3D_MMUC_CONTROL,
		  V3D_MMUC_CONTROL_FLUSH |
		  V3D_MMUC_CONTROL_ENABLE);

	ret = wait_for(!(V3D_READ(V3D_MMU_CTL) &
			 V3D_MMU_CTL_TLB_CLEARING), 100);
	if (ret) {
		dev_err(v3d->dev, "TLB clear wait idle failed\n");
		return ret;
	}

	ret = wait_for(!(V3D_READ(V3D_MMUC_CONTROL) &
			 V3D_MMUC_CONTROL_FLUSHING), 100);
	if (ret)
		dev_err(v3d->dev, "MMUC flush wait idle failed\n");

	return ret;
}

int v3d_mmu_set_page_table(struct v3d_dev *v3d)
{
	V3D_WRITE(V3D_MMU_PT_PA_BASE, v3d->pt_paddr >> V3D_MMU_PAGE_SHIFT);
	V3D_WRITE(V3D_MMU_CTL,
		  V3D_MMU_CTL_ENABLE |
		  V3D_MMU_CTL_PT_INVALID |
		  V3D_MMU_CTL_PT_INVALID_ABORT |
		  V3D_MMU_CTL_WRITE_VIOLATION_ABORT |
		  V3D_MMU_CTL_CAP_EXCEEDED_ABORT);
	V3D_WRITE(V3D_MMU_ILLEGAL_ADDR,
		  (v3d->mmu_scratch_paddr >> V3D_MMU_PAGE_SHIFT) |
		  V3D_MMU_ILLEGAL_ADDR_ENABLE);
	V3D_WRITE(V3D_MMUC_CONTROL, V3D_MMUC_CONTROL_ENABLE);

	return v3d_mmu_flush_all(v3d);
}

void v3d_mmu_insert_ptes(struct v3d_bo *bo)
{
	struct v3d_dev *v3d = to_v3d_dev(bo->base.dev);
	u32 page = bo->node.start;
	u32 page_prot = V3D_PTE_WRITEABLE | V3D_PTE_VALID;
	unsigned int count;
	struct scatterlist *sgl;

	for_each_sg(bo->sgt->sgl, sgl, bo->sgt->nents, count) {
		u32 page_address = sg_dma_address(sgl) >> V3D_MMU_PAGE_SHIFT;
		u32 pte = page_prot | page_address;
		u32 i;

		BUG_ON(page_address + (sg_dma_len(sgl) >> V3D_MMU_PAGE_SHIFT) >=
		       BIT(24));

		for (i = 0; i < sg_dma_len(sgl) >> V3D_MMU_PAGE_SHIFT; i++)
			v3d->pt[page++] = pte + i;
	}

	WARN_ON_ONCE(page - bo->node.start !=
		     bo->base.size >> V3D_MMU_PAGE_SHIFT);

	if (v3d_mmu_flush_all(v3d))
		dev_err(v3d->dev, "MMU flush timeout\n");
}

void v3d_mmu_remove_ptes(struct v3d_bo *bo)
{
	struct v3d_dev *v3d = to_v3d_dev(bo->base.dev);
	u32 npages = bo->base.size >> V3D_MMU_PAGE_SHIFT;
	u32 page;

	for (page = bo->node.start; page < bo->node.start + npages; page++)
		v3d->pt[page] = 0;

	if (v3d_mmu_flush_all(v3d))
		dev_err(v3d->dev, "MMU flush timeout\n");
}
