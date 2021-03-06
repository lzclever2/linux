/*
 * vsp1_dl.h  --  R-Car VSP1 Display List
 *
 * Copyright (C) 2015-2017 Renesas Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>
#include <linux/workqueue.h>

#include <media/rcar-fcp.h>

#include "vsp1.h"
#include "vsp1_dl.h"
#include "vsp1_pipe.h"
#include "vsp1_rwpf.h"

#define VSP1_DL_NUM_ENTRIES		256
#define VSP1_DL_EXT_OFFSET		0x1000

#define VSP1_DLH_INT_ENABLE		(1 << 1)
#define VSP1_DLH_AUTO_START		(1 << 0)

struct vsp1_dl_header_list {
	u32 num_bytes;
	u32 addr;
} __attribute__((__packed__));

struct vsp1_dl_header {
	u32 num_lists;
	struct vsp1_dl_header_list lists[8];
	u32 next_header;
	u32 flags;
	/* if (VI6_DL_EXT_CTRL.EXT) */
	u32 zero_bits;
	/* zero_bits:6 + pre_ext_dl_exec:1 + */
	/* post_ext_dl_exec:1 + zero_bits:8 + pre_ext_dl_num_cmd:16 */
	u32 pre_post_num;
	u32 pre_ext_dl_plist;
	/* zero_bits:16 + post_ext_dl_num_cmd:16 */
	u32 post_ext_dl_num_cmd;
	u32 post_ext_dl_p_list;
} __attribute__((__packed__));

struct vsp1_ext_dl_body {
	u32 ext_dl_cmd[2];
	u32 ext_dl_data[2];
} __attribute__((__packed__));

struct vsp1_ext_addr {
	u32 addr;
} __attribute__((__packed__));

struct vsp1_dl_entry {
	u32 addr;
	u32 data;
} __attribute__((__packed__));

/**
 * struct vsp1_dl_body - Display list body
 * @list: entry in the display list list of bodies
 * @vsp1: the VSP1 device
 * @entries: array of entries
 * @dma: DMA address of the entries
 * @size: size of the DMA memory in bytes
 * @num_entries: number of stored entries
 */
struct vsp1_dl_body {
	struct list_head list;
	struct vsp1_device *vsp1;

	struct vsp1_dl_entry *entries;
	dma_addr_t dma;
	size_t size;

	unsigned int num_entries;
};

/**
 * struct vsp1_dl_list - Display list
 * @list: entry in the display list manager lists
 * @dlm: the display list manager
 * @header: display list header, NULL for headerless lists
 * @dma: DMA address for the header
 * @ext_body: display list extended body
 * @ext_dma: DMA address for extended body
 * @src_dst_addr: display list (Auto-FLD) source/destination address
 * @ext_addr_dma: DMA address for display list (Auto-FLD)
 * @body0: first display list body
 * @fragments: list of extra display list bodies
 * @chain: entry in the display list partition chain
 */
struct vsp1_dl_list {
	struct list_head list;
	struct vsp1_dl_manager *dlm;

	struct vsp1_dl_header *header;
	dma_addr_t dma;

	struct vsp1_ext_dl_body *ext_body;
	dma_addr_t ext_dma;

	struct vsp1_ext_addr *src_dst_addr;
	dma_addr_t ext_addr_dma;

	struct vsp1_dl_body body0;
	struct list_head fragments;

	bool has_chain;
	struct list_head chain;
};

enum vsp1_dl_mode {
	VSP1_DL_MODE_HEADER,
	VSP1_DL_MODE_HEADERLESS,
};

/**
 * struct vsp1_dl_manager - Display List manager
 * @index: index of the related WPF
 * @mode: display list operation mode (header or headerless)
 * @vsp1: the VSP1 device
 * @lock: protects the free, active, queued, pending and gc_fragments lists
 * @free: array of all free display lists
 * @active: list currently being processed (loaded) by hardware
 * @queued: list queued to the hardware (written to the DL registers)
 * @pending: list waiting to be queued to the hardware
 * @gc_work: fragments garbage collector work struct
 * @gc_fragments: array of display list fragments waiting to be freed
 */
struct vsp1_dl_manager {
	unsigned int index;
	enum vsp1_dl_mode mode;
	struct vsp1_device *vsp1;

	spinlock_t lock;
	struct list_head free;
	struct vsp1_dl_list *active;
	struct vsp1_dl_list *queued;
	struct vsp1_dl_list *pending;

	struct work_struct gc_work;
	struct list_head gc_fragments;
};

/* -----------------------------------------------------------------------------
 * Display List Body Management
 */

/*
 * Initialize a display list body object and allocate DMA memory for the body
 * data. The display list body object is expected to have been initialized to
 * 0 when allocated.
 */
static int vsp1_dl_body_init(struct vsp1_device *vsp1,
			     struct vsp1_dl_body *dlb, unsigned int num_entries,
			     size_t extra_size)
{
	size_t size = num_entries * sizeof(*dlb->entries) + extra_size;
	struct device *fcp = rcar_fcp_get_device(vsp1->fcp);

	dlb->vsp1 = vsp1;
	dlb->size = size;

	dlb->entries = dma_alloc_wc(fcp ? fcp : vsp1->dev, dlb->size +
			 (VSP1_DL_EXT_OFFSET * 2), &dlb->dma, GFP_KERNEL);
	if (!dlb->entries)
		return -ENOMEM;

	return 0;
}

/*
 * Cleanup a display list body and free allocated DMA memory allocated.
 */
static void vsp1_dl_body_cleanup(struct vsp1_dl_body *dlb)
{
	struct device *fcp = rcar_fcp_get_device(dlb->vsp1->fcp);

	dma_free_wc(fcp ? fcp : dlb->vsp1->dev, dlb->size +
			 (VSP1_DL_EXT_OFFSET * 2), dlb->entries, dlb->dma);
}

/**
 * vsp1_dl_fragment_alloc - Allocate a display list fragment
 * @vsp1: The VSP1 device
 * @num_entries: The maximum number of entries that the fragment can contain
 *
 * Allocate a display list fragment with enough memory to contain the requested
 * number of entries.
 *
 * Return a pointer to a fragment on success or NULL if memory can't be
 * allocated.
 */
struct vsp1_dl_body *vsp1_dl_fragment_alloc(struct vsp1_device *vsp1,
					    unsigned int num_entries)
{
	struct vsp1_dl_body *dlb;
	int ret;

	dlb = kzalloc(sizeof(*dlb), GFP_KERNEL);
	if (!dlb)
		return NULL;

	ret = vsp1_dl_body_init(vsp1, dlb, num_entries, 0);
	if (ret < 0) {
		kfree(dlb);
		return NULL;
	}

	return dlb;
}

/**
 * vsp1_dl_fragment_free - Free a display list fragment
 * @dlb: The fragment
 *
 * Free the given display list fragment and the associated DMA memory.
 *
 * Fragments must only be freed explicitly if they are not added to a display
 * list, as the display list will take ownership of them and free them
 * otherwise. Manual free typically happens at cleanup time for fragments that
 * have been allocated but not used.
 *
 * Passing a NULL pointer to this function is safe, in that case no operation
 * will be performed.
 */
void vsp1_dl_fragment_free(struct vsp1_dl_body *dlb)
{
	if (!dlb)
		return;

	vsp1_dl_body_cleanup(dlb);
	kfree(dlb);
}

/**
 * vsp1_dl_fragment_write - Write a register to a display list fragment
 * @dlb: The fragment
 * @reg: The register address
 * @data: The register value
 *
 * Write the given register and value to the display list fragment. The maximum
 * number of entries that can be written in a fragment is specified when the
 * fragment is allocated by vsp1_dl_fragment_alloc().
 */
void vsp1_dl_fragment_write(struct vsp1_dl_body *dlb, u32 reg, u32 data)
{
	dlb->entries[dlb->num_entries].addr = reg;
	dlb->entries[dlb->num_entries].data = data;
	dlb->num_entries++;
}

/* -----------------------------------------------------------------------------
 * Display List Transaction Management
 */

void vsp1_dl_set_addr_auto_fld(struct vsp1_dl_list *dl, struct vsp1_rwpf *rpf)
{
	const struct vsp1_format_info *fmtinfo = rpf->fmtinfo;
	const struct v4l2_rect *crop;
	u32 y_top_index, y_bot_index;
	u32 u_top_index, u_bot_index;
	u32 v_top_index, v_bot_index;
	dma_addr_t y_top_addr, y_bot_addr;
	dma_addr_t u_top_addr, u_bot_addr;
	dma_addr_t v_top_addr, v_bot_addr;
	u32 width, stride;

	crop = vsp1_rwpf_get_crop(rpf, rpf->entity.config);
	width = ALIGN(crop->width, 16);
	stride = width * fmtinfo->bpp[0] / 8;

	y_top_index = rpf->entity.index * 8;
	y_bot_index = rpf->entity.index * 8 + 1;
	u_top_index = rpf->entity.index * 8 + 2;
	u_bot_index = rpf->entity.index * 8 + 3;
	v_top_index = rpf->entity.index * 8 + 4;
	v_bot_index = rpf->entity.index * 8 + 5;

	switch (rpf->fmtinfo->fourcc) {
	case V4L2_PIX_FMT_YUV420M:
		y_top_addr = rpf->mem.addr[0] + rpf->offsets[0];
		y_bot_addr = rpf->mem.addr[0] + rpf->offsets[0] + stride;
		u_top_addr = rpf->mem.addr[1] + rpf->offsets[1];
		u_bot_addr = rpf->mem.addr[1] + rpf->offsets[1] + stride / 2;
		v_top_addr = rpf->mem.addr[2] + rpf->offsets[1];
		v_bot_addr = rpf->mem.addr[2] + rpf->offsets[1] + stride / 2;
		break;

	case V4L2_PIX_FMT_YUV422M:
		y_top_addr = rpf->mem.addr[0] + rpf->offsets[0];
		y_bot_addr = rpf->mem.addr[0] + rpf->offsets[0] + stride * 2;
		u_top_addr = rpf->mem.addr[1] + rpf->offsets[1];
		u_bot_addr = rpf->mem.addr[1] + rpf->offsets[1] + stride;
		v_top_addr = rpf->mem.addr[2] + rpf->offsets[1];
		v_bot_addr = rpf->mem.addr[2] + rpf->offsets[1] + stride;
		break;

	case V4L2_PIX_FMT_YUV444M:
		y_top_addr = rpf->mem.addr[0] + rpf->offsets[0];
		y_bot_addr = rpf->mem.addr[0] + rpf->offsets[0] + stride * 3;
		u_top_addr = rpf->mem.addr[1] + rpf->offsets[1];
		u_bot_addr = rpf->mem.addr[1] + rpf->offsets[1] + stride * 3;
		v_top_addr = rpf->mem.addr[2] + rpf->offsets[1];
		v_bot_addr = rpf->mem.addr[2] + rpf->offsets[1] + stride * 3;
		break;

	case V4L2_PIX_FMT_YVU420M:
		y_top_addr = rpf->mem.addr[0] + rpf->offsets[0];
		y_bot_addr = rpf->mem.addr[0] + rpf->offsets[0] + stride;
		u_top_addr = rpf->mem.addr[2] + rpf->offsets[1];
		u_bot_addr = rpf->mem.addr[2] + rpf->offsets[1] + stride / 2;
		v_top_addr = rpf->mem.addr[1] + rpf->offsets[1];
		v_bot_addr = rpf->mem.addr[1] + rpf->offsets[1] + stride / 2;
		break;

	case V4L2_PIX_FMT_YVU422M:
		y_top_addr = rpf->mem.addr[0] + rpf->offsets[0];
		y_bot_addr = rpf->mem.addr[0] + rpf->offsets[0] + stride * 2;
		u_top_addr = rpf->mem.addr[2] + rpf->offsets[1];
		u_bot_addr = rpf->mem.addr[2] + rpf->offsets[1] + stride;
		v_top_addr = rpf->mem.addr[1] + rpf->offsets[1];
		v_bot_addr = rpf->mem.addr[1] + rpf->offsets[1] + stride;
		break;

	case V4L2_PIX_FMT_YVU444M:
		y_top_addr = rpf->mem.addr[0] + rpf->offsets[0];
		y_bot_addr = rpf->mem.addr[0] + rpf->offsets[0] + stride * 3;
		u_top_addr = rpf->mem.addr[2] + rpf->offsets[1];
		u_bot_addr = rpf->mem.addr[2] + rpf->offsets[1] + stride * 3;
		v_top_addr = rpf->mem.addr[1] + rpf->offsets[1];
		v_bot_addr = rpf->mem.addr[1] + rpf->offsets[1] + stride * 3;
		break;

	default:
		y_top_addr = rpf->mem.addr[0] + rpf->offsets[0];
		y_bot_addr = rpf->mem.addr[0] + rpf->offsets[0] + stride;
		u_top_addr = rpf->mem.addr[1] + rpf->offsets[1];
		u_bot_addr = rpf->mem.addr[1] + rpf->offsets[1] + stride;
		v_top_addr = rpf->mem.addr[2] + rpf->offsets[1];
		v_bot_addr = rpf->mem.addr[2] + rpf->offsets[1] + stride;
		break;
	}

	dl->src_dst_addr[y_top_index].addr = y_top_addr;
	dl->src_dst_addr[y_bot_index].addr = y_bot_addr;
	dl->src_dst_addr[u_top_index].addr = u_top_addr;
	dl->src_dst_addr[u_bot_index].addr = u_bot_addr;
	dl->src_dst_addr[v_top_index].addr = v_top_addr;
	dl->src_dst_addr[v_bot_index].addr = v_bot_addr;
}

static struct vsp1_dl_list *vsp1_dl_list_alloc(struct vsp1_dl_manager *dlm)
{
	struct vsp1_dl_list *dl;
	size_t header_size;
	int ret;

	dl = kzalloc(sizeof(*dl), GFP_KERNEL);
	if (!dl)
		return NULL;

	INIT_LIST_HEAD(&dl->fragments);
	dl->dlm = dlm;

	/* Initialize the display list body and allocate DMA memory for the body
	 * and the optional header. Both are allocated together to avoid memory
	 * fragmentation, with the header located right after the body in
	 * memory.
	 */
	header_size = dlm->mode == VSP1_DL_MODE_HEADER
		    ? ALIGN(sizeof(struct vsp1_dl_header), 8)
		    : 0;

	ret = vsp1_dl_body_init(dlm->vsp1, &dl->body0, VSP1_DL_NUM_ENTRIES,
				header_size);
	if (ret < 0) {
		kfree(dl);
		return NULL;
	}

	if (dlm->mode == VSP1_DL_MODE_HEADER) {
		size_t header_offset = VSP1_DL_NUM_ENTRIES
				     * sizeof(*dl->body0.entries);

		dl->header = ((void *)dl->body0.entries) + header_offset;
		dl->dma = dl->body0.dma + header_offset;

		dl->ext_body = ((void *)dl->body0.entries) +
				 header_offset + VSP1_DL_EXT_OFFSET;
		dl->ext_dma = dl->body0.dma + header_offset +
				 VSP1_DL_EXT_OFFSET;

		dl->src_dst_addr = ((void *)dl->body0.entries) +
				 header_offset + (VSP1_DL_EXT_OFFSET * 2);
		dl->ext_addr_dma = dl->body0.dma + header_offset +
				 (VSP1_DL_EXT_OFFSET * 2);

		memset(dl->header, 0, sizeof(*dl->header));
		dl->header->lists[0].addr = dl->body0.dma;
	}

	return dl;
}

static void vsp1_dl_list_free(struct vsp1_dl_list *dl)
{
	vsp1_dl_body_cleanup(&dl->body0);
	list_splice_init(&dl->fragments, &dl->dlm->gc_fragments);
	kfree(dl);
}

/**
 * vsp1_dl_list_get - Get a free display list
 * @dlm: The display list manager
 *
 * Get a display list from the pool of free lists and return it.
 *
 * This function must be called without the display list manager lock held.
 */
struct vsp1_dl_list *vsp1_dl_list_get(struct vsp1_dl_manager *dlm)
{
	struct vsp1_dl_list *dl = NULL;
	unsigned long flags;

	spin_lock_irqsave(&dlm->lock, flags);

	if (!list_empty(&dlm->free)) {
		dl = list_first_entry(&dlm->free, struct vsp1_dl_list, list);
		list_del(&dl->list);

		/*
		 * The display list chain must be initialised to ensure every
		 * display list can assert list_empty() if it is not in a chain.
		 */
		INIT_LIST_HEAD(&dl->chain);
	}

	spin_unlock_irqrestore(&dlm->lock, flags);

	return dl;
}

/* This function must be called with the display list manager lock held.*/
static void __vsp1_dl_list_put(struct vsp1_dl_list *dl)
{
	struct vsp1_dl_list *dl_child;

	if (!dl)
		return;

	/*
	 * Release any linked display-lists which were chained for a single
	 * hardware operation.
	 */
	if (dl->has_chain) {
		list_for_each_entry(dl_child, &dl->chain, chain)
			__vsp1_dl_list_put(dl_child);
	}

	dl->has_chain = false;

	/*
	 * We can't free fragments here as DMA memory can only be freed in
	 * interruptible context. Move all fragments to the display list
	 * manager's list of fragments to be freed, they will be
	 * garbage-collected by the work queue.
	 */
	if (!list_empty(&dl->fragments)) {
		list_splice_init(&dl->fragments, &dl->dlm->gc_fragments);
		schedule_work(&dl->dlm->gc_work);
	}

	dl->body0.num_entries = 0;

	list_add_tail(&dl->list, &dl->dlm->free);
}

/**
 * vsp1_dl_list_put - Release a display list
 * @dl: The display list
 *
 * Release the display list and return it to the pool of free lists.
 *
 * Passing a NULL pointer to this function is safe, in that case no operation
 * will be performed.
 */
void vsp1_dl_list_put(struct vsp1_dl_list *dl)
{
	unsigned long flags;

	if (!dl)
		return;

	spin_lock_irqsave(&dl->dlm->lock, flags);
	__vsp1_dl_list_put(dl);
	spin_unlock_irqrestore(&dl->dlm->lock, flags);
}

/**
 * vsp1_dl_list_write - Write a register to the display list
 * @dl: The display list
 * @reg: The register address
 * @data: The register value
 *
 * Write the given register and value to the display list. Up to 256 registers
 * can be written per display list.
 */
void vsp1_dl_list_write(struct vsp1_dl_list *dl, u32 reg, u32 data)
{
	vsp1_dl_fragment_write(&dl->body0, reg, data);
}

/**
 * vsp1_dl_list_add_fragment - Add a fragment to the display list
 * @dl: The display list
 * @dlb: The fragment
 *
 * Add a display list body as a fragment to a display list. Registers contained
 * in fragments are processed after registers contained in the main display
 * list, in the order in which fragments are added.
 *
 * Adding a fragment to a display list passes ownership of the fragment to the
 * list. The caller must not touch the fragment after this call, and must not
 * free it explicitly with vsp1_dl_fragment_free().
 *
 * Fragments are only usable for display lists in header mode. Attempt to
 * add a fragment to a header-less display list will return an error.
 */
int vsp1_dl_list_add_fragment(struct vsp1_dl_list *dl,
			      struct vsp1_dl_body *dlb)
{
	/* Multi-body lists are only available in header mode. */
	if (dl->dlm->mode != VSP1_DL_MODE_HEADER)
		return -EINVAL;

	list_add_tail(&dlb->list, &dl->fragments);
	return 0;
}

/**
 * vsp1_dl_list_add_chain - Add a display list to a chain
 * @head: The head display list
 * @dl: The new display list
 *
 * Add a display list to an existing display list chain. The chained lists
 * will be automatically processed by the hardware without intervention from
 * the CPU. A display list end interrupt will only complete after the last
 * display list in the chain has completed processing.
 *
 * Adding a display list to a chain passes ownership of the display list to
 * the head display list item. The chain is released when the head dl item is
 * put back with __vsp1_dl_list_put().
 *
 * Chained display lists are only usable in header mode. Attempts to add a
 * display list to a chain in header-less mode will return an error.
 */
int vsp1_dl_list_add_chain(struct vsp1_dl_list *head,
			   struct vsp1_dl_list *dl)
{
	/* Chained lists are only available in header mode. */
	if (head->dlm->mode != VSP1_DL_MODE_HEADER)
		return -EINVAL;

	head->has_chain = true;
	list_add_tail(&dl->chain, &head->chain);
	return 0;
}

static void vsp1_dl_list_fill_header(struct vsp1_dl_list *dl, bool is_last,
				     unsigned int lif_index)
{
	struct vsp1_dl_header_list *hdr = dl->header->lists;
	struct vsp1_dl_body *dlb;
	struct vsp1_device *vsp1 = dl->dlm->vsp1;
	unsigned int num_lists = 0;
	unsigned int init_bru_num, end_bru_num;
	unsigned int init_brs_num, end_brs_num;
	unsigned int i, rpf_update = 0;

	/*
	 * Fill the header with the display list bodies addresses and sizes. The
	 * address of the first body has already been filled when the display
	 * list was allocated.
	 */

	hdr->num_bytes = dl->body0.num_entries
		       * sizeof(*dl->header->lists);

	list_for_each_entry(dlb, &dl->fragments, list) {
		num_lists++;
		hdr++;

		hdr->addr = dlb->dma;
		hdr->num_bytes = dlb->num_entries
			       * sizeof(*dl->header->lists);
	}

	dl->header->num_lists = num_lists;

	if (vsp1_gen3_vspdl_check(vsp1)) {
		if (!vsp1->brs || !vsp1->lif[1])
			return;

		init_bru_num = 0;
		init_brs_num = vsp1->info->rpf_count - vsp1->num_brs_inputs;
		end_bru_num = vsp1->info->rpf_count - vsp1->num_brs_inputs;
		end_brs_num = vsp1->info->rpf_count;
	} else {
		init_bru_num = 0;
		init_brs_num = 0;
		end_bru_num = vsp1->info->rpf_count;
		end_brs_num = 0;
	}

	if (lif_index == 1) {
		for (i = init_brs_num; i < end_brs_num; ++i)
			rpf_update |= (0x01 << (16 + i));
	} else {
		for (i = init_bru_num; i < end_bru_num; ++i)
			rpf_update |= (0x01 << (16 + i));
	}
	/*
	 * If this display list's chain is not empty, we are on a list, where
	 * the next item in the list is the display list entity which should be
	 * automatically queued by the hardware.
	 */
	if (!list_empty(&dl->chain) && !is_last) {
		struct vsp1_dl_list *next = list_next_entry(dl, chain);

		dl->header->next_header = next->dma;
		dl->header->flags = VSP1_DLH_AUTO_START;
	} else {
		dl->header->flags = VSP1_DLH_INT_ENABLE;
		if (dl->dlm->vsp1->info->header_mode) {
			dl->header->next_header = dl->dma;
			dl->header->flags |= VSP1_DLH_AUTO_START;
		}

		if (!(dl->dlm->vsp1->ths_quirks & VSP1_AUTO_FLD_NOT_SUPPORT)) {
			/* Set extended display list header */
			/* pre_ext_dl_exec = 1, pre_ext_dl_num_cmd = 1 */
			dl->header->pre_post_num = (1 << 25) | (0x01);
			dl->header->pre_ext_dl_plist = dl->ext_dma;
			dl->header->post_ext_dl_num_cmd = 0;
			dl->header->post_ext_dl_p_list = 0;

			/* Set extended display list (Auto-FLD) */
			/* Set opecode */
			dl->ext_body->ext_dl_cmd[0] = 0x00000003;
			/* RPF[0]-[4] address is updated */
			dl->ext_body->ext_dl_cmd[1] = 0x00000001 | rpf_update;

			/* Set pointer of source/destination address */
			dl->ext_body->ext_dl_data[0] = dl->ext_addr_dma;
			/* Should be set to 0 */
			dl->ext_body->ext_dl_data[1] = 0;
		}
	}
}

void vsp1_dl_list_commit(struct vsp1_dl_list *dl, unsigned int lif_index)
{
	struct vsp1_dl_manager *dlm = dl->dlm;
	struct vsp1_device *vsp1 = dlm->vsp1;
	unsigned long flags;
	bool update;

	spin_lock_irqsave(&dlm->lock, flags);

	if (dl->dlm->mode == VSP1_DL_MODE_HEADER) {
		struct vsp1_dl_list *dl_child;

		/*
		 * In header mode the caller guarantees that the hardware is
		 * idle at this point.
		 */

		/* Fill the header for the head and chained display lists. */
		vsp1_dl_list_fill_header(dl, list_empty(&dl->chain),
							 lif_index);

		list_for_each_entry(dl_child, &dl->chain, chain) {
			bool last = list_is_last(&dl_child->chain, &dl->chain);

			vsp1_dl_list_fill_header(dl_child, last, lif_index);
		}

		/*
		 * Commit the head display list to hardware. Chained headers
		 * will auto-start.
		 */
		vsp1_write(vsp1, VI6_DL_HDR_ADDR(dlm->index), dl->dma);

		if (vsp1->ths_quirks & VSP1_UNDERRUN_WORKAROUND)
			vsp1->dl_addr = dl->dma;

		dlm->active = dl;
		dlm->queued = dl;
		__vsp1_dl_list_put(dlm->queued);

		goto done;
	}

	/* Once the UPD bit has been set the hardware can start processing the
	 * display list at any time and we can't touch the address and size
	 * registers. In that case mark the update as pending, it will be
	 * queued up to the hardware by the frame end interrupt handler.
	 */
	update = !!(vsp1_read(vsp1, VI6_DL_BODY_SIZE) & VI6_DL_BODY_SIZE_UPD);
	if (update) {
		__vsp1_dl_list_put(dlm->pending);
		dlm->pending = dl;
		goto done;
	}

	/* Program the hardware with the display list body address and size.
	 * The UPD bit will be cleared by the device when the display list is
	 * processed.
	 */
	vsp1_write(vsp1, VI6_DL_HDR_ADDR(0), dl->body0.dma);
	vsp1_write(vsp1, VI6_DL_BODY_SIZE, VI6_DL_BODY_SIZE_UPD |
		   (dl->body0.num_entries * sizeof(*dl->header->lists)));

	if (vsp1->ths_quirks & VSP1_UNDERRUN_WORKAROUND) {
		vsp1->dl_addr = dl->body0.dma;
		vsp1->dl_body = VI6_DL_BODY_SIZE_UPD |
			(dl->body0.num_entries * sizeof(*dl->header->lists));
	}

	__vsp1_dl_list_put(dlm->queued);
	dlm->queued = dl;

done:
	spin_unlock_irqrestore(&dlm->lock, flags);
}

/* -----------------------------------------------------------------------------
 * Display List Manager
 */

/* Interrupt Handling */
void vsp1_dlm_irq_display_start(struct vsp1_dl_manager *dlm)
{
	spin_lock(&dlm->lock);

	/* The display start interrupt signals the end of the display list
	 * processing by the device. The active display list, if any, won't be
	 * accessed anymore and can be reused.
	 */
	if (dlm->mode != VSP1_DL_MODE_HEADER)
		__vsp1_dl_list_put(dlm->active);
	dlm->active = NULL;

	spin_unlock(&dlm->lock);
}

/**
 * vsp1_dlm_irq_frame_end - Display list handler for the frame end interrupt
 * @dlm: the display list manager
 *
 * Return true if the previous display list has completed at frame end, or false
 * if it has been delayed by one frame because the display list commit raced
 * with the frame end interrupt. The function always returns true in header mode
 * as display list processing is then not continuous and races never occur.
 */
bool vsp1_dlm_irq_frame_end(struct vsp1_dl_manager *dlm, bool interlaced)
{
	struct vsp1_device *vsp1 = dlm->vsp1;
	bool completed = false;

	spin_lock(&dlm->lock);

	if (dlm->mode != VSP1_DL_MODE_HEADER)
		__vsp1_dl_list_put(dlm->active);
	dlm->active = NULL;

	/* Header mode is used for mem-to-mem pipelines only. We don't need to
	 * perform any operation as there can't be any new display list queued
	 * in that case.
	 */
	if (dlm->mode == VSP1_DL_MODE_HEADER) {
		/* The UPDHDR bit set indicates that the commit operation
		 * raced with the interrupt and occurred after the frame end
		 * event and UPD clear but before interrupt processing.
		 * The hardware hasn't taken the update into account yet,
		 * we'll thus skip one frame and retry.
		 */
		if ((vsp1_read(vsp1, VI6_CMD(dlm->index)) & VI6_CMD_UPDHDR))
			goto done;

		if (interlaced && ((vsp1_read(vsp1, VI6_STATUS) &
			VI6_STATUS_FLD_STD(dlm->index)) !=
			VI6_STATUS_FLD_STD(dlm->index)))
			goto done;

		if (dlm->queued) {
			dlm->active = dlm->queued;
			dlm->queued = NULL;
			completed = true;
		}
		goto done;
	}

	/* The UPD bit set indicates that the commit operation raced with the
	 * interrupt and occurred after the frame end event and UPD clear but
	 * before interrupt processing. The hardware hasn't taken the update
	 * into account yet, we'll thus skip one frame and retry.
	 */
	if (vsp1_read(vsp1, VI6_DL_BODY_SIZE) & VI6_DL_BODY_SIZE_UPD)
		goto done;

	/* The device starts processing the queued display list right after the
	 * frame end interrupt. The display list thus becomes active.
	 */
	if (dlm->queued) {
		dlm->active = dlm->queued;
		dlm->queued = NULL;
		completed = true;
	}

	/* Now that the UPD bit has been cleared we can queue the next display
	 * list to the hardware if one has been prepared.
	 */
	if (dlm->pending) {
		struct vsp1_dl_list *dl = dlm->pending;

		vsp1_write(vsp1, VI6_DL_HDR_ADDR(0), dl->body0.dma);
		vsp1_write(vsp1, VI6_DL_BODY_SIZE, VI6_DL_BODY_SIZE_UPD |
			   (dl->body0.num_entries *
			    sizeof(*dl->header->lists)));

		if (vsp1->ths_quirks & VSP1_UNDERRUN_WORKAROUND) {
			vsp1->dl_addr = dl->body0.dma;
			vsp1->dl_body = VI6_DL_BODY_SIZE_UPD |
				(dl->body0.num_entries *
				 sizeof(*dl->header->lists));
		}
		dlm->queued = dl;
		dlm->pending = NULL;
	}

done:
	spin_unlock(&dlm->lock);

	return completed;
}

/* Hardware Setup */
void vsp1_dlm_setup(struct vsp1_device *vsp1, unsigned int lif_index)
{
	u32 ctrl = (256 << VI6_DL_CTRL_AR_WAIT_SHIFT)
		 | VI6_DL_CTRL_DC2 | VI6_DL_CTRL_DC1 | VI6_DL_CTRL_DC0
		 | VI6_DL_CTRL_DLE;

	if ((vsp1->info->header_mode) &&
		(!(vsp1->ths_quirks & VSP1_AUTO_FLD_NOT_SUPPORT))) {
		vsp1_write(vsp1, VI6_DL_EXT_CTRL(lif_index),
			(0x02 << VI6_DL_EXT_CTRL_POLINT_SHIFT)
			| VI6_DL_EXT_CTRL_DLPRI | VI6_DL_EXT_CTRL_EXT);
	}

	/* The DRM pipeline operates with display lists in Continuous Frame
	 * Mode, all other pipelines use manual start.
	 */
	if ((vsp1->drm) && (!vsp1->info->header_mode))
		ctrl |= VI6_DL_CTRL_CFM0 | VI6_DL_CTRL_NH0;

	vsp1_write(vsp1, VI6_DL_CTRL, ctrl);
	vsp1_write(vsp1, VI6_DL_SWAP(lif_index), VI6_DL_SWAP_LWS |
				    (lif_index == 1 ? VI6_DL_SWAP_IND : 0));
}

void vsp1_dlm_reset(struct vsp1_dl_manager *dlm)
{
	unsigned long flags;

	spin_lock_irqsave(&dlm->lock, flags);

	__vsp1_dl_list_put(dlm->active);
	__vsp1_dl_list_put(dlm->queued);
	__vsp1_dl_list_put(dlm->pending);

	spin_unlock_irqrestore(&dlm->lock, flags);

	dlm->active = NULL;
	dlm->queued = NULL;
	dlm->pending = NULL;
}

/*
 * Free all fragments awaiting to be garbage-collected.
 *
 * This function must be called without the display list manager lock held.
 */
static void vsp1_dlm_fragments_free(struct vsp1_dl_manager *dlm)
{
	unsigned long flags;

	spin_lock_irqsave(&dlm->lock, flags);

	while (!list_empty(&dlm->gc_fragments)) {
		struct vsp1_dl_body *dlb;

		dlb = list_first_entry(&dlm->gc_fragments, struct vsp1_dl_body,
				       list);
		list_del(&dlb->list);

		spin_unlock_irqrestore(&dlm->lock, flags);
		vsp1_dl_fragment_free(dlb);
		spin_lock_irqsave(&dlm->lock, flags);
	}

	spin_unlock_irqrestore(&dlm->lock, flags);
}

static void vsp1_dlm_garbage_collect(struct work_struct *work)
{
	struct vsp1_dl_manager *dlm =
		container_of(work, struct vsp1_dl_manager, gc_work);

	vsp1_dlm_fragments_free(dlm);
}

struct vsp1_dl_manager *vsp1_dlm_create(struct vsp1_device *vsp1,
					unsigned int index,
					unsigned int prealloc)
{
	struct vsp1_dl_manager *dlm;
	unsigned int i;

	dlm = devm_kzalloc(vsp1->dev, sizeof(*dlm), GFP_KERNEL);
	if (!dlm)
		return NULL;

	dlm->index = index;
	dlm->mode = index == 0 && !vsp1->info->uapi && !vsp1->info->header_mode
		  ? VSP1_DL_MODE_HEADERLESS : VSP1_DL_MODE_HEADER;
	dlm->vsp1 = vsp1;

	spin_lock_init(&dlm->lock);
	INIT_LIST_HEAD(&dlm->free);
	INIT_LIST_HEAD(&dlm->gc_fragments);
	INIT_WORK(&dlm->gc_work, vsp1_dlm_garbage_collect);

	for (i = 0; i < prealloc; ++i) {
		struct vsp1_dl_list *dl;

		dl = vsp1_dl_list_alloc(dlm);
		if (!dl)
			return NULL;

		list_add_tail(&dl->list, &dlm->free);
	}

	return dlm;
}

void vsp1_dlm_destroy(struct vsp1_dl_manager *dlm)
{
	struct vsp1_dl_list *dl, *next;
	struct vsp1_device *vsp1 = dlm->vsp1;

	if (!dlm)
		return;

	cancel_work_sync(&dlm->gc_work);

	if (vsp1->info->header_mode)
		return;

	list_for_each_entry_safe(dl, next, &dlm->free, list) {
		list_del(&dl->list);
		vsp1_dl_list_free(dl);
	}

	vsp1_dlm_fragments_free(dlm);
}
