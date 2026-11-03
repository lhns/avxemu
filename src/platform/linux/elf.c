#include "elf.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* All on-disk structures are copied: callers may supply unaligned bytes. */
static int span(size_t size, uint64_t off, uint64_t n) {
    return off <= size && n <= size - off;
}
static int add(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) return 0;
    *out = a + b; return 1;
}
static int signed_add(uint64_t a, int64_t b, uint64_t *out) {
    if (b >= 0) return add(a, (uint64_t)b, out);
    uint64_t mag = (uint64_t)(-(b + 1)) + 1;
    if (mag > a) return 0;
    *out = a - mag; return 1;
}
static int append(void **items, size_t *count, size_t size, const void *item) {
    if (*count >= SIZE_MAX / size) return 0;
    void *p = realloc(*items, (*count + 1) * size);
    if (!p) return 0;
    memcpy((uint8_t *)p + *count * size, item, size);
    *items = p; ++*count; return 1;
}

typedef struct { const uint8_t *data; size_t size, pos; uint64_t vaddr; } reader;
static int bytes(reader *r, void *out, size_t n) {
    if (!span(r->size, r->pos, n)) return 0;
    memcpy(out, r->data + r->pos, n); r->pos += n; return 1;
}
static int leb(reader *r, uint64_t *out, int sign) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 10; i++) {
        uint8_t b;
        if (!bytes(r, &b, 1)) return 0;
        if (i == 9 && (sign ? ((b & 0x7f) != 0 && (b & 0x7f) != 0x7f)
                           : (b & 0x7e) != 0)) return 0;
        v |= (uint64_t)(b & 0x7f) << (i * 7);
        if (!(b & 0x80)) {
            if (sign && i < 9 && (b & 0x40)) v |= UINT64_MAX << (i * 7 + 7);
            *out = v; return 1;
        }
    }
    return 0;
}
/* datarel is accepted only when the format defines its base (.eh_frame_hdr).
 * .eh_frame itself supplies no such base, so datarel there is unsupported. */
static int encoding_known(uint8_t enc, int have_data, int indirect_ok) {
    if (enc == 0xff || ((enc & 0x80) && !indirect_ok)) return 0;
    unsigned format = enc & 15, application = enc & 0x70;
    if (!(format <= 4 || (format >= 9 && format <= 12))) return 0;
    return application == 0 || application == 0x10 || (application == 0x30 && have_data);
}
static int encoded(reader *r, uint8_t enc, int have_data, uint64_t data_base,
                   int indirect_ok, uint64_t *out) {
    uint64_t raw = 0, base;
    size_t pos = r->pos;
    int sign = 0;
    if (!encoding_known(enc, have_data, indirect_ok)) return 0;
    switch (enc & 15) {
    case 0: if (!bytes(r, &raw, 8)) return 0; break;
    case 1: if (!leb(r, &raw, 0)) return 0; break;
    case 2: { uint16_t v; if (!bytes(r, &v, 2)) return 0; raw = v; break; }
    case 3: { uint32_t v; if (!bytes(r, &v, 4)) return 0; raw = v; break; }
    case 4: if (!bytes(r, &raw, 8)) return 0; break;
    case 9: if (!leb(r, &raw, 1)) return 0; sign = 1; break;
    case 10: { int16_t v; if (!bytes(r, &v, 2)) return 0; raw = (uint64_t)(int64_t)v; sign = 1; break; }
    case 11: { int32_t v; if (!bytes(r, &v, 4)) return 0; raw = (uint64_t)(int64_t)v; sign = 1; break; }
    case 12: if (!bytes(r, &raw, 8)) return 0; sign = 1; break;
    default: return 0;
    }
    switch (enc & 0x70) {
    case 0: *out = raw; return !sign || (int64_t)raw >= 0;
    case 0x10: if (!add(r->vaddr, pos, &base)) return 0; break;
    case 0x30: if (!have_data) return 0; base = data_base; break;
    default: return 0;
    }
    return sign ? signed_add(base, (int64_t)raw, out) : add(base, raw, out);
}
static int bound_compare(const void *a, const void *b) {
    const avxemu_elf_bound *x = a, *y = b;
    if (x->begin != y->begin) return x->begin < y->begin ? -1 : 1;
    return x->end == y->end ? 0 : x->end < y->end ? -1 : 1;
}
static int range_compare(const void *a, const void *b) {
    const avxemu_elf_range *x = a, *y = b;
    return x->begin == y->begin ? 0 : x->begin < y->begin ? -1 : 1;
}
static int section(const avxemu_elf_view *v, const Elf64_Ehdr *h, size_t i, Elf64_Shdr *s) {
    if (i >= h->e_shnum) return 0;
    memcpy(s, v->data + h->e_shoff + i * sizeof *s, sizeof *s); return 1;
}
static const char *name(const avxemu_elf_view *v, const Elf64_Shdr *strings, uint32_t off) {
    if (off >= strings->sh_size) return 0;
    const char *p = (const char *)v->data + strings->sh_offset + off;
    return memchr(p, 0, strings->sh_size - off) ? p : 0;
}
static int load_contains(const avxemu_elf *e, uint64_t addr, uint64_t n, uint64_t off,
                         unsigned flags) {
    for (size_t i = 0; i < e->nloads; i++) {
        const Elf64_Phdr *p = &e->loads[i];
        if ((p->p_flags & flags) == flags && addr >= p->p_vaddr &&
            addr - p->p_vaddr <= p->p_filesz && n <= p->p_filesz - (addr - p->p_vaddr) &&
            off >= p->p_offset && off - p->p_offset == addr - p->p_vaddr) return 1;
    }
    return 0;
}
static int in_range(const avxemu_elf *e, uint64_t begin, uint64_t end) {
    for (size_t i = 0; i < e->nranges; i++)
        if (begin >= e->ranges[i].begin && begin < end && end <= e->ranges[i].end) return 1;
    return 0;
}

/* A domain/length-prefixed four-lane FNV digest. This is a cache identity,
 * not authentication; device/inode and verified mapped-file association are
 * independently required by the image backend. */
static void normalize_id(const uint8_t *p, size_t n, uint8_t out[32]) {
    for (unsigned lane = 0; lane < 4; lane++) {
        uint64_t hash = UINT64_C(14695981039346656037) ^ lane;
        const uint8_t domain[] = {'E','L','F',0,'G','N','U',1};
        for (size_t i = 0; i < sizeof domain; i++) hash = (hash ^ domain[i]) * UINT64_C(1099511628211);
        for (unsigned i = 0; i < 8; i++) hash = (hash ^ ((uint64_t)n >> (i * 8) & 255)) * UINT64_C(1099511628211);
        for (size_t i = 0; i < n; i++) hash = (hash ^ p[i]) * UINT64_C(1099511628211);
        for (unsigned i = 0; i < 8; i++) out[lane * 8 + i] = (uint8_t)(hash >> (i * 8));
    }
}
static int notes(const avxemu_elf_view *v, const Elf64_Phdr *p, avxemu_elf *e, int *found) {
    reader r = {v->data + p->p_offset, p->p_filesz, 0, p->p_vaddr};
    while (r.pos < r.size) {
        Elf64_Nhdr h;
        if (!bytes(&r, &h, sizeof h)) return 0;
        uint64_t namesz = ((uint64_t)h.n_namesz + 3) & ~UINT64_C(3);
        uint64_t descsz = ((uint64_t)h.n_descsz + 3) & ~UINT64_C(3);
        if (!span(r.size, r.pos, namesz) || !span(r.size, r.pos + namesz, descsz)) return 0;
        if (h.n_type == NT_GNU_BUILD_ID && h.n_namesz == 4 && !memcmp(r.data + r.pos, "GNU", 4)) {
            uint8_t id[32];
            if (!h.n_descsz) return 0;
            normalize_id(r.data + r.pos + namesz, h.n_descsz, id);
            if (*found && memcmp(id, e->build_id, sizeof id)) return 0;
            memcpy(e->build_id, id, sizeof id); *found = 1;
        }
        r.pos += namesz + descsz;
    }
    return 1;
}

typedef struct { uint64_t address, begin; } fde_index;

typedef struct { size_t offset; uint8_t encoding, lsda; int augmented; } cie;
static int unwind(const avxemu_elf_view *v, const Elf64_Shdr *s, avxemu_elf *e,
                  fde_index **index, size_t *nindex) {
    reader r = {v->data + s->sh_offset, s->sh_size, 0, s->sh_addr};
    cie *cies = 0; size_t ncies = 0; int ok = 0;
    while (r.pos < r.size) {
        size_t start = r.pos; uint32_t len, id;
        if (!bytes(&r, &len, 4)) goto done;
        if (!len) { while (r.pos < r.size) if (r.data[r.pos++]) goto done; break; }
        /* DWARF64 CFI records are deliberately unsupported. */
        if (len == UINT32_MAX || len < 4 || !span(r.size, r.pos, len)) goto done;
        size_t finish = r.pos + len, idpos = r.pos;
        reader record = r; record.size = finish;
        if (!bytes(&record, &id, 4)) goto done;
        if (!id) {
            cie c = {start, 0, 0xff, 0}; uint8_t version;
            if (!bytes(&record, &version, 1) || (version != 1 && version != 3 && version != 4)) goto done;
            size_t a = record.pos;
            while (record.pos < finish && record.data[record.pos]) record.pos++;
            if (record.pos == finish) goto done;
            const char *aug = (const char *)record.data + a; record.pos++;
            if (version == 4) { uint8_t as, ss; if (!bytes(&record, &as, 1) || !bytes(&record, &ss, 1) || as != 8 || ss) goto done; }
            uint64_t code, data, reg;
            if (!leb(&record, &code, 0) || !code || !leb(&record, &data, 1)) goto done;
            if (version == 1) { uint8_t b; if (!bytes(&record, &b, 1)) goto done; reg = b; }
            else if (!leb(&record, &reg, 0)) goto done;
            (void)data; (void)reg;
            if (*aug) {
                uint64_t n;
                if (*aug++ != 'z' || !leb(&record, &n, 0) || !span(finish, record.pos, n)) goto done;
                c.augmented = 1; size_t augend = record.pos + n;
                reader ar = record; ar.size = augend;
                unsigned seen = 0;
                while (*aug) {
                    unsigned bit; uint64_t ignored; uint8_t enc;
                    switch (*aug++) {
                    case 'R': bit = 1; if (!bytes(&ar, &c.encoding, 1) || !encoding_known(c.encoding, 0, 0)) goto done; break;
                    case 'L': bit = 2; if (!bytes(&ar, &c.lsda, 1) || (c.lsda != 0xff && !encoding_known(c.lsda, 0, 1))) goto done; break;
                    case 'P': bit = 4; if (!bytes(&ar, &enc, 1) || !encoded(&ar, enc, 0, 0, 1, &ignored)) goto done; break;
                    case 'S': bit = 8; break;
                    default: goto done;
                    }
                    if (seen & bit) goto done;
                    seen |= bit;
                }
                if (ar.pos != augend) goto done;
                record.pos = augend;
            }
            if (!append((void **)&cies, &ncies, sizeof c, &c)) goto done;
        } else {
            if (id > idpos) goto done;
            cie *c = 0;
            size_t lo = 0, hi = ncies;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (cies[mid].offset < idpos - id) lo = mid + 1; else hi = mid;
            }
            if (lo < ncies && cies[lo].offset == idpos - id) c = &cies[lo];
            uint64_t begin, length, end;
            if (!c || !encoded(&record, c->encoding, 0, 0, 0, &begin) ||
                !encoded(&record, c->encoding & 15, 0, 0, 0, &length) || !add(begin, length, &end)) goto done;
            if (c->augmented) {
                uint64_t n, ignored;
                if (!leb(&record, &n, 0) || !span(finish, record.pos, n)) goto done;
                size_t augend = record.pos + n; reader ar = record; ar.size = augend;
                if (c->lsda != 0xff && !encoded(&ar, c->lsda, 0, 0, 1, &ignored)) goto done;
                if (ar.pos != augend) goto done;
                record.pos = augend;
            }
            fde_index entry = {s->sh_addr + start, begin};
            if (!append((void **)index, nindex, sizeof entry, &entry)) goto done;
            /* An empty FDE (discarded or zero-size function) covers no byte: it is
             * indexed for the header cross-check but publishes no bound. */
            avxemu_elf_bound f = {begin, end};
            if (length && !append((void **)&e->functions, &e->nfunctions, sizeof f, &f)) goto done;
        }
        r.pos = finish;
    }
    ok = 1;
done:
    free(cies); return ok;
}

/* The binary search table is only a consistency check. Its sorted starts never
 * define function ends; the FDE's own length remains authoritative. */
static int unwind_header(const avxemu_elf_view *v, const Elf64_Shdr *s,
                         uint64_t frame, const fde_index *index, size_t nindex) {
    reader r = {v->data + s->sh_offset, s->sh_size, 0, s->sh_addr};
    uint8_t version, frame_enc, count_enc, table_enc; uint64_t address, count;
    if (!bytes(&r, &version, 1) || version != 1 || !bytes(&r, &frame_enc, 1) ||
        !bytes(&r, &count_enc, 1) || !bytes(&r, &table_enc, 1) ||
        !encoded(&r, frame_enc, 1, s->sh_addr, 0, &address) || address != frame) return 0;
    if (count_enc == 0xff && table_enc == 0xff) return r.pos == r.size;
    if ((count_enc & 0xf0) || !encoded(&r, count_enc, 0, 0, 0, &count) || count != nindex) return 0;
    uint64_t previous = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t begin, fde;
        if (!encoded(&r, table_enc, 1, s->sh_addr, 0, &begin) ||
            !encoded(&r, table_enc, 1, s->sh_addr, 0, &fde) || (i && begin < previous)) return 0;
        size_t lo = 0, hi = nindex;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (index[mid].address < fde) lo = mid + 1; else hi = mid;
        }
        if (lo == nindex || index[lo].address != fde || index[lo].begin != begin) return 0;
        previous = begin;
    }
    return r.pos == r.size;
}

void avxemu_elf_destroy(avxemu_elf *e) {
    if (!e) return;
    free(e->loads); free(e->ranges); free(e->functions); memset(e, 0, sizeof *e);
}
int avxemu_elf_parse(const avxemu_elf_view *v, avxemu_elf *out) {
    avxemu_elf e = {0}; avxemu_elf_bound *objects = 0; size_t nobjects = 0;
    fde_index *index = 0; size_t nindex = 0;
    Elf64_Shdr eh = {0}, eh_header = {0}; int have_header = 0;
    if (!out || !v || !v->data || v->vaddr || v->size < sizeof e.header) return 0;
    memcpy(&e.header, v->data, sizeof e.header);
    const Elf64_Ehdr *h = &e.header;
    if (memcmp(h->e_ident, ELFMAG, SELFMAG) || h->e_ident[EI_CLASS] != ELFCLASS64 ||
        h->e_ident[EI_DATA] != ELFDATA2LSB || h->e_ident[EI_VERSION] != EV_CURRENT ||
        h->e_machine != EM_X86_64 || h->e_version != EV_CURRENT ||
        (h->e_type != ET_EXEC && h->e_type != ET_DYN) || h->e_ehsize != sizeof *h ||
        h->e_phentsize != sizeof(Elf64_Phdr) || !h->e_phnum || h->e_phnum == PN_XNUM ||
        !span(v->size, h->e_phoff, (uint64_t)h->e_phnum * sizeof(Elf64_Phdr)) ||
        !h->e_shnum || h->e_shentsize != sizeof(Elf64_Shdr) || !h->e_shstrndx || h->e_shstrndx >= h->e_shnum ||
        !span(v->size, h->e_shoff, (uint64_t)h->e_shnum * sizeof(Elf64_Shdr))) goto fail;
    int found_id = 0;
    for (size_t i = 0; i < h->e_phnum; i++) {
        Elf64_Phdr p; memcpy(&p, v->data + h->e_phoff + i * sizeof p, sizeof p);
        if (!span(v->size, p.p_offset, p.p_filesz) || p.p_memsz > UINT64_MAX - p.p_vaddr) goto fail;
        if (p.p_type == PT_LOAD) {
            if (p.p_filesz > p.p_memsz || (p.p_align > 1 &&
                ((p.p_align & (p.p_align - 1)) || p.p_vaddr % p.p_align != p.p_offset % p.p_align))) goto fail;
            for (size_t j = 0; j < e.nloads; j++)
                if (p.p_memsz && e.loads[j].p_memsz && p.p_vaddr < e.loads[j].p_vaddr + e.loads[j].p_memsz &&
                    e.loads[j].p_vaddr < p.p_vaddr + p.p_memsz) goto fail;
            if (!append((void **)&e.loads, &e.nloads, sizeof p, &p)) goto fail;
        }
        if (p.p_type == PT_NOTE && !notes(v, &p, &e, &found_id)) goto fail;
    }
    if (!e.nloads) goto fail;
    Elf64_Shdr strings;
    section(v, h, h->e_shstrndx, &strings);
    if (strings.sh_type != SHT_STRTAB || !span(v->size, strings.sh_offset, strings.sh_size)) goto fail;
    for (size_t i = 0; i < h->e_shnum; i++) {
        Elf64_Shdr s; section(v, h, i, &s);
        const char *nm = name(v, &strings, s.sh_name);
        if (!nm || s.sh_size > UINT64_MAX - s.sh_addr ||
            (s.sh_type != SHT_NOBITS && !span(v->size, s.sh_offset, s.sh_size))) goto fail;
        if (!(s.sh_flags & SHF_EXECINSTR) || !s.sh_size) continue;
        if (!(s.sh_flags & SHF_ALLOC) || s.sh_type != SHT_PROGBITS ||
            !load_contains(&e, s.sh_addr, s.sh_size, s.sh_offset, PF_R | PF_X)) goto fail;
        /* Exclusions cannot be bypassed by an overlapping ordinary section. */
        for (size_t j = 0; j < i; j++) {
            Elf64_Shdr other; section(v, h, j, &other);
            if ((other.sh_flags & SHF_EXECINSTR) && other.sh_size &&
                s.sh_addr < other.sh_addr + other.sh_size && other.sh_addr < s.sh_addr + s.sh_size) goto fail;
        }
        if (!strcmp(nm, ".avxemu_raw") || !strcmp(nm, ".avxemu_test")) continue;
        avxemu_elf_range r = {s.sh_addr, s.sh_addr + s.sh_size, s.sh_offset, 5};
        for (size_t j = 0; j < e.nloads; j++)
            if (s.sh_addr >= e.loads[j].p_vaddr && s.sh_addr - e.loads[j].p_vaddr < e.loads[j].p_filesz &&
                (e.loads[j].p_flags & PF_W)) r.prot |= 2;
        if (!append((void **)&e.ranges, &e.nranges, sizeof r, &r)) goto fail;
    }
    if (!e.nranges) goto fail;
    qsort(e.ranges, e.nranges, sizeof *e.ranges, range_compare);
    for (size_t i = 1; i < e.nranges; i++) if (e.ranges[i].begin < e.ranges[i-1].end) goto fail;
    int have_eh = 0;
    for (size_t i = 0; i < h->e_shnum; i++) {
        Elf64_Shdr s; section(v, h, i, &s);
        const char *nm = name(v, &strings, s.sh_name);
        if (!strcmp(nm, ".eh_frame_hdr")) {
            if (have_header++ || s.sh_type != SHT_PROGBITS || !(s.sh_flags & SHF_ALLOC) ||
                !load_contains(&e, s.sh_addr, s.sh_size, s.sh_offset, PF_R)) goto fail;
            eh_header = s;
        }
        if (!strcmp(nm, ".eh_frame")) {
            eh = s;
            if (have_eh++ || s.sh_type != SHT_PROGBITS || !(s.sh_flags & SHF_ALLOC) ||
                !load_contains(&e, s.sh_addr, s.sh_size, s.sh_offset, PF_R) || !unwind(v, &s, &e, &index, &nindex)) goto fail;
        }
        if (s.sh_type != SHT_SYMTAB && s.sh_type != SHT_DYNSYM) continue;
        Elf64_Shdr strs;
        if (s.sh_entsize != sizeof(Elf64_Sym) || s.sh_size % s.sh_entsize ||
            !section(v, h, s.sh_link, &strs) || strs.sh_type != SHT_STRTAB ||
            !span(v->size, strs.sh_offset, strs.sh_size)) goto fail;
        for (size_t j = 0; j < s.sh_size / sizeof(Elf64_Sym); j++) {
            Elf64_Sym sym; memcpy(&sym, v->data + s.sh_offset + j * sizeof sym, sizeof sym);
            if (!name(v, &strs, sym.st_name)) goto fail;
            unsigned type = ELF64_ST_TYPE(sym.st_info);
            if (type != STT_FUNC && type != STT_OBJECT) continue;
            if (!sym.st_size || sym.st_shndx == SHN_UNDEF || sym.st_shndx == SHN_ABS) continue;
            Elf64_Shdr target; uint64_t end;
            if (!section(v, h, sym.st_shndx, &target) || !add(sym.st_value, sym.st_size, &end) ||
                sym.st_value < target.sh_addr || sym.st_value - target.sh_addr > target.sh_size ||
                sym.st_size > target.sh_size - (sym.st_value - target.sh_addr)) goto fail;
            if (!(target.sh_flags & SHF_ALLOC)) continue;
            avxemu_elf_bound f = {sym.st_value, end};
            if (type == STT_OBJECT) { if (!append((void **)&objects, &nobjects, sizeof f, &f)) goto fail; }
            else if (!append((void **)&e.functions, &e.nfunctions, sizeof f, &f)) goto fail;
        }
    }
    if (have_header && (!have_eh || !unwind_header(v, &eh_header, eh.sh_addr, index, nindex))) goto fail;
    if (e.nfunctions) qsort(e.functions, e.nfunctions, sizeof *e.functions, bound_compare);
    if (nobjects) qsort(objects, nobjects, sizeof *objects, bound_compare);
    size_t n = 0, object = 0;
    /* Retain out-of-range bounds through conflict detection: a crossing FDE
     * or allocated data object must not leave a smaller symbol authoritative.
     * Reject whole connected overlap groups before filtering publishable
     * ranges. Identical symbol/FDE aliases contribute one bound. */
    for (size_t i = 0; i < e.nfunctions;) {
        avxemu_elf_bound f = e.functions[i++]; int conflict = 0;
        uint64_t end = f.end;
        while (i < e.nfunctions && e.functions[i].begin < end) {
            avxemu_elf_bound g = e.functions[i++];
            if (g.begin != f.begin || g.end != f.end) conflict = 1;
            if (g.end > end) end = g.end;
        }
        while (object < nobjects && objects[object].end <= f.begin) object++;
        if (object < nobjects && objects[object].begin < end) conflict = 1;
        if (!conflict && in_range(&e, f.begin, f.end)) e.functions[n++] = f;
    }
    e.nfunctions = n;
    free(index); free(objects); *out = e; return 1;
fail:
    free(index); free(objects); avxemu_elf_destroy(&e); return 0;
}
int avxemu_elf_functions(const avxemu_elf_view *v, avxemu_elf_function_sink sink, void *arg) {
    avxemu_elf e;
    if (!sink || !avxemu_elf_parse(v, &e)) return 0;
    int ok = 1;
    for (size_t i = 0; i < e.nfunctions; i++)
        if (!sink(e.functions[i].begin, e.functions[i].end, arg)) { ok = 0; break; }
    avxemu_elf_destroy(&e); return ok;
}
