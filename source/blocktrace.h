#ifndef INFINITY_BLADE_NX_BLOCKTRACE_H
#define INFINITY_BLADE_NX_BLOCKTRACE_H

/* Live "who is blocked where" tracker.
 *
 * Every shimmed blocking primitive (futex, condvar, mutex, semaphore, sleep)
 * brackets its wait with bt_enter()/bt_leave(). A watchdog thread prints, every
 * 2 s, each thread that has been blocked for a while (kind, address, caller as
 * an offset into libib3.so) plus how many blocking calls each thread made in the
 * interval, so both a deadlock and a polling loop are visible in the log. */

void bt_enter(const char *kind, const void *addr, const void *caller,
              long long timeout_ns);
void bt_leave(void);
void bt_start_watchdog(void);

#endif
