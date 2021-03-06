/*-------------------------------------------------------------------------
 *
 * tpdxlog.c
 *	  WAL replay logic for tpd.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/zheap/tpdxlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tpd.h"
#include "access/tpd_xlog.h"
#include "access/xlogutils.h"
#include "access/zheapam_xlog.h"

/*
 * replay of tpd entry allocation
 */
static void
tpd_xlog_allocate_entry(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_tpd_allocate_entry *xlrec;
	Buffer	tpdbuffer;
	Buffer	heap_page_buffer;
	Buffer	metabuf = InvalidBuffer;
	Buffer	last_used_buf = InvalidBuffer;
	Page	tpdpage;
	TPDPageOpaque tpdopaque;
	XLogRedoAction action;

	xlrec = (xl_tpd_allocate_entry *) XLogRecGetData(record);

	/*
	 * If we inserted the first and only tpd entry on the page, re-initialize
	 * the page from scratch.
	 */
	if (XLogRecGetInfo(record) & XLOG_TPD_INIT_PAGE)
	{
		tpdbuffer = XLogInitBufferForRedo(record, 0);
		tpdpage = BufferGetPage(tpdbuffer);
		TPDInitPage(tpdpage, BufferGetPageSize(tpdbuffer));
		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &tpdbuffer);
	if (action == BLK_NEEDS_REDO)
	{
		char	*tpd_entry;
		Size	size_tpd_entry;
		OffsetNumber	offnum;

		tpd_entry = XLogRecGetBlockData(record, 0, &size_tpd_entry);
		tpdpage = BufferGetPage(tpdbuffer);
		offnum = TPDPageAddEntry(tpdpage, tpd_entry, size_tpd_entry,
								 xlrec->offnum);
		if (offnum == InvalidOffsetNumber)
			elog(PANIC, "failed to add TPD entry");
		MarkBufferDirty(tpdbuffer);
		PageSetLSN(tpdpage, lsn);

		/* The TPD entry must be added at the provided offset. */
		Assert(offnum == xlrec->offnum);

		tpdopaque = (TPDPageOpaque) PageGetSpecialPointer(tpdpage);
		tpdopaque->tpd_prevblkno = xlrec->prevblk;

		MarkBufferDirty(tpdbuffer);
		PageSetLSN(tpdpage, lsn);
	}
	else if (action == BLK_RESTORED)
	{
		/*
		 * Note that we still update the page even if it was restored from a full
		 * page image, because the special space is not included in the image.
		 */
		tpdpage = BufferGetPage(tpdbuffer);

		tpdopaque = (TPDPageOpaque) PageGetSpecialPointer(tpdpage);
		tpdopaque->tpd_prevblkno = xlrec->prevblk;

		MarkBufferDirty(tpdbuffer);
		PageSetLSN(tpdpage, lsn);
	}

	if (XLogReadBufferForRedo(record, 1, &heap_page_buffer) == BLK_NEEDS_REDO)
	{
		/* Set the TPD location in last transaction slot of heap page. */
		SetTPDLocation(heap_page_buffer, tpdbuffer, xlrec->offnum);
		MarkBufferDirty(heap_page_buffer);

		PageSetLSN(BufferGetPage(heap_page_buffer), lsn);
	}

	/* replay the record for meta page */
	if (XLogRecHasBlockRef(record, 2))
	{
		xl_zheap_metadata	*xlrecmeta;
		char	   *ptr;
		Size		len;

		metabuf = XLogInitBufferForRedo(record, 2);
		ptr = XLogRecGetBlockData(record, 2, &len);

		Assert(len == SizeOfMetaData);
		Assert(BufferGetBlockNumber(metabuf) == ZHEAP_METAPAGE);
		xlrecmeta = (xl_zheap_metadata *) ptr;

		zheap_init_meta_page(metabuf, xlrecmeta->first_used_tpd_page,
							 xlrecmeta->last_used_tpd_page);
		MarkBufferDirty(metabuf);
		PageSetLSN(BufferGetPage(metabuf), lsn);

		/*
		 * We can have reference of block 3, iff we have reference for block
		 * 2.
		 */
		if (XLogRecHasBlockRef(record, 3))
		{
			action = XLogReadBufferForRedo(record, 3, &last_used_buf);
			/*
			 * Note that we still update the page even if it was restored from a full
			 * page image, because the special space is not included in the image.
			 */
			if (action == BLK_NEEDS_REDO || action == BLK_RESTORED)
			{
				Page	last_used_page;
				TPDPageOpaque last_tpdopaque;

				last_used_page = BufferGetPage(last_used_buf);
				last_tpdopaque = (TPDPageOpaque) PageGetSpecialPointer(last_used_page);
				last_tpdopaque->tpd_nextblkno = xlrec->nextblk;

				MarkBufferDirty(last_used_buf);
				PageSetLSN(last_used_page, lsn);
			}
		}
	}

	if (BufferIsValid(tpdbuffer))
		UnlockReleaseBuffer(tpdbuffer);
	if (BufferIsValid(heap_page_buffer))
		UnlockReleaseBuffer(heap_page_buffer);
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
	if (BufferIsValid(last_used_buf))
		UnlockReleaseBuffer(last_used_buf);
}

/*
 * replay of pruning tpd page
 */
static void
tpd_xlog_clean(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer	tpdbuf;
	XLogRedoAction action;

	/*
	 * If we have a full-page image, restore it (using a cleanup lock) and
	 * we're done.
	 */
	action = XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true,
										   &tpdbuf);
	if (action == BLK_NEEDS_REDO)
	{
		Page		tpdpage = (Page) BufferGetPage(tpdbuf);
		OffsetNumber *end;
		OffsetNumber *nowunused;
		int			nunused;
		Size		datalen;

		nowunused = (OffsetNumber *) XLogRecGetBlockData(record, 0, &datalen);
		end = (OffsetNumber *) ((char *) nowunused + datalen);
		nunused = (end - nowunused);
		Assert(nunused >= 0);

		/* Update all item pointers per the record, and repair fragmentation */
		TPDPagePruneExecute(tpdbuf, nowunused, nunused);

		/*
		 * Note: we don't worry about updating the page's prunability hints.
		 * At worst this will cause an extra prune cycle to occur soon.
		 */

		MarkBufferDirty(tpdbuf);
		PageSetLSN(tpdpage, lsn);
	}
	if (BufferIsValid(tpdbuf))
		UnlockReleaseBuffer(tpdbuf);
}

/*
 * replay for clearing tpd location from heap page.
 */
static void
tpd_xlog_clear_location(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer	buffer;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page	page = (Page) BufferGetPage(buffer);

		ClearTPDLocation(page);
		MarkBufferDirty(buffer);
		PageSetLSN(page, lsn);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

void
tpd_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info & XLOG_TPD_OPMASK)
	{
		case XLOG_ALLOCATE_TPD_ENTRY:
			tpd_xlog_allocate_entry(record);
			break;
		case XLOG_TPD_CLEAN:
			tpd_xlog_clean(record);
			break;
		case XLOG_TPD_CLEAR_LOCATION:
			tpd_xlog_clear_location(record);
			break;
		default:
			elog(PANIC, "tpd_redo: unknown op code %u", info);
	}
}
