#include "elf_loader.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <vector>
#include <cstring>
#include <cstdio>

#include "cpu.h"
#include "mem_win32.h"

// =============================================================================
// Self-Contained ELF32 Definitions for Windows
// =============================================================================
#ifndef EI_NIDENT
#define EI_NIDENT 16
#endif

using Elf32_Addr  = zb::u32;
using Elf32_Half  = zb::u16;
using Elf32_Off   = zb::u32;
using Elf32_Sword = zb::i32;
using Elf32_Word  = zb::u32;

#pragma pack(push, 1)
struct Elf32_Ehdr {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
};

struct Elf32_Phdr {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
};

struct Elf32_Dyn {
    Elf32_Sword d_tag;
    union {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
};

struct Elf32_Sym {
    Elf32_Word    st_name;
    Elf32_Addr    st_value;
    Elf32_Word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half    st_shndx;
};

struct Elf32_Rel {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
};
#pragma pack(pop)

// ELF Constants
constexpr unsigned char ELFMAG0 = 0x7f;
constexpr unsigned char ELFMAG1 = 'E';
constexpr unsigned char ELFMAG2 = 'L';
constexpr unsigned char ELFMAG3 = 'F';

constexpr int EI_MAG0    = 0;
constexpr int EI_MAG1    = 1;
constexpr int EI_MAG2    = 2;
constexpr int EI_MAG3    = 3;
constexpr int EI_CLASS   = 4;
constexpr int EI_DATA    = 5;

constexpr unsigned char ELFCLASS32 = 1;
constexpr unsigned char ELFDATA2LSB = 1;

constexpr Elf32_Half ET_DYN = 3;
constexpr Elf32_Half EM_ARM = 40;

constexpr Elf32_Word PT_NULL      = 0;
constexpr Elf32_Word PT_LOAD      = 1;
constexpr Elf32_Word PT_DYNAMIC   = 2;
constexpr Elf32_Word PT_ARM_EXIDX = 0x70000001;

constexpr Elf32_Word PF_X = 1;
constexpr Elf32_Word PF_W = 2;
constexpr Elf32_Word PF_R = 4;

constexpr Elf32_Sword DT_NULL         = 0;
constexpr Elf32_Sword DT_NEEDED       = 1;
constexpr Elf32_Sword DT_PLTRELSZ     = 2;
constexpr Elf32_Sword DT_PLTGOT       = 3;
constexpr Elf32_Sword DT_HASH         = 4;
constexpr Elf32_Sword DT_STRTAB       = 5;
constexpr Elf32_Sword DT_SYMTAB       = 6;
constexpr Elf32_Sword DT_RELA         = 7;
constexpr Elf32_Sword DT_STRSZ        = 10;
constexpr Elf32_Sword DT_INIT         = 12;
constexpr Elf32_Sword DT_FINI         = 13;
constexpr Elf32_Sword DT_SONAME       = 14;
constexpr Elf32_Sword DT_REL          = 17;
constexpr Elf32_Sword DT_RELSZ        = 18;
constexpr Elf32_Sword DT_RELENT       = 19;
constexpr Elf32_Sword DT_PLTREL       = 20;
constexpr Elf32_Sword DT_TEXTREL      = 22;
constexpr Elf32_Sword DT_JMPREL       = 23;
constexpr Elf32_Sword DT_INIT_ARRAY   = 25;
constexpr Elf32_Sword DT_FINI_ARRAY   = 26;
constexpr Elf32_Sword DT_INIT_ARRAYSZ = 27;
constexpr Elf32_Sword DT_FINI_ARRAYSZ = 28;
constexpr Elf32_Sword DT_GNU_HASH     = 0x6ffffef5;

#define ELF32_ST_BIND(val)     (((unsigned char)(val)) >> 4)
#define ELF32_ST_TYPE(val)     ((val) & 0xf)
constexpr unsigned char STB_GLOBAL = 1;
constexpr unsigned char STB_WEAK   = 2;
constexpr unsigned char STT_NOTYPE = 0;
constexpr unsigned char STT_OBJECT = 1;
constexpr unsigned char STT_FUNC   = 2;
constexpr Elf32_Half    SHN_UNDEF  = 0;

#define ELF32_R_SYM(i)         ((i) >> 8)
#define ELF32_R_TYPE(i)        ((unsigned char)(i))

constexpr unsigned char R_ARM_NONE      = 0;
constexpr unsigned char R_ARM_ABS32     = 2;
constexpr unsigned char R_ARM_GLOB_DAT  = 21;
constexpr unsigned char R_ARM_JUMP_SLOT = 22;
constexpr unsigned char R_ARM_RELATIVE  = 23;

// =============================================================================
// Implementation
// =============================================================================
namespace zb::elf {
namespace {

std::mutex g_lock;
std::vector<Module*> g_modules;
std::vector<PatchHook> g_patch_hooks;

std::string symbolize_impl(gaddr a) {
    Module* m = module_for_addr(a);
    char buf[64];
    if (!m) {
        snprintf(buf, sizeof(buf), "0x%08x", a);
        return buf;
    }
    const auto& f = m->sorted_funcs;
    auto it = std::upper_bound(f.begin(), f.end(), a,
                               [](gaddr v, const std::pair<gaddr, std::string>& e) { return v < e.first; });
    if (it == f.begin()) {
        snprintf(buf, sizeof(buf), "%s+0x%x", m->name.c_str(), a - m->bias);
        return buf;
    }
    --it;
    snprintf(buf, sizeof(buf), "+0x%x (%s+0x%x)", a - it->first, m->name.c_str(), a - m->bias);
    return it->second + buf;
}

}  // namespace

void add_patch_hook(PatchHook hook) {
    std::lock_guard lk(g_lock);
    g_patch_hooks.push_back(hook);
}

Module* load(const std::string& name, const u8* data, size_t size, Resolver resolve) {
    if (size < sizeof(Elf32_Ehdr)) {
        fatal("%s: file size (%zu) too small for ELF header", name.c_str(), size);
    }

    const auto* eh = reinterpret_cast<const Elf32_Ehdr*>(data);
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3 ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_machine != EM_ARM || eh->e_type != ET_DYN) {
        fatal("%s: not a valid ARM32 Little-Endian ELF shared object", name.c_str());
    }

    if (eh->e_phoff + eh->e_phnum * sizeof(Elf32_Phdr) > size) {
        fatal("%s: program headers exceed file size", name.c_str());
    }
    const auto* ph = reinterpret_cast<const Elf32_Phdr*>(data + eh->e_phoff);

    // Compute virtual address span for PT_LOAD segments
    u32 min_v = 0xFFFFFFFFu;
    u32 max_v = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        min_v = std::min(min_v, ph[i].p_vaddr & ~0xFFFu);
        max_v = std::max(max_v, ph[i].p_vaddr + ph[i].p_memsz);
    }
    if (min_v >= max_v) {
        fatal("%s: no valid PT_LOAD segments found", name.c_str());
    }

    u32 span = max_v - min_v;
    std::string tag = "zb:" + name;
    gaddr base = zb::mem::map_anon(span, 3 /* PROT_READ | PROT_WRITE */, tag.c_str());
    if (!base) {
        fatal("%s: failed to map %u bytes in guest memory", name.c_str(), span);
    }

    auto* m = new Module();
    m->name = name;
    m->bias = base - min_v;
    m->lo = base;
    m->hi = base + span;

    // Load segments and zero BSS
    gaddr dyn = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf32_Phdr& p = ph[i];
        if (p.p_type == PT_LOAD) {
            if (p.p_offset + p.p_filesz > size) {
                delete m;
                fatal("%s: truncated PT_LOAD segment offset %u size %u", name.c_str(), p.p_offset, p.p_filesz);
            }
            gaddr dst = m->bias + p.p_vaddr;
            std::memcpy(hp(dst), data + p.p_offset, p.p_filesz);
            if (p.p_memsz > p.p_filesz) {
                std::memset(hp(dst + p.p_filesz), 0, p.p_memsz - p.p_filesz);
            }
            if (!(p.p_flags & PF_W)) {
                m->rx_lo = dst;
                m->rx_hi = dst + p.p_memsz;
            }
        } else if (p.p_type == PT_DYNAMIC) {
            dyn = m->bias + p.p_vaddr;
        } else if (p.p_type == PT_ARM_EXIDX) {
            m->exidx = m->bias + p.p_vaddr;
            m->exidx_count = p.p_memsz / 8;
        }
    }

    if (!dyn) {
        delete m;
        fatal("%s: missing PT_DYNAMIC section", name.c_str());
    }

    // Parse Dynamic section
    gaddr symtab = 0, strtab = 0, hash = 0, gnu_hash = 0, rel = 0, jmprel = 0;
    gaddr init_array = 0, fini_array = 0;
    u32 relsz = 0, pltrelsz = 0, init_arraysz = 0, fini_arraysz = 0;
    std::vector<u32> needed_offsets;

    for (gaddr d = dyn;; d += sizeof(Elf32_Dyn)) {
        auto tag32 = zb::rd<Elf32_Sword>(d);
        u32 val = zb::rd<u32>(d + 4);
        if (tag32 == DT_NULL) break;

        switch (tag32) {
        case DT_SYMTAB:       symtab = m->bias + val; break;
        case DT_STRTAB:       strtab = m->bias + val; break;
        case DT_HASH:         hash = m->bias + val; break;
        case DT_GNU_HASH:     gnu_hash = m->bias + val; break;
        case DT_REL:          rel = m->bias + val; break;
        case DT_RELSZ:        relsz = val; break;
        case DT_JMPREL:       jmprel = m->bias + val; break;
        case DT_PLTRELSZ:     pltrelsz = val; break;
        case DT_INIT:         m->init = m->bias + val; break;
        case DT_INIT_ARRAY:   init_array = m->bias + val; break;
        case DT_INIT_ARRAYSZ: init_arraysz = val; break;
        case DT_FINI:         m->fini = m->bias + val; break;
        case DT_FINI_ARRAY:   fini_array = m->bias + val; break;
        case DT_FINI_ARRAYSZ: fini_arraysz = val; break;
        case DT_NEEDED:       needed_offsets.push_back(val); break;
        case DT_SONAME:       /* string offset in strtab */ break;
        case DT_RELA:
        case DT_TEXTREL:
            delete m;
            fatal("%s: unsupported dynamic tag %d", name.c_str(), tag32);
        default:
            break;
        }
    }

    if (!symtab || !strtab) {
        delete m;
        fatal("%s: missing dynamic symbol table or string table", name.c_str());
    }

    auto sym_at = [&](u32 i) { return hp<const Elf32_Sym>(symtab + i * sizeof(Elf32_Sym)); };
    auto str_at = [&](u32 off) { return hp<const char>(strtab + off); };

    // Record DT_NEEDED library names
    for (u32 off : needed_offsets) {
        m->needed.emplace_back(str_at(off));
    }

    // Determine number of symbols via SysV Hash or GNU Hash
    u32 nsyms = 0;
    if (hash) {
        nsyms = zb::rd<u32>(hash + 4);  // nchain
    } else if (gnu_hash) {
        u32 nbuckets = zb::rd<u32>(gnu_hash);
        u32 symoffset = zb::rd<u32>(gnu_hash + 4);
        u32 bloom = zb::rd<u32>(gnu_hash + 8);
        gaddr buckets = gnu_hash + 16 + bloom * 4;
        gaddr chains = buckets + nbuckets * 4;
        u32 last = 0;
        for (u32 b = 0; b < nbuckets; ++b) {
            last = std::max(last, zb::rd<u32>(buckets + 4 * b));
        }
        if (last >= symoffset) {
            while (!(zb::rd<u32>(chains + 4 * (last - symoffset)) & 1)) last++;
            nsyms = last + 1;
        } else {
            nsyms = symoffset;
        }
    }

    // Index exported symbols
    for (u32 i = 1; i < nsyms; ++i) {
        const Elf32_Sym* s = sym_at(i);
        unsigned char bind = ELF32_ST_BIND(s->st_info);
        unsigned char type = ELF32_ST_TYPE(s->st_info);
        if (s->st_shndx == SHN_UNDEF || (bind != STB_GLOBAL && bind != STB_WEAK)) continue;

        std::string sname = str_at(s->st_name);
        gaddr v = m->bias + s->st_value;
        m->exports.emplace(sname, v);
        if (type == STT_FUNC) {
            m->sorted_funcs.emplace_back(v & ~1u, sname);
        }
    }
    std::sort(m->sorted_funcs.begin(), m->sorted_funcs.end());

    std::string dep_list;
    for (const auto& s : m->needed) dep_list += s + " ";
    ZLOGI("%s: loaded at 0x%08x-0x%08x (bias 0x%08x), %u symbols, %zu exports, needs: %s",
          name.c_str(), m->lo, m->hi, m->bias, nsyms, m->exports.size(), dep_list.c_str());

    // Relocation processing with symbol cache
    std::unordered_map<u32, gaddr> resolved;
    auto resolve_sym = [&](u32 si, bool is_func) -> gaddr {
        if (auto it = resolved.find(si); it != resolved.end()) return it->second;
        const Elf32_Sym* s = sym_at(si);
        gaddr v = 0;
        if (s->st_shndx != SHN_UNDEF) {
            v = m->bias + s->st_value;
        } else {
            std::string sname = str_at(s->st_name);
            bool func = is_func || (ELF32_ST_TYPE(s->st_info) == STT_FUNC);
            if (resolve) {
                v = resolve(sname, func);
            }
            if (!v) {
                // Fallback to checking cpu registered thunks and data objects
                v = func ? cpu::find_thunk(sname) : cpu::find_data(sname);
                if (!v && !func) {
                    // Check if registered as thunk anyway
                    v = cpu::find_thunk(sname);
                }
            }
            if (!v) {
                unsigned char bind = ELF32_ST_BIND(s->st_info);
                if (bind == STB_WEAK) {
                    v = 0;
                } else if (func || ELF32_ST_TYPE(s->st_info) == STT_NOTYPE) {
                    v = cpu::missing_import_stub(sname);
                    m->missing.push_back(sname);
                } else {
                    ZLOGE("%s: unresolved data symbol %s", name.c_str(), sname.c_str());
                    v = zb::mem::gcalloc(1, 256);
                    m->missing.push_back(sname);
                }
            }
        }
        resolved.emplace(si, v);
        return v;
    };

    auto apply_relocations = [&](gaddr table, u32 byte_count) {
        for (gaddr r = table; r < table + byte_count; r += sizeof(Elf32_Rel)) {
            u32 off = zb::rd<u32>(r);
            u32 info = zb::rd<u32>(r + 4);
            u32 type = ELF32_R_TYPE(info);
            u32 si = ELF32_R_SYM(info);
            gaddr where = m->bias + off;

            switch (type) {
            case R_ARM_RELATIVE: {
                u32 addend = zb::rd<u32>(where);
                zb::wr<u32>(where, addend + m->bias);
                break;
            }
            case R_ARM_ABS32: {
                u32 addend = zb::rd<u32>(where);
                gaddr sym_val = si ? resolve_sym(si, false) : m->bias;
                zb::wr<u32>(where, addend + sym_val);
                break;
            }
            case R_ARM_GLOB_DAT: {
                gaddr sym_val = resolve_sym(si, false);
                zb::wr<u32>(where, sym_val);
                break;
            }
            case R_ARM_JUMP_SLOT: {
                gaddr sym_val = resolve_sym(si, true);
                zb::wr<u32>(where, sym_val);
                break;
            }
            case R_ARM_NONE:
                break;
            default:
                fatal("%s: unsupported relocation type %u at 0x%08x", name.c_str(), type, off);
            }
        }
    };

    if (rel) apply_relocations(rel, relsz);
    if (jmprel) apply_relocations(jmprel, pltrelsz);

    if (!m->missing.empty()) {
        std::string list;
        for (const auto& s : m->missing) list += s + " ";
        ZLOGW("%s: %zu imports without implementation: %s", name.c_str(), m->missing.size(), list.c_str());
    }

    // Populate static constructor pointers
    for (u32 i = 0; i < init_arraysz / 4; ++i) {
        m->init_array.push_back(zb::rd<u32>(init_array + 4 * i));
    }

    // Populate static destructor pointers
    for (u32 i = 0; i < fini_arraysz / 4; ++i) {
        m->fini_array.push_back(zb::rd<u32>(fini_array + 4 * i));
    }

    // Execute patch hooks while memory is still writable
    {
        std::lock_guard lk(g_lock);
        for (PatchHook h : g_patch_hooks) h(m);
    }

    // Apply read-only page protection to executable / code segments
    if (m->rx_hi > m->rx_lo) {
        u32 page = zb::mem::host_page_size();
        gaddr plo = (m->rx_lo + page - 1) & ~(page - 1);
        gaddr phi = m->rx_hi & ~(page - 1);
        if (phi > plo) {
            int err = 0;
            zb::mem::guest_mprotect(plo, phi - plo, 1 /* PROT_READ */, &err);
        }
        zb::mem::add_readonly_range(m->rx_lo, m->rx_hi);
    }

    {
        std::lock_guard lk(g_lock);
        g_modules.push_back(m);
    }
    cpu::set_symbolizer(&symbolize_impl);
    return m;
}

Module* load_file(const std::string& path, Resolver resolve) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        fatal("elf::load_file: cannot open '%s'", path.c_str());
    }
    auto size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<u8> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        fatal("elf::load_file: failed reading %zu bytes from '%s'", size, path.c_str());
    }

    // Extract module name from path
    std::string filename = path;
    auto pos = filename.find_last_of("/\\");
    if (pos != std::string::npos) filename = filename.substr(pos + 1);

    return load(filename, buffer.data(), buffer.size(), resolve);
}

void run_initializers(Module* m) {
    if (!m) return;
    if (m->init) {
        ZLOGD("%s: executing DT_INIT at 0x%08x", m->name.c_str(), m->init);
        cpu::call(m->init);
    }
    size_t n = 0;
    for (size_t i = 0; i < m->init_array.size(); ++i) {
        gaddr f = m->init_array[i];
        if (f == 0 || f == 0xFFFFFFFFu) continue;
        ZLOGD("%s: init_array[%zu] -> %s (0x%08x)", m->name.c_str(), i,
              cpu::symbolize(f & ~1u).c_str(), f);
        cpu::call(f);
        n++;
    }
    ZLOGI("%s: executed %zu static constructors", m->name.c_str(), n);
}

void run_destructors(Module* m) {
    if (!m) return;
    for (size_t i = m->fini_array.size(); i > 0; --i) {
        gaddr f = m->fini_array[i - 1];
        if (f == 0 || f == 0xFFFFFFFFu) continue;
        cpu::call(f);
    }
    if (m->fini) {
        cpu::call(m->fini);
    }
}

Module* find_module(const std::string& name) {
    std::lock_guard lk(g_lock);
    for (Module* m : g_modules) {
        if (m->name == name) return m;
    }
    return nullptr;
}

Module* module_for_addr(gaddr a) {
    std::lock_guard lk(g_lock);
    for (Module* m : g_modules) {
        if (a >= m->lo && a < m->hi) return m;
    }
    return nullptr;
}

gaddr resolve_symbol(const char* name) {
    if (!name) return 0;
    std::lock_guard lk(g_lock);
    for (Module* m : g_modules) {
        auto it = m->exports.find(name);
        if (it != m->exports.end()) return it->second;
    }
    return 0;
}

gaddr lookup(const std::string& name) {
    return resolve_symbol(name.c_str());
}

void unload(Module* m) {
    if (!m) return;
    run_destructors(m);
    {
        std::lock_guard lk(g_lock);
        auto it = std::find(g_modules.begin(), g_modules.end(), m);
        if (it != g_modules.end()) g_modules.erase(it);
    }
    zb::mem::unmap_anon(m->lo, m->hi - m->lo);
    delete m;
}

void unload_all() {
    std::vector<Module*> copy;
    {
        std::lock_guard lk(g_lock);
        copy = g_modules;
    }
    for (Module* m : copy) {
        unload(m);
    }
}

}  // namespace zb::elf
