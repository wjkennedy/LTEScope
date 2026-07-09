#pragma once

// Minimal timerfd shim for platforms that do not provide <sys/timerfd.h>.
// The LTE decode path does not use these helpers directly; they only need to
// exist so legacy srsRAN headers compile on macOS.

#include <time.h>

#ifndef TFD_CLOEXEC
#define TFD_CLOEXEC 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline int timerfd_create(int clockid, int flags)
{
  (void)clockid;
  (void)flags;
  return -1;
}

static inline int timerfd_settime(int fd, int flags, const struct itimerspec* nv, struct itimerspec* ov)
{
  (void)fd;
  (void)flags;
  (void)nv;
  (void)ov;
  return -1;
}

#ifdef __cplusplus
}
#endif
