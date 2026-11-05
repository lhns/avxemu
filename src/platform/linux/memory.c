#define _GNU_SOURCE
#include "platform/memory.h"
#include "maps.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static _Atomic size_t page_size;

size_t avxemu_page_size(void) {
    size_t page = atomic_load_explicit(&page_size, memory_order_relaxed);
    if (!page) {
        long value = sysconf(_SC_PAGESIZE);
        if (value > 0) {
            page = (size_t)value;
            atomic_store_explicit(&page_size, page, memory_order_relaxed);
        }
    }
    return page;
}

int avxemu_memory_query(uintptr_t address, avxemu_mapping *mapping) {
    maps_reader reader;
    if (!mapping || !avxemu_maps_open(&reader)) return -1;
    avxemu_mapping found;
    int status;
    while ((status = avxemu_maps_next(&reader, &found, 0)) == 1) {
        if (address < found.begin) { status = 0; break; }
        if (address < found.end) break;
    }
    if (close(reader.fd) != 0) return -1;
    if (status == 1) *mapping = found;
    return status;
}

size_t avxemu_memory_read(void *dst, uintptr_t src, size_t size) {
    size_t page = avxemu_page_size(), done = 0;
    uintptr_t target = (uintptr_t)dst;
    if (!page || size > UINTPTR_MAX - src || size > UINTPTR_MAX - target) return 0;
    while (done < size) {
        size_t n = page - (src + done) % page;
        if (n > size - done) n = size - done;
        struct iovec local = {(void *)(target + done), n};
        struct iovec remote = {(void *)(src + done), n};
        ssize_t got;
        do {
            got = syscall(SYS_process_vm_readv, getpid(), &local, 1UL, &remote, 1UL, 0UL);
        } while (got < 0 && errno == EINTR);
        /* ENOSYS/EPERM are failures too; never dereference the input instead. */
        if (got <= 0 || (size_t)got > n) break;
        done += (size_t)got;
        if ((size_t)got != n) break;
    }
    return done;
}
