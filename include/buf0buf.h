/****************************************************************************
Copyright (c) 1995, 2010, Innobase Oy. All Rights Reserved.
Copyright (c) 2024 Sunny Bains. All rights reserved.

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

/*** @file include/buf0buf.h
The database buffer pool high-level routines

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#pragma once

#include "innodb0types.h"

#include "buf0types.h"
#include "fil0types.h"
#include "mach0data.h"
#include "mtr0types.h"
#include "page0types.h"
#include "ut0crc32.h"

/** mutex protecting the buffer pool struct and control blocks, except the
read-write lock in them */
extern mutex_t buf_pool_mutex;

#ifdef UNIV_DEBUG
/**
 * Sets file_page_was_freed true if the page is found in the buffer pool.
 * This function should be called when we free a file page and want the
 * debug version to check that it is not accessed any more unless reallocated.
 *
 * @param space     in: space id
 * @param page_no   in: page number
 * @return          control block if found in page hash table, otherwise nullptr
 */
buf_page_t *buf_page_set_file_page_was_freed(space_id_t space, page_no_t page_no);

/** Flag to forbid the release of the buffer pool mutex.
Protected by buf_pool_mutex. */
extern ulint buf_pool_mutex_exit_forbidden;

/** Forbid the release of the buffer pool mutex. */
#define buf_pool_mutex_exit_forbid() \
  do {                               \
    ut_ad(buf_pool_mutex_own());     \
    ++buf_pool_mutex_exit_forbidden; \
  } while (0)

/** Allow the release of the buffer pool mutex. */
#define buf_pool_mutex_exit_allow()      \
  do {                                   \
    ut_ad(buf_pool_mutex_own());         \
    ut_a(buf_pool_mutex_exit_forbidden); \
    --buf_pool_mutex_exit_forbidden;     \
  } while (0)

/** Release the buffer pool mutex. */
#define buf_pool_mutex_exit()                 \
  do {                                        \
    ut_a(buf_pool_mutex_exit_forbidden == 0); \
    mutex_exit(&buf_pool_mutex);              \
  } while (0)

#else /* UNIV_DEBUG */
/** Forbid the release of the buffer pool mutex. */
#define buf_pool_mutex_exit_forbid() ((void)0)

/** Allow the release of the buffer pool mutex. */
#define buf_pool_mutex_exit_allow() ((void)0)

/** Release the buffer pool mutex. */
#define buf_pool_mutex_exit() mutex_exit(&buf_pool_mutex)

#endif /* UNIV_DEBUG */

/** Find out if a pointer corresponds to a buf_block_t::mutex.
@param m	in: mutex candidate
@return		true if m is a buf_block_t::mutex */
#define buf_pool_is_block_mutex(m) buf_pool->pointer_is_block_field((const void *)(m))

/** Find out if a pointer corresponds to a buf_block_t::lock.
@param l	in: rw-lock candidate
@return		true if l is a buf_block_t::lock */
#define buf_pool_is_block_lock(l) buf_pool->pointer_is_block_field((const void *)(l))

/**
 * @brief Prints a page to stderr.
 *
 * @param read_buf  in: a database page
 * @param ulint
 */
void buf_page_print(const byte *read_buf, ulint);

/** mutex protecting the buffer pool struct and control blocks, except the
read-write lock in them */
extern mutex_t buf_pool_mutex;

/** @name Accessors for buf_pool_mutex.
Use these instead of accessing buf_pool_mutex directly. */
/* @{ */

/** Test if buf_pool_mutex is owned. */
#define buf_pool_mutex_own() mutex_own(&buf_pool_mutex)
/** Acquire the buffer pool mutex. */
#define buf_pool_mutex_enter()    \
  do {                            \
    mutex_enter(&buf_pool_mutex); \
  } while (0)

/* @} */

/*** Let us list the consistency conditions for different control block states.

NOT_USED:
  is in free list,
  not in LRU list,
  not in flush list,
  nor page hash table

READY_FOR_USE:
  is not in free list
  is not in LRU list
  is not in flush list
  is not in the page hash table

MEMORY:
	is not in free list
  is not in LRU list
  is not in flush list
  is not in the page hash table

FILE_PAGE:	space and page_no are defined, is in page hash table
  if io_fix == BUF_IO_WRITE,
    pool: no_flush[flush_type] is in reset state,
    pool: n_flush[flush_type] > 0

  (1) if buf_fix_count == 0, then
    is in LRU list, not in free list
    is in flush list,
      if and only if oldest_modification > 0
    is x-locked,
      if and only if io_fix == BUF_IO_READ
    is s-locked,
      if and only if io_fix == BUF_IO_WRITE

  (2) if buf_fix_count > 0, then
    is not in LRU list, not in free list
    is in flush list,
      if and only if oldest_modification > 0
    if io_fix == BUF_IO_READ,
      is x-locked
    if io_fix == BUF_IO_WRITE,
      is s-locked

State transitions:

NOT_USED => READY_FOR_USE
READY_FOR_USE => MEMORY
READY_FOR_USE => FILE_PAGE
MEMORY => NOT_USED

FILE_PAGE => NOT_USED	NOTE: This transition is allowed if and only if
  (1) buf_fix_count == 0,
  (2) oldest_modification == 0, and
  (3) io_fix == 0.
*/

inline uint64_t Buf_pool::get_oldest_modification() const {
  buf_pool_mutex_enter();

  auto bpage = UT_LIST_GET_LAST(m_flush_list);

  uint64_t lsn;

  if (bpage == nullptr) {
    lsn = 0;
  } else {
    ut_ad(bpage->m_in_flush_list);
    lsn = bpage->m_oldest_modification;
  }

  buf_pool_mutex_exit();

  /* The returned answer may be out of date: the flush_list can
  change after the mutex has been released. */

  return lsn;
}

inline buf_page_state buf_page_t::get_state() const {
#ifdef UNIV_DEBUG
  switch (m_state) {
    case BUF_BLOCK_NOT_USED:
    case BUF_BLOCK_READY_FOR_USE:
    case BUF_BLOCK_FILE_PAGE:
    case BUF_BLOCK_MEMORY:
    case BUF_BLOCK_REMOVE_HASH:
      break;
    default:
      ut_error;
  }
#endif /* UNIV_DEBUG */

  return static_cast<buf_page_state>(m_state);
}

inline buf_page_state buf_block_t::get_state() const {
  return m_page.get_state();
}

/**
 * Sets the state of a block.
 *
 * @param bpage Pointer to the control block.
 * @param state The state to set.
 */
inline void buf_page_set_state(buf_page_t *bpage, buf_page_state state) {
#ifdef UNIV_DEBUG
  auto old_state = bpage->get_state();

  switch (old_state) {
    case BUF_BLOCK_NOT_USED:
      ut_a(state == BUF_BLOCK_READY_FOR_USE);
      break;
    case BUF_BLOCK_READY_FOR_USE:
      ut_a(state == BUF_BLOCK_MEMORY || state == BUF_BLOCK_FILE_PAGE || state == BUF_BLOCK_NOT_USED);
      break;
    case BUF_BLOCK_MEMORY:
      ut_a(state == BUF_BLOCK_NOT_USED);
      break;
    case BUF_BLOCK_FILE_PAGE:
      ut_a(state == BUF_BLOCK_NOT_USED || state == BUF_BLOCK_REMOVE_HASH);
      break;
    case BUF_BLOCK_REMOVE_HASH:
      ut_a(state == BUF_BLOCK_MEMORY);
      break;
  }
#endif /* UNIV_DEBUG */

  bpage->m_state = state;
  ut_ad(bpage->get_state() == state);
}

/**
 * Sets the state of a block.
 *
 * @param block Pointer to the control block.
 * @param state The state to set.
 */
inline void buf_block_set_state(buf_block_t *block, buf_page_state state) {
  buf_page_set_state(&block->m_page, state);
}

/**
 * Gets the mutex of a block.
 *
 * @param bpage Pointer to the control block.
 * @return Pointer to the mutex protecting the block.
 */
inline mutex_t *buf_page_get_mutex(const buf_page_t *bpage) {
  return &bpage->get_block()->m_mutex;
}

/**
 * Get the flush type of a page.
 *
 * @param bpage Pointer to the buffer page.
 * @return The flush type of the page.
 */
inline auto buf_page_get_flush_type(const buf_page_t *bpage) {
  auto flush_type = static_cast<buf_flush>(bpage->m_flush_type);

#ifdef UNIV_DEBUG
  switch (flush_type) {
    case BUF_FLUSH_LRU:
    case BUF_FLUSH_SINGLE_PAGE:
    case BUF_FLUSH_LIST:
      return flush_type;
    case BUF_FLUSH_N_TYPES:
      break;
  }
  ut_error;
#endif /* UNIV_DEBUG */

  return flush_type;
}

/**
 * Set the flush type of a page.
 *
 * @param bpage Pointer to the buffer page.
 * @param flush_type The flush type to set.
 */
inline void buf_page_set_flush_type(buf_page_t *bpage, buf_flush flush_type) {
  bpage->m_flush_type = flush_type;
  ut_ad(buf_page_get_flush_type(bpage) == flush_type);
}

/**
 * Map a block to a file page.
 *
 * @param block Pointer to the control block.
 * @param space Tablespace ID.
 * @param page_no Page number.
 */
inline void buf_block_set_file_page(buf_block_t *block, space_id_t space, page_no_t page_no) {
  buf_block_set_state(block, BUF_BLOCK_FILE_PAGE);
  block->m_page.m_space = space;
  block->m_page.m_page_no = page_no;
}

/**
 * Gets the io_fix state of a block.
 *
 * @param bpage Pointer to the control block.
 * @return The io_fix state of the block.
 */
inline auto buf_page_get_io_fix(const buf_page_t *bpage) {
  auto io_fix = static_cast<buf_io_fix>(bpage->m_io_fix);

#ifdef UNIV_DEBUG
  switch (io_fix) {
    case BUF_IO_NONE:
    case BUF_IO_READ:
    case BUF_IO_WRITE:
      return io_fix;
  }
  ut_error;
#endif /* UNIV_DEBUG */

  return io_fix;
}

/**
 * Gets the io_fix state of a block.
 *
 * @param block Pointer to the control block.
 * @return The io_fix state of the block.
 */
inline buf_io_fix buf_block_get_io_fix(const buf_block_t *block) {
  return buf_page_get_io_fix(&block->m_page);
}

/**
 * Sets the io_fix state of a block.
 *
 * @param bpage Pointer to the control block.
 * @param io_fix The io_fix state to set.
 */
inline void buf_page_set_io_fix(buf_page_t *bpage, buf_io_fix io_fix) {
  ut_ad(buf_pool_mutex_own());
  ut_ad(mutex_own(buf_page_get_mutex(bpage)));

  bpage->m_io_fix = io_fix;
  ut_ad(buf_page_get_io_fix(bpage) == io_fix);
}

/**
 * Sets the io_fix state of a block.
 *
 * @param block Pointer to the control block.
 * @param io_fix The io_fix state to set.
 */
inline void buf_block_set_io_fix(buf_block_t *block, buf_io_fix io_fix) {
  buf_page_set_io_fix(&block->m_page, io_fix);
}

/**
 * Determine if a buffer block can be relocated in memory.
 * The block can be dirty, but it must not be I/O-fixed or bufferfixed.
 *
 * @param bpage Pointer to the control block being relocated.
 * @return True if the block can be relocated, false otherwise.
 */
inline bool buf_page_can_relocate(const buf_page_t *bpage) {
  ut_ad(buf_pool_mutex_own());
  ut_ad(mutex_own(buf_page_get_mutex(bpage)));
  ut_ad(bpage->in_file());
  ut_ad(bpage->m_in_LRU_list);

  return buf_page_get_io_fix(bpage) == BUF_IO_NONE && bpage->m_buf_fix_count == 0;
}

/**
 * Determine if a block has been flagged as old.
 *
 * @param bpage Pointer to the control block.
 * @return True if the block is flagged as old, false otherwise.
 */
inline bool buf_page_is_old(const buf_page_t *bpage) {
  ut_ad(bpage->in_file());
  ut_ad(buf_pool_mutex_own());

  return bpage->m_old;
}

/**
 * Flag a block as old.
 *
 * @param bpage Pointer to the control block.
 * @param old Flag indicating if the block is old.
 */
inline void buf_page_set(buf_page_t *bpage, bool old) {
  ut_a(bpage->in_file());
  ut_ad(buf_pool_mutex_own());
  ut_ad(bpage->m_in_LRU_list);

  bpage->m_old = old;
}

/**
 * Determine the time of first access of a block in the buffer pool.
 *
 * @param bpage Pointer to the control block.
 * @return The time of first access (ut_time_ms()) if the block has been accessed, 0 otherwise.
 */
inline unsigned buf_page_is_accessed(const buf_page_t *bpage) {
  ut_ad(bpage->in_file());

  return bpage->m_access_time;
}

/**
 * Flag a block as accessed.
 *
 * @param bpage Pointer to the control block.
 * @param time_ms The current time in milliseconds.
 */
inline void buf_page_set_accessed(buf_page_t *bpage, ulint time_ms) {
  ut_a(bpage->in_file());
  ut_ad(buf_pool_mutex_own());

  if (!bpage->m_access_time) {
    /* Make this the time of the first access. */
    bpage->m_access_time = time_ms;
  }
}

/**
 * Get the buf_block_t handle of a buffered file block if an uncompressed page frame exists, or nullptr.
 *
 * @param bpage Pointer to the control block.
 * @return The control block if an uncompressed page frame exists, or nullptr.
 */
inline buf_block_t *buf_page_get_block(buf_page_t *bpage) {
  if (likely(bpage != nullptr)) {
    ut_ad(bpage->in_file());

    if (bpage->get_state() == BUF_BLOCK_FILE_PAGE) {
      return reinterpret_cast<buf_block_t *>(bpage);
    }
  }

  return nullptr;
}

/**
 * @brief Gets the space id, page no, and byte offset within page of a pointer pointing to a buffer frame containing a file page.
 *
 * @param ptr Pointer to a buffer frame.
 * @param space Pointer to store the space id.
 * @param addr Pointer to store the page offset and byte offset.
 */
inline void buf_ptr_get_fsp_addr(const void *ptr, ulint *space, fil_addr_t *addr) {
  auto page = reinterpret_cast<const page_t *>(ut_align_down(ptr, UNIV_PAGE_SIZE));

  *space = mach_read_from_4(page + FIL_PAGE_SPACE_ID);
  addr->m_page_no = mach_read_from_4(page + FIL_PAGE_OFFSET);
  addr->m_boffset = ut_align_offset(ptr, UNIV_PAGE_SIZE);
}

/**
 * Gets the hash value of the page the pointer is pointing to. This can be used in searches in the lock hash table.
 *
 * @param block Pointer to the buffer block.
 * @return Lock hash value.
 */
inline ulint buf_block_get_lock_hash_val(const buf_block_t *block) {
  ut_ad(block->m_page.in_file());

  IF_SYNC_DEBUG(ut_ad(
                  rw_lock_own(&(((buf_block_t *)block)->m_rw_lock), RW_LOCK_EXCLUSIVE) ||
                  rw_lock_own(&(((buf_block_t *)block)->m_rw_lock), RW_LOCK_SHARED)
  );)

  return block->m_lock_hash_val;
}

/**
 * @brief Copies contents of a buffer frame to a given buffer.
 *
 * @param buf Pointer to the buffer to copy to.
 * @param frame Pointer to the buffer frame.
 * @return Pointer to the copied buffer.
 */
inline byte *buf_frame_copy(byte *buf, const buf_frame_t *frame) {
  memcpy(buf, frame, UNIV_PAGE_SIZE);

  return buf;
}

/**
 * @brief Calculates a folded value of a file page address to use in the page hash table.
 *
 * @param space Space id.
 * @param page_no Page number within space.
 * @return The folded value.
 */
inline ulint buf_page_address_fold(space_id_t space, page_no_t page_no) {
  return (space << 20) + space + page_no;
}

/**
 * @brief Gets the youngest modification log sequence number for a frame.
 * Returns zero if not a file page or no modification occurred yet.
 *
 * @param bpage Pointer to the block containing the page frame.
 * @return The newest modification to the page.
 */
inline lsn_t buf_page_get_newest_modification(const buf_page_t *bpage) {
  auto block_mutex = buf_page_get_mutex(bpage);

  mutex_enter(block_mutex);

  lsn_t lsn;

  if (bpage->in_file()) {
    lsn = bpage->m_newest_modification;
  } else {
    lsn = 0;
  }

  mutex_exit(block_mutex);

  return lsn;
}

/**
 * @brief Increments the modify clock of a frame by 1.
 * The caller must (1) own the srv_buf_pool mutex and block bufferfix count has to be zero,
 * (2) or own an x-lock on the block.
 *
 * @param block Pointer to the buffer block.
 */
inline void buf_block_modify_clock_inc(buf_block_t *block) {

  IF_SYNC_DEBUG(
    ut_ad((buf_pool_mutex_own() && block->m_page.buf_fix_count == 0) || rw_lock_own(&block->m_rw_lock, RW_LOCK_EXCLUSIVE));
  )

  ++block->m_modify_clock;
}

/**
 * @brief Returns the value of the modify clock.
 * The caller must have an s-lock or x-lock on the block.
 *
 * @param block Pointer to the buffer block.
 * @return The value of the modify clock.
 */
inline uint64_t buf_block_get_modify_clock(buf_block_t *block) {

  IF_SYNC_DEBUG(ut_ad(rw_lock_own(&(block->m_rw_lock), RW_LOCK_SHARED) || rw_lock_own(&(block->m_rw_lock), RW_LOCK_EXCLUSIVE));)

  return block->m_modify_clock;
}

/**
 * @brief Increments the bufferfix count.
 *
 * @param file File name.
 * @param line Line number.
 * @param block Pointer to the buffer block.
 */
inline void buf_block_buf_fix_inc_func(IF_SYNC_DEBUG(const char *file, ulint line, ) buf_block_t *block) {
  IF_SYNC_DEBUG(auto ret = rw_lock_s_lock_nowait(&(block->m_debug_latch), file, line); ut_a(ret););

  ut_ad(mutex_own(&block->m_mutex));

  ++block->m_page.m_buf_fix_count;
}

#define buf_block_buf_fix_inc(b, f, l) buf_block_buf_fix_inc_func(IF_SYNC_DEBUG(f, l, ) b)

inline void buf_block_t::fix_dec() {
  ut_ad(mutex_own(&m_mutex));

  --m_page.m_buf_fix_count;

  IF_SYNC_DEBUG(rw_lock_s_unlock(&m_debug_latch));
}

inline buf_page_t *Buf_pool::hash_get_page(space_id_t space_id, page_no_t page_no) {
  ut_ad(buf_pool_mutex_own());

  // Look for the page in the hash table
  buf_page_t *bpage{nullptr};
  if (auto it = m_page_hash->find(Page_id(space_id, page_no)); it != m_page_hash->end()) {
    bpage = it->second;
  }

  if (bpage != nullptr) {
    ut_a(bpage->in_file());
    ut_ad(bpage->m_in_page_hash);
    UNIV_MEM_ASSERT_RW(bpage, sizeof(*bpage));
  }

  return bpage;
}

inline buf_block_t *Buf_pool::hash_get_block(space_id_t space, page_no_t page_no) {
  return buf_page_get_block(hash_get_page(space, page_no));
}

inline bool Buf_pool::peek(space_id_t space_id, page_no_t page_no) {
  buf_pool_mutex_enter();

  auto bpage = hash_get_page(space_id, page_no);

  buf_pool_mutex_exit();

  return bpage != nullptr;
}

inline buf_frame_t *buf_block_t::get_frame() const {
#ifdef UNIV_DEBUG
  switch (get_state()) {
    default:
    case BUF_BLOCK_NOT_USED:
      ut_error;
      break;
    case BUF_BLOCK_FILE_PAGE:
      ut_a(m_page.m_buf_fix_count > 0 || buf_block_get_io_fix(this) == BUF_IO_READ);
      /* fall through */
    case BUF_BLOCK_MEMORY:
    case BUF_BLOCK_REMOVE_HASH:
    case BUF_BLOCK_READY_FOR_USE:
      break;
  }
#endif /* UNIV_DEBUG */

  return reinterpret_cast<buf_frame_t *>(m_frame);
}

/**
 * @brief Adds latch level info for the rw-lock protecting the buffer frame.
 *
 * This should be called in the debug version after a successful latching of a page
 * if we know the latching order level of the acquired latch.
 *
 * @param block The buffer page where we have acquired latch.
 * @param level The latching order level.
 */
inline void buf_block_dbg_add_level(IF_SYNC_DEBUG(buf_block_t *block, ulint level)) {
  IF_SYNC_DEBUG(sync_thread_add_level(&block->m_rw_lock, level));
}

/** Releases a latch, if specified.
@param[in] block             Block for which to release the latch
@param[in] rw_latch          The latch type. */
inline void buf_page_release_latch(buf_block_t *block, ulint rw_latch) {
  if (rw_latch == RW_S_LATCH) {
    rw_lock_s_unlock(&block->m_rw_lock);
  } else if (rw_latch == RW_X_LATCH) {
    rw_lock_x_unlock(&block->m_rw_lock);
  }
}

inline uint32_t buf_page_data_calc_checksum(const byte *page) {
  return crc32::checksum(page + FIL_PAGE_OFFSET, UNIV_PAGE_SIZE - FIL_PAGE_DATA);
}
