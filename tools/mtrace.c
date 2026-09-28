/* mtrace.so (_RLD_LIST preload): log allocations >= 4 KB with caller and stack words to
 * /var/tmp/mtrace.PID. Only for diagnosing WPEWebProcess memory use. */
#include <sys/types.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void *_malloc(size_t);
extern void *_calloc(size_t, size_t);
extern void *_realloc(void *, size_t);
extern void *_memalign(size_t, size_t);

static int logfd = -1, busy;

static void note(const char *kind, size_t size, size_t align, void *ret, void *ra)
{
    char buf[512];
    unsigned long *sp = (unsigned long *)__builtin_frame_address(0);
    int n, i, k = 0;
    if (busy) return;
    busy = 1;
    if (logfd < 0) {
        char path[64];
        void *h, *anchor = 0;
        snprintf(path, sizeof path, "/var/tmp/mtrace.%d", (int)getpid());
        logfd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
        if ((h = dlopen("libWPEWebKit-2.0.so.1", RTLD_LAZY)))
            anchor = dlsym(h, "wpe_display_get_type");
        n = snprintf(buf, sizeof buf, "anchor wpe_display_get_type %p\n", anchor);
        write(logfd, buf, n);
    }
    n = snprintf(buf, sizeof buf, "%s %lu %lu %p ra %p :", kind, (unsigned long)size, (unsigned long)align, ret, ra);
    for (i = 0; i < 256 && k < 10; i++) {
        unsigned long w = sp[i];
        if (w >= 0x4000000 && w < 0xc000000 && !(w & 3)) {
            n += snprintf(buf + n, sizeof buf - n, " %lx", w);
            k++;
        }
    }
    buf[n++] = '\n';
    write(logfd, buf, n);
    busy = 0;
}

void *malloc(size_t s)
{
    void *p = _malloc(s);
    if (s >= 4096) note("malloc", s, 0, p, __builtin_return_address(0));
    return p;
}
void *calloc(size_t a, size_t b)
{
    void *p = _calloc(a, b);
    if (a * b >= 4096) note("calloc", a * b, 0, p, __builtin_return_address(0));
    return p;
}
void *realloc(void *o, size_t s)
{
    void *p = _realloc(o, s);
    if (s >= 4096) note("realloc", s, 0, p, __builtin_return_address(0));
    return p;
}
void *memalign(size_t al, size_t s)
{
    void *p = _memalign(al, s);
    if (s >= 4096) note("memalign", s, al, p, __builtin_return_address(0));
    return p;
}
