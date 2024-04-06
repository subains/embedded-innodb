/**
Copyright (c) 1995, 2010, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/** @file buf/buf0rea.c
The database buffer read

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "buf0rea.h"

#include "fil0fil.h"
#include "mtr0mtr.h"

#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0lru.h"
#include "log0recv.h"
#include "os0file.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "ut0logger.h"

/** If there are buf_pool->m_curr_size per the number below pending reads, then
read-ahead is not done: this is to prevent flooding the buffer pool with
i/o-fixed buffer blocks */
constexpr ulint BUF_READ_AHEAD_PEND_LIMIT = 2;

/**
 * @brief Low-level function which reads a page asynchronously from a file to
 * the buffer buf_pool if it is not already there, in which case does nothing.
 * Sets the io_fix flag and sets an exclusive lock on the buffer frame. The flag
 * is cleared and the x-lock released by an i/o-handler thread.
 * 
 * @param[out] err out: DB_SUCCESS or DB_TABLESPACE_DELETED if we are trying to
 *  read from a non-existent tablespace, or a tablespace which is just now being dropped
 * 
 * @param[in] batch in: if a batch operation is required.
 * 
 * @param[in] space in: space id
 * 
 * @param[in] tablespace_version in: if the space memory object has this timestamp different from
 *  what we are giving here, treat the tablespace as dropped; this is a timestamp we use to stop
 *  dangling page reads from a tablespace which we have DISCARDed + IMPORTed back
 * 
 * @param[in] page_no_t in: page number
 * 
 * @return DB_SUCCESS if a request was posted to the IO layer, DB_FAIL the request was not posted
 *  or error code from the IO layer.
 */
static db_err buf_read_page(
  IO_request io_request,
  bool batch,
  space_id_t space,
  page_no_t page_no,
  int64_t tablespace_version
) {
  ut_a(io_request == IO_request::Async_read ||
       io_request == IO_request::Sync_read);

  if (trx_doublewrite &&
      space == TRX_SYS_SPACE &&
      ((page_no >= trx_doublewrite->block1 && page_no < trx_doublewrite->block1 + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) ||
       (page_no >= trx_doublewrite->block2 && page_no < trx_doublewrite->block2 + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE))) {

    log_warn(std::format("Trying to read the doublewrite buffer page {}", page_no));

    return DB_FAIL;
  }

  db_err err;

  /* The following call will also check if the tablespace does not exist
  or is being dropped; if we succeed in initing the page in the buffer
  pool for read, then DISCARD cannot proceed until the read has
  completed */
  auto bpage = buf_pool->init_for_read(&err, space, page_no, tablespace_version);

  if (bpage == nullptr) {
    ut_a(err == DB_TABLESPACE_DELETED);
    return DB_FAIL;
  }

  ut_ad(bpage->in_file());

  ut_a(bpage->get_state() == BUF_BLOCK_FILE_PAGE);

  err = fil_io(
    io_request,
    batch,
    space,
    page_no,
    0,
    UNIV_PAGE_SIZE,
    buf_page_get_block(bpage)->get_frame(),
    bpage);

  ut_a(err == DB_SUCCESS);

  if (io_request == IO_request::Sync_read) {
    /* The i/o is already completed when we arrive from fil_read */
    buf_pool->io_complete(bpage);
  } else {
    ut_a(io_request == IO_request::Async_read);
  }

  return DB_SUCCESS;
}

bool buf_read_page(ulint space, ulint offset) {
  auto tablespace_version = fil_space_get_version(space);
  auto err = buf_read_page(IO_request::Sync_read, false, space, offset, tablespace_version);

  if (err == DB_SUCCESS) {

    ++srv_buf_pool_reads;

  } else if (err == DB_TABLESPACE_DELETED) {

    log_err(
      std::format(
        "Trying to access tablespace {} page no. {}, but the "
        " tablespace does not exist or is being dropped.",
        space, offset)
    );
  }

  /* Flush pages from the end of the LRU list if necessary */
  buf_pool->m_flusher->free_margin();

  /* Increment number of I/O operations used for LRU policy. */
  buf_pool->m_LRU->stat_inc_io();

  return err == DB_SUCCESS;
}

ulint buf_read_ahead_linear(space_id_t space, page_no_t offset) {
  buf_page_t *bpage;
  buf_frame_t *frame;
  buf_page_t *pred_bpage = nullptr;
  ulint pred_offset;
  ulint succ_offset;
  ulint count;
  int asc_or_desc;
  ulint new_offset;
  ulint fail_count;
  db_err err;
  ulint i;
  const ulint buf_read_ahead_linear_area = buf_pool->get_read_ahead_area();
  ulint threshold;

  if (unlikely(srv_startup_is_before_trx_rollback_phase)) {
    /* No read-ahead to avoid thread deadlocks */
    return 0;
  }

  auto low = (offset / buf_read_ahead_linear_area) * buf_read_ahead_linear_area;
  auto high = (offset / buf_read_ahead_linear_area + 1) * buf_read_ahead_linear_area;

  if (offset != low && offset != high - 1) {
    /* This is not a border page of the area: return */

    return 0;
  }

  if (trx_sys_hdr_page(space, offset)) {
    /* Don't do a read-ahead in the system area. Being cautious. */
    return 0;
  }

  /* Remember the tablespace version before we ask te tablespace size
  below: if DISCARD + IMPORT changes the actual .ibd file meanwhile, we
  do not try to read outside the bounds of the tablespace! */

  auto tablespace_version = fil_space_get_version(space);

  buf_pool_mutex_enter();

  if (high > fil_space_get_size(space)) {
    buf_pool_mutex_exit();
    /* The area is not whole, return */

    return 0;
  }

  if (buf_pool->m_n_pend_reads > buf_pool->m_curr_size / BUF_READ_AHEAD_PEND_LIMIT) {
    buf_pool_mutex_exit();

    return 0;
  }

  /* Check that almost all pages in the area have been accessed; if
  offset == low, the accesses must be in a descending order, otherwise,
  in an ascending order. */

  asc_or_desc = 1;

  if (offset == low) {
    asc_or_desc = -1;
  }

  /* How many out of order accessed pages can we ignore
  when working out the access pattern for linear readahead */
  threshold = ut_min((64 - srv_read_ahead_threshold), buf_pool->get_read_ahead_area());

  fail_count = 0;

  for (i = low; i < high; i++) {
    bpage = buf_pool->hash_get_page(space, i);

    if (bpage == nullptr || !buf_page_is_accessed(bpage)) {
      /* Not accessed */
      fail_count++;

    } else if (pred_bpage != nullptr) {
      /* Note that buf_page_is_accessed() returns the time of the first
      access.  If some blocks of the extent existed in the buffer pool
      at the time of a linear access pattern, the first access times
      may be nonmonotonic, even though the latest access times were
      linear.  The threshold (srv_read_ahead_factor) should help a
      little against this. */
      int res = ut_ulint_cmp(buf_page_is_accessed(bpage), buf_page_is_accessed(pred_bpage));

      /* Accesses not in the right order */
      if (res != 0 && res != asc_or_desc) {
        fail_count++;
      }
    }

    if (fail_count > threshold) {
      /* Too many failures: return */
      buf_pool_mutex_exit();
      return 0;
    }

    if (bpage != nullptr && buf_page_is_accessed(bpage)) {
      pred_bpage = bpage;
    }
  }

  /* If we got this far, we know that enough pages in the area have
  been accessed in the right order: linear read-ahead can be sensible */

  bpage = buf_pool->hash_get_page(space, offset);

  if (bpage == nullptr) {
    buf_pool_mutex_exit();

    return 0;
  }

  switch (bpage->get_state()) {
    case BUF_BLOCK_FILE_PAGE:
      frame = bpage->get_block()->get_frame();
      break;
    default:
      ut_error;
      break;
  }

  /* Read the natural predecessor and successor page addresses from
  the page; NOTE that because the calling thread may have an x-latch
  on the page, we do not acquire an s-latch on the page, this is to
  prevent deadlocks. Even if we read values which are nonsense, the
  algorithm will work. */

  pred_offset = fil_page_get_prev(frame);
  succ_offset = fil_page_get_next(frame);

  buf_pool_mutex_exit();

  if (offset == low && succ_offset == offset + 1) {

    /* This is ok, we can continue */
    new_offset = pred_offset;

  } else if (offset == high - 1 && pred_offset == offset - 1) {

    /* This is ok, we can continue */
    new_offset = succ_offset;
  } else {
    /* Successor or predecessor not in the right order */

    return 0;
  }

  low = (new_offset / buf_read_ahead_linear_area) * buf_read_ahead_linear_area;
  high = (new_offset / buf_read_ahead_linear_area + 1) * buf_read_ahead_linear_area;

  if (new_offset != low && new_offset != high - 1) {
    /* This is not a border page of the area: return */

    return 0;

  } else if (high > fil_space_get_size(space)) {

    /* The area is not whole, return */

    return 0;
  }

  count = 0;

  for (i = low; i < high; i++) {
    /* It is only sensible to do read-ahead in the non-sync
    aio mode: hence false as the first parameter */

    err = buf_read_page(IO_request::Async_read, true, space, i, tablespace_version);

    if (err == DB_SUCCESS) {

      ++count;

    } else if (err == DB_TABLESPACE_DELETED) {

      log_info(
        std::format(
          "Lnear readahead trying to access tablespace {} page {},but the tablespace does not"
          " exist or is just being dropped.", space, i)
      );

    } else {
      ut_a(err == DB_FAIL);
    }
  }

  /* In simulated aio we wake the aio handler threads only after
  queuing all aio requests, in native aio the following call does
  nothing: */

  os_aio_simulated_wake_handler_threads();

  /* Flush pages from the end of the LRU list if necessary */
  buf_pool->m_flusher->free_margin();

  /* Read ahead is considered one I/O operation for the purpose of LRU policy decision. */
  buf_pool->m_LRU->stat_inc_io();

  buf_pool->m_stat.n_ra_pages_read += count;

  return count;
}

void buf_read_recv_pages(bool sync, space_id_t space, const page_no_t *page_nos, ulint n_stored) {
  if (fil_space_get_size(space) == ULINT_UNDEFINED) {
    /* It is a single table tablespace and the .ibd file is
    missing: do nothing */
    return;
  }

  auto tablespace_version = fil_space_get_version(space);

  for (ulint i = 0; i < n_stored; i++) {
    ulint count{};

    while (buf_pool->m_n_pend_reads >= recv_n_pool_free_frames / 2) {

      os_aio_simulated_wake_handler_threads();
      os_thread_sleep(10000);

      count++;

      if (count > 1000) {

        log_err(
          std::format(
            "Waited for 10 seconds for pending reads to the buffer pool to"
            " be finished. Number of pending reads {}. pending pread calls {}",
            buf_pool->m_n_pend_reads, os_file_n_pending_preads.load())
	);
      }
    }

    if ((i + 1 == n_stored) && sync) {
      buf_read_page(IO_request::Sync_read, false, space, page_nos[i], tablespace_version);
    } else {
      buf_read_page(IO_request::Async_read, true, space, page_nos[i], tablespace_version);
    }
  }

  os_aio_simulated_wake_handler_threads();

  /* Flush pages from the end of the LRU list if necessary */
  buf_pool->m_flusher->free_margin();
}
