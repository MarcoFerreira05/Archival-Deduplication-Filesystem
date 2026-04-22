// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2021 Wenbo Zhang
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define MAX_PIDS 100

__s64 total = 0;	/* total cache accesses without counting dirties */
__s64 misses = 0;	/* total of add to lru because of read misses */
__u64 mbd = 0;  	/* total of mark_buffer_dirty events */

// map to store	pids to be considered
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_PIDS);
	__type(key, u32);
	__type(value, u32);
} pids_to_consider SEC(".maps");

// return true if the pid must not be considered
int to_ignore(u32 pid) {
	u32 *pid_to_check = bpf_map_lookup_elem(&pids_to_consider, &pid);
	if(pid_to_check != NULL) return 0;
	else return 1;
}

SEC("fentry/add_to_page_cache_lru")
int BPF_PROG(fentry_add_to_page_cache_lru) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&misses, 1);
	return 0;
}

SEC("fentry/mark_page_accessed")
int BPF_PROG(fentry_mark_page_accessed) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&total, 1);
	return 0;
}

SEC("fentry/account_page_dirtied")
int BPF_PROG(fentry_account_page_dirtied) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&misses, -1);
	return 0;
}

SEC("fentry/mark_buffer_dirty")
int BPF_PROG(fentry_mark_buffer_dirty) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) {
		__sync_fetch_and_add(&total, -1);
		__sync_fetch_and_add(&mbd, 1);
	}
	return 0;
}

SEC("kprobe/add_to_page_cache_lru")
int BPF_KPROBE(kprobe_add_to_page_cache_lru) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&misses, 1);
	return 0;
}

SEC("kprobe/mark_page_accessed")
int BPF_KPROBE(kprobe_mark_page_accessed) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&total, 1);
	return 0;
}

SEC("kprobe/account_page_dirtied")
int BPF_KPROBE(kprobe_account_page_dirtied) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&misses, -1);
	return 0;
}

SEC("kprobe/folio_account_dirtied")
int BPF_KPROBE(kprobe_folio_account_dirtied) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&misses, -1);
	return 0;
}

SEC("kprobe/mark_buffer_dirty")
int BPF_KPROBE(kprobe_mark_buffer_dirty) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) {
		__sync_fetch_and_add(&total, -1);
		__sync_fetch_and_add(&mbd, 1);
	}
	return 0;
}

SEC("tracepoint/writeback/writeback_dirty_folio")
int tracepoint__writeback_dirty_folio(struct trace_event_raw_sys_enter* ctx) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&misses, -1);
	return 0;
}

SEC("tracepoint/writeback/writeback_dirty_page")
int tracepoint__writeback_dirty_page(struct trace_event_raw_sys_enter* ctx) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&misses, -1);
	return 0;
}

SEC("fentry/folio_mark_accessed")
int BPF_PROG(fentry_folio_mark_accessed) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&total, 1);
	return 0;
}

SEC("kprobe/folio_mark_accessed")
int BPF_KPROBE(kprobe_folio_mark_accessed) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&total, 1);
	return 0;
}

SEC("fentry/folio_add_lru")
int BPF_PROG(fentry_folio_add_lru) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&misses, 1);
	return 0;
}

SEC("kprobe/folio_add_lru")
int BPF_KPROBE(kprobe_folio_add_lru) {
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	if(!to_ignore(pid)) __sync_fetch_and_add(&misses, 1);
	return 0;
}

// sudo cat /sys/kernel/tracing/events/sched/sched_process_fork/format
SEC("tracepoint/sched/sched_process_fork")
int trace_sched_process_fork(struct trace_event_raw_sched_process_fork *ctx)
{
    pid_t parent_pid = ctx->parent_pid;
    pid_t child_pid = ctx->child_pid;
    int value=1;

    // check if the parent pid is in the filter list or not. If not, return 0 to ignore this event.
    if (to_ignore(parent_pid)) return 0;

    // Add the child pid to the filter list to trace it as well
    bpf_map_update_elem(&pids_to_consider, &child_pid, &value, 0);

    return 0;
}

char LICENSE[] SEC("license") = "GPL";
