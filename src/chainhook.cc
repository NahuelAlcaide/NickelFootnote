// SPDX-License-Identifier: GPL-3.0-or-later

// Based on nh_dlhook (NickelHook/nh.c), except for where the original comes from.

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "chainhook.h"
#include "log.h"

void *nfn_chain_hook(const char *libname, const char *sym, void *target) {
    void *lib = dlopen(libname, RTLD_LAZY | RTLD_NODELETE); // as NickelHook does; already loaded
    if (!lib) {
        nfn_log("hook: %s not loaded", libname);
        return nullptr;
    }
    void *real = dlsym(lib, sym);
    if (!real) {
        nfn_log("hook: %s not found", sym);
        return nullptr;
    }

    struct link_map *lm;
    if (dlinfo(lib, RTLD_DI_LINKMAP, &lm)) {
        nfn_log("hook: no link_map for %s", libname);
        return nullptr;
    }

    ElfW(Rel) *plt = nullptr;
    size_t plt_sz = 0, ent_sz = 0;
    bool rela = false;
    ElfW(Sym) *symtab = nullptr;
    const char *strtab = nullptr;
    for (size_t i = 0; lm->l_ld[i].d_tag != DT_NULL; i++) {
        switch (lm->l_ld[i].d_tag) {
        case DT_PLTREL:   rela = lm->l_ld[i].d_un.d_val == DT_RELA; break;
        case DT_JMPREL:   plt = (ElfW(Rel)*)lm->l_ld[i].d_un.d_ptr; break;
        case DT_PLTRELSZ: plt_sz = lm->l_ld[i].d_un.d_val; break;
        case DT_RELENT:   if (!ent_sz) ent_sz = lm->l_ld[i].d_un.d_val; break;
        case DT_SYMTAB:   symtab = (ElfW(Sym)*)lm->l_ld[i].d_un.d_ptr; break;
        case DT_STRTAB:   strtab = (const char*)lm->l_ld[i].d_un.d_ptr; break;
        }
    }
    if (rela || !plt || !ent_sz || ent_sz != sizeof(ElfW(Rel)) || !symtab || !strtab) {
        nfn_log("hook: unexpected DT_DYNAMIC in %s", libname);
        return nullptr;
    }

    for (size_t i = 0; i < plt_sz / ent_sz; i++) {
        ElfW(Rel) *rel = &plt[i];
        if (ELF32_R_TYPE(rel->r_info) != R_ARM_JUMP_SLOT)
            continue;
        if (strcmp(&strtab[symtab[ELF32_R_SYM(rel->r_info)].st_name], sym))
            continue;

        void **got = (void**)(lm->l_addr + rel->r_offset);
        void *prev = *got;

        // An unresolved lazy-binding slot points back into libnickel's own PLT,
        // which can't be called directly; use the real function then. Anything
        // in another object is an earlier plugin's hook: chain to it.
        // Objects are compared by load address: the library may be mapped under
        // another name (libnickel.so.1.0.0 shows up as libnickel.so.1).
        void *next = real;
        Dl_info prev_info, real_info;
        const char *owner = "?";
        if (prev && prev != real && dladdr(prev, &prev_info) && dladdr(real, &real_info)) {
            owner = prev_info.dli_fname ? prev_info.dli_fname : "?";
            if (prev_info.dli_fbase && prev_info.dli_fbase != real_info.dli_fbase)
                next = prev;
        }

        long page = sysconf(_SC_PAGESIZE);
        void *got_page = (void*)((size_t)got & ~(page - 1));
        if (mprotect(got_page, page, PROT_READ | PROT_WRITE)) {
            nfn_log("hook: mprotect failed for %s", sym);
            return nullptr;
        }
        *got = target;
        nfn_log("hook: %s: previous %p (%s), calling %s", sym, prev, owner,
                 next == real ? "the original" : "the earlier hook");
        return next;
    }

    nfn_log("hook: %s has no PLT entry in %s", sym, libname);
    return nullptr;
}
