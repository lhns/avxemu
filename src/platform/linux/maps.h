#ifndef AVXEMU_LINUX_MAPS_H
#define AVXEMU_LINUX_MAPS_H
#include "platform/memory.h"
typedef struct {
    int fd;
    char bytes[1024];
    size_t pos, count, total;
    uintptr_t previous_end;
} maps_reader;
typedef struct { uintptr_t offset, major, minor; uint64_t ino; } avxemu_map_file;
/* Bounded, allocation-free procfs reader safe to use from signal handlers. */
int avxemu_maps_open(maps_reader *reader);
int avxemu_maps_next(maps_reader *reader, avxemu_mapping *mapping, avxemu_map_file *file);
#endif
