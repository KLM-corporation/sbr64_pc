#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"

namespace zb::elf {

struct Module {
    std::string name;
    std::string soname;
    gaddr bias = 0;             // load bias: guest_addr = bias + p_vaddr
    gaddr lo = 0;               // mapped range [lo, hi)
    gaddr hi = 0;
    gaddr rx_lo = 0;            // read-only/executable segment [rx_lo, rx_hi)
    gaddr rx_hi = 0;
    gaddr exidx = 0;            // ARM.exidx base address
    u32 exidx_count = 0;        // ARM.exidx entry count
    gaddr init = 0;             // DT_INIT function pointer
    std::vector<gaddr> init_array; // DT_INIT_ARRAY function pointers
    gaddr fini = 0;             // DT_FINI function pointer
    std::vector<gaddr> fini_array; // DT_FINI_ARRAY function pointers
    std::vector<std::string> needed; // DT_NEEDED shared library names

    // Exported symbols: name -> relocated guest address (includes Thumb bit)
    std::unordered_map<std::string, gaddr> exports;
    // Sorted function entries for binary search symbolization: (vaddr_without_thumb_bit, name)
    std::vector<std::pair<gaddr, std::string>> sorted_funcs;
    // Missing imports that were assigned dummy stubs
    std::vector<std::string> missing;
};

// Resolver callback: returns guest address for symbol name, or 0 if unknown
using Resolver = gaddr (*)(const std::string& name, bool is_func);

// PatchHook: executed after relocations are applied while memory is still writable
using PatchHook = void (*)(Module*);
void add_patch_hook(PatchHook hook);

// Load an ARM32 ELF shared object from memory buffer
Module* load(const std::string& name, const u8* data, size_t size, Resolver resolve);

// Convenience wrapper: reads ELF file from disk into buffer and calls load()
Module* load_file(const std::string& path, Resolver resolve);

// Execute static constructors (DT_INIT and DT_INIT_ARRAY) via Dynarmic cpu::call()
void run_initializers(Module* m);

// Execute static destructors (DT_FINI_ARRAY and DT_FINI)
void run_destructors(Module* m);

// Find loaded module by name
Module* find_module(const std::string& name);

// Find loaded module containing specified guest address
Module* module_for_addr(gaddr a);

// Exported symbol lookup across all loaded modules
gaddr lookup(const std::string& name);
gaddr resolve_symbol(const char* name);

// Unload module and release guest memory
void unload(Module* m);
void unload_all();

}  // namespace zb::elf

