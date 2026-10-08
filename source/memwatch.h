#ifndef MEMWATCH_H
#define MEMWATCH_H

#include <stddef.h>

/* Start the 10-second memory/texture telemetry thread (idempotent). */
void memwatch_start(void);
/* Log one telemetry line now; `why` is appended to the tag (may be NULL). */
void memwatch_report(const char *why);
/* Called by the allocator shims when an allocation returns NULL. */
void memwatch_alloc_failed(size_t size, const char *what);

#endif
