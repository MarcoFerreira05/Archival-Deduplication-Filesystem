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

// Define a map to store the counters for each pid and syscall
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_PIDS*MAX_OPS);
    __type(key, CounterKey);
    __type(value, CounterValue);
} counter SEC(".maps");


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

/* Function to update the counter for a given pid and syscall when entering the syscall
 * Receives a pid (u32 pid) and a syscall id (int op) as arguments
 * Returns 0 on success or 1 on failure
 */
int update_counter_enter(u32 pid, int op) {

    CounterKey key = {
        .pid = pid,
        .op = op
    };
    bpf_get_current_comm(&key.command, sizeof(key.command));
	u64 timestamp = bpf_ktime_get_ns();

    // First try to lookup the value for this key.
	// If found, increment the counter by 1 and save the timestamp.
    CounterValue *val = bpf_map_lookup_elem(&counter, &key);
    if (val) {
        __sync_fetch_and_add(&val->count, 1);
		__sync_fetch_and_add(&val->enter_time_sum, timestamp);
        return 0;
    }

    // If not found, insert with initial values
	CounterValue new = {1, timestamp, 0};
    bpf_map_update_elem(&counter, &key, &new, BPF_NOEXIST);
    return 0;
}

/*
 * Similar to the one above, but for exiting the syscall
 */
int update_counter_exit(u32 pid, int op) {

    CounterKey key = {
        .pid = pid,
        .op = op
    };
    bpf_get_current_comm(&key.command, sizeof(key.command));
	u64 timestamp = bpf_ktime_get_ns();

	// lookup the value to save the exit timestamp
    CounterValue *val = bpf_map_lookup_elem(&counter, &key);
    if (val) {
		__sync_fetch_and_add(&val->exit_time_sum, timestamp);
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
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;

    // update counter for this pid and syscall openat
    return update_counter_enter(pid, syscall_id);
}

SEC("tp/syscalls/sys_exit_openat")
int BPF_PROG(sys_exit_openat, struct pt_regs *regs, int syscall_id, long ret) {

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, syscall_id);
}

SEC("tp/syscalls/sys_enter_write")
int BPF_PROG(sys_enter_write, struct pt_regs *regs, long syscall_id, u32 fd,
			 char *buf, size_t count) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(fd);
    if (check_file_mode_from_file(file)) return 0;

    // update counter for this pid and syscall write
    return update_counter_enter(pid, syscall_id);
}

SEC("tp/syscalls/sys_exit_write")
int BPF_PROG(sys_exit_write, struct pt_regs *regs, int syscall_id, long ret) {

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, syscall_id);
}

SEC("tp/syscalls/sys_enter_pwrite64")
int BPF_PROG(sys_enter_pwrite64, struct pt_regs *regs, long syscall_id, u32 fd,
			 char *buf, size_t count) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(fd);
    if (check_file_mode_from_file(file)) return 0;

    // update counter for this pid and syscall pwrite64
    return update_counter_enter(pid, syscall_id);
}

SEC("tp/syscalls/sys_exit_pwrite64")
int BPF_PROG(sys_exit_pwrite64, struct pt_regs *regs, int syscall_id, long ret) {

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, syscall_id);
}

SEC("tp/syscalls/sys_enter_read")
int BPF_PROG(sys_enter_read, struct pt_regs *regs, long syscall_id, u32 fd,
			 char *buf, size_t count) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(fd);
    if (check_file_mode_from_file(file)) return 0;

    // update counter for this pid and syscall read
    return update_counter_enter(pid, syscall_id);
}

SEC("tp/syscalls/sys_exit_read")
int BPF_PROG(sys_exit_read, struct pt_regs *regs, int syscall_id, long ret) {

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, syscall_id);
}

SEC("tp/syscalls/sys_enter_pread64")
int BPF_PROG(sys_enter_pread64, struct pt_regs *regs, long syscall_id, u32 fd,
			 char *buf, size_t count) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(fd);
    if (check_file_mode_from_file(file)) return 0;

    // update counter for this pid and syscall pread64
    return update_counter_enter(pid, syscall_id);
}

SEC("tp/syscalls/sys_exit_pread64")
int BPF_PROG(sys_exit_pread64, struct pt_regs *regs, int syscall_id, long ret) {

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, syscall_id);
}

SEC("tp/syscalls/sys_enter_close")
int BPF_PROG(sys_enter_close, struct pt_regs *regs, long syscall_id, u32 fd) {

    // check if the pid is in the filter list or not. If not, return 0 to ignore this event.
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;

    // get file path from fd and check if it is a regular file. If not, return 0 to ignore this event.
    struct file *file = get_file_from_fd(fd);
    if (check_file_mode_from_file(file)) return 0;

    // update counter for this pid and syscall close
    return update_counter_enter(pid, syscall_id);
}

SEC("tp/syscalls/sys_exit_close")
int BPF_PROG(sys_exit_close, struct pt_regs *regs, int syscall_id, long ret) {

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (to_discard(pid)) return 0;
	
	return update_counter_exit(pid, syscall_id);
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
