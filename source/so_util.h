/* so_util.h -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

#define SO_MAX_SEGMENTS 8
#define SO_STUB_SIZE 0x4000

typedef struct {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

typedef struct so_module {
  struct so_module *next;
  char name[64];

  // entire LOAD zone
  void *load_base, *load_virtbase;
  size_t load_size;
  void *load_memrv; // VirtmemReservation *

  // extra code-memory area reserved right after the LOAD zone (within
  // branch range of it); mapped later by ib3_shim.c, not by so_finalize
  void *stub_virtbase;
  size_t stub_size;

  // unmodified program headers (link-time vaddrs) for dl_iterate_phdr / unwinding
  Elf64_Phdr phdr[SO_MAX_SEGMENTS * 2];
  int phnum;

  // temporary file image
  void *so_base;
  size_t so_size;

  Elf64_Ehdr *elf_hdr;
  Elf64_Phdr *prog_hdr;
  Elf64_Shdr *sec_hdr;
  Elf64_Sym *syms;
  int num_syms;
  char *shstrtab;
  char *dynstrtab;

  // PT_TLS template (static TLS, laid out after the bionic slot area)
  void *tls_image;     // malloc'd copy of .tdata
  size_t tls_filesz, tls_memsz, tls_align;
  size_t tls_off;      // offset of this module's block inside the TLS area
  int tls_assigned;
} so_module;

// copy every module's .tdata into a fresh per-thread TLS block (block is
// already zeroed, so .tbss needs nothing)
void so_tls_init_block(void *block);

void hook_arm64(uintptr_t addr, uintptr_t dst);

void so_flush_caches(so_module *mod);
void so_free_temp(so_module *mod);
int so_load(so_module *mod, const char *filename, void *base, size_t max_size);
int so_relocate(so_module *mod);
int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
void so_execute_init_array(so_module *mod);
uintptr_t so_find_addr(so_module *mod, const char *symbol);
uintptr_t so_find_addr_rx(so_module *mod, const char *symbol);
// returns 0 instead of aborting when the symbol is missing
uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol);
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name);
void so_finalize(so_module *mod);
int so_unload(so_module *mod);

// dl_iterate_phdr() over all loaded modules; needed by the embedded libunwind
int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data);

#endif

