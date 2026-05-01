#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// =======================================================================
// Enhanced Write-Ahead Logging (Journaling) for xv6
//
// Enhancements over the baseline xv6 log:
//   1. CRC32 checksum on every log block for integrity verification.
//   2. Journal statistics: commits, recoveries, checksum errors tracked.
//   3. get_journal_stats() exported so a user-space syscall can query them.
//
// The on-disk log format:
//   [header block]  - contains n (block count), block numbers, checksums
//   [block 0 data]
//   [block 1 data]
//   ...
// =======================================================================

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

// -----------------------------------------------------------------------
// Enhanced log header: stores per-block checksums alongside block numbers.
// -----------------------------------------------------------------------
struct logheader {
  int  n;               // number of log entries
  int  block[LOGSIZE];  // destination block numbers
  uint cksum[LOGSIZE];  // CRC32 checksum of each log block's data
};

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
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
  jstats.total_commits    = 0;
  jstats.total_recoveries = 0;
  jstats.checksum_errors  = 0;
  jstats.blocks_written   = 0;
  jstats.blocks_installed = 0;

  cprintf("journal: initialising log at block %d, size %d\n",
          log.start, log.size);

  recover_from_log();
}

// -----------------------------------------------------------------------
// install_trans — copy committed blocks from the log to their home
// location, verifying the CRC32 checksum of each block first.
// -----------------------------------------------------------------------
static void
install_trans(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *lbuf = bread(log.dev, log.start + tail + 1); // log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]);   // home block

    // --- Integrity check: verify CRC32 of the log block data ---
    uint computed = crc32((uchar*)lbuf->data, BSIZE);
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

    memmove(dbuf->data, lbuf->data, BSIZE); // copy block to home location
    bwrite(dbuf);                           // write to disk
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
    log.lh.block[i] = lh->block[i];
    log.lh.cksum[i] = lh->cksum[i];  // restore per-block checksums
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
    hb->block[i] = log.lh.block[i];
    hb->cksum[i] = log.lh.cksum[i];  // persist per-block checksums
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
      release(&log.lock);
      break;
    }
  }
}

// -----------------------------------------------------------------------
// end_op — called at the end of every FS system call.
// Commits if this was the last outstanding operation.
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
    do_commit = 1;
    log.committing = 1;
  } else {
    // begin_op() may be waiting for log space; wake it up.
    wakeup(&log);
  }
  release(&log.lock);

  if (do_commit) {
    // Call commit() without holding any lock (not allowed to sleep with locks).
    commit();
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);
    release(&log.lock);
  }
}

// -----------------------------------------------------------------------
// write_log — copy modified blocks from the buffer cache into the log
// area on disk, computing and storing a CRC32 for each block.
// -----------------------------------------------------------------------
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to   = bread(log.dev, log.start + tail + 1);  // log slot
    struct buf *from = bread(log.dev, log.lh.block[tail]);    // cached block

    memmove(to->data, from->data, BSIZE);

    // Compute and store CRC32 checksum for this block.
    log.lh.cksum[tail] = crc32((uchar*)to->data, BSIZE);

    bwrite(to);  // write log block to disk
    jstats.blocks_written++;
    brelse(from);
    brelse(to);
  }
}

// -----------------------------------------------------------------------
// commit — the core journaling transaction commit sequence:
//   Phase 1: write data to log area  (write_log)
//   Phase 2: write header to disk    (write_head)  <- atomic commit point
//   Phase 3: install to home blocks  (install_trans)
//   Phase 4: clear the log           (write_head with n=0)
// -----------------------------------------------------------------------
static void
commit(void)
{
  if (log.lh.n > 0) {
    write_log();     // Phase 1: write modified blocks to log on disk
    write_head();    // Phase 2: write header — this IS the commit point
    install_trans(); // Phase 3: install writes to their home locations
    log.lh.n = 0;
    write_head();    // Phase 4: erase the transaction from the log
    jstats.total_commits++;
  }
}

// -----------------------------------------------------------------------
// log_write — mark a buffer as part of the current transaction.
// Replaces bwrite(); the actual disk write happens at commit time.
//
// Typical use:
//   bp = bread(dev, blockno)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
// -----------------------------------------------------------------------
void
log_write(struct buf *b)
{
  int i;

  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("log_write: transaction too large");
  if (log.outstanding < 1)
    panic("log_write: called outside of a transaction");

  acquire(&log.lock);
  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)  // log absorption: reuse existing slot
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n)
    log.lh.n++;         // new block — extend the log
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
  out->total_commits    = jstats.total_commits;
  out->total_recoveries = jstats.total_recoveries;
  out->checksum_errors  = jstats.checksum_errors;
  out->blocks_written   = jstats.blocks_written;
  out->blocks_installed = jstats.blocks_installed;
}
