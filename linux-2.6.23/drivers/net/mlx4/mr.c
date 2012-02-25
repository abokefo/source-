/*
 * Copyright (c) 2004 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2006, 2007 Cisco Systems, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/init.h>
#include <linux/errno.h>

#include <linux/mlx4/cmd.h>

#include "mlx4.h"
#include "icm.h"

/*
 * Must be packed because mtt_seg is 64 bits but only aligned to 32 bits.
 */
struct mlx4_mpt_entry {
	__be32 flags;
	__be32 qpn;
	__be32 key;
	__be32 pd;
	__be64 start;
	__be64 length;
	__be32 lkey;
	__be32 win_cnt;
	u8	reserved1[3];
	u8	mtt_rep;
	__be64 mtt_seg;
	__be32 mtt_sz;
	__be32 entity_size;
	__be32 first_byte_offset;
} __attribute__((packed));

#define MLX4_MPT_FLAG_SW_OWNS	    (0xfUL << 28)
#define MLX4_MPT_FLAG_MIO	    (1 << 17)
#define MLX4_MPT_FLAG_BIND_ENABLE   (1 << 15)
#define MLX4_MPT_FLAG_PHYSICAL	    (1 <<  9)
#define MLX4_MPT_FLAG_REGION	    (1 <<  8)

#define MLX4_MTT_FLAG_PRESENT		1

static u32 mlx4_buddy_alloc(struct mlx4_buddy *buddy, int order)
{
	int o;
	int m;
	u32 seg;

	spin_lock(&buddy->lock);

	for (o = order; o <= buddy->max_order; ++o) {
		m = 1 << (buddy->max_order - o);
		seg = find_first_bit(buddy->bits[o], m);
		if (seg < m)
			goto found;
	}

	spin_unlock(&buddy->lock);
	return -1;

 found:
	clear_bit(seg, buddy->bits[o]);

	while (o > order) {
		--o;
		seg <<= 1;
		set_bit(seg ^ 1, buddy->bits[o]);
	}

	spin_unlock(&buddy->lock);

	seg <<= order;

	return seg;
}

static void mlx4_buddy_free(struct mlx4_buddy *buddy, u32 seg, int order)
{
	seg >>= order;

	spin_lock(&buddy->lock);

	while (test_bit(seg ^ 1, buddy->bits[order])) {
		clear_bit(seg ^ 1, buddy->bits[order]);
		seg >>= 1;
		++order;
	}

	set_bit(seg, buddy->bits[order]);

	spin_unlock(&buddy->lock);
}

static int __devinit mlx4_buddy_init(struct mlx4_buddy *buddy, int max_order)
{
	int i, s;

	buddy->max_order = max_order;
	spin_lock_init(&buddy->lock);

	buddy->bits = kzalloc((buddy->max_order + 1) * sizeof (long *),
			      GFP_KERNEL);
	if (!buddy->bits)
		goto err_out;

	for (i = 0; i <= buddy->max_order; ++i) {
		s = BITS_TO_LONGS(1 << (buddy->max_order - i));
		buddy->bits[i] = kmalloc(s * sizeof (long), GFP_KERNEL);
		if (!buddy->bits[i])
			goto err_out_free;
		bitmap_zero(buddy->bits[i], 1 << (buddy->max_order - i));
	}

	set_bit(0, buddy->bits[buddy->max_order]);

	return 0;

err_out_free:
	for (i = 0; i <= buddy->max_order; ++i)
		kfree(buddy->bits[i]);

	kfree(buddy->bits);

err_out:
	return -ENOMEM;
}

static void mlx4_buddy_cleanup(struct mlx4_buddy *buddy)
{
	int i;

	for (i = 0; i <= buddy->max_order; ++i)
		kfree(buddy->bits[i]);

	kfree(buddy->bits);
}

static u32 mlx4_alloc_mtt_range(struct mlx4_dev *dev, int order)
{
	struct mlx4_mr_table *mr_table = &mlx4_priv(dev)->mr_table;
	u32 seg;

	seg = mlx4_buddy_alloc(&mr_table->mtt_buddy, order);
	if (seg == -1)
		return -1;

	if (mlx4_table_get_range(dev, &mr_table->mtt_table, seg,
				 seg + (1 << order) - 1)) {
		mlx4_buddy_free(&mr_table->mtt_buddy, seg, order);
		return -1;
	}

	return seg;
}

int mlx4_mtt_init(struct mlx4_dev *dev, int npages, int page_shift,
		  struct mlx4_mtt *mtt)
{
	int i;

	if (!npages) {
		mtt->order      = -1;
		mtt->page_shift = MLX4_ICM_PAGE_SHIFT;
		return 0;
	} else
		mtt->page_shift = page_shift;

	for (mtt->order = 0, i = MLX4_MTT_ENTRY_PER_SEG; i < npages; i <<= 1)
		++mtt->order;

	mtt->first_seg = mlx4_alloc_mtt_range(dev, mtt->order);
	if (mtt->first_seg == -1)
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL_GPL(mlx4_mtt_init);

void mlx4_mtt_cleanup(struct mlx4_dev *dev, struct mlx4_mtt *mtt)
{
	struct mlx4_mr_table *mr_table = &mlx4_priv(dev)->mr_table;

	if (mtt->order < 0)
		return;

	mlx4_buddy_free(&mr_table->mtt_buddy, mtt->first_seg, mtt->order);
	mlx4_table_put_range(dev, &mr_table->mtt_table, mtt->first_seg,
			     mtt->first_seg + (1 << mtt->order) - 1);
}
EXPORT_SYMBOL_GPL(mlx4_mtt_cleanup);

u64 mlx4_mtt_addr(struct mlx4_dev *dev, struct mlx4_mtt *mtt)
{
	return (u64) mtt->first_seg * dev->caps.mtt_entry_sz;
}
EXPORT_SYMBOL_GPL(mlx4_mtt_addr);

static u32 hw_index_to_key(u32 ind)
{
	return (ind >> 24) | (ind << 8);
}

static u32 key_to_hw_index(u32 key)
{
	return (key << 24) | (key >> 8);
}

static int mlx4_SW2HW_MPT(struct mlx4_dev *dev, struct mlx4_cmd_mailbox *mailbox,
			  int mpt_index)
{
	return mlx4_cmd(dev, mailbox->dma, mpt_index, 0, MLX4_CMD_SW2HW_MPT,
			MLX4_CMD_TIME_CLASS_B);
}

static int mlx4_HW2SW_MPT(struct mlx4_dev *dev, struct mlx4_cmd_mailbox *mailbox,
			  int mpt_index)
{
	return mlx4_cmd_box(dev, 0, mailbox ? mailbox->dma : 0, mpt_index,
			    !mailbox, MLX4_CMD_HW2SW_MPT, MLX4_CMD_TIME_CLASS_B);
}

int mlx4_mr_alloc(struct mlx4_dev *dev, u32 pd, u64 iova, u64 size, u32 access,
		  int npages, int page_shift, struct mlx4_mr *mr)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	u32 index;
	int err;

	index = mlx4_bitmap_alloc(&priv->mr_table.mpt_bitmap);
	if (index == -1)
		return -ENOMEM;

	mr->iova       = iova;
	mr->size       = size;
	mr->pd	       = pd;
	mr->access     = access;
	mr->enabled    = 0;
	mr->key	       = hw_index_to_key(index);

	err = mlx4_mtt_init(dev, npages, page_shift, &mr->mtt);
	if (err)
		mlx4_bitmap_free(&priv->mr_table.mpt_bitmap, index);

	return err;
}
EXPORT_SYMBOL_GPL(mlx4_mr_alloc);

void mlx4_mr_free(struct mlx4_dev *dev, struct mlx4_mr *mr)
{
	struct mlx4_priv *priv = mlx4_priv(dev);
	int err;

	if (mr->enabled) {
		err = mlx4_HW2SW_MPT(dev, NULL,
				     key_to_hw_index(mr->key) &
				     (dev->caps.num_mpts - 1));
		if (err)
			mlx4_warn(dev, "HW2SW_MPT failed (%d)\n", err);
	}

	mlx4_mtt_cleanup(dev, &mr->mtt);
	mlx4_bitmap_free(&priv->mr_table.mpt_bitmap, key_to_hw_index(mr->key));
}
EXPORT_SYMBOL_GPL(mlx4_mr_free);

int mlx4_mr_enable(struct mlx4_dev *dev, struct mlx4_mr *mr)
{
	struct mlx4_mr_table *mr_table = &mlx4_priv(dev)->mr_table;
	struct mlx4_cmd_mailbox *mailbox;
	struct mlx4_mpt_entry *mpt_entry;
	int err;

	err = mlx4_table_get(dev, &mr_table->dmpt_table, key_to_hw_index(mr->key));
	if (err)
		return err;

	mailbox = mlx4_alloc_cmd_mailbox(dev);
	if (IS_ERR(mailbox)) {
		err = PTR_ERR(mailbox);
		goto err_table;
	}
	mpt_entry = mailbox->buf;

	memset(mpt_entry, 0, sizeof *mpt_entry);

	mpt_entry->flags = cpu_to_be32(MLX4_MPT_FLAG_SW_OWNS	 |
				       MLX4_MPT_FLAG_MIO	 |
				       MLX4_MPT_FLAG_REGION	 |
				       mr->access);

	mpt_entry->key	       = cpu_to_be32(key_to_hw_index(mr->key));
	mpt_entry->pd	       = cpu_to_be32(mr->pd);
	mpt_entry->start       = cpu_to_be64(mr->iova);
	mpt_entry->length      = cpu_to_be64(mr->size);
	mpt_entry->entity_size = cpu_to_be32(mr->mtt.page_shift);
	if (mr->mtt.order < 0) {
		mpt_entry->flags |= cpu_to_be32(MLX4_MPT_FLAG_PHYSICAL);
		mpt_entry->mtt_seg = 0;
	} else
		mpt_entry->mtt_seg = cpu_to_be64(mlx4_mtt_addr(dev, &mr->mtt));

	err = mlx4_SW2HW_MPT(dev, mailbox,
			     key_to_hw_index(mr->key) & (dev->caps.num_mpts - 1));
	if (err) {
		mlx4_warn(dev, "SW2HW_MPT failed (%d)\n", err);
		goto err_cmd;
	}

	mr->enabled = 1;

	mlx4_free_cmd_mailbox(dev, mailbox);

	return 0;

err_cmd:
	mlx4_free_cmd_mailbox(dev, mailbox);

err_table:
	mlx4_table_put(dev, &mr_table->dmpt_table, key_to_hw_index(mr->key));
	return err;
}
EXPORT_SYMBOL_GPL(mlx4_mr_enable);

static int mlx4_WRITE_MTT(struct mlx4_dev *dev, struct mlx4_cmd_mailbox *mailbox,
			  int num_mtt)
{
	return mlx4_cmd(dev, mailbox->dma, num_mtt, 0, MLX4_CMD_WRITE_MTT,
			MLX4_CMD_TIME_CLASS_B);
}

int mlx4_write_mtt(struct mlx4_dev *dev, struct mlx4_mtt *mtt,
		   int start_index, int npages, u64 *page_list)
{
	struct mlx4_cmd_mailbox *mailbox;
	__be64 *mtt_entry;
	int i;
	int err = 0;

	if (mtt->order < 0)
		return -EINVAL;

	mailbox = mlx4_alloc_cmd_mailbox(dev);
	if (IS_ERR(mailbox))
		return PTR_ERR(mailbox);

	mtt_entry = mailbox->buf;

	while (npages > 0) {
		mtt_entry[0] = cpu_to_be64(mlx4_mtt_addr(dev, mtt) + start_index * 8);
		mtt_entry[1] = 0;

		for (i = 0; i < npages && i < MLX4_MAILBOX_SIZE / 8 - 2; ++i)
			mtt_entry[i + 2] = cpu_to_be64(page_list[i] |
						       MLX4_MTT_FLAG_PRESENT);

		/*
		 * If we have an odd number of entries to write, add
		 * one more dummy entry for firmware efficiency.
		 */
		if (i & 1)
			mtt_entry[i + 2] = 0;

		err = mlx4_WRITE_MTT(dev, mailbox, (i + 1) & ~1);
		if (err)
			goto out;

		npages      -= i;
		start_index += i;
		page_list   += i;
	}

out:
	mlx4_free_cmd_mailbox(dev, mailbox);

	return err;
}
EXPORT_SYMBOL_GPL(mlx4_write_mtt);

int mlx4_buf_write_mtt(struct mlx4_dev *dev, struct mlx4_mtt *mtt,
		       struct mlx4_buf *buf)
{
	u64 *page_list;
	int err;
	int i;

	page_list = kmalloc(buf->npages * sizeof *page_list, GFP_KERNEL);
	if (!page_list)
		return -ENOMEM;

	for (i = 0; i < buf->npages; ++i)
		if (buf->nbufs == 1)
			page_list[i] = buf->u.direct.map + (i << buf->page_shift);
		else
			page_list[i] = buf->u.page_list[i].map;

	err = mlx4_write_mtt(dev, mtt, 0, buf->npages, page_list);

	kfree(page_list);
	return err;
}
EXPORT_SYMBOL_GPL(mlx4_buf_write_mtt);

int __devinit mlx4_init_mr_table(struct mlx4_dev *dev)
{
	struct mlx4_mr_table *mr_table = &mlx4_priv(dev)->mr_table;
	int err;

	err = mlx4_bitmap_init(&mr_table->mpt_bitmap, dev->caps.num_mpts,
			       ~0, dev->caps.reserved_mrws);
	if (err)
		return err;

	err = mlx4_buddy_init(&mr_table->mtt_buddy,
			      ilog2(dev->caps.num_mtt_segs));
	if (err)
		goto err_buddy;

	if (dev->caps.reserved_mtts) {
		if (mlx4_alloc_mtt_range(dev, ilog2(dev->caps.reserved_mtts)) == -1) {
			mlx4_warn(dev, "MTT table of order %d is too small.\n",
				  mr_table->mtt_buddy.max_order);
			err = -ENOMEM;
			goto err_reserve_mtts;
		}
	}

	return 0;

err_reserve_mtts:
	mlx4_buddy_cleanup(&mr_table->mtt_buddy);

err_buddy:
	mlx4_bitmap_cleanup(&mr_table->mpt_bitmap);

	return err;
}

void mlx4_cleanup_mr_table(struct mlx4_dev *dev)
{
	struct mlx4_mr_table *mr_table = &mlx4_priv(dev)->mr_table;

	mlx4_buddy_cleanup(&mr_table->mtt_buddy);
	mlx4_bitmap_cleanup(&mr_table->mpt_bitmap);
}
