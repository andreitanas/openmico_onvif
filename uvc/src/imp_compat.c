// Symbols Ingenic's prebuilt libimp.a expects from its original uClibc
// toolchain (and from libalog), provided for a static musl build.

#ifdef UVC_IMP

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>

// Not from <unistd.h>: it declares the 64-bit off_t ftruncate defined below.
long syscall(long, ...);

// libimp was built with uClibc's 32-bit off_t; musl's off_t is always 64-bit
// (time_t functions keep 32-bit compat symbols in musl, these do not). Calls
// compiled against the 32-bit prototypes land here instead of musl's.
// Nothing else in the binary may call mmap/ftruncate with a 64-bit offset.
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    return (void *)syscall(SYS_mmap2, addr, len, prot, flags, fd, (unsigned long)off >> 12);
}

int ftruncate(int fd, long len) { return syscall(SYS_ftruncate, fd, len); }

// libalog: libimp's logging backend. Levels from imp_log.h: 4 info, 5 warn,
// 6 error, 7 fatal.
int IMP_Log_Get_Option(void) { return 0; }
int imp_log_level = 4;

void imp_log_fun(int level, int option, int type, const char *tag, const char *file, int line,
                 const char *func, const char *fmt, ...) {
    static const char *names[] = {"?", "?", "V", "D", "I", "W", "E", "F"};
    if (level < imp_log_level || level > 7)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "libimp %s %s %s:%d: ", names[level], tag ? tag : "", func ? func : "", line);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// uClibc's assert() target.
void __assert(const char *expr, const char *file, int line, const char *func) {
    fprintf(stderr, "libimp assertion failed: %s (%s:%d %s)\n", expr, file, line, func);
    abort();
}

// pthread_cleanup_push/pop in glibc/uClibc headers expand to these. Thread
// cancellation cleanup handlers are not run; libimp only cancels its
// threads when shutting down.
struct cleanup_frame;
void __pthread_register_cancel(struct cleanup_frame *f) {}
void __pthread_unregister_cancel(struct cleanup_frame *f) {}
void __pthread_unwind_next(struct cleanup_frame *f) { abort(); }

#endif
