#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "platform/memory.h"

/* Treating PROT_NONE as a gap must fail these checks against real kernel mappings. */
static void expect_fault(volatile unsigned char *p, int writing) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        (void)signal(SIGSEGV, SIG_DFL);
        (void)signal(SIGBUS, SIG_DFL);
        if (writing) *p = 0x42;
        else { volatile unsigned char value = *p; (void)value; }
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
}

#ifdef __linux__
static void query_failure(void) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct rlimit limit = {0, 0};
        assert(!setrlimit(RLIMIT_NOFILE, &limit));
        avxemu_mapping mapping = {11, 22, 33}, saved = mapping;
        assert(avxemu_memory_query((uintptr_t)&limit, &mapping) == -1);
        assert(!memcmp(&mapping, &saved, sizeof mapping));
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
#endif

int main(void) {
    size_t page = avxemu_page_size();
    assert(page > 0 && page == (size_t)sysconf(_SC_PAGESIZE));
    unsigned char *p = mmap(NULL, 8 * page, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(p != MAP_FAILED);
    memset(p, 0x5a, 8 * page);
    avxemu_mapping mapping;
    assert(avxemu_memory_query((uintptr_t)p, &mapping) == 1);
    assert(mapping.begin <= (uintptr_t)p && mapping.end >= (uintptr_t)p + 8 * page);
    assert(mapping.prot == (AVXEMU_PROT_R | AVXEMU_PROT_W));

    unsigned char data[32];
    assert(avxemu_memory_read(data, (uintptr_t)p + 4 * page, sizeof data) == sizeof data);
    for (size_t i = 0; i < sizeof data; i++) assert(data[i] == 0x5a);
    assert(avxemu_memory_read(data, UINTPTR_MAX, sizeof data) == 0);
    assert(avxemu_memory_read(data, (uintptr_t)p, 0) == 0);

    unsigned char *guard = p + 5 * page, *hole = p + 6 * page;
    assert(mprotect(guard, page, PROT_NONE) == 0);
    assert(avxemu_memory_query((uintptr_t)guard, &mapping) == 1);
    assert(mapping.prot == 0);
    assert(avxemu_memory_read(data, (uintptr_t)guard, sizeof data) == 0);
    expect_fault(guard, 0);
    expect_fault(guard, 1);
    assert(munmap(hole, page) == 0);
    assert(avxemu_memory_query((uintptr_t)hole, &mapping) == 0);
    assert(avxemu_memory_read(data, (uintptr_t)hole, sizeof data) == 0);
    assert(avxemu_memory_read(data, (uintptr_t)guard - 16, sizeof data) == 16);
    for (int i = 0; i < 16; i++) assert(data[i] == 0x5a);
    expect_fault(hole, 0);

    assert(munmap(p, 8 * page) == 0);
#ifdef __linux__
    query_failure();
#endif
    puts("memory-contract: all checks passed");
    return 0;
}
