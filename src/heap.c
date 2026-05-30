/*
 * heap.c — thin C wrappers for the Cygert "Base_IO_CPP" heap helpers.
 *
 * In the original these are C++ method exports of a small allocator class
 * ( = ::alloc, = ::free, = ::new
 * with vtable hook, = ::calloc-like with init flag). They map
 * to malloc/free for the reconstruction.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void *xmalloc(uint32_t sz)            { return malloc(sz); }
void  xfree  (void *p)                { free(p);             }
void *xcalloc(uint32_t sz, int zero)  { void *p = malloc(sz);
                                        if (p && zero) memset(p,0,sz);
                                        return p; }
