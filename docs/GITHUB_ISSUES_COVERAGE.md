# GitHub Issues Coverage

This document tracks NixOS/nix GitHub issues related to process handling bugs,
our test coverage, and fixes implemented in straylight/nix.

## Process Handling / ECHILD Race Conditions

| Issue | Title | Status | Test Coverage | Fix Status |
|-------|-------|--------|---------------|------------|
| [#2176](https://github.com/NixOS/nix/issues/2176) | Builder is sometimes unexpectedly killed | OPEN | `processes_test.cpp`: concurrent reap, SIGCHLD tests | **Fixed** - ECHILD handled |
| [#1426](https://github.com/NixOS/nix/issues/1426) | Don't kill builder too early if stdout/stderr are closed | OPEN | Not covered | **Fixed** - wait for process exit, not pipe EOF |
| [#8232](https://github.com/NixOS/nix/issues/8232) | Darwin builds forking off processes never finish | OPEN | Not covered | **Fixed** - FD_CLOEXEC on pty slave |
| [#2714](https://github.com/NixOS/nix/issues/2714) | Nix on WSL killing processes unnecessarily | OPEN | `processes_test.cpp`: ESRCH handling | **Fixed** - ESRCH returns synthetic status |
| [#12514](https://github.com/NixOS/nix/issues/12514) | Deadlock when using user namespace (musl) | CLOSED | Not covered | Not applicable |
| [#2395](https://github.com/NixOS/nix/issues/2395) | PR_SET_PDEATHSIG results in Broken pipe | CLOSED | Not covered | Not applicable |

## Process Management / Signal Issues

| Issue | Title | Status | Test Coverage | Fix Status |
|-------|-------|--------|---------------|------------|
| [#9142](https://github.com/NixOS/nix/issues/9142) | Daemon kills unrelated processes in containers | OPEN | Not covered | **Fixed** - cgroups enabled by default |
| [#2398](https://github.com/NixOS/nix/issues/2398) | nix-daemon ignores error messages from forked children | OPEN | Not covered | **Fixed** - drain stderr before killing |
| [#14760](https://github.com/NixOS/nix/issues/14760) | Shouldn't kill build hook with SIGKILL immediately | OPEN | `processes_test.cpp` | **Fixed** - SIGTERM first, 5s grace period |
| [#7245](https://github.com/NixOS/nix/issues/7245) | Various Nix commands ignore Ctrl-C | OPEN | Not covered | **Fixed** - EINTR handling in poll, check_interrupt |
| [#3022](https://github.com/NixOS/nix/issues/3022) | Doesn't reset SIGPIPE handler in children | CLOSED | Not covered | Not applicable |

## Daemon Crashes

| Issue | Title | Status | Test Coverage | Fix Status |
|-------|-------|--------|---------------|------------|
| [#14758](https://github.com/NixOS/nix/issues/14758) | Random nix-daemon crash with nh os switch | OPEN | Not covered | **Fixed** - exception safety in finally block |
| [#13484](https://github.com/NixOS/nix/issues/13484) | Daemon crashes with assertion failure (mlibc) | OPEN | Not covered | **Fixed** - graceful double callback |
| [#11918](https://github.com/NixOS/nix/issues/11918) | M4 Mac migrated daemon crashes immediately | OPEN | Not covered | N/A - upstream daemon fork issue |
| [#2523](https://github.com/NixOS/nix/issues/2523) | Darwin daemon crashes (OBJC fork safety) | CLOSED | Not covered | Not applicable |
| [#13342](https://github.com/NixOS/nix/issues/13342) | Daemon crash on macOS 26 Beta | CLOSED | Not covered | Not applicable |

## Deadlocks / Concurrency

| Issue | Title | Status | Test Coverage | Fix Status |
|-------|-------|--------|---------------|------------|
| [#4216](https://github.com/NixOS/nix/issues/4216) | Recursive Nix deadlocks often | OPEN | Not covered | **Fixed** - fork child for inner builds |
| [#6666](https://github.com/NixOS/nix/issues/6666) | CA-derivations deadlock | OPEN | Not covered | **Already fixed** in codebase |
| [#2087](https://github.com/NixOS/nix/issues/2087) | fetchGit multiple instances deadlock | OPEN | Not covered | **Fixed** - flock EINTR retry |
| [#11979](https://github.com/NixOS/nix/issues/11979) | Concurrent store instances hang | OPEN | Not covered | **Fixed** - unique temp roots per instance |
| [#9548](https://github.com/NixOS/nix/issues/9548) | Race condition between GC and build | OPEN | Not covered | **Fixed** - temp root before registration |
| [#62](https://github.com/NixOS/nix/issues/62) | Deadlock in nix 1.1 worker | CLOSED | Not covered | Not applicable |

## Fetch / SSH Process Issues

| Issue | Title | Status | Test Coverage | Fix Status |
|-------|-------|--------|---------------|------------|
| [#14615](https://github.com/NixOS/nix/issues/14615) | nix copy ssh hangs for max-connections > 1 | OPEN | Not covered | **Fixed** - release lock during blocking I/O |
| [#10645](https://github.com/NixOS/nix/issues/10645) | SSH ControlMaster hangs forever | OPEN | Not covered | **Fixed** - configurable timeout (60s default) |
| [#7505](https://github.com/NixOS/nix/issues/7505) | nix copy hangs with missing ssh keys | OPEN | Not covered | **Fixed** - BatchMode=yes |
| [#5701](https://github.com/NixOS/nix/issues/5701) | Remote builders slow due to stderr not drained | OPEN | Not covered | **Fixed** - SSH stderr now captured |
| [#3017](https://github.com/NixOS/nix/issues/3017) | nix copy hangs forever sometimes | OPEN | Not covered | **Fixed** - skip callbacks on shutdown |

## macOS-Specific Process Issues

| Issue | Title | Status | Test Coverage | Fix Status |
|-------|-------|--------|---------------|------------|
| [#8232](https://github.com/NixOS/nix/issues/8232) | Darwin builds forking processes never finish | OPEN | Not covered | **Fixed** - FD_CLOEXEC on pty |
| [#3605](https://github.com/NixOS/nix/issues/3605) | macOS: unexpected EOF reading a line | OPEN | Not covered | **Fixed** - EAGAIN handling with poll |
| [#759](https://github.com/NixOS/nix/issues/759) | Darwin sandbox fork not permitted | CLOSED | Not covered | Not applicable |
| [#5018](https://github.com/NixOS/nix/issues/5018) | Daemon doesn't kill build processes on ^C | CLOSED | Not covered | Not applicable |

---

## Fixes Implemented

### 1. ECHILD Race Condition Fix (#2176, #2714)

**File:** `src/nix/util/unix/processes.cpp`

**Problem:** When a child process is reaped by another thread or signal handler
before `process_handle_t::wait()` is called, `waitpid()` returns -1 with
`errno = ECHILD`. The original code only handled `EINTR`, causing an exception.

**Fix:** Handle `ECHILD` in `wait()` by returning a synthetic status (exit 0).

### 2. ESRCH Handling in kill() (#2714)

**File:** `src/nix/util/unix/processes.cpp`

**Problem:** When `kill()` is called on a process that has already exited,
`::kill()` returns ESRCH. The original code then called `wait()`, which failed.

**Fix:** Return synthetic status when `ESRCH` is detected, downgraded to debug log.

### 3. EOF vs Process Exit Race Fix (#1426)

**File:** `src/nix/store/unix/build/derivation-builder.cpp`

**Problem:** Nix killed builders when pipe EOF was detected, but pipes can close
before the process actually exits.

**Fix:** In `unprepare_build()`, check if process exited with `waitpid(WNOHANG)` before killing.

### 4. Darwin Fork Hang Fix (#8232)

**File:** `src/nix/store/unix/build/darwin-derivation-builder.inc`

**Problem:** On macOS, builds forking background processes hung indefinitely
because forked processes inherited the pty slave fd, preventing EOF.

**Fix:** Set `FD_CLOEXEC` on stdout/stderr, use `posix_spawn_file_actions` to re-inherit.

### 5. Container UID Kill Fix (#9142)

**File:** `src/nix/store/globals.h`

**Problem:** `kill_user()` sent SIGKILL to all processes with matching UID,
killing processes in other containers.

**Fix:** Enable cgroups by default on Linux.

### 6. Child Error Message Propagation (#2398, #5701)

**File:** `src/nix/store/ssh.cpp`

**Problem:** When SSH child processes failed, the parent never read stderr.

**Fix:** Capture stderr pipe and drain it before throwing.

### 7. CA-Derivations Deadlock (#6666) - Already Fixed

**File:** `src/nix/store/local-store.cpp`

**Status:** Already fixed. Releases shared lock before acquiring exclusive.

### 8. Graceful Process Shutdown (#14760)

**File:** `src/nix/util/unix/processes.cpp`, `src/nix/store/unix/build/derivation-builder.cpp`

**Problem:** Build processes received SIGKILL immediately without cleanup chance.

**Fix:** Send SIGTERM first, wait up to 5 seconds, then SIGKILL if needed.

### 9. Ctrl-C / SIGINT Handling (#7245)

**Files:** `src/nix/util/unix/file-descriptor.cpp`, `src/nix/store/gc.cpp`, `src/nix/store/build/worker.cpp`

**Problem:** Blocking operations didn't handle EINTR properly or check for interrupts.

**Fix:** Handle EINTR in poll(), add check_interrupt() after blocking calls.

### 10. SSH max-connections Hang (#14615)

**Files:** `src/nix/store/ssh.cpp`, `src/nix/store/ssh.h`

**Problem:** SSH connection startup held mutex during blocking I/O, causing deadlock.

**Fix:** Release lock during blocking operations, use condition variable for coordination.

### 11. SSH ControlMaster Timeout (#10645)

**Files:** `src/nix/store/ssh.cpp`, `src/nix/store/globals.h`

**Problem:** SSH ControlMaster connections could hang indefinitely.

**Fix:** Add configurable `ssh-timeout` setting (default 60s), kill process on timeout.

### 12. Daemon Crash Fix (#14758)

**File:** `src/nix/store/daemon.cpp`

**Problem:** Exception in finally block during stack unwinding caused std::terminate.

**Fix:** Wrap finally block code in try-catch with `ignore_exception_in_destructor()`.

### 13. fetchGit Deadlock Fix (#2087)

**File:** `src/nix/store/unix/pathlocks.cpp`

**Problem:** `flock()` returned false on EINTR instead of retrying, causing lock failures.

**Fix:** Remove erroneous `return false` so while loop retries on EINTR.

### 14. Concurrent Store Instances (#11979)

**Files:** `src/nix/store/local-store.cpp`, `src/nix/store/gc.cpp`

**Problem:** Multiple LocalStore instances shared the same temp roots file path.

**Fix:** Use unique `{pid}-{instance}` format for temp roots files.

### 15. GC vs Build Race (#9548)

**File:** `src/nix/store/unix/build/derivation-builder.cpp`

**Problem:** Output paths could be GC'd between creation and database registration.

**Fix:** Call `addTempRoot()` before moving output to final location.

### 16. Recursive Nix Deadlock (#4216)

**File:** `src/nix/store/restricted-store.cpp`

**Problem:** Inner daemon builds created nested Workers that deadlocked with outer Worker.

**Fix:** Fork child process for inner builds to break circular wait.

### 17. macOS EOF Reading (#3605)

**File:** `src/nix/util/serialise.cpp`

**Problem:** `read_unbuffered()` didn't handle EAGAIN/EWOULDBLOCK.

**Fix:** Handle EAGAIN with poll() wait before retry.

---

## Test File Summary

**File:** `src/nix/util/tests/processes_test.cpp`

| Test Category | Test Name | Issues Covered |
|---------------|-----------|----------------|
| Basic | `process_handle_t basic wait` | - |
| Basic | `process_handle_t kill sends signal` | #14760 |
| Basic | `process_handle_t move semantics` | - |
| ECHILD | `wait handles ECHILD when already reaped` | #2176, #2714 |
| ECHILD | `kill handles ESRCH then ECHILD` | #2714 |
| Concurrency | `concurrent reap and wait` | #2176 |
| Property | `wait never throws ECHILD after fix` | #2176, #2714 |
| Property | `kill handles all race conditions` | #2714 |
| Stress | `many short-lived processes` | #2176 |
| Fuzz | `adversarial timing attacks` | #2176, #2714 |
| Fuzz | `concurrent multi-handle chaos` | #2176 |
| Signal | `SIGCHLD does not cause ECHILD errors` | #2176 |
| Edge | `wait on invalid PID` | - |
| Edge | `double wait` | - |
| Edge | `destructor kills if not waited` | - |

---

## Summary

**17 issues fixed**, covering:
- Process handling race conditions (ECHILD, ESRCH, EOF vs exit)
- Signal handling (SIGTERM before SIGKILL, Ctrl-C/SIGINT)
- Deadlocks (recursive Nix, CA derivations, fetchGit, concurrent stores)
- SSH issues (max-connections, ControlMaster timeout, error propagation)
- Platform-specific (Darwin fork hang, macOS EOF)
- Daemon stability (crash fix, GC race)

### 18. Double Callback Assertion (#13484)

**File:** `src/nix/util/callback.h`

**Problem:** Race condition in async file transfer could invoke callbacks twice,
causing assertion failure in mlibc.

**Fix:** Use atomic flag to ensure callback is invoked at most once.

### 19. SSH BatchMode for Missing Keys (#7505)

**File:** `src/nix/store/ssh.cpp`

**Problem:** `nix copy` hung waiting for SSH key password prompt.

**Fix:** Add `-o BatchMode=yes` to SSH options to fail immediately on missing keys.

### 20. Skip Callbacks on Shutdown (#3017)

**File:** `src/nix/store/filetransfer.cpp`

**Problem:** Callbacks invoked during shutdown could cause hangs/crashes.

**Fix:** Check shutdown flag before invoking callbacks.

### 21. Clear Builders on Remote (#10740)

**Files:** `src/nix/store/remote-store.cpp`, `src/nix/store/unix/build/hook-instance.cpp`

**Problem:** Cyclic builder configurations (A→B→A) caused deadlocks.

**Fix:** Clear `builders` setting when connecting to remote stores.

### 22. PR_SET_PDEATHSIG for Recursive Builds (#12142)

**File:** `src/nix/store/restricted-store.cpp`

**Problem:** Orphaned child processes from recursive builds could hold lock fds.

**Fix:** Set PR_SET_PDEATHSIG(SIGKILL) so child dies if parent dies.

---

## Summary

**22 issues fixed**, covering:
- Process handling race conditions (ECHILD, ESRCH, EOF vs exit)
- Signal handling (SIGTERM before SIGKILL, Ctrl-C/SIGINT)
- Deadlocks (recursive Nix, CA derivations, fetchGit, concurrent stores, cyclic builders)
- SSH issues (max-connections, ControlMaster timeout, error propagation, BatchMode, shutdown)
- Platform-specific (Darwin fork hang, macOS EOF, PR_SET_PDEATHSIG)
- Daemon stability (crash fix, GC race, double callback)

**Not applicable / upstream issues:**
- #11918 - M4 Mac migration crashes (upstream daemon fork issue)
- #2781 - Ctrl-Z suspend propagation (needs more invasive changes)
