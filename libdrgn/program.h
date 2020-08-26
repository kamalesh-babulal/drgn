// Copyright (c) Facebook, Inc. and its affiliates.
// SPDX-License-Identifier: GPL-3.0+

/**
 * @file
 *
 * Program internals.
 *
 * See @ref ProgramInternals.
 */

#ifndef DRGN_PROGRAM_H
#define DRGN_PROGRAM_H

#include <elfutils/libdwfl.h>
#ifdef WITH_LIBKDUMPFILE
#include <libkdumpfile/kdumpfile.h>
#endif

#include "hash_table.h"
#include "memory_reader.h"
#include "object_index.h"
#include "platform.h"
#include "type_index.h"
#include "vector.h"

/**
 * @ingroup Internals
 *
 * @defgroup ProgramInternals Programs
 *
 * Program internals.
 *
 * @{
 */

/** The important parts of the VMCOREINFO note of a Linux kernel core. */
struct vmcoreinfo {
	/** <tt>uname -r</tt> */
	char osrelease[128];
	/** PAGE_SIZE of the kernel. */
	uint64_t page_size;
	/**
	 * The offset from the compiled address of the kernel image to its
	 * actual address in memory.
	 *
	 * This is non-zero if kernel address space layout randomization (KASLR)
	 * is enabled.
	 */
	uint64_t kaslr_offset;
	/** Kernel page table. */
	uint64_t swapper_pg_dir;
	/** Whether 5-level paging was enabled. */
	bool pgtable_l5_enabled;
};

DEFINE_VECTOR_TYPE(drgn_prstatus_vector, struct string)
DEFINE_HASH_MAP_TYPE(drgn_prstatus_map, uint32_t, struct string)

struct drgn_dwarf_info_cache;
struct drgn_dwarf_index;

struct drgn_program {
	/** @privatesection */

	/*
	 * Memory/core dump.
	 */
	struct drgn_memory_reader reader;
	/* Elf core dump or /proc/pid/mem file segments. */
	struct drgn_memory_file_segment *file_segments;
	/* Elf core dump. Not valid for live programs or kdump files. */
	Elf *core;
	/* File descriptor for ELF core dump, kdump file, or /proc/pid/mem. */
	int core_fd;
	/* PID of live userspace program. */
	pid_t pid;
#ifdef WITH_LIBKDUMPFILE
	kdump_ctx_t *kdump_ctx;
#endif

	/*
	 * Debugging information.
	 */
	struct drgn_type_index tindex;
	struct drgn_object_index oindex;
	struct drgn_dwarf_info_cache *_dicache;

	/*
	 * Program information.
	 */
	/* Default language of the program. */
	const struct drgn_language *lang;
	struct drgn_platform platform;
	bool has_platform;
	enum drgn_program_flags flags;

	/*
	 * Stack traces.
	 */
	union {
		/*
		 * For the Linux kernel, PRSTATUS notes indexed by CPU. See @ref
		 * drgn_architecture_info::linux_kernel_set_initial_registers
		 * for why we don't use the PID map.
		 */
		struct drgn_prstatus_vector prstatus_vector;
		/* For userspace programs, PRSTATUS notes indexed by PID. */
		struct drgn_prstatus_map prstatus_map;
	};
	/* See @ref drgn_object_stack_trace(). */
	struct drgn_error *stack_trace_err;
	/* See @ref drgn_object_stack_trace_next_thread(). */
	const struct drgn_object *stack_trace_obj;
	uint32_t stack_trace_tid;
	bool prstatus_cached;
	bool attached_dwfl_state;

	/*
	 * Linux kernel-specific.
	 */
	struct vmcoreinfo vmcoreinfo;
	/* Cached PAGE_OFFSET. */
	uint64_t page_offset;
	/* Cached vmemmap. */
	uint64_t vmemmap;
	/* Cached THREAD_SIZE. */
	uint64_t thread_size;
	/* Cache for @ref linux_helper_task_state_to_char(). */
	char *task_state_chars;
	uint64_t task_report;
	/* Page table iterator for linux_helper_read_vm(). */
	struct pgtable_iterator *pgtable_it;
	/*
	 * Whether @ref drgn_program::pgtable_it is currently being used. Used
	 * to prevent address translation from recursing.
	 */
	bool pgtable_it_in_use;
};

/** Initialize a @ref drgn_program. */
void drgn_program_init(struct drgn_program *prog,
		       const struct drgn_platform *platform);

/** Deinitialize a @ref drgn_program. */
void drgn_program_deinit(struct drgn_program *prog);

/**
 * Set the @ref drgn_platform of a @ref drgn_program if it hasn't been set
 * yet.
 */
void drgn_program_set_platform(struct drgn_program *prog,
			       const struct drgn_platform *platform);

/**
 * Implement @ref drgn_program_from_core_dump() on an initialized @ref
 * drgn_program.
 */
struct drgn_error *drgn_program_init_core_dump(struct drgn_program *prog,
					       const char *path);

/**
 * Implement @ref drgn_program_from_kernel() on an initialized @ref
 * drgn_program.
 */
struct drgn_error *drgn_program_init_kernel(struct drgn_program *prog);

/**
 * Implement @ref drgn_program_from_pid() on an initialized @ref drgn_program.
 */
struct drgn_error *drgn_program_init_pid(struct drgn_program *prog, pid_t pid);

static inline struct drgn_error *
drgn_program_is_little_endian(struct drgn_program *prog, bool *ret)
{
	if (!prog->has_platform) {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "program byte order is not known");
	}
	*ret = prog->platform.flags & DRGN_PLATFORM_IS_LITTLE_ENDIAN;
	return NULL;
}

/**
 * Return whether a @ref drgn_program has a different endianness than the host
 * system.
 */
static inline struct drgn_error *
drgn_program_bswap(struct drgn_program *prog, bool *ret)
{
	bool is_little_endian;
	struct drgn_error *err = drgn_program_is_little_endian(prog,
							       &is_little_endian);
	if (err)
		return err;
	*ret = is_little_endian != (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
	return NULL;
}

static inline struct drgn_error *
drgn_program_is_64_bit(struct drgn_program *prog, bool *ret)
{
	if (!prog->has_platform) {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "program word size is not known");
	}
	*ret = prog->platform.flags & DRGN_PLATFORM_IS_64_BIT;
	return NULL;
}

struct drgn_error *drgn_program_get_dwfl(struct drgn_program *prog, Dwfl **ret);

/**
 * Find the @c NT_PRSTATUS note for the given CPU.
 *
 * This is only valid for the Linux kernel.
 *
 * @param[out] ret Returned note data. If not found, <tt>ret->str</tt> is set to
 * @c NULL and <tt>ret->len</tt> is set to zero.
 * @param[out] tid_ret Returned thread ID of note.
 */
struct drgn_error *drgn_program_find_prstatus_by_cpu(struct drgn_program *prog,
						     uint32_t cpu,
						     struct string *ret,
						     uint32_t *tid_ret);

/**
 * Find the @c NT_PRSTATUS note for the given thread ID.
 *
 * This is only valid for userspace programs.
 *
 * @param[out] ret Returned note data. If not found, <tt>ret->str</tt> is set to
 * @c NULL and <tt>ret->len</tt> is set to zero.
 */
struct drgn_error *drgn_program_find_prstatus_by_tid(struct drgn_program *prog,
						     uint32_t tid,
						     struct string *ret);

/**
 * Cache the @c NT_PRSTATUS note provided by @p data in @p prog.
 *
 * @param[in] data The pointer to the note data.
 * @param[in] size Size of data in note.
 */
struct drgn_error *drgn_program_cache_prstatus_entry(struct drgn_program *prog,
                                                     const char *data,
						     size_t size);

/*
 * Like @ref drgn_program_find_symbol_by_address(), but @p ret is already
 * allocated, we may already know the module, and doesn't return a @ref
 * drgn_error.
 *
 * @param[in] module Module containing the address. May be @c NULL, in which
 * case this will look it up.
 * @return Whether the symbol was found.
 */
bool drgn_program_find_symbol_by_address_internal(struct drgn_program *prog,
						  uint64_t address,
						  Dwfl_Module *module,
						  struct drgn_symbol *ret);

/** @} */

#endif /* DRGN_PROGRAM_H */
