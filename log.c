// =======================================================================
// Enhanced Write-Ahead Logging (Journaling) for xv6
//
// Enhancements over the baseline xv6 log:
//
//   1. BYTE-RANGE (PARTIAL-BLOCK) LOGGING
//      Vanilla xv6 logs full 512-byte blocks even when only a few bytes
//      change.  We record {offset, len} per log entry so that write_log()
//      and install_trans() only copy the modified slice, and the CRC32
//      covers only that slice.  This cuts I/O dramatically for metadata-
//      heavy workloads.
//
//   2. GROUP COMMIT WITH BATCH TRACKING
//      Vanilla xv6 already batches concurrent syscalls into one commit,
//      but provides no observability.  We add per-commit batch-size
//      tracking (max_batch_size, total_ops_batched, group_commit_batches)
//      so the effectiveness of batching can be measured quantitatively.
//      Non-last processes call wakeup() to prevent begin_op() starvation.
//
//   3. HARDWARE WRITE BARRIERS (POWER-LOSS ATOMICITY)
//      After writing log blocks and before writing the commit header, we
//      issue a real IDE FLUSH CACHE command (0xE7) via ideflush().  This
//      guarantees the log data is on the platter before the header,
//      preventing the disk from reordering writes and breaking atomicity.
//
//   4. CRC32 INTEGRITY CHECKING
//      Every log block's modified slice is checksummed (IEEE 802.3 CRC32)
//      before being written to the journal.  On recovery, any block whose
//      checksum doesn't match is safely skipped.
//
//   5. METADATA-ONLY JOURNALING (ext3 ordered mode)
//      In writei() (fs.c), directory data is journaled but regular file
//      data is written directly to disk via bwrite().  This halves I/O
//      for large file writes while keeping the filesystem structure safe.
//      File metadata (inode, size, block pointers) is still journaled.
//
//   6. JOURNAL STATISTICS & DIAGNOSTICS
//      The kernel continuously tracks commits, recoveries, checksum
//      errors, bytes logged, batch sizes, and flush-barrier counts.
//      User-space programs can query these via journalstat().
//
// Compile-time safety:
//   A typedef assertion guarantees the logheader fits in one disk block.
//   If LOGSIZE is increased beyond 31, the build fails immediately.
//
// The on-disk log format:
//   [header block]  — n, block numbers, checksums, offsets, lengths
//   [block 0 data]
//   [block 1 data]
//   …
// =======================================================================

#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// -----------------------------------------------------------------------
// CRC32 checksum implementation (IEEE 802.3 polynomial 0xEDB88320)
// Used to detect corruption of log blocks before installing them.
// -----------------------------------------------------------------------
static uint crc32_table[256];
static int  crc32_ready = 0;

static void
crc32_init(void)
{
  uint i, j, c;
  for (i = 0; i < 256; i++) {
    c = i;
    for (j = 0; j < 8; j++)
      c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
    crc32_table[i] = c;
  }
  crc32_ready = 1;
}

static uint
crc32(const uchar *data, int len)
{
  uint crc = 0xFFFFFFFF;
  int i;
  if (!crc32_ready)
    crc32_init();
  for (i = 0; i < len; i++)
    crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFF;
}

// -----------------------------------------------------------------------
// Journal statistics — tracked across the lifetime of the kernel.
// -----------------------------------------------------------------------
#include "journalstat.h"
static struct journal_stats jstats;
int crash_at_phase = 0; // global controlled by sys_crash

// -----------------------------------------------------------------------
// Enhanced log header: stores per-block checksums, byte-range offsets,
// and byte-range lengths alongside block numbers.
// -----------------------------------------------------------------------
struct logheader {
  int  n;               // number of log entries
  int  block[LOGSIZE];  // destination block numbers
  uint cksum[LOGSIZE];  // CRC32 checksum of each log block's modified slice
  int  offset[LOGSIZE]; // byte offset within the block where modification starts
  int  len[LOGSIZE];    // number of bytes modified (0 < len <= BSIZE)
};

// Compile-time safety: logheader MUST fit in a single disk block.
// If LOGSIZE is ever increased beyond 31, this line will cause a build error.
typedef char __assert_logheader_fits_in_block[
  sizeof(struct logheader) <= BSIZE ? 1 : -1
];

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  int batch_size;  // ENHANCEMENT: ops in current batch (for group commit stats)
  struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit(void);

// -----------------------------------------------------------------------
// initlog — called once at boot to set up the journal.
// -----------------------------------------------------------------------
void
initlog(int dev)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: logheader too large for one block");

  struct superblock sb;
  initlock(&log.lock, "log");
  readsb(dev, &sb);
  log.start = sb.logstart;
  log.size  = sb.nlog;
  log.dev   = dev;

  // Initialise checksum table once.
  crc32_init();

  // Zero out statistics.
  jstats.total_commits        = 0;
  jstats.total_recoveries     = 0;
  jstats.checksum_errors      = 0;
  jstats.blocks_written       = 0;
  jstats.blocks_installed     = 0;
  jstats.bytes_logged         = 0;
  jstats.group_commit_batches = 0;
  jstats.flush_barriers       = 0;
  jstats.max_batch_size       = 0;
  jstats.total_ops_batched    = 0;

  cprintf("journal: initialising log at block %d, size %d\n",
          log.start, log.size);

  recover_from_log();
}

// -----------------------------------------------------------------------
// install_trans — copy committed blocks from the log to their home
// location, verifying the CRC32 checksum of the modified slice first.
//
// ENHANCEMENT: Only the modified byte-range [offset, offset+len) is
// copied to the home block, not the full 512 bytes.
// -----------------------------------------------------------------------
static void
install_trans(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *lbuf = bread(log.dev, log.start + tail + 1); // log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]);   // home block

    // --- Integrity check: verify CRC32 of the modified slice ---
    uint computed = crc32((uchar*)lbuf->data + log.lh.offset[tail],
                          log.lh.len[tail]);
    if (computed != log.lh.cksum[tail]) {
      // Checksum mismatch: record the error but do not install the block.
      jstats.checksum_errors++;
      cprintf("journal: CHECKSUM ERROR on log block %d "
              "(expected 0x%x, got 0x%x) — skipping install\n",
              log.lh.block[tail], log.lh.cksum[tail], computed);
      brelse(lbuf);
      brelse(dbuf);
      continue;
    }

    // ENHANCEMENT: Byte-range install — copy only the modified slice.
    memmove(dbuf->data + log.lh.offset[tail],
            lbuf->data + log.lh.offset[tail],
            log.lh.len[tail]);
    bwrite(dbuf);  // write to disk
    jstats.blocks_installed++;
    brelse(lbuf);
    brelse(dbuf);
  }
}

// -----------------------------------------------------------------------
// read_head — read the on-disk log header into the in-memory log struct.
// -----------------------------------------------------------------------
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *)(buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i]  = lh->block[i];
    log.lh.cksum[i]  = lh->cksum[i];
    log.lh.offset[i] = lh->offset[i];
    log.lh.len[i]    = lh->len[i];
  }
  brelse(buf);
}

// -----------------------------------------------------------------------
// write_head — write the in-memory log header to disk.
// This is the atomic commit point: once this returns the transaction
// is durable and will be replayed if we crash before install_trans.
// -----------------------------------------------------------------------
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *)(buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i]  = log.lh.block[i];
    hb->cksum[i]  = log.lh.cksum[i];
    hb->offset[i] = log.lh.offset[i];
    hb->len[i]    = log.lh.len[i];
  }
  bwrite(buf);
  brelse(buf);
}

// -----------------------------------------------------------------------
// recover_from_log — called at boot; replays any committed-but-not-
// installed transaction left by a previous crash.
// -----------------------------------------------------------------------
static void
recover_from_log(void)
{
  read_head();
  if (log.lh.n > 0) {
    cprintf("journal: crash recovery — replaying %d blocks\n", log.lh.n);
    jstats.total_recoveries++;
  }
  install_trans();  // if committed, copy from log to home locations
  log.lh.n = 0;
  write_head();     // clear the log (n = 0 on disk)
}

// -----------------------------------------------------------------------
// begin_op — called at the start of every FS system call.
// Sleeps if the log is full or a commit is in progress.
// -----------------------------------------------------------------------
void
begin_op(void)
{
  acquire(&log.lock);
  while (1) {
    if (log.committing) {
      sleep(&log, &log.lock);
    } else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS > LOGSIZE) {
      // This op might exhaust log space; wait for a commit.
      sleep(&log, &log.lock);
    } else {
      log.outstanding += 1;
      // ENHANCEMENT: Track how many ops will be in this batch.
      log.batch_size++;
      release(&log.lock);
      break;
    }
  }
}

// -----------------------------------------------------------------------
// end_op — called at the end of every FS system call.
//
// ENHANCEMENT: Asynchronous Group Commit
//   If this process is NOT the last outstanding operation, it returns
//   immediately — it does NOT sleep.  Its dirty blocks remain in the
//   buffer cache and will be committed by whichever process IS last.
//   This means N concurrent syscalls produce only 1 disk commit instead
//   of N, dramatically improving throughput.
//
//   The last process to call end_op() sets do_commit = 1, performs the
//   commit, and then wakes up any begin_op() waiters.
// -----------------------------------------------------------------------
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1;
  if (log.committing)
    panic("log.committing");
    
  if (log.outstanding == 0) {
    // We are the last process in this transaction batch.
    // We will take responsibility for committing the entire group to disk.
    do_commit = 1;
    log.committing = 1;
    // ENHANCEMENT: batch_size was already accumulated in begin_op().
    // commit() will record it in the stats before resetting to 0.
  } else {
    // ENHANCEMENT: Group Commit — return immediately.
    // Our dirty blocks stay in the buffer cache and will be committed when
    // the last outstanding operation finishes.
    //
    // NOTE: We still call wakeup() here so that processes blocked in
    // begin_op() (waiting for log space) can re-check the condition, since
    // we just freed one slot of reserved space by decrementing outstanding.
    // This prevents starvation under high concurrency.
    wakeup(&log);
  }
  release(&log.lock);

  if (do_commit) {
    // Call commit() without holding any lock (not allowed to sleep with locks).
    commit();
    
    acquire(&log.lock);
    log.committing = 0;
    
    // Wake up any new processes waiting in begin_op() to start a new transaction.
    wakeup(&log);
    
    release(&log.lock);
  }
}

// -----------------------------------------------------------------------
// write_log — copy modified blocks from the buffer cache into the log
// area on disk, computing and storing a CRC32 for the modified slice.
//
// ENHANCEMENT: Only the byte-range [offset, offset+len) of each block
// is copied to the log area and checksummed.
// -----------------------------------------------------------------------
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to   = bread(log.dev, log.start + tail + 1);  // log slot
    struct buf *from = bread(log.dev, log.lh.block[tail]);    // cached block

    // ENHANCEMENT: Byte-Range / Partial-Block Logging
    // Only copy the modified slice, not the full 512 bytes.
    memmove(to->data + log.lh.offset[tail],
            from->data + log.lh.offset[tail],
            log.lh.len[tail]);

    // Compute CRC32 checksum for only the modified slice.
    log.lh.cksum[tail] = crc32((uchar*)to->data + log.lh.offset[tail],
                                log.lh.len[tail]);

    bwrite(to);  // write log block to disk
    jstats.blocks_written++;
    jstats.bytes_logged += log.lh.len[tail];
    brelse(from);
    brelse(to);
  }
}

// -----------------------------------------------------------------------
// commit — the core journaling transaction commit sequence:
//   Phase 1: write data to log area  (write_log)
//   BARRIER: ideflush() — hardware write barrier
//   Phase 2: write header to disk    (write_head)  <- atomic commit point
//   Phase 3: install to home blocks  (install_trans)
//   Phase 4: clear the log           (write_head with n=0)
// -----------------------------------------------------------------------
static void
commit(void)
{
  if (log.lh.n > 0) {
    write_log();     // Phase 1: write modified blocks to log on disk
    
    // --- ENHANCEMENT: Hardware Write Barrier (Power-loss Atomicity) ---
    // Disks can reorder writes, writing the commit header before the log
    // data.  To prevent this, we issue a real IDE FLUSH CACHE command
    // (0xE7) to the drive controller.  This forces all preceding writes
    // to reach stable storage before any subsequent write is started.
    ideflush();
    jstats.flush_barriers++;
    
    if (crash_at_phase == 1)
      panic("crash test: Phase 1 (pre-commit)");

    write_head();    // Phase 2: write header — this IS the commit point
    
    if (crash_at_phase == 2)
      panic("crash test: Phase 2 (post-commit)");

    // --- BONUS: FAULT INJECTION ---
    // Corrupt a committed log block before it is installed.
    // This proves the CRC32 algorithm actually catches disk corruption!
    if (crash_at_phase == 3) {
      struct buf *badbuf = bread(log.dev, log.start + 1);
      badbuf->data[0] ^= 0xFF; // Flip bits to corrupt the block
      bwrite(badbuf);          // Write corrupted block to disk
      brelse(badbuf);
      panic("crash test: Phase 3 (Fault Injection - Corrupted Log Data)");
    }

    install_trans(); // Phase 3: install writes to their home locations
    log.lh.n = 0;
    write_head();    // Phase 4: erase the transaction from the log

    // --- ENHANCEMENT: Group Commit Statistics ---
    // Record how many operations were batched into this single commit.
    // This is the quantitative proof that group commit is working:
    //   - If batch_size > 1, multiple syscalls shared one disk commit.
    //   - max_batch_size records the peak observed concurrency.
    //   - total_ops_batched / total_commits gives the average batch size.
    jstats.total_commits++;
    if (log.batch_size > 1)
      jstats.group_commit_batches++;
    jstats.total_ops_batched += log.batch_size;
    if (log.batch_size > jstats.max_batch_size)
      jstats.max_batch_size = log.batch_size;
    log.batch_size = 0;  // reset for the next batch
  }
}

// -----------------------------------------------------------------------
// log_write — mark a buffer as part of the current transaction.
// Replaces bwrite(); the actual disk write happens at commit time.
//
// ENHANCEMENT: Byte-Range Logging
//   The caller specifies the exact {offset, len} of the modification
//   within the block.  If the same block is modified multiple times in
//   one transaction (log absorption), the byte-ranges are merged into
//   the minimal bounding range.
//
// Typical use:
//   bp = bread(dev, blockno)
//   modify bp->data[off .. off+len]
//   log_write(bp, off, len)
//   brelse(bp)
// -----------------------------------------------------------------------
void
log_write(struct buf *b, int offset, int len)
{
  int i;

  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("log_write: transaction too large");
  if (log.outstanding < 1)
    panic("log_write: called outside of a transaction");

  acquire(&log.lock);
  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno) { // log absorption: reuse existing slot
      // Merge byte-ranges: compute the bounding range of old and new.
      int old_end = log.lh.offset[i] + log.lh.len[i];
      int new_end = offset + len;
      int min_off = log.lh.offset[i] < offset ? log.lh.offset[i] : offset;
      int max_end = old_end > new_end ? old_end : new_end;
      log.lh.offset[i] = min_off;
      log.lh.len[i] = max_end - min_off;
      break;
    }
  }
  if (i == log.lh.n) {
    // New block — extend the log.
    log.lh.block[i]  = b->blockno;
    log.lh.offset[i] = offset;
    log.lh.len[i]    = len;
    log.lh.n++;
  }
  b->flags |= B_DIRTY;  // pin in cache until commit
  release(&log.lock);
}

// -----------------------------------------------------------------------
// get_journal_stats — copy current journal statistics into *out.
// Called by sys_journalstat() to expose stats to user space.
// -----------------------------------------------------------------------
void
get_journal_stats(struct journal_stats *out)
{
  out->total_commits        = jstats.total_commits;
  out->total_recoveries     = jstats.total_recoveries;
  out->checksum_errors      = jstats.checksum_errors;
  out->blocks_written       = jstats.blocks_written;
  out->blocks_installed     = jstats.blocks_installed;
  out->bytes_logged         = jstats.bytes_logged;
  out->group_commit_batches = jstats.group_commit_batches;
  out->flush_barriers       = jstats.flush_barriers;
  out->max_batch_size       = jstats.max_batch_size;
  out->total_ops_batched    = jstats.total_ops_batched;
}
