/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */
#ifndef __SCX_COMMON_BPF_H
#define __SCX_COMMON_BPF_H

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <asm-generic/errno.h>
#include "user_exit_info.h"

#define PF_WQ_WORKER			0x00000020	/* I'm a workqueue worker */
#define PF_KTHREAD			0x00200000	/* I am a kernel thread */
#define PF_EXITING			0x00000004
#define CLOCK_MONOTONIC			1

/*
 * Earlier versions of clang/pahole lost upper 32bits in 64bit enums which can
 * lead to really confusing misbehaviors. Let's trigger a build failure.
 */
static inline void ___vmlinux_h_sanity_check___(void)
{
	_Static_assert(SCX_DSQ_FLAG_BUILTIN,
		       "bpftool generated vmlinux.h is missing high bits for 64bit enums, upgrade clang and pahole");
}

void scx_bpf_error_bstr(char *fmt, unsigned long long *data, u32 data_len) __ksym;

static inline __attribute__((format(printf, 1, 2)))
void ___scx_bpf_error_format_checker(const char *fmt, ...) {}

/*
 * scx_bpf_error() wraps the scx_bpf_error_bstr() kfunc with variadic arguments
 * instead of an array of u64. Note that __param[] must have at least one
 * element to keep the verifier happy.
 */
#define scx_bpf_error(fmt, args...)						\
({										\
	static char ___fmt[] = fmt;						\
	unsigned long long ___param[___bpf_narg(args) ?: 1] = {};		\
										\
	_Pragma("GCC diagnostic push")						\
	_Pragma("GCC diagnostic ignored \"-Wint-conversion\"")			\
	___bpf_fill(___param, args);						\
	_Pragma("GCC diagnostic pop")						\
										\
	scx_bpf_error_bstr(___fmt, ___param, sizeof(___param));			\
										\
	___scx_bpf_error_format_checker(fmt, ##args);				\
})

void scx_bpf_switch_all(void) __ksym;
s32 scx_bpf_create_dsq(u64 dsq_id, s32 node) __ksym;
bool scx_bpf_consume(u64 dsq_id) __ksym;
void scx_bpf_dispatch(struct task_struct *p, u64 dsq_id, u64 slice, u64 enq_flags) __ksym;
void scx_bpf_dispatch_vtime(struct task_struct *p, u64 dsq_id, u64 slice, u64 vtime, u64 enq_flags) __ksym;
u32 scx_bpf_dispatch_nr_slots(void) __ksym;
void scx_bpf_dispatch_cancel(void) __ksym;
void scx_bpf_kick_cpu(s32 cpu, u64 flags) __ksym;
s32 scx_bpf_dsq_nr_queued(u64 dsq_id) __ksym;
bool scx_bpf_test_and_clear_cpu_idle(s32 cpu) __ksym;
s32 scx_bpf_pick_idle_cpu(const cpumask_t *cpus_allowed, u64 flags) __ksym;
s32 scx_bpf_pick_any_cpu(const cpumask_t *cpus_allowed, u64 flags) __ksym;
const struct cpumask *scx_bpf_get_idle_cpumask(void) __ksym;
const struct cpumask *scx_bpf_get_idle_smtmask(void) __ksym;
void scx_bpf_put_idle_cpumask(const struct cpumask *cpumask) __ksym;
void scx_bpf_destroy_dsq(u64 dsq_id) __ksym;
s32 scx_bpf_select_cpu_dfl(struct task_struct *p, s32 prev_cpu, u64 wake_flags, bool *is_idle) __ksym;
bool scx_bpf_task_running(const struct task_struct *p) __ksym;
s32 scx_bpf_task_cpu(const struct task_struct *p) __ksym;
struct cgroup *scx_bpf_task_cgroup(struct task_struct *p) __ksym;
u32 scx_bpf_reenqueue_local(void) __ksym;

#define BPF_STRUCT_OPS(name, args...)						\
SEC("struct_ops/"#name)								\
BPF_PROG(name, ##args)

#define BPF_STRUCT_OPS_SLEEPABLE(name, args...)					\
SEC("struct_ops.s/"#name)							\
BPF_PROG(name, ##args)

/**
 * RESIZABLE_ARRAY - Generates annotations for an array that may be resized
 * @elfsec: the data section of the BPF program in which to place the array
 * @arr: the name of the array
 *
 * libbpf has an API for setting map value sizes. Since data sections (i.e.
 * bss, data, rodata) themselves are maps, a data section can be resized. If
 * a data section has an array as its last element, the BTF info for that
 * array will be adjusted so that length of the array is extended to meet the
 * new length of the data section. This macro annotates an array to have an
 * element count of one with the assumption that this array can be resized
 * within the userspace program. It also annotates the section specifier so
 * this array exists in a custom sub data section which can be resized
 * independently.
 *
 * See RESIZE_ARRAY() for the userspace convenience macro for resizing an
 * array declared with RESIZABLE_ARRAY().
 */
#define RESIZABLE_ARRAY(elfsec, arr) arr[1] SEC("."#elfsec"."#arr)

/**
 * MEMBER_VPTR - Obtain the verified pointer to a struct or array member
 * @base: struct or array to index
 * @member: dereferenced member (e.g. .field, [idx0][idx1], .field[idx0] ...)
 *
 * The verifier often gets confused by the instruction sequence the compiler
 * generates for indexing struct fields or arrays. This macro forces the
 * compiler to generate a code sequence which first calculates the byte offset,
 * checks it against the struct or array size and add that byte offset to
 * generate the pointer to the member to help the verifier.
 *
 * Ideally, we want to abort if the calculated offset is out-of-bounds. However,
 * BPF currently doesn't support abort, so evaluate to %NULL instead. The caller
 * must check for %NULL and take appropriate action to appease the verifier. To
 * avoid confusing the verifier, it's best to check for %NULL and dereference
 * immediately.
 *
 *	vptr = MEMBER_VPTR(my_array, [i][j]);
 *	if (!vptr)
 *		return error;
 *	*vptr = new_value;
 *
 * sizeof(@base) should encompass the memory area to be accessed and thus can't
 * be a pointer to the area. Use `MEMBER_VPTR(*ptr, .member)` instead of
 * `MEMBER_VPTR(ptr, ->member)`.
 */
#define MEMBER_VPTR(base, member) (typeof((base) member) *)({			\
	u64 __base = (u64)&(base);						\
	u64 __addr = (u64)&((base) member) - __base;				\
	_Static_assert(sizeof(base) >= sizeof((base) member),			\
		       "@base is smaller than @member, is @base a pointer?");	\
	asm volatile (								\
		"if %0 <= %[max] goto +2\n"					\
		"%0 = 0\n"							\
		"goto +1\n"							\
		"%0 += %1\n"							\
		: "+r"(__addr)							\
		: "r"(__base),							\
		  [max]"i"(sizeof(base) - sizeof((base) member)));		\
	__addr;									\
})

/**
 * ARRAY_ELEM_PTR - Obtain the verified pointer to an array element
 * @arr: array to index into
 * @i: array index
 * @n: number of elements in array
 *
 * Similar to MEMBER_VPTR() but is intended for use with arrays where the
 * element count needs to be explicit.
 * It can be used in cases where a global array is defined with an initial
 * size but is intended to be be resized before loading the BPF program.
 * Without this version of the macro, MEMBER_VPTR() will use the compile time
 * size of the array to compute the max, which will result in rejection by
 * the verifier.
 */
#define ARRAY_ELEM_PTR(arr, i, n) (typeof(arr[i]) *)({	  \
	u64 __base = (u64)arr;				  \
	u64 __addr = (u64)&(arr[i]) - __base;		  \
	asm volatile (					  \
		"if %0 <= %[max] goto +2\n"		  \
		"%0 = 0\n"				  \
		"goto +1\n"				  \
		"%0 += %1\n"				  \
		: "+r"(__addr)				  \
		: "r"(__base),				  \
		  [max]"r"(sizeof(arr[0]) * ((n) - 1)));  \
	__addr;						  \
})

/*
 * BPF core and other generic helpers
 */

/* list and rbtree */
#define __contains(name, node) __attribute__((btf_decl_tag("contains:" #name ":" #node)))
#define private(name) SEC(".data." #name) __hidden __attribute__((aligned(8)))

void *bpf_obj_new_impl(__u64 local_type_id, void *meta) __ksym;
void bpf_obj_drop_impl(void *kptr, void *meta) __ksym;

#define bpf_obj_new(type) ((type *)bpf_obj_new_impl(bpf_core_type_id_local(type), NULL))
#define bpf_obj_drop(kptr) bpf_obj_drop_impl(kptr, NULL)

void bpf_list_push_front(struct bpf_list_head *head, struct bpf_list_node *node) __ksym;
void bpf_list_push_back(struct bpf_list_head *head, struct bpf_list_node *node) __ksym;
struct bpf_list_node *bpf_list_pop_front(struct bpf_list_head *head) __ksym;
struct bpf_list_node *bpf_list_pop_back(struct bpf_list_head *head) __ksym;
struct bpf_rb_node *bpf_rbtree_remove(struct bpf_rb_root *root,
				      struct bpf_rb_node *node) __ksym;
int bpf_rbtree_add_impl(struct bpf_rb_root *root, struct bpf_rb_node *node,
			bool (less)(struct bpf_rb_node *a, const struct bpf_rb_node *b),
			void *meta, __u64 off) __ksym;
#define bpf_rbtree_add(head, node, less) bpf_rbtree_add_impl(head, node, less, NULL, 0)

struct bpf_rb_node *bpf_rbtree_first(struct bpf_rb_root *root) __ksym;

/* task */
struct task_struct *bpf_task_from_pid(s32 pid) __ksym;
struct task_struct *bpf_task_acquire(struct task_struct *p) __ksym;
void bpf_task_release(struct task_struct *p) __ksym;

/* cgroup */
struct cgroup *bpf_cgroup_ancestor(struct cgroup *cgrp, int level) __ksym;
void bpf_cgroup_release(struct cgroup *cgrp) __ksym;
struct cgroup *bpf_cgroup_from_id(u64 cgid) __ksym;

/* cpumask */
struct bpf_cpumask *bpf_cpumask_create(void) __ksym;
struct bpf_cpumask *bpf_cpumask_acquire(struct bpf_cpumask *cpumask) __ksym;
void bpf_cpumask_release(struct bpf_cpumask *cpumask) __ksym;
u32 bpf_cpumask_first(const struct cpumask *cpumask) __ksym;
u32 bpf_cpumask_first_zero(const struct cpumask *cpumask) __ksym;
void bpf_cpumask_set_cpu(u32 cpu, struct bpf_cpumask *cpumask) __ksym;
void bpf_cpumask_clear_cpu(u32 cpu, struct bpf_cpumask *cpumask) __ksym;
bool bpf_cpumask_test_cpu(u32 cpu, const struct cpumask *cpumask) __ksym;
bool bpf_cpumask_test_and_set_cpu(u32 cpu, struct bpf_cpumask *cpumask) __ksym;
bool bpf_cpumask_test_and_clear_cpu(u32 cpu, struct bpf_cpumask *cpumask) __ksym;
void bpf_cpumask_setall(struct bpf_cpumask *cpumask) __ksym;
void bpf_cpumask_clear(struct bpf_cpumask *cpumask) __ksym;
bool bpf_cpumask_and(struct bpf_cpumask *dst, const struct cpumask *src1,
		     const struct cpumask *src2) __ksym;
void bpf_cpumask_or(struct bpf_cpumask *dst, const struct cpumask *src1,
		    const struct cpumask *src2) __ksym;
void bpf_cpumask_xor(struct bpf_cpumask *dst, const struct cpumask *src1,
		     const struct cpumask *src2) __ksym;
bool bpf_cpumask_equal(const struct cpumask *src1, const struct cpumask *src2) __ksym;
bool bpf_cpumask_intersects(const struct cpumask *src1, const struct cpumask *src2) __ksym;
bool bpf_cpumask_subset(const struct cpumask *src1, const struct cpumask *src2) __ksym;
bool bpf_cpumask_empty(const struct cpumask *cpumask) __ksym;
bool bpf_cpumask_full(const struct cpumask *cpumask) __ksym;
void bpf_cpumask_copy(struct bpf_cpumask *dst, const struct cpumask *src) __ksym;
u32 bpf_cpumask_any_distribute(const struct cpumask *cpumask) __ksym;
u32 bpf_cpumask_any_and_distribute(const struct cpumask *src1,
				   const struct cpumask *src2) __ksym;

/* rcu */
void bpf_rcu_read_lock(void) __ksym;
void bpf_rcu_read_unlock(void) __ksym;

#include "compat.bpf.h"

#endif	/* __SCX_COMMON_BPF_H */
