/* Host-side shims for symbols the iOS app link needs but cannot see.
 *
 * Two unrelated groups:
 *
 * 1. pipe2(). Linux-only; iOS has no such call. wine's process_ios.c and
 *    server_ios.c call it directly. Implemented here on pipe() + FD_CLOEXEC.
 *
 * 2. FEX symbols that live in the ARM64EC PE module (xtajit64.dll), not in
 *    the iOS host link: IosJitAlias.cpp and Module.cpp are compiled by
 *    llvm-mingw into the PE, while FEXCore/Core.cpp and ArchHelpers/Arm64.cpp
 *    reference them from code that is NOT inside an #ifdef FEX_IOS_HOST guard
 *    (Core.cpp's guard closes at 1398 and the FFS/cb-entry reporters that
 *    follow sit outside it; same for the rpm-cas probe and Arm64.cpp's
 *    IosMonoResolveRW fallback). In the PE build those resolve; in the host
 *    build they dangle. These definitions make the host link close.
 *
 *    Every one of them is a diagnostic read EXCEPT IosMonoResolveRW -- see
 *    the note on it below. The real implementations still run inside the PE
 *    module, which is where the counters are actually written.
 */
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0x1000000
#endif

int pipe2(int fds[2], int flags);
int pipe2(int fds[2], int flags) {
  if (pipe(fds) != 0) {
    return -1;
  }
  if (flags & O_CLOEXEC) {
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) == -1 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) == -1) {
      close(fds[0]);
      close(fds[1]);
      return -1;
    }
  }
  if (flags & O_NONBLOCK) {
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) == -1 || fcntl(fds[1], F_SETFL, O_NONBLOCK) == -1) {
      close(fds[0]);
      close(fds[1]);
      return -1;
    }
  }
  return 0;
}

/* ExitToX64's FFS-bypass counters. Written by Module.S inside the PE; the
 * host-side reporter only ever reads them, and reports nothing while they
 * stay zero. */
uint64_t IosFfsBypassLog[4] = {0, 0, 0, 0};

/* Maps a module-pool-copy address back to its PE VA. The PE implementation
 * documents "returns the input unchanged on no match", so returning the input
 * IS the no-match answer rather than an invented one. */
uint64_t IosJitReverseTranslate(uint64_t Addr);
uint64_t IosJitReverseTranslate(uint64_t Addr) {
  return Addr;
}

/* Mono-bridge pending-fault queue. Host side has no bridge armed, so there is
 * never a pending capture and the counters are zero. */
int ios_fex_mono_take_pending(uint64_t *BlockBegin, uint64_t *HostPC, uint64_t *FaultAddr);
int ios_fex_mono_take_pending(uint64_t *BlockBegin, uint64_t *HostPC, uint64_t *FaultAddr) {
  (void)BlockBegin;
  (void)HostPC;
  (void)FaultAddr;
  return 0;
}

uint64_t ios_fex_mono_captured_count(void);
uint64_t ios_fex_mono_captured_count(void) {
  return 0;
}

void ios_fex_mono_count_activated(void);
void ios_fex_mono_count_activated(void) {}

void ios_fex_mono_count_helper(int Miss);
void ios_fex_mono_count_helper(int Miss) {
  (void)Miss;
}

/* Gates the entire host-side mono path: Core.cpp:1321 does
 * `return ios_fex_mono_bridge_armed() != 0;`, so 0 means "no bridge on this
 * side" and the path is skipped rather than half-entered. */
int ios_fex_mono_bridge_armed(void);
int ios_fex_mono_bridge_armed(void) {
  return 0;
}

/* The ONLY non-diagnostic one. Arm64.cpp asks it for a writable alias of a
 * code address after the fixed JIT pool (WINE_IOS_JIT_RX) has already missed,
 * and treats 0 as "no alias found" -- `if (Rw) { ...; return Rw; }` -- so 0
 * falls through to the behaviour that predates that fallback (ml656) rather
 * than to anything undefined. The anonymous alias table it consults lives in
 * the PE module and is populated there; a host-side caller has no view of it.
 */
uint64_t IosMonoResolveRW(uint64_t GuestAddr, uint64_t Size);
uint64_t IosMonoResolveRW(uint64_t GuestAddr, uint64_t Size) {
  (void)GuestAddr;
  (void)Size;
  return 0;
}

/* rpmalloc CAS-spin snapshot. The fork's rpmalloc is built for the PE side
 * (its sources take the _WIN32 path and do not compile for an iOS host), so
 * the host has no snapshot to take. Core.cpp gates its [rpm-cas] log on a
 * non-zero return, so 0 simply prints nothing. */
struct rpm_cas_snapshot;
int rpm_cas_snapshot_take(struct rpm_cas_snapshot *out);
int rpm_cas_snapshot_take(struct rpm_cas_snapshot *out) {
  (void)out;
  return 0;
}
