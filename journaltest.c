// journaltest.c - User-space test program for the enhanced xv6 journaling system.
//
// Tests the Write-Ahead Log (WAL) by:
//   1. Creating files and writing data through the journal.
//   2. Reading back and verifying data integrity byte-by-byte.
//   3. Exercising directory operations through the log.
//   4. Querying journal statistics via the journalstat() syscall.
//   5. Printing a clear pass/fail report.
//
// Run from the xv6 shell:  $ journaltest

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "journalstat.h"

// -----------------------------------------------------------------------
// Helper: write a string directly (no format parsing needed)
// -----------------------------------------------------------------------
static void
prints(const char *s)
{
  int i = 0;
  while (s[i]) i++;
  write(1, s, i);
}

// -----------------------------------------------------------------------
// Helper: print a single decimal integer
// -----------------------------------------------------------------------
static void
printi(int n)
{
  printf(1, "%d", n);
}

// -----------------------------------------------------------------------
// Helper: print a single hex integer
// -----------------------------------------------------------------------
static void
printx(int n)
{
  printf(1, "%x", n);
}

// -----------------------------------------------------------------------
// Helper: portable byte comparison (xv6 user libc has no memcmp)
// Returns 1 if equal, 0 otherwise.
// -----------------------------------------------------------------------
static int
bytes_equal(const char *a, const char *b, int n)
{
  int i;
  for (i = 0; i < n; i++)
    if (a[i] != b[i])
      return 0;
  return 1;
}

// -----------------------------------------------------------------------
// phase1_create_and_write
//   Create TEST_FILES files and write TEST_WRITES x BUF_SIZE bytes each.
//   Every open/write/close wraps a complete FS transaction via the journal.
// -----------------------------------------------------------------------
#define TEST_FILES  5
#define TEST_WRITES 4
#define BUF_SIZE    256

static int
phase1_create_and_write(void)
{
  char fname[16];
  char buf[BUF_SIZE];
  int fd, i, j, k;

  prints("\n[Phase 1] Creating files and writing through the journal...\n");

  for (i = 0; i < TEST_FILES; i++) {
    // Build filename: "jtest0" .. "jtest4"
    fname[0]='j'; fname[1]='t'; fname[2]='e';
    fname[3]='s'; fname[4]='t'; fname[5]='0'+i; fname[6]=0;

    fd = open(fname, O_CREATE | O_RDWR);
    if (fd < 0) {
      prints("  ERROR: could not create "); prints(fname); prints("\n");
      return -1;
    }

    for (j = 0; j < TEST_WRITES; j++) {
      for (k = 0; k < BUF_SIZE; k++)
        buf[k] = (char)((i * TEST_WRITES + j + k) & 0xFF);
      if (write(fd, buf, BUF_SIZE) != BUF_SIZE) {
        prints("  ERROR: write failed on "); prints(fname); prints("\n");
        close(fd);
        return -1;
      }
    }
    close(fd);
    prints("  Created "); prints(fname); prints(" (");
    printi(TEST_WRITES * BUF_SIZE); prints(" bytes)\n");
  }
  return 0;
}

// -----------------------------------------------------------------------
// phase2_read_and_verify
//   Reopen each file and verify every byte matches the written pattern.
// -----------------------------------------------------------------------
static int
phase2_read_and_verify(void)
{
  char fname[16];
  char buf[BUF_SIZE];
  char expected[BUF_SIZE];
  int fd, i, j, k, n;
  int errors = 0;

  prints("\n[Phase 2] Reading back and verifying data integrity...\n");

  for (i = 0; i < TEST_FILES; i++) {
    fname[0]='j'; fname[1]='t'; fname[2]='e';
    fname[3]='s'; fname[4]='t'; fname[5]='0'+i; fname[6]=0;

    fd = open(fname, O_RDONLY);
    if (fd < 0) {
      prints("  ERROR: could not open "); prints(fname); prints("\n");
      errors++;
      continue;
    }

    for (j = 0; j < TEST_WRITES; j++) {
      n = read(fd, buf, BUF_SIZE);
      if (n != BUF_SIZE) {
        prints("  ERROR: short read on "); prints(fname); prints("\n");
        errors++;
        continue;
      }
      for (k = 0; k < BUF_SIZE; k++)
        expected[k] = (char)((i * TEST_WRITES + j + k) & 0xFF);

      if (!bytes_equal(buf, expected, BUF_SIZE)) {
        prints("  CORRUPTION in "); prints(fname);
        prints(" block "); printi(j); prints("!\n");
        errors++;
      }
    }
    close(fd);
    prints("  "); prints(fname); prints(": ");
    prints(errors == 0 ? "PASS\n" : "FAIL\n");
  }
  return errors;
}

// -----------------------------------------------------------------------
// phase3_cleanup
//   Unlink all test files. Each unlink is a separate logged transaction.
// -----------------------------------------------------------------------
static void
phase3_cleanup(void)
{
  char fname[16];
  int i;

  prints("\n[Phase 3] Removing test files (each unlink = 1 transaction)...\n");
  for (i = 0; i < TEST_FILES; i++) {
    fname[0]='j'; fname[1]='t'; fname[2]='e';
    fname[3]='s'; fname[4]='t'; fname[5]='0'+i; fname[6]=0;
    if (unlink(fname) < 0) {
      prints("  WARNING: could not remove "); prints(fname); prints("\n");
    } else {
      prints("  Removed "); prints(fname); prints("\n");
    }
  }
}

// -----------------------------------------------------------------------
// phase4_mkdir_rmdir
//   Exercise directory-level journaling.
// -----------------------------------------------------------------------
static void
phase4_mkdir_rmdir(void)
{
  prints("\n[Phase 4] Directory create / remove (logged transactions)...\n");
  if (mkdir("jtestdir") < 0) {
    prints("  ERROR: mkdir jtestdir failed\n");
    return;
  }
  prints("  Created directory: jtestdir\n");
  if (unlink("jtestdir") < 0)
    prints("  WARNING: could not remove jtestdir\n");
  else
    prints("  Removed directory: jtestdir\n");
}

// -----------------------------------------------------------------------
// print_journal_report
//   Call journalstat() and display a formatted statistics report.
// -----------------------------------------------------------------------
static void
print_journal_report(int data_errors)
{
  struct journal_stats js;

  if (journalstat(&js) < 0) {
    prints("ERROR: journalstat() syscall failed\n");
    return;
  }

  prints("\n");
  prints("+------------------------------------------+\n");
  prints("|    XV6 ENHANCED JOURNAL STATISTICS       |\n");
  prints("+------------------------------------------+\n");
  prints("| Total commits              : "); printi(js.total_commits);    prints("\n");
  prints("| Crash recoveries (on boot) : "); printi(js.total_recoveries); prints("\n");
  prints("| Log blocks written         : "); printi(js.blocks_written);   prints("\n");
  prints("| Log blocks installed       : "); printi(js.blocks_installed); prints("\n");
  prints("| CRC32 checksum errors      : "); printi(js.checksum_errors);  prints("\n");
  prints("+------------------------------------------+\n");

  prints("|\n");
  if (js.checksum_errors == 0)
    prints("|  Journal CRC32 : PASS (0 checksum errors)\n");
  else {
    prints("|  Journal CRC32 : FAIL ("); printi(js.checksum_errors);
    prints(" errors)\n");
  }

  if (data_errors == 0)
    prints("|  Data verify   : PASS (all bytes match)\n");
  else {
    prints("|  Data verify   : FAIL ("); printi(data_errors);
    prints(" block(s) corrupted)\n");
  }
  prints("|\n");
  prints("+------------------------------------------+\n");

  if (js.total_recoveries > 0) {
    prints("\nNOTE: Crash recovery ran on boot (");
    printi(js.total_recoveries);
    prints(" time(s)).\n");
    prints("      Committed transactions were replayed safely.\n");
  } else {
    prints("\nNOTE: No crash recovery needed on this boot.\n");
  }
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int
main(void)
{
  int data_errors = 0;

  prints("\n");
  prints("================================================\n");
  prints("  XV6 Enhanced Journaling Test\n");
  prints("  WAL + CRC32 Integrity Checking\n");
  prints("================================================\n");

  if (phase1_create_and_write() < 0) {
    prints("FATAL: Phase 1 failed. Aborting.\n");
    exit();
  }

  data_errors  = phase2_read_and_verify();
  phase3_cleanup();
  phase4_mkdir_rmdir();

  print_journal_report(data_errors);

  prints("\njournaltest complete.\n\n");
  exit();
}
