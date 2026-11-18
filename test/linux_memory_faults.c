#define _GNU_SOURCE
#undef NDEBUG
#include "platform/memory.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

enum { NORMAL, OPEN_ERROR, READ_ERROR, EMPTY, TRUNCATED, MALFORMED };
static int mode, maps_fd = -1, synthetic_eof;

int __real_open(const char *, int, ...);
ssize_t __real_read(int, void *, size_t);

int __wrap_open(const char *path, int flags, ...) {
    if (!strcmp(path, "/proc/self/maps")) {
        if (mode == OPEN_ERROR) { errno = EACCES; return -1; }
        synthetic_eof = 0;
        maps_fd = __real_open(path, flags);
        return maps_fd;
    }
    return __real_open(path, flags);
}

ssize_t __wrap_read(int fd, void *dst, size_t size) {
    if (fd == maps_fd) {
        if (mode == READ_ERROR) { errno = EIO; return -1; }
        if (mode == EMPTY) return 0;
        if (mode == TRUNCATED || mode == MALFORMED) {
            if (synthetic_eof) return 0;
            const char *text = mode == TRUNCATED ? "1000-2000 rw-p " :
                              "fffffffffffffffff-2000 rw-p 0 0:0 0\n";
            size_t n = strlen(text);
            assert(n <= size);
            memcpy(dst, text, n);
            /* The following read returns EOF, even for a truncated line. */
            synthetic_eof = 1;
            return (ssize_t)n;
        }
    }
    return __real_read(fd, dst, size);
}

static void failures(void) {
    size_t page = avxemu_page_size();
    unsigned char *p = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(p != MAP_FAILED);
    assert(!munmap(p + page, page));
    for (mode = OPEN_ERROR; mode <= MALFORMED; mode++) {
        avxemu_mapping mapping = {11, 22, 33}, original = mapping;
        assert(avxemu_memory_query((uintptr_t)p, &mapping) == -1);
        assert(!memcmp(&mapping, &original, sizeof mapping));
    }
    mode = NORMAL;
    avxemu_mapping mapping;
    assert(avxemu_memory_query((uintptr_t)p + page, &mapping) == 0);
    mode = NORMAL;
    assert(!munmap(p, 3 * page));
}

int main(void) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) { failures(); _exit(0); }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("linux-memory-faults: all checks passed");
}
