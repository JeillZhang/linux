// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2023 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_btree.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_sb.h"
#include "xfs_inode.h"
#include "xfs_alloc.h"
#include "xfs_alloc_btree.h"
#include "xfs_ialloc.h"
#include "xfs_ialloc_btree.h"
#include "xfs_rmap.h"
#include "xfs_rmap_btree.h"
#include "xfs_refcount_btree.h"
#include "xfs_rtbitmap.h"
#include "xfs_extent_busy.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_quota.h"
#include "xfs_qm.h"
#include "xfs_defer.h"
#include "xfs_errortag.h"
#include "xfs_error.h"
#include "xfs_reflink.h"
#include "xfs_health.h"
#include "xfs_buf_mem.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_attr.h"
#include "xfs_dir2.h"
#include "xfs_rtrmap_btree.h"
#include "xfs_rtbitmap.h"
#include "xfs_rtgroup.h"
#include "xfs_rtalloc.h"
#include "xfs_metafile.h"
#include "xfs_rtrefcount_btree.h"
#include "xfs_zone_alloc.h"
#include "scrub/scrub.h"
#include "scrub/common.h"
#include "scrub/trace.h"
#include "scrub/repair.h"
#include "scrub/bitmap.h"
#include "scrub/stats.h"
#include "scrub/xfile.h"
#include "scrub/attr_repair.h"

/*
 * Attempt to repair some metadata, if the metadata is corrupt and userspace
 * told us to fix it.  This function returns -EAGAIN to mean "re-run scrub",
 * and will set *fixed to true if it thinks it repaired anything.
 */
int
xrep_attempt(
	struct xfs_scrub	*sc,
	struct xchk_stats_run	*run)
{
	u64			repair_start;
	int			error = 0;

	trace_xrep_attempt(XFS_I(file_inode(sc->file)), sc->sm, error);

	xchk_ag_btcur_free(&sc->sa);
	xchk_rtgroup_btcur_free(&sc->sr);

	/* Repair whatever's broken. */
	ASSERT(sc->ops->repair);
	run->repair_attempted = true;
	repair_start = xchk_stats_now();
	error = sc->ops->repair(sc);
	trace_xrep_done(XFS_I(file_inode(sc->file)), sc->sm, error);
	run->repair_ns += xchk_stats_elapsed_ns(repair_start);
	switch (error) {
	case 0:
		/*
		 * Repair succeeded.  Commit the fixes and perform a second
		 * scrub so that we can tell userspace if we fixed the problem.
		 */
		sc->sm->sm_flags &= ~XFS_SCRUB_FLAGS_OUT;
		sc->flags |= XREP_ALREADY_FIXED;
		run->repair_succeeded = true;
		return -EAGAIN;
	case -ECHRNG:
		sc->flags |= XCHK_NEED_DRAIN;
		run->retries++;
		return -EAGAIN;
	case -EDEADLOCK:
		/* Tell the caller to try again having grabbed all the locks. */
		if (!(sc->flags & XCHK_TRY_HARDER)) {
			sc->flags |= XCHK_TRY_HARDER;
			run->retries++;
			return -EAGAIN;
		}
		/*
		 * We tried harder but still couldn't grab all the resources
		 * we needed to fix it.  The corruption has not been fixed,
		 * so exit to userspace with the scan's output flags unchanged.
		 */
		return 0;
	default:
		/*
		 * EAGAIN tells the caller to re-scrub, so we cannot return
		 * that here.
		 */
		ASSERT(error != -EAGAIN);
		return error;
	}
}

/*
 * Complain about unfixable problems in the filesystem.  We don't log
 * corruptions when IFLAG_REPAIR wasn't set on the assumption that the driver
 * program is xfs_scrub, which will call back with IFLAG_REPAIR set if the
 * administrator isn't running xfs_scrub in no-repairs mode.
 *
 * Use this helper function because _ratelimited silently declares a static
 * structure to track rate limiting information.
 */
void
xrep_failure(
	struct xfs_mount	*mp)
{
	xfs_alert_ratelimited(mp,
"Corruption not fixed during online repair.  Unmount and run xfs_repair.");
}

/*
 * Repair probe -- userspace uses this to probe if we're willing to repair a
 * given mountpoint.
 */
int
xrep_probe(
	struct xfs_scrub	*sc)
{
	int			error = 0;

	if (xchk_should_terminate(sc, &error))
		return error;

	return 0;
}

/*
 * Roll a transaction, keeping the AG headers locked and reinitializing
 * the btree cursors.
 */
int
xrep_roll_ag_trans(
	struct xfs_scrub	*sc)
{
	int			error;

	/*
	 * Keep the AG header buffers locked while we roll the transaction.
	 * Ensure that both AG buffers are dirty and held when we roll the
	 * transaction so that they move forward in the log without losing the
	 * bli (and hence the bli type) when the transaction commits.
	 *
	 * Normal code would never hold clean buffers across a roll, but repair
	 * needs both buffers to maintain a total lock on the AG.
	 */
	if (sc->sa.agi_bp) {
		xfs_ialloc_log_agi(sc->tp, sc->sa.agi_bp, XFS_AGI_MAGICNUM);
		xfs_trans_bhold(sc->tp, sc->sa.agi_bp);
	}

	if (sc->sa.agf_bp) {
		xfs_alloc_log_agf(sc->tp, sc->sa.agf_bp, XFS_AGF_MAGICNUM);
		xfs_trans_bhold(sc->tp, sc->sa.agf_bp);
	}

	/*
	 * Roll the transaction.  We still hold the AG header buffers locked
	 * regardless of whether or not that succeeds.  On failure, the buffers
	 * will be released during teardown on our way out of the kernel.  If
	 * successful, join the buffers to the new transaction and move on.
	 */
	error = xfs_trans_roll(&sc->tp);
	if (error)
		return error;

	/* Join the AG headers to the new transaction. */
	if (sc->sa.agi_bp)
		xfs_trans_bjoin(sc->tp, sc->sa.agi_bp);
	if (sc->sa.agf_bp)
		xfs_trans_bjoin(sc->tp, sc->sa.agf_bp);

	return 0;
}

/* Roll the scrub transaction, holding the primary metadata locked. */
int
xrep_roll_trans(
	struct xfs_scrub	*sc)
{
	if (!sc->ip)
		return xrep_roll_ag_trans(sc);
	return xfs_trans_roll_inode(&sc->tp, sc->ip);
}

/* Finish all deferred work attached to the repair transaction. */
int
xrep_defer_finish(
	struct xfs_scrub	*sc)
{
	int			error;

	/*
	 * Keep the AG header buffers locked while we complete deferred work
	 * items.  Ensure that both AG buffers are dirty and held when we roll
	 * the transaction so that they move forward in the log without losing
	 * the bli (and hence the bli type) when the transaction commits.
	 *
	 * Normal code would never hold clean buffers across a roll, but repair
	 * needs both buffers to maintain a total lock on the AG.
	 */
	if (sc->sa.agi_bp) {
		xfs_ialloc_log_agi(sc->tp, sc->sa.agi_bp, XFS_AGI_MAGICNUM);
		xfs_trans_bhold(sc->tp, sc->sa.agi_bp);
	}

	if (sc->sa.agf_bp) {
		xfs_alloc_log_agf(sc->tp, sc->sa.agf_bp, XFS_AGF_MAGICNUM);
		xfs_trans_bhold(sc->tp, sc->sa.agf_bp);
	}

	/*
	 * Finish all deferred work items.  We still hold the AG header buffers
	 * locked regardless of whether or not that succeeds.  On failure, the
	 * buffers will be released during teardown on our way out of the
	 * kernel.  If successful, join the buffers to the new transaction
	 * and move on.
	 */
	error = xfs_defer_finish(&sc->tp);
	if (error)
		return error;

	/*
	 * Release the hold that we set above because defer_finish won't do
	 * that for us.  The defer roll code redirties held buffers after each
	 * roll, so the AG header buffers should be ready for logging.
	 */
	if (sc->sa.agi_bp)
		xfs_trans_bhold_release(sc->tp, sc->sa.agi_bp);
	if (sc->sa.agf_bp)
		xfs_trans_bhold_release(sc->tp, sc->sa.agf_bp);

	return 0;
}

/*
 * Does the given AG have enough space to rebuild a btree?  Neither AG
 * reservation can be critical, and we must have enough space (factoring
 * in AG reservations) to construct a whole btree.
 */
bool
xrep_ag_has_space(
	struct xfs_perag	*pag,
	xfs_extlen_t		nr_blocks,
	enum xfs_ag_resv_type	type)
{
	return  !xfs_ag_resv_critical(pag, XFS_AG_RESV_RMAPBT) &&
		!xfs_ag_resv_critical(pag, XFS_AG_RESV_METADATA) &&
		pag->pagf_freeblks > xfs_ag_resv_needed(pag, type) + nr_blocks;
}

/*
 * Figure out how many blocks to reserve for an AG repair.  We calculate the
 * worst case estimate for the number of blocks we'd need to rebuild one of
 * any type of per-AG btree.
 */
xfs_extlen_t
xrep_calc_ag_resblks(
	struct xfs_scrub		*sc)
{
	struct xfs_mount		*mp = sc->mp;
	struct xfs_scrub_metadata	*sm = sc->sm;
	struct xfs_perag		*pag;
	struct xfs_buf			*bp;
	xfs_agino_t			icount = NULLAGINO;
	xfs_extlen_t			aglen = NULLAGBLOCK;
	xfs_extlen_t			usedlen;
	xfs_extlen_t			freelen;
	xfs_extlen_t			bnobt_sz;
	xfs_extlen_t			inobt_sz;
	xfs_extlen_t			rmapbt_sz;
	xfs_extlen_t			refcbt_sz;
	int				error;

	if (!(sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR))
		return 0;

	pag = xfs_perag_get(mp, sm->sm_agno);
	if (xfs_perag_initialised_agi(pag)) {
		/* Use in-core icount if possible. */
		icount = pag->pagi_count;
	} else {
		/* Try to get the actual counters from disk. */
		error = xfs_ialloc_read_agi(pag, NULL, 0, &bp);
		if (!error) {
			icount = pag->pagi_count;
			xfs_buf_relse(bp);
		}
	}

	/* Now grab the block counters from the AGF. */
	error = xfs_alloc_read_agf(pag, NULL, 0, &bp);
	if (error) {
		aglen = pag_group(pag)->xg_block_count;
		freelen = aglen;
		usedlen = aglen;
	} else {
		struct xfs_agf	*agf = bp->b_addr;

		aglen = be32_to_cpu(agf->agf_length);
		freelen = be32_to_cpu(agf->agf_freeblks);
		usedlen = aglen - freelen;
		xfs_buf_relse(bp);
	}

	/* If the icount is impossible, make some worst-case assumptions. */
	if (icount == NULLAGINO ||
	    !xfs_verify_agino(pag, icount)) {
		icount = pag->agino_max - pag->agino_min + 1;
	}

	/* If the block counts are impossible, make worst-case assumptions. */
	if (aglen == NULLAGBLOCK ||
	    aglen != pag_group(pag)->xg_block_count ||
	    freelen >= aglen) {
		aglen = pag_group(pag)->xg_block_count;
		freelen = aglen;
		usedlen = aglen;
	}

	trace_xrep_calc_ag_resblks(pag, icount, aglen, freelen, usedlen);

	/*
	 * Figure out how many blocks we'd need worst case to rebuild
	 * each type of btree.  Note that we can only rebuild the
	 * bnobt/cntbt or inobt/finobt as pairs.
	 */
	bnobt_sz = 2 * xfs_allocbt_calc_size(mp, freelen);
	if (xfs_has_sparseinodes(mp))
		inobt_sz = xfs_iallocbt_calc_size(mp, icount /
				XFS_INODES_PER_HOLEMASK_BIT);
	else
		inobt_sz = xfs_iallocbt_calc_size(mp, icount /
				XFS_INODES_PER_CHUNK);
	if (xfs_has_finobt(mp))
		inobt_sz *= 2;
	if (xfs_has_reflink(mp))
		refcbt_sz = xfs_refcountbt_calc_size(mp, usedlen);
	else
		refcbt_sz = 0;
	if (xfs_has_rmapbt(mp)) {
		/*
		 * Guess how many blocks we need to rebuild the rmapbt.
		 * For non-reflink filesystems we can't have more records than
		 * used blocks.  However, with reflink it's possible to have
		 * more than one rmap record per AG block.  We don't know how
		 * many rmaps there could be in the AG, so we start off with
		 * what we hope is an generous over-estimation.
		 */
		if (xfs_has_reflink(mp))
			rmapbt_sz = xfs_rmapbt_calc_size(mp,
					(unsigned long long)aglen * 2);
		else
			rmapbt_sz = xfs_rmapbt_calc_size(mp, usedlen);
	} else {
		rmapbt_sz = 0;
	}

	trace_xrep_calc_ag_resblks_btsize(pag, bnobt_sz, inobt_sz, rmapbt_sz,
			refcbt_sz);
	xfs_perag_put(pag);

	return max(max(bnobt_sz, inobt_sz), max(rmapbt_sz, refcbt_sz));
}

#ifdef CONFIG_XFS_RT
/*
 * Figure out how many blocks to reserve for a rtgroup repair.  We calculate
 * the worst case estimate for the number of blocks we'd need to rebuild one of
 * any type of per-rtgroup btree.
 */
xfs_extlen_t
xrep_calc_rtgroup_resblks(
	struct xfs_scrub		*sc)
{
	struct xfs_mount		*mp = sc->mp;
	struct xfs_scrub_metadata	*sm = sc->sm;
	uint64_t			usedlen;
	xfs_extlen_t			rmapbt_sz = 0;

	if (!(sm->sm_flags & XFS_SCRUB_IFLAG_REPAIR))
		return 0;
	if (!xfs_has_rtgroups(mp)) {
		ASSERT(0);
		return -EFSCORRUPTED;
	}

	usedlen = xfs_rtbxlen_to_blen(mp, xfs_rtgroup_extents(mp, sm->sm_agno));
	ASSERT(usedlen <= XFS_MAX_RGBLOCKS);

	if (xfs_has_rmapbt(mp))
		rmapbt_sz = xfs_rtrmapbt_calc_size(mp, usedlen);

	trace_xrep_calc_rtgroup_resblks_btsize(mp, sm->sm_agno, usedlen,
			rmapbt_sz);

	return rmapbt_sz;
}
#endif /* CONFIG_XFS_RT */

/*
 * Reconstructing per-AG Btrees
 *
 * When a space btree is corrupt, we don't bother trying to fix it.  Instead,
 * we scan secondary space metadata to derive the records that should be in
 * the damaged btree, initialize a fresh btree root, and insert the records.
 * Note that for rebuilding the rmapbt we scan all the primary data to
 * generate the new records.
 *
 * However, that leaves the matter of removing all the metadata describing the
 * old broken structure.  For primary metadata we use the rmap data to collect
 * every extent with a matching rmap owner (bitmap); we then iterate all other
 * metadata structures with the same rmap owner to collect the extents that
 * cannot be removed (sublist).  We then subtract sublist from bitmap to
 * derive the blocks that were used by the old btree.  These blocks can be
 * reaped.
 *
 * For rmapbt reconstructions we must use different tactics for extent
 * collection.  First we iterate all primary metadata (this excludes the old
 * rmapbt, obviously) to generate new rmap records.  The gaps in the rmap
 * records are collected as bitmap.  The bnobt records are collected as
 * sublist.  As with the other btrees we subtract sublist from bitmap, and the
 * result (since the rmapbt lives in the free space) are the blocks from the
 * old rmapbt.
 */

/* Ensure the freelist is the correct size. */
int
xrep_fix_freelist(
	struct xfs_scrub	*sc,
	int			alloc_flags)
{
	struct xfs_alloc_arg	args = {0};

	args.mp = sc->mp;
	args.tp = sc->tp;
	args.agno = pag_agno(sc->sa.pag);
	args.alignment = 1;
	args.pag = sc->sa.pag;

	return xfs_alloc_fix_freelist(&args, alloc_flags);
}

/*
 * Finding per-AG Btree Roots for AGF/AGI Reconstruction
 *
 * If the AGF or AGI become slightly corrupted, it may be necessary to rebuild
 * the AG headers by using the rmap data to rummage through the AG looking for
 * btree roots.  This is not guaranteed to work if the AG is heavily damaged
 * or the rmap data are corrupt.
 *
 * Callers of xrep_find_ag_btree_roots must lock the AGF and AGFL
 * buffers if the AGF is being rebuilt; or the AGF and AGI buffers if the
 * AGI is being rebuilt.  It must maintain these locks until it's safe for
 * other threads to change the btrees' shapes.  The caller provides
 * information about the btrees to look for by passing in an array of
 * xrep_find_ag_btree with the (rmap owner, buf_ops, magic) fields set.
 * The (root, height) fields will be set on return if anything is found.  The
 * last element of the array should have a NULL buf_ops to mark the end of the
 * array.
 *
 * For every rmapbt record matching any of the rmap owners in btree_info,
 * read each block referenced by the rmap record.  If the block is a btree
 * block from this filesystem matching any of the magic numbers and has a
 * level higher than what we've already seen, remember the block and the
 * height of the tree required to have such a block.  When the call completes,
 * we return the highest block we've found for each btree description; those
 * should be the roots.
 */

struct xrep_findroot {
	struct xfs_scrub		*sc;
	struct xfs_buf			*agfl_bp;
	struct xfs_agf			*agf;
	struct xrep_find_ag_btree	*btree_info;
};

/* See if our block is in the AGFL. */
STATIC int
xrep_findroot_agfl_walk(
	struct xfs_mount	*mp,
	xfs_agblock_t		bno,
	void			*priv)
{
	xfs_agblock_t		*agbno = priv;

	return (*agbno == bno) ? -ECANCELED : 0;
}

/* Does this block match the btree information passed in? */
STATIC int
xrep_findroot_block(
	struct xrep_findroot		*ri,
	struct xrep_find_ag_btree	*fab,
	uint64_t			owner,
	xfs_agblock_t			agbno,
	bool				*done_with_block)
{
	struct xfs_mount		*mp = ri->sc->mp;
	struct xfs_buf			*bp;
	struct xfs_btree_block		*btblock;
	xfs_daddr_t			daddr;
	int				block_level;
	int				error = 0;

	daddr = xfs_agbno_to_daddr(ri->sc->sa.pag, agbno);

	/*
	 * Blocks in the AGFL have stale contents that might just happen to
	 * have a matching magic and uuid.  We don't want to pull these blocks
	 * in as part of a tree root, so we have to filter out the AGFL stuff
	 * here.  If the AGFL looks insane we'll just refuse to repair.
	 */
	if (owner == XFS_RMAP_OWN_AG) {
		error = xfs_agfl_walk(mp, ri->agf, ri->agfl_bp,
				xrep_findroot_agfl_walk, &agbno);
		if (error == -ECANCELED)
			return 0;
		if (error)
			return error;
	}

	/*
	 * Read the buffer into memory so that we can see if it's a match for
	 * our btree type.  We have no clue if it is beforehand, and we want to
	 * avoid xfs_trans_read_buf's behavior of dumping the DONE state (which
	 * will cause needless disk reads in subsequent calls to this function)
	 * and logging metadata verifier failures.
	 *
	 * Therefore, pass in NULL buffer ops.  If the buffer was already in
	 * memory from some other caller it will already have b_ops assigned.
	 * If it was in memory from a previous unsuccessful findroot_block
	 * call, the buffer won't have b_ops but it should be clean and ready
	 * for us to try to verify if the read call succeeds.  The same applies
	 * if the buffer wasn't in memory at all.
	 *
	 * Note: If we never match a btree type with this buffer, it will be
	 * left in memory with NULL b_ops.  This shouldn't be a problem unless
	 * the buffer gets written.
	 */
	error = xfs_trans_read_buf(mp, ri->sc->tp, mp->m_ddev_targp, daddr,
			mp->m_bsize, 0, &bp, NULL);
	if (error)
		return error;

	/* Ensure the block magic matches the btree type we're looking for. */
	btblock = XFS_BUF_TO_BLOCK(bp);
	ASSERT(fab->buf_ops->magic[1] != 0);
	if (btblock->bb_magic != fab->buf_ops->magic[1])
		goto out;

	/*
	 * If the buffer already has ops applied and they're not the ones for
	 * this btree type, we know this block doesn't match the btree and we
	 * can bail out.
	 *
	 * If the buffer ops match ours, someone else has already validated
	 * the block for us, so we can move on to checking if this is a root
	 * block candidate.
	 *
	 * If the buffer does not have ops, nobody has successfully validated
	 * the contents and the buffer cannot be dirty.  If the magic, uuid,
	 * and structure match this btree type then we'll move on to checking
	 * if it's a root block candidate.  If there is no match, bail out.
	 */
	if (bp->b_ops) {
		if (bp->b_ops != fab->buf_ops)
			goto out;
	} else {
		ASSERT(!xfs_trans_buf_is_dirty(bp));
		if (!uuid_equal(&btblock->bb_u.s.bb_uuid,
				&mp->m_sb.sb_meta_uuid))
			goto out;
		/*
		 * Read verifiers can reference b_ops, so we set the pointer
		 * here.  If the verifier fails we'll reset the buffer state
		 * to what it was before we touched the buffer.
		 */
		bp->b_ops = fab->buf_ops;
		fab->buf_ops->verify_read(bp);
		if (bp->b_error) {
			bp->b_ops = NULL;
			bp->b_error = 0;
			goto out;
		}

		/*
		 * Some read verifiers will (re)set b_ops, so we must be
		 * careful not to change b_ops after running the verifier.
		 */
	}

	/*
	 * This block passes the magic/uuid and verifier tests for this btree
	 * type.  We don't need the caller to try the other tree types.
	 */
	*done_with_block = true;

	/*
	 * Compare this btree block's level to the height of the current
	 * candidate root block.
	 *
	 * If the level matches the root we found previously, throw away both
	 * blocks because there can't be two candidate roots.
	 *
	 * If level is lower in the tree than the root we found previously,
	 * ignore this block.
	 */
	block_level = xfs_btree_get_level(btblock);
	if (block_level + 1 == fab->height) {
		fab->root = NULLAGBLOCK;
		goto out;
	} else if (block_level < fab->height) {
		goto out;
	}

	/*
	 * This is the highest block in the tree that we've found so far.
	 * Update the btree height to reflect what we've learned from this
	 * block.
	 */
	fab->height = block_level + 1;

	/*
	 * If this block doesn't have sibling pointers, then it's the new root
	 * block candidate.  Otherwise, the root will be found farther up the
	 * tree.
	 */
	if (btblock->bb_u.s.bb_leftsib == cpu_to_be32(NULLAGBLOCK) &&
	    btblock->bb_u.s.bb_rightsib == cpu_to_be32(NULLAGBLOCK))
		fab->root = agbno;
	else
		fab->root = NULLAGBLOCK;

	trace_xrep_findroot_block(ri->sc->sa.pag, agbno,
			be32_to_cpu(btblock->bb_magic), fab->height - 1);
out:
	xfs_trans_brelse(ri->sc->tp, bp);
	return error;
}

/*
 * Do any of the blocks in this rmap record match one of the btrees we're
 * looking for?
 */
STATIC int
xrep_findroot_rmap(
	struct xfs_btree_cur		*cur,
	const struct xfs_rmap_irec	*rec,
	void				*priv)
{
	struct xrep_findroot		*ri = priv;
	struct xrep_find_ag_btree	*fab;
	xfs_agblock_t			b;
	bool				done;
	int				error = 0;

	/* Ignore anything that isn't AG metadata. */
	if (!XFS_RMAP_NON_INODE_OWNER(rec->rm_owner))
		return 0;

	/* Otherwise scan each block + btree type. */
	for (b = 0; b < rec->rm_blockcount; b++) {
		done = false;
		for (fab = ri->btree_info; fab->buf_ops; fab++) {
			if (rec->rm_owner != fab->rmap_owner)
				continue;
			error = xrep_findroot_block(ri, fab,
					rec->rm_owner, rec->rm_startblock + b,
					&done);
			if (error)
				return error;
			if (done)
				break;
		}
	}

	return 0;
}

/* Find the roots of the per-AG btrees described in btree_info. */
int
xrep_find_ag_btree_roots(
	struct xfs_scrub		*sc,
	struct xfs_buf			*agf_bp,
	struct xrep_find_ag_btree	*btree_info,
	struct xfs_buf			*agfl_bp)
{
	struct xfs_mount		*mp = sc->mp;
	struct xrep_findroot		ri;
	struct xrep_find_ag_btree	*fab;
	struct xfs_btree_cur		*cur;
	int				error;

	ASSERT(xfs_buf_islocked(agf_bp));
	ASSERT(agfl_bp == NULL || xfs_buf_islocked(agfl_bp));

	ri.sc = sc;
	ri.btree_info = btree_info;
	ri.agf = agf_bp->b_addr;
	ri.agfl_bp = agfl_bp;
	for (fab = btree_info; fab->buf_ops; fab++) {
		ASSERT(agfl_bp || fab->rmap_owner != XFS_RMAP_OWN_AG);
		ASSERT(XFS_RMAP_NON_INODE_OWNER(fab->rmap_owner));
		fab->root = NULLAGBLOCK;
		fab->height = 0;
	}

	cur = xfs_rmapbt_init_cursor(mp, sc->tp, agf_bp, sc->sa.pag);
	error = xfs_rmap_query_all(cur, xrep_findroot_rmap, &ri);
	xfs_btree_del_cursor(cur, error);

	return error;
}

#ifdef CONFIG_XFS_QUOTA
/* Update some quota flags in the superblock. */
void
xrep_update_qflags(
	struct xfs_scrub	*sc,
	unsigned int		clear_flags,
	unsigned int		set_flags)
{
	struct xfs_mount	*mp = sc->mp;
	struct xfs_buf		*bp;

	mutex_lock(&mp->m_quotainfo->qi_quotaofflock);
	if ((mp->m_qflags & clear_flags) == 0 &&
	    (mp->m_qflags & set_flags) == set_flags)
		goto no_update;

	mp->m_qflags &= ~clear_flags;
	mp->m_qflags |= set_flags;

	spin_lock(&mp->m_sb_lock);
	mp->m_sb.sb_qflags &= ~clear_flags;
	mp->m_sb.sb_qflags |= set_flags;
	spin_unlock(&mp->m_sb_lock);

	/*
	 * Update the quota flags in the ondisk superblock without touching
	 * the summary counters.  We have not quiesced inode chunk allocation,
	 * so we cannot coordinate with updates to the icount and ifree percpu
	 * counters.
	 */
	bp = xfs_trans_getsb(sc->tp);
	xfs_sb_to_disk(bp->b_addr, &mp->m_sb);
	xfs_trans_buf_set_type(sc->tp, bp, XFS_BLFT_SB_BUF);
	xfs_trans_log_buf(sc->tp, bp, 0, sizeof(struct xfs_dsb) - 1);

no_update:
	mutex_unlock(&mp->m_quotainfo->qi_quotaofflock);
}

/* Force a quotacheck the next time we mount. */
void
xrep_force_quotacheck(
	struct xfs_scrub	*sc,
	xfs_dqtype_t		type)
{
	uint			flag;

	flag = xfs_quota_chkd_flag(type);
	if (!(flag & sc->mp->m_qflags))
		return;

	xrep_update_qflags(sc, flag, 0);
}

/*
 * Attach dquots to this inode, or schedule quotacheck to fix them.
 *
 * This function ensures that the appropriate dquots are attached to an inode.
 * We cannot allow the dquot code to allocate an on-disk dquot block here
 * because we're already in transaction context.  The on-disk dquot should
 * already exist anyway.  If the quota code signals corruption or missing quota
 * information, schedule quotacheck, which will repair corruptions in the quota
 * metadata.
 */
int
xrep_ino_dqattach(
	struct xfs_scrub	*sc)
{
	int			error;

	ASSERT(sc->tp != NULL);
	ASSERT(sc->ip != NULL);

	error = xfs_qm_dqattach(sc->ip);
	switch (error) {
	case -EFSBADCRC:
	case -EFSCORRUPTED:
	case -ENOENT:
		xfs_err_ratelimited(sc->mp,
"inode %llu repair encountered quota error %d, quotacheck forced.",
				(unsigned long long)sc->ip->i_ino, error);
		if (XFS_IS_UQUOTA_ON(sc->mp) && !sc->ip->i_udquot)
			xrep_force_quotacheck(sc, XFS_DQTYPE_USER);
		if (XFS_IS_GQUOTA_ON(sc->mp) && !sc->ip->i_gdquot)
			xrep_force_quotacheck(sc, XFS_DQTYPE_GROUP);
		if (XFS_IS_PQUOTA_ON(sc->mp) && !sc->ip->i_pdquot)
			xrep_force_quotacheck(sc, XFS_DQTYPE_PROJ);
		fallthrough;
	case -ESRCH:
		error = 0;
		break;
	default:
		break;
	}

	return error;
}
#endif /* CONFIG_XFS_QUOTA */

/*
 * Ensure that the inode being repaired is ready to handle a certain number of
 * extents, or return EFSCORRUPTED.  Caller must hold the ILOCK of the inode
 * being repaired and have joined it to the scrub transaction.
 */
int
xrep_ino_ensure_extent_count(
	struct xfs_scrub	*sc,
	int			whichfork,
	xfs_extnum_t		nextents)
{
	xfs_extnum_t		max_extents;
	bool			inode_has_nrext64;

	inode_has_nrext64 = xfs_inode_has_large_extent_counts(sc->ip);
	max_extents = xfs_iext_max_nextents(inode_has_nrext64, whichfork);
	if (nextents <= max_extents)
		return 0;
	if (inode_has_nrext64)
		return -EFSCORRUPTED;
	if (!xfs_has_large_extent_counts(sc->mp))
		return -EFSCORRUPTED;

	max_extents = xfs_iext_max_nextents(true, whichfork);
	if (nextents > max_extents)
		return -EFSCORRUPTED;

	sc->ip->i_diflags2 |= XFS_DIFLAG2_NREXT64;
	xfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE);
	return 0;
}

/*
 * Initialize all the btree cursors for an AG repair except for the btree that
 * we're rebuilding.
 */
void
xrep_ag_btcur_init(
	struct xfs_scrub	*sc,
	struct xchk_ag		*sa)
{
	struct xfs_mount	*mp = sc->mp;

	/* Set up a bnobt cursor for cross-referencing. */
	if (sc->sm->sm_type != XFS_SCRUB_TYPE_BNOBT &&
	    sc->sm->sm_type != XFS_SCRUB_TYPE_CNTBT) {
		sa->bno_cur = xfs_bnobt_init_cursor(mp, sc->tp, sa->agf_bp,
				sc->sa.pag);
		sa->cnt_cur = xfs_cntbt_init_cursor(mp, sc->tp, sa->agf_bp,
				sc->sa.pag);
	}

	/* Set up a inobt cursor for cross-referencing. */
	if (sc->sm->sm_type != XFS_SCRUB_TYPE_INOBT &&
	    sc->sm->sm_type != XFS_SCRUB_TYPE_FINOBT) {
		sa->ino_cur = xfs_inobt_init_cursor(sc->sa.pag, sc->tp,
				sa->agi_bp);
		if (xfs_has_finobt(mp))
			sa->fino_cur = xfs_finobt_init_cursor(sc->sa.pag,
					sc->tp, sa->agi_bp);
	}

	/* Set up a rmapbt cursor for cross-referencing. */
	if (sc->sm->sm_type != XFS_SCRUB_TYPE_RMAPBT &&
	    xfs_has_rmapbt(mp))
		sa->rmap_cur = xfs_rmapbt_init_cursor(mp, sc->tp, sa->agf_bp,
				sc->sa.pag);

	/* Set up a refcountbt cursor for cross-referencing. */
	if (sc->sm->sm_type != XFS_SCRUB_TYPE_REFCNTBT &&
	    xfs_has_reflink(mp))
		sa->refc_cur = xfs_refcountbt_init_cursor(mp, sc->tp,
				sa->agf_bp, sc->sa.pag);
}

/*
 * Reinitialize the in-core AG state after a repair by rereading the AGF
 * buffer.  We had better get the same AGF buffer as the one that's attached
 * to the scrub context.
 */
int
xrep_reinit_pagf(
	struct xfs_scrub	*sc)
{
	struct xfs_perag	*pag = sc->sa.pag;
	struct xfs_buf		*bp;
	int			error;

	ASSERT(pag);
	ASSERT(xfs_perag_initialised_agf(pag));

	clear_bit(XFS_AGSTATE_AGF_INIT, &pag->pag_opstate);
	error = xfs_alloc_read_agf(pag, sc->tp, 0, &bp);
	if (error)
		return error;

	if (bp != sc->sa.agf_bp) {
		ASSERT(bp == sc->sa.agf_bp);
		return -EFSCORRUPTED;
	}

	return 0;
}

/*
 * Reinitialize the in-core AG state after a repair by rereading the AGI
 * buffer.  We had better get the same AGI buffer as the one that's attached
 * to the scrub context.
 */
int
xrep_reinit_pagi(
	struct xfs_scrub	*sc)
{
	struct xfs_perag	*pag = sc->sa.pag;
	struct xfs_buf		*bp;
	int			error;

	ASSERT(pag);
	ASSERT(xfs_perag_initialised_agi(pag));

	clear_bit(XFS_AGSTATE_AGI_INIT, &pag->pag_opstate);
	error = xfs_ialloc_read_agi(pag, sc->tp, 0, &bp);
	if (error)
		return error;

	if (bp != sc->sa.agi_bp) {
		ASSERT(bp == sc->sa.agi_bp);
		return -EFSCORRUPTED;
	}

	return 0;
}

/*
 * Given an active reference to a perag structure, load AG headers and cursors.
 * This should only be called to scan an AG while repairing file-based metadata.
 */
int
xrep_ag_init(
	struct xfs_scrub	*sc,
	struct xfs_perag	*pag,
	struct xchk_ag		*sa)
{
	int			error;

	ASSERT(!sa->pag);

	error = xfs_ialloc_read_agi(pag, sc->tp, 0, &sa->agi_bp);
	if (error)
		return error;

	error = xfs_alloc_read_agf(pag, sc->tp, 0, &sa->agf_bp);
	if (error)
		return error;

	/* Grab our own passive reference from the caller's ref. */
	sa->pag = xfs_perag_hold(pag);
	xrep_ag_btcur_init(sc, sa);
	return 0;
}

#ifdef CONFIG_XFS_RT
/* Initialize all the btree cursors for a RT repair. */
void
xrep_rtgroup_btcur_init(
	struct xfs_scrub	*sc,
	struct xchk_rt		*sr)
{
	struct xfs_mount	*mp = sc->mp;

	ASSERT(sr->rtg != NULL);

	if (sc->sm->sm_type != XFS_SCRUB_TYPE_RTRMAPBT &&
	    (sr->rtlock_flags & XFS_RTGLOCK_RMAP) &&
	    xfs_has_rtrmapbt(mp))
		sr->rmap_cur = xfs_rtrmapbt_init_cursor(sc->tp, sr->rtg);

	if (sc->sm->sm_type != XFS_SCRUB_TYPE_RTREFCBT &&
	    (sr->rtlock_flags & XFS_RTGLOCK_REFCOUNT) &&
	    xfs_has_rtreflink(mp))
		sr->refc_cur = xfs_rtrefcountbt_init_cursor(sc->tp, sr->rtg);
}

/*
 * Given a reference to a rtgroup structure, lock rtgroup btree inodes and
 * create btree cursors.  Must only be called to repair a regular rt file.
 */
int
xrep_rtgroup_init(
	struct xfs_scrub	*sc,
	struct xfs_rtgroup	*rtg,
	struct xchk_rt		*sr,
	unsigned int		rtglock_flags)
{
	ASSERT(sr->rtg == NULL);

	xfs_rtgroup_lock(rtg, rtglock_flags);
	sr->rtlock_flags = rtglock_flags;

	/* Grab our own passive reference from the caller's ref. */
	sr->rtg = xfs_rtgroup_hold(rtg);
	xrep_rtgroup_btcur_init(sc, sr);
	return 0;
}

/* Ensure that all rt blocks in the given range are not marked free. */
int
xrep_require_rtext_inuse(
	struct xfs_scrub	*sc,
	xfs_rgblock_t		rgbno,
	xfs_filblks_t		len)
{
	struct xfs_mount	*mp = sc->mp;
	xfs_rtxnum_t		startrtx;
	xfs_rtxnum_t		endrtx;
	bool			is_free = false;
	int			error = 0;

	if (xfs_has_zoned(mp)) {
		if (!xfs_zone_rgbno_is_valid(sc->sr.rtg, rgbno + len - 1))
			return -EFSCORRUPTED;
		return 0;
	}

	startrtx = xfs_rgbno_to_rtx(mp, rgbno);
	endrtx = xfs_rgbno_to_rtx(mp, rgbno + len - 1);

	error = xfs_rtalloc_extent_is_free(sc->sr.rtg, sc->tp, startrtx,
			endrtx - startrtx + 1, &is_free);
	if (error)
		return error;
	if (is_free)
		return -EFSCORRUPTED;

	return 0;
}
#endif /* CONFIG_XFS_RT */

/* Reinitialize the per-AG block reservation for the AG we just fixed. */
int
xrep_reset_perag_resv(
	struct xfs_scrub	*sc)
{
	int			error;

	if (!(sc->flags & XREP_RESET_PERAG_RESV))
		return 0;

	ASSERT(sc->sa.pag != NULL);
	ASSERT(sc->ops->type == ST_PERAG);
	ASSERT(sc->tp);

	sc->flags &= ~XREP_RESET_PERAG_RESV;
	xfs_ag_resv_free(sc->sa.pag);
	error = xfs_ag_resv_init(sc->sa.pag, sc->tp);
	if (error == -ENOSPC) {
		xfs_err(sc->mp,
"Insufficient free space to reset per-AG reservation for AG %u after repair.",
				pag_agno(sc->sa.pag));
		error = 0;
	}

	return error;
}

/* Decide if we are going to call the repair function for a scrub type. */
bool
xrep_will_attempt(
	struct xfs_scrub	*sc)
{
	/* Userspace asked us to rebuild the structure regardless. */
	if (sc->sm->sm_flags & XFS_SCRUB_IFLAG_FORCE_REBUILD)
		return true;

	/* Let debug users force us into the repair routines. */
	if (XFS_TEST_ERROR(false, sc->mp, XFS_ERRTAG_FORCE_SCRUB_REPAIR))
		return true;

	/* Metadata is corrupt or failed cross-referencing. */
	if (xchk_needs_repair(sc->sm))
		return true;

	return false;
}

/* Try to fix some part of a metadata inode by calling another scrubber. */
STATIC int
xrep_metadata_inode_subtype(
	struct xfs_scrub	*sc,
	unsigned int		scrub_type)
{
	struct xfs_scrub_subord	*sub;
	int			error;

	/*
	 * Let's see if the inode needs repair.  Use a subordinate scrub context
	 * to call the scrub and repair functions so that we can hang on to the
	 * resources that we already acquired instead of using the standard
	 * setup/teardown routines.
	 */
	sub = xchk_scrub_create_subord(sc, scrub_type);
	error = sub->sc.ops->scrub(&sub->sc);
	if (error)
		goto out;
	if (!xrep_will_attempt(&sub->sc))
		goto out;

	/*
	 * Repair some part of the inode.  This will potentially join the inode
	 * to the transaction.
	 */
	error = sub->sc.ops->repair(&sub->sc);
	if (error)
		goto out;

	/*
	 * Finish all deferred intent items and then roll the transaction so
	 * that the inode will not be joined to the transaction when we exit
	 * the function.
	 */
	error = xfs_defer_finish(&sub->sc.tp);
	if (error)
		goto out;
	error = xfs_trans_roll(&sub->sc.tp);
	if (error)
		goto out;

	/*
	 * Clear the corruption flags and re-check the metadata that we just
	 * repaired.
	 */
	sub->sc.sm->sm_flags &= ~XFS_SCRUB_FLAGS_OUT;
	error = sub->sc.ops->scrub(&sub->sc);
	if (error)
		goto out;

	/* If corruption persists, the repair has failed. */
	if (xchk_needs_repair(sub->sc.sm)) {
		error = -EFSCORRUPTED;
		goto out;
	}
out:
	xchk_scrub_free_subord(sub);
	return error;
}

/*
 * Repair the ondisk forks of a metadata inode.  The caller must ensure that
 * sc->ip points to the metadata inode and the ILOCK is held on that inode.
 * The inode must not be joined to the transaction before the call, and will
 * not be afterwards.
 */
int
xrep_metadata_inode_forks(
	struct xfs_scrub	*sc)
{
	bool			dirty = false;
	int			error;

	/* Repair the inode record and the data fork. */
	error = xrep_metadata_inode_subtype(sc, XFS_SCRUB_TYPE_INODE);
	if (error)
		return error;

	error = xrep_metadata_inode_subtype(sc, XFS_SCRUB_TYPE_BMBTD);
	if (error)
		return error;

	/*
	 * Metadata files can only have extended attributes on metadir
	 * filesystems, either for parent pointers or for actual xattr data.
	 * For a non-metadir filesystem, make sure the attr fork looks ok
	 * before we delete it.
	 */
	if (xfs_inode_hasattr(sc->ip)) {
		error = xrep_metadata_inode_subtype(sc, XFS_SCRUB_TYPE_BMBTA);
		if (error)
			return error;
	}

	/* Clear the reflink flag since metadata never shares. */
	if (xfs_is_reflink_inode(sc->ip)) {
		dirty = true;
		xfs_trans_ijoin(sc->tp, sc->ip, 0);
		error = xfs_reflink_clear_inode_flag(sc->ip, &sc->tp);
		if (error)
			return error;
	}

	/*
	 * Metadata files on non-metadir filesystems cannot have attr forks,
	 * so clear them now.
	 */
	if (xfs_inode_hasattr(sc->ip) && !xfs_has_metadir(sc->mp)) {
		if (!dirty) {
			dirty = true;
			xfs_trans_ijoin(sc->tp, sc->ip, 0);
		}
		error = xrep_xattr_reset_fork(sc);
		if (error)
			return error;
	}

	/*
	 * If we modified the inode, roll the transaction but don't rejoin the
	 * inode to the new transaction because xrep_bmap_data can do that.
	 */
	if (dirty) {
		error = xfs_trans_roll(&sc->tp);
		if (error)
			return error;
		dirty = false;
	}

	return 0;
}

/*
 * Set up an in-memory buffer cache so that we can use the xfbtree.  Allocating
 * a shmem file might take loks, so we cannot be in transaction context.  Park
 * our resources in the scrub context and let the teardown function take care
 * of them at the right time.
 */
int
xrep_setup_xfbtree(
	struct xfs_scrub	*sc,
	const char		*descr)
{
	ASSERT(sc->tp == NULL);

	return xmbuf_alloc(sc->mp, descr, &sc->xmbtp);
}

/*
 * See if this buffer can pass the given ->verify_struct() function.
 *
 * If the buffer already has ops attached and they're not the ones that were
 * passed in, we reject the buffer.  Otherwise, we perform the structure test
 * (note that we do not check CRCs) and return the outcome of the test.  The
 * buffer ops and error state are left unchanged.
 */
bool
xrep_buf_verify_struct(
	struct xfs_buf			*bp,
	const struct xfs_buf_ops	*ops)
{
	const struct xfs_buf_ops	*old_ops = bp->b_ops;
	xfs_failaddr_t			fa;
	int				old_error;

	if (old_ops) {
		if (old_ops != ops)
			return false;
	}

	old_error = bp->b_error;
	bp->b_ops = ops;
	fa = bp->b_ops->verify_struct(bp);
	bp->b_ops = old_ops;
	bp->b_error = old_error;

	return fa == NULL;
}

/* Check the sanity of a rmap record for a metadata btree inode. */
int
xrep_check_ino_btree_mapping(
	struct xfs_scrub		*sc,
	const struct xfs_rmap_irec	*rec)
{
	enum xbtree_recpacking		outcome;
	int				error;

	/*
	 * Metadata btree inodes never have extended attributes, and all blocks
	 * should have the bmbt block flag set.
	 */
	if ((rec->rm_flags & XFS_RMAP_ATTR_FORK) ||
	    !(rec->rm_flags & XFS_RMAP_BMBT_BLOCK))
		return -EFSCORRUPTED;

	/* Make sure the block is within the AG. */
	if (!xfs_verify_agbext(sc->sa.pag, rec->rm_startblock,
				rec->rm_blockcount))
		return -EFSCORRUPTED;

	/* Make sure this isn't free space. */
	error = xfs_alloc_has_records(sc->sa.bno_cur, rec->rm_startblock,
			rec->rm_blockcount, &outcome);
	if (error)
		return error;
	if (outcome != XBTREE_RECPACKING_EMPTY)
		return -EFSCORRUPTED;

	return 0;
}

/*
 * Reset the block count of the inode being repaired, and adjust the dquot
 * block usage to match.  The inode must not have an xattr fork.
 */
void
xrep_inode_set_nblocks(
	struct xfs_scrub	*sc,
	int64_t			new_blocks)
{
	int64_t			delta =
		new_blocks - sc->ip->i_nblocks;

	sc->ip->i_nblocks = new_blocks;

	xfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE);
	if (delta != 0)
		xfs_trans_mod_dquot_byino(sc->tp, sc->ip, XFS_TRANS_DQ_BCOUNT,
				delta);
}

/* Reset the block reservation for a metadata inode. */
int
xrep_reset_metafile_resv(
	struct xfs_scrub	*sc)
{
	struct xfs_mount	*mp = sc->mp;
	int64_t			delta;
	int			error;

	delta = mp->m_metafile_resv_used + mp->m_metafile_resv_avail -
		mp->m_metafile_resv_target;
	if (delta == 0)
		return 0;

	/*
	 * Too many blocks have been reserved, transfer some from the incore
	 * reservation back to the filesystem.
	 */
	if (delta > 0) {
		int64_t		give_back;

		give_back = min_t(uint64_t, delta, mp->m_metafile_resv_avail);
		if (give_back > 0) {
			xfs_mod_sb_delalloc(mp, -give_back);
			xfs_add_fdblocks(mp, give_back);
			mp->m_metafile_resv_avail -= give_back;
		}

		return 0;
	}

	/*
	 * Not enough reservation; try to take some blocks from the filesystem
	 * to the metabtree reservation.
	 */
	delta = -delta; /* delta is negative here, so invert the sign. */
	error = xfs_dec_fdblocks(mp, delta, true);
	while (error == -ENOSPC) {
		delta--;
		if (delta == 0) {
			xfs_warn(sc->mp,
"Insufficient free space to reset metabtree reservation after repair.");
			return 0;
		}
		error = xfs_dec_fdblocks(mp, delta, true);
	}
	if (error)
		return error;

	xfs_mod_sb_delalloc(mp, delta);
	mp->m_metafile_resv_avail += delta;
	return 0;
}
