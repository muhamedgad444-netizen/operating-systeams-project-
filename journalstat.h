// journalstat.h - Journal statistics structure shared between kernel and user space.
// Included by: log.c, sysproc.c (kernel) and journaltest.c (user).

#ifndef JOURNALSTAT_H
#define JOURNALSTAT_H

struct journal_stats {
  int total_commits;    // number of successful commits
  int total_recoveries; // number of crash recoveries replayed on boot
  int checksum_errors;  // log blocks that failed CRC32 verification
  int blocks_written;   // cumulative log blocks written to the journal area
  int blocks_installed; // cumulative log blocks installed to home locations
};

#endif // JOURNALSTAT_H
