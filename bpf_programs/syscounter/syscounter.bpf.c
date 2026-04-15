#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "syscounter.h"

#define MYOP_OPEN 2
#define MYOP_READ 0
#define MYOP_WRITE 1

// Define a map to store the pids to be filtered (i.e., traced)
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_PIDS);
    __type(key, u32);
    __type(value, u32);
} pids_to_consider SEC(".maps");

// map to store temporary entries
// the key is just the tid because a thread can't do multiple syscalls at the same time, that is,
// if it enters one, it must exit before entering another one
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_PIDS*MAX_THREADS);
	__type(key, u32);
	__type(value, u64);
} enter_timestamps SEC(".maps");

// Define a map to store the final counters for each pid and syscall
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_PIDS*MAX_OPS);
    __type(key, CounterKey);
    __type(value, CounterValue);
} counters SEC(".maps");


/* Function to check if pid is on pids_to_consider map or not
 * Returns 1 (to discard) or 0 (to collect)
 */
int to_discard (u32 pid) { // 1 - discard, 0 - collect
    u32 *pid_p = bpf_map_lookup_elem(&pids_to_consider, &pid);
    if(pid_p) return 0;
    return 1;
}

/* Function to get struct file* from a file descriptor
 * Receives a file descriptor (int fd) as argument
 * Returns the corresponding struct file* or NULL
 */
struct file* get_file_from_fd(int fd) {
	if (fd < 0) return NULL;
    struct task_struct *ts = (struct task_struct*)bpf_get_current_task();
    struct file **fd_array = BPF_CORE_READ(ts, files, fdt, fd);
    struct file *file;
    bpf_core_read(&file, sizeof(file), &fd_array[fd]);
    return file;
}

/* Function to check if the file mode is regular file or not
 * Receives a umode_t mode as argument
 * Returns 1 (not regular file) or 0 (regular file)
 */
int check_file_mode(umode_t mode) {
    if ((mode & S_IFMT) != S_IFREG) return 1;
    return 0;
}

/* Function to check if the file mode is regular file or not
 * Receives a struct file* as argument
 * Returns 1 (not regular file) or 0 (regular file)
 */
int check_file_mode_from_file(struct file *file) {
    struct dentry *d = BPF_CORE_READ(file, f_path.dentry);
    struct inode *inode = BPF_CORE_READ(d, d_inode);
    umode_t mode = BPF_CORE_READ(inode, i_mode);
    return check_file_mode(mode);
}

/* This function saves the enter timestamp for a given thread.
 * The op id isn't needed because a single thread can only perform one syscall at a time.
 * Returns 0 on success or 1 on failure
 */
int update_counter_enter(u32 tid) {

	u64 timestamp = bpf_ktime_get_ns();

    // always insert even if there was a value already
	// this way, we ignore enters that never got their respective exit for some reason
	bpf_map_update_elem(&enter_timestamps, &tid, &timestamp, BPF_ANY);
    return 0;
}

/*
 * This one takes the timestamp stored in update_counter_enter for the given thread and calculates the delta.
 * The final result is stored by pid, command and op. 
 * If it didn't exist, we just ignore it, so we don't introduce time measuring errors.
 */
int update_counter_exit(u32 pid, u32 tid, int op) {

    CounterKey key = {
        .pid = pid,
        .op = op
    };
    bpf_get_current_comm(&key.command, sizeof(key.command));

    u64 *enter_time = bpf_map_lookup_elem(&enter_timestamps, &tid);
    if (enter_time) {
		// delete the entry to avoid possible exits without a corresponding enter performing erroneous calculations
		bpf_map_delete_elem(&enter_timestamps, &tid);
		
		CounterValue *value = bpf_map_lookup_elem(&counters, &key);

		u64 timestamp = bpf_ktime_get_ns();
		u64 delta = timestamp - *enter_time;
	
		// if entry is NULL, it's the first time this key is being updated
		if (value == NULL) {
			CounterValue new = {1, delta};
			bpf_map_update_elem(&counters, &key, &new, BPF_NOEXIST);
		}
		else {
			__sync_fetch_and_add(&value->time_sum, delta);
			__sync_fetch_and_add(&value->count, 1);
		}

        return 0;
    }
	
	// if the entry isn't found, something's not right...
	return -1;
}

/* ----SYSCALL-----  */
/* Probes at the system call layer for open, read, pread, write, pwrite and close operations */

SEC("tp/syscalls/sys_enter_openat")
int BPF_PROG(sys_enter_openat, struct pt_regs *regs, int syscall_id, int dfd,
			 const char * filename, int flags, umode_t mode) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;

    // update counter for this pid and syscall openat
    return update_counter_enter(tid);
}

SEC("tp/syscalls/sys_exit_openat")
int BPF_PROG(sys_exit_openat, struct pt_regs *regs, int syscall_id, long ret) {

    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(ret);
    if (file != NULL && check_file_mode_from_file(file)) {
		// if this fd is not from a file, delete the entry in the timestamp map to avoid
		// filling it with non-file entries
		bpf_map_delete_elem(&enter_timestamps, &tid);
		return 0;
	}
	
	return update_counter_exit(pid, tid, syscall_id);
}

SEC("tp/syscalls/sys_enter_write")
int BPF_PROG(sys_enter_write, struct pt_regs *regs, long syscall_id, u32 fd,
			 char *buf, size_t count) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(fd);
    if (check_file_mode_from_file(file)) return 0;

    // update counter for this pid and syscall write
    return update_counter_enter(tid);
}

SEC("tp/syscalls/sys_exit_write")
int BPF_PROG(sys_exit_write, struct pt_regs *regs, int syscall_id, long ret) {

    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, tid, syscall_id);
}

SEC("tp/syscalls/sys_enter_pwrite64")
int BPF_PROG(sys_enter_pwrite64, struct pt_regs *regs, long syscall_id, u32 fd,
			 char *buf, size_t count) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(fd);
    if (check_file_mode_from_file(file)) return 0;

    // update counter for this pid and syscall pwrite64
    return update_counter_enter(tid);
}

SEC("tp/syscalls/sys_exit_pwrite64")
int BPF_PROG(sys_exit_pwrite64, struct pt_regs *regs, int syscall_id, long ret) {

    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, tid, syscall_id);
}

SEC("tp/syscalls/sys_enter_read")
int BPF_PROG(sys_enter_read, struct pt_regs *regs, long syscall_id, u32 fd,
			 char *buf, size_t count) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(fd);
    if (check_file_mode_from_file(file)) return 0;

    // update counter for this pid and syscall read
    return update_counter_enter(tid);
}

SEC("tp/syscalls/sys_exit_read")
int BPF_PROG(sys_exit_read, struct pt_regs *regs, int syscall_id, long ret) {

    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, tid, syscall_id);
}

SEC("tp/syscalls/sys_enter_pread64")
int BPF_PROG(sys_enter_pread64, struct pt_regs *regs, long syscall_id, u32 fd,
			 char *buf, size_t count) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(fd);
    if (check_file_mode_from_file(file)) return 0;

    // update counter for this pid and syscall pread64
    return update_counter_enter(tid);
}

SEC("tp/syscalls/sys_exit_pread64")
int BPF_PROG(sys_exit_pread64, struct pt_regs *regs, int syscall_id, long ret) {

    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, tid, syscall_id);
}

SEC("tp/syscalls/sys_enter_close")
int BPF_PROG(sys_enter_close, struct pt_regs *regs, long syscall_id, u32 fd) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(fd);
    if (check_file_mode_from_file(file)) return 0;

    // update counter for this pid and syscall close
    return update_counter_enter(tid);
}

SEC("tp/syscalls/sys_exit_close")
int BPF_PROG(sys_exit_close, struct pt_regs *regs, int syscall_id, long ret) {

    u64 pidtid = bpf_get_current_pid_tgid();
	u32 pid = pidtid >> 32;
	u32 tid = pidtid;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, tid, syscall_id);
}

/* ----PROC-----  */

// sudo cat /sys/kernel/tracing/events/sched/sched_process_fork/format
SEC("tracepoint/sched/sched_process_fork")
int trace_sched_process_fork(struct trace_event_raw_sched_process_fork *ctx)
{
    pid_t parent_pid = ctx->parent_pid;
    pid_t child_pid = ctx->child_pid;
    int value=1;

    // check if the parent pid is in the filter list or not. If not, return 0 to ignore this event.
    if (to_discard(parent_pid)) return 0;

    // Add the child pid to the filter list to trace it as well
    bpf_map_update_elem(&pids_to_consider, &child_pid, &value, 0);

    return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
