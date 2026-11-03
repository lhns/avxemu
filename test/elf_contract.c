#include "platform/linux/elf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned calls;
static int sink(uint64_t begin, uint64_t end, void *arg) {
    (void)arg; assert(begin < end); calls++; return 1;
}
static void malformed(void) {
    uint8_t data[256] = {0};
    avxemu_elf_view view = {data, 0, 0};
    assert(!avxemu_elf_functions(&view, sink, 0));
    view.size = sizeof data;
    assert(!avxemu_elf_functions(&view, sink, 0));
    Elf64_Ehdr h = {0};
    memcpy(h.e_ident, ELFMAG, SELFMAG);
    h.e_ident[EI_CLASS] = ELFCLASS64; h.e_ident[EI_DATA] = ELFDATA2LSB;
    h.e_ident[EI_VERSION] = EV_CURRENT; h.e_type = ET_EXEC;
    h.e_machine = EM_X86_64; h.e_version = EV_CURRENT;
    h.e_ehsize = sizeof h; h.e_phentsize = sizeof(Elf64_Phdr);
    h.e_phoff = sizeof h; h.e_phnum = 1;
    h.e_shentsize = sizeof(Elf64_Shdr);
    memcpy(data, &h, sizeof h);
    for (size_t n = 0; n < sizeof h; n++) {
        view.size = n; assert(!avxemu_elf_functions(&view, sink, 0));
    }
    view.size = sizeof data;
    h.e_phoff = UINT64_MAX - 3; memcpy(data, &h, sizeof h);
    assert(!avxemu_elf_functions(&view, sink, 0));
    assert(calls == 0);
}


static uint8_t fixture[4096];
static const uint64_t base = 0x400000;
static void put(size_t off, const void *p, size_t n) { assert(off + n <= sizeof fixture); memcpy(fixture + off, p, n); }
static void put32(size_t off, uint32_t v) { put(off, &v, 4); }
static void synthetic(void) {
    memset(fixture, 0, sizeof fixture);
    Elf64_Ehdr h = {0}; memcpy(h.e_ident, ELFMAG, SELFMAG);
    h.e_ident[EI_CLASS] = ELFCLASS64; h.e_ident[EI_DATA] = ELFDATA2LSB;
    h.e_ident[EI_VERSION] = EV_CURRENT; h.e_type = ET_EXEC; h.e_machine = EM_X86_64;
    h.e_version = EV_CURRENT; h.e_ehsize = sizeof h;
    h.e_phoff = sizeof h; h.e_phnum = 1; h.e_phentsize = sizeof(Elf64_Phdr);
    h.e_shoff = 0x900; h.e_shnum = 9; h.e_shstrndx = 8; h.e_shentsize = sizeof(Elf64_Shdr);
    put(0, &h, sizeof h);
    Elf64_Phdr ph = {PT_LOAD, PF_R|PF_X, 0, 0x400000, 0, sizeof fixture, sizeof fixture, 0x1000};
    put(h.e_phoff, &ph, sizeof ph);
    const char names[] = "\0.text\0.other\0.avxemu_raw\0.avxemu_test\0.eh_frame\0.symtab\0.strtab\0.shstrtab\0";
    put(0x800, names, sizeof names);
    const size_t offsets[] = {0, 0x200, 0x280, 0x300, 0x320, 0x500, 0x600, 0x780, 0x800};
    const size_t sizes[] = {0, 64, 16, 8, 8, 64, 4*sizeof(Elf64_Sym), 12, sizeof names};
    size_t noff = 1;
    for (size_t i = 1; i < 9; i++) {
        Elf64_Shdr sh = {0}; sh.sh_name = noff; noff += strlen(names + noff) + 1;
        sh.sh_type = i < 6 ? SHT_PROGBITS : i == 6 ? SHT_SYMTAB : SHT_STRTAB;
        sh.sh_flags = i < 6 ? SHF_ALLOC | (i < 5 ? SHF_EXECINSTR : 0) : 0;
        sh.sh_addr = i < 6 ? base + offsets[i] : 0; sh.sh_offset = offsets[i]; sh.sh_size = sizes[i];
        if (i == 6) { sh.sh_link = 7; sh.sh_entsize = sizeof(Elf64_Sym); }
        put(h.e_shoff + i*sizeof sh, &sh, sizeof sh);
    }
    put(0x780, "\0one\0alias\0", 12);
    for (size_t i = 1; i < 4; i++) {
        Elf64_Sym sym = {0}; sym.st_name = 1; sym.st_info = ELF64_ST_INFO(STB_GLOBAL, i == 3 ? STT_OBJECT : STT_FUNC);
        sym.st_shndx = 1; sym.st_value = base + 0x200 + (i == 3 ? 8 : 0); sym.st_size = 8;
        put(0x600 + i*sizeof sym, &sym, sizeof sym);
    }
    const uint8_t cie[] = {16,0,0,0, 0,0,0,0, 1,'z','R',0, 1,0x78,16, 1,0x1b, 0,0,0};
    put(0x500, cie, sizeof cie);
    for (size_t i = 0; i < 2; i++) {
        size_t off = 0x514 + 20*i;
        put32(off, 16); put32(off+4, (uint32_t)(off+4-0x500));
        put32(off+8, (uint32_t)((base+0x200+32*i) - (base+off+8))); put32(off+12, 8);
    }
}
static Elf64_Shdr getsh(size_t i) { Elf64_Shdr s; memcpy(&s, fixture+0x900+i*sizeof s, sizeof s); return s; }
static void setsh(size_t i, Elf64_Shdr s) { put(0x900+i*sizeof s, &s, sizeof s); }
static void rejected(void) {
    avxemu_elf_view v = {fixture, sizeof fixture, 0}; calls = 0;
    assert(!avxemu_elf_functions(&v, sink, 0)); assert(calls == 0);
}
static int refuse(uint64_t b, uint64_t e, void *arg) { (void)b; (void)e; (void)arg; calls++; return 0; }
/* Linkers keep zero-length FDEs (discarded or empty functions) in .eh_frame and, as equal
 * starts, in .eh_frame_hdr. They cover no byte, so they must neither refuse the image nor
 * publish a bound; an out-of-order table is still refused. */
static void empty_fde_tests(void) {
    avxemu_elf_view v = {fixture, sizeof fixture, 0}; avxemu_elf e;
    static const char hn[] = ".eh_frame_hdr";
    synthetic();
    Elf64_Ehdr h; memcpy(&h, fixture, sizeof h); h.e_shnum = 10; put(0, &h, sizeof h);
    Elf64_Shdr str = getsh(8), eh = getsh(5), hdr = {0};
    put(0x800 + str.sh_size, hn, sizeof hn);
    hdr.sh_name = (uint32_t)str.sh_size; str.sh_size += sizeof hn; setsh(8, str);
    eh.sh_size = 80; setsh(5, eh);
    size_t off = 0x514 + 40;
    put32(off, 16); put32(off+4, (uint32_t)(off+4-0x500));
    put32(off+8, (uint32_t)((base+0x220) - (base+off+8))); put32(off+12, 0);
    hdr.sh_type = SHT_PROGBITS; hdr.sh_flags = SHF_ALLOC; hdr.sh_addr = base + 0x560;
    hdr.sh_offset = 0x560; hdr.sh_size = 36; setsh(9, hdr);
    const uint8_t head[] = {1, 0x1b, 0x03, 0x3b};
    put(0x560, head, sizeof head);
    put32(0x564, (uint32_t)(0x500 - 0x564)); put32(0x568, 3);
    const uint32_t begins[] = {0x200, 0x220, 0x220}, fdes[] = {0x514, 0x528, 0x53c};
    for (size_t i = 0; i < 3; i++) {
        put32(0x56c + 8*i, begins[i] - 0x560); put32(0x570 + 8*i, fdes[i] - 0x560);
    }
    assert(avxemu_elf_parse(&v, &e));
    assert(e.nfunctions == 2 && e.functions[1].begin == base+0x220 && e.functions[1].end == base+0x228);
    avxemu_elf_destroy(&e);
    put32(0x56c, 0x230 - 0x560); rejected();    /* table disagrees with the FDE */
}
static void synthetic_tests(void) {
    avxemu_elf_view v = {fixture, sizeof fixture, 0}; avxemu_elf e;
    synthetic(); assert(avxemu_elf_parse(&v, &e));
    assert(e.nranges == 2 && e.nfunctions == 2);
    assert(e.functions[0].begin == base+0x200 && e.functions[0].end == base+0x208);
    assert(e.functions[1].begin == base+0x220 && e.functions[1].end == base+0x228);
    avxemu_elf_destroy(&e);
    calls = 0; assert(!avxemu_elf_functions(&v, 0, 0)); assert(!calls);
    assert(!avxemu_elf_functions(&v, refuse, 0)); assert(calls == 1);
    /* Unaligned buffers remain valid. */
    uint8_t copy[sizeof fixture + 1]; memcpy(copy+1, fixture, sizeof fixture);
    avxemu_elf_view unaligned = {copy+1, sizeof fixture, 0};
    assert(avxemu_elf_parse(&unaligned, &e)); avxemu_elf_destroy(&e);
    for (unsigned i = 0; i < 3; i++) {
        synthetic(); fixture[i == 0 ? EI_CLASS : i == 1 ? EI_DATA : 18] = 0; rejected();
    }
    synthetic(); Elf64_Ehdr h; memcpy(&h, fixture, sizeof h); h.e_phnum = UINT16_MAX; put(0,&h,sizeof h); rejected();
    synthetic(); memcpy(&h, fixture, sizeof h); h.e_shoff = UINT64_MAX-3; put(0,&h,sizeof h); rejected();
    synthetic(); memcpy(&h, fixture, sizeof h); h.e_shnum = 0; put(0,&h,sizeof h); rejected();
    synthetic(); Elf64_Phdr ph; memcpy(&ph, fixture+sizeof h, sizeof ph); ph.p_vaddr = UINT64_MAX-4; put(sizeof h,&ph,sizeof ph); rejected();
    synthetic(); Elf64_Shdr sh = getsh(1); sh.sh_name = UINT32_MAX; setsh(1,sh); rejected();
    synthetic(); Elf64_Sym sym; memcpy(&sym,fixture+0x618,sizeof sym); sym.st_size = UINT64_MAX; put(0x618,&sym,sizeof sym); rejected();
    synthetic(); memcpy(&sym,fixture+0x618,sizeof sym); sym.st_size = 0x90; put(0x618,&sym,sizeof sym); rejected();
    synthetic(); memcpy(&sym,fixture+0x618,sizeof sym); sym.st_name = 100; put(0x618,&sym,sizeof sym); rejected();
    synthetic(); sh = getsh(5); sh.sh_size = 12; setsh(5,sh); rejected();
    synthetic(); sh = getsh(5); sh.sh_size = 30; setsh(5,sh); rejected();
    synthetic(); put32(0x514, UINT32_MAX); rejected();
    synthetic(); put32(0x518, 1); rejected();
    synthetic(); fixture[0x50a] = 'Q'; rejected();
    synthetic(); fixture[0x510] = 0x3b; rejected(); /* No datarel base in .eh_frame. */
    synthetic(); fixture[0x50f] = 20; rejected();
    /* A malformed late symbol must not leak earlier FDE callbacks. */
    synthetic(); sh = getsh(6); sh.sh_link = UINT32_MAX; setsh(6,sh); rejected();
    /* Conflicting aliases and a data island invalidate the entire function. */
    synthetic(); memcpy(&sym,fixture+0x630,sizeof sym); sym.st_size = 7; put(0x630,&sym,sizeof sym);
    assert(avxemu_elf_parse(&v,&e) && e.nfunctions == 1); avxemu_elf_destroy(&e);
    synthetic(); memcpy(&sym,fixture+0x648,sizeof sym); sym.st_value = base+0x204; put(0x648,&sym,sizeof sym);
    assert(avxemu_elf_parse(&v,&e) && e.nfunctions == 1); avxemu_elf_destroy(&e);
    /* No unwind: symbols authorize their exact bounds, never the next start. */
    synthetic(); sh = getsh(5); sh.sh_size = 0; setsh(5,sh);
    assert(avxemu_elf_parse(&v,&e) && e.nfunctions == 1 && e.functions[0].end == base+0x208); avxemu_elf_destroy(&e);
    /* With neither source of function bounds, executable ranges remain gaps. */
    sh = getsh(6); sh.sh_size = 0; setsh(6,sh);
    assert(avxemu_elf_parse(&v,&e) && e.nfunctions == 0); avxemu_elf_destroy(&e);
}

static void cross_range_conflicts(void) {
    avxemu_elf_view v = {fixture, sizeof fixture, 0}; avxemu_elf e;
    const uint32_t lengths[] = {8, 16, 0x50};
    const size_t expected[] = {2, 1, 0};
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
        synthetic(); put32(0x520, lengths[i]);
        assert(avxemu_elf_parse(&v, &e) && e.nfunctions == expected[i]);
        avxemu_elf_destroy(&e);
        calls = 0; assert(avxemu_elf_functions(&v, sink, 0) && calls == expected[i]);
    }
    /* A bound beginning before .text still contradicts the contained symbol. */
    synthetic(); put32(0x51c, (uint32_t)(0x1f8 - 0x51c)); put32(0x520, 0x10);
    assert(avxemu_elf_parse(&v, &e) && e.nfunctions == 1);
    assert(e.functions[0].begin == base+0x220); avxemu_elf_destroy(&e);
    /* Unrelated out-of-range metadata is neither published nor a conflict. */
    synthetic(); put32(0x51c, (uint32_t)(0x260 - 0x51c));
    assert(avxemu_elf_parse(&v, &e) && e.nfunctions == 2); avxemu_elf_destroy(&e);
    /* An allocated data section may overlap code without EXECINSTR. Its
     * cross-range object must block code even though it cannot be published. */
    synthetic(); Elf64_Shdr sh = getsh(2);
    sh.sh_flags = SHF_ALLOC; sh.sh_addr = base+0x1f8; sh.sh_offset = 0x1f8; sh.sh_size = 0x58;
    setsh(2, sh);
    Elf64_Sym sym; memcpy(&sym, fixture+0x648, sizeof sym);
    sym.st_shndx = 2; sym.st_value = base+0x1f8; sym.st_size = 0x58;
    put(0x648, &sym, sizeof sym);
    assert(avxemu_elf_parse(&v, &e) && e.nranges == 1 && e.nfunctions == 0);
    avxemu_elf_destroy(&e); calls = 0;
    assert(avxemu_elf_functions(&v, sink, 0) && calls == 0);
    /* Apply the same conflict rule to a symbol in that allocated section. */
    sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC); put(0x648, &sym, sizeof sym);
    assert(avxemu_elf_parse(&v, &e) && e.nfunctions == 0); avxemu_elf_destroy(&e);
    /* Non-allocated metadata does not describe live code or data. */
    sh.sh_flags = 0; setsh(2, sh);
    assert(avxemu_elf_parse(&v, &e) && e.nfunctions == 2); avxemu_elf_destroy(&e);
}

static size_t putleb(size_t pos, uint64_t value) {
    do { uint8_t b = value & 0x7f; value >>= 7; fixture[pos++] = b | (value ? 0x80 : 0); } while(value);
    return pos;
}
static void encoding_tests(void) {
    avxemu_elf_view v = {fixture, sizeof fixture, 0}; avxemu_elf e;
    synthetic(); fixture[0x510] = 0; memset(fixture+0x514,0,44);
    put32(0x514,24); put32(0x518,24);
    uint64_t addr=base+0x220, len=8; put(0x51c,&addr,8); put(0x524,&len,8);
    assert(avxemu_elf_parse(&v,&e) && e.nfunctions==2); avxemu_elf_destroy(&e);
    synthetic(); fixture[0x510] = 1; memset(fixture+0x514,0,44);
    put32(0x518,24); size_t end=putleb(0x51c,base+0x220); end=putleb(end,8); fixture[end++]=0;
    put32(0x514,(uint32_t)(end-0x518));
    assert(avxemu_elf_parse(&v,&e) && e.nfunctions==2); avxemu_elf_destroy(&e);
    synthetic(); memset(fixture+0x50c,0x80,8); rejected();
    synthetic(); fixture[0x510]=0x7f; rejected();
    synthetic(); fixture[0x510]=0x9b; rejected();
    synthetic(); fixture[0x510]=0x3b; Elf64_Shdr orphan=getsh(5); orphan.sh_size=20; setsh(5,orphan); rejected();
    synthetic(); put32(0x520,UINT32_MAX); rejected();
    synthetic(); Elf64_Shdr sh=getsh(3); sh.sh_addr=base+0x200; sh.sh_offset=0x200; setsh(3,sh); rejected();
    /* Different Build-ID lengths stay distinct even with equal zero padding. */
    synthetic(); Elf64_Ehdr h; memcpy(&h,fixture,sizeof h); h.e_phnum=2; put(0,&h,sizeof h);
    Elf64_Phdr ph={PT_NOTE,PF_R,0x400,0x400400,0,20,20,4}; put(sizeof h+sizeof ph,&ph,sizeof ph);
    Elf64_Nhdr note={4,3,NT_GNU_BUILD_ID}; put(0x400,&note,sizeof note); put(0x40c,"GNU",4);
    fixture[0x410]=1; fixture[0x411]=2; fixture[0x412]=3;
    assert(avxemu_elf_parse(&v,&e)); uint8_t id[32]; memcpy(id,e.build_id,32); avxemu_elf_destroy(&e);
    note.n_descsz=4; put(0x400,&note,sizeof note);
    assert(avxemu_elf_parse(&v,&e)); assert(memcmp(id,e.build_id,32)); avxemu_elf_destroy(&e);
    note.n_namesz=UINT32_MAX; put(0x400,&note,sizeof note); rejected();
    /* Every shortened view of this load is invalid and publishes nothing. */
    synthetic();
    for (size_t n=0;n<sizeof fixture;n++) { v.size=n; calls=0; assert(!avxemu_elf_functions(&v,sink,0) && !calls); }
    v.size=sizeof fixture;
    uint32_t random=0x12345678;
    for (unsigned i=0;i<1000;i++) {
        synthetic(); random=random*1664525+1013904223; size_t off=random%sizeof fixture;
        fixture[off]^=(uint8_t)(random>>16); calls=0;
        if (!avxemu_elf_functions(&v,sink,0)) assert(!calls);
    }
}
static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path,"rb"); assert(f && !fseek(f,0,SEEK_END)); long len = ftell(f); assert(len > 0);
    rewind(f); uint8_t *p = malloc((size_t)len); assert(p && fread(p,1,(size_t)len,f)==(size_t)len);
    fclose(f); *n = (size_t)len; return p;
}
static uint64_t symbol(const avxemu_elf_view *v, const char *wanted) {
    Elf64_Ehdr h; memcpy(&h,v->data,sizeof h);
    for (size_t i=0;i<h.e_shnum;i++) {
        Elf64_Shdr sh; memcpy(&sh,v->data+h.e_shoff+i*sizeof sh,sizeof sh);
        if (sh.sh_type != SHT_SYMTAB) continue;
        Elf64_Shdr str; memcpy(&str,v->data+h.e_shoff+sh.sh_link*sizeof str,sizeof str);
        for (size_t j=0;j<sh.sh_size/sizeof(Elf64_Sym);j++) {
            Elf64_Sym sym; memcpy(&sym,v->data+sh.sh_offset+j*sizeof sym,sizeof sym);
            if (!strcmp((const char *)v->data+str.sh_offset+sym.st_name,wanted)) return sym.st_value;
        }
    }
    assert(!"missing fixture symbol"); return 0;
}
static void real_tests(const char *plain, const char *stripped) {
    size_t n; uint8_t *p=read_file(plain,&n); avxemu_elf_view v={p,n,0}; avxemu_elf e;
    assert(avxemu_elf_parse(&v,&e));
    uint64_t begin=symbol(&v,"elf_asm_probe"), end=symbol(&v,"elf_asm_end");
    uint64_t data=symbol(&v,"elf_data_island"), raw=symbol(&v,"elf_raw_probe"), test=symbol(&v,"elf_test_probe");
    avxemu_elf_destroy(&e); free(p);
    for (unsigned pass=0;pass<2;pass++) {
        p=read_file(pass ? stripped : plain,&n); v=(avxemu_elf_view){p,n,0};
        assert(avxemu_elf_parse(&v,&e)); int found=0;
        for (size_t i=0;i<e.nfunctions;i++) {
            avxemu_elf_bound f=e.functions[i];
            if (f.begin==begin && f.end==end) found++;
            assert(data < f.begin || data >= f.end);
            assert(raw < f.begin || raw >= f.end); assert(test < f.begin || test >= f.end);
        }
        assert(found==1);
        for (size_t i=0;i<e.nranges;i++) {
            assert(raw < e.ranges[i].begin || raw >= e.ranges[i].end);
            assert(test < e.ranges[i].begin || test >= e.ranges[i].end);
        }
        avxemu_elf_destroy(&e); free(p);
    }
}
int main(int argc, char **argv) {
    malformed(); synthetic_tests(); empty_fde_tests(); cross_range_conflicts(); encoding_tests();
    assert(argc == 1 || argc == 5);
    if (argc == 5) { real_tests(argv[1],argv[2]); real_tests(argv[3],argv[4]); }
    puts("elf-contract: OK"); return 0;
}
