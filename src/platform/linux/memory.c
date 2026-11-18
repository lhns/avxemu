#define _GNU_SOURCE
#include "platform/memory.h"
#include "maps.h"
#include <stdatomic.h>
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
