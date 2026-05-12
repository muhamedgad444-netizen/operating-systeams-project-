// crash.c - Controlled crash/fault-injection tool for the enhanced journal.
//
// Usage:
//   crash 1   — panic BEFORE the commit header is written (pre-commit)
//   crash 2   — panic AFTER the commit header but BEFORE install (post-commit)
//   crash 3   — corrupt a log block, then panic (fault injection)
//
// The tool first sets crash_at_phase via the crash() syscall, then performs
// a real filesystem write so that the journal's commit() path is exercised.
// The kernel will panic at the chosen phase inside commit().

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int
main(int argc, char *argv[])
{
  if(argc < 2){
    printf(1, "Usage: crash <phase>\n");
    printf(1, "  1 = Crash before commit point   (no recovery expected)\n");
    printf(1, "  2 = Crash after commit point     (recovery WILL replay)\n");
    printf(1, "  3 = Corrupt log + crash          (CRC32 catches it)\n");
    exit();
  }

  int phase = atoi(argv[1]);
  printf(1, "Setting crash_at_phase = %d\n", phase);

  // Arm the crash trigger in the kernel.
  crash(phase);

  // Now perform a REAL filesystem operation so that begin_op / end_op /
  // commit() actually runs — this is where the kernel will panic.
  printf(1, "Performing filesystem write to trigger commit...\n");

  int fd = open("__crash_trigger__", O_CREATE | O_RDWR);
  if(fd < 0){
    printf(1, "ERROR: could not create trigger file\n");
    exit();
  }
  char buf[64];
  int i;
  for(i = 0; i < 64; i++) buf[i] = 'X';
  write(fd, buf, 64);
  close(fd);

  // If we reach here the crash did NOT fire (should not happen).
  printf(1, "ERROR: System did not crash! (phase %d was not triggered)\n", phase);
  unlink("__crash_trigger__");
  exit();
}
