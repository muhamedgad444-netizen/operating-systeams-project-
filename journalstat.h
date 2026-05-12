// journalstat.h - Journal statistics structure shared between kernel and user space.
// Included by: log.c, sysproc.c (kernel) and journaltest.c, journalbench.c (user).

#ifndef JOURNALSTAT_H
#define JOURNALSTAT_H

struct journal_stats {
  int total_commits;        // number of successful commits
  int total_recoveries;     // number of crash recoveries replayed on boot
  int checksum_errors;      // log blocks that failed CRC32 verification
  int blocks_written;       // cumulative log blocks written to the journal area
  int blocks_installed;     // cumulative log blocks installed to home locations
  int bytes_logged;         // cumulative bytes logged (partial-block metric)
  int group_commit_batches; // commits that batched >1 ops together
  int flush_barriers;       // number of ideflush() hardware barriers issued
  int max_batch_size;       // largest number of ops batched in a single commit
  int total_ops_batched;    // cumulative ops committed (for computing avg batch)
};

#endif // JOURNALSTAT_H
