#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include "syscounter.h"
#include "syscounter.skel.h"
#include <sys/stat.h>

#define POLL_TIMEOUT_MS 100
#define MAX_SYSCALLS 512
#define COMM_STR_LEN 32

static volatile bool exiting = false;
static void sig_handler(int sig){
	exiting = true;
}

static const char *syscall_table[MAX_SYSCALLS] = {
#include "syscalls_table.h"
};

/* Function to resolve syscall number to name
 * Receives a syscall number (int nr) as argument
 * Returns the corresponding syscall name or "unknown" if not found
 */
const char *resolve_syscall(int nr) {
    if (nr >= 0 && nr < MAX_SYSCALLS && syscall_table[nr])
        return syscall_table[nr];
    return "unknown";
}

/* Function to get all values from the counter map
 * Receives a pointer to the BPF skeleton (struct syscounter_bpf *skel), an array of counter_key structs to store the
 * keys (CounterKey keys[MAX_PIDS * MAX_OPS]) and an array of long long to store the values
 * (long long values[MAX_PIDS * MAX_OPS]) as arguments
 * Returns the number of entries retrieved from the map
 */
int get_values_from_counter_map(struct syscounter_bpf *skel, CounterKey *keys, CounterValue *values) {

	int num_entries = 0;

	CounterKey cur_key, next_key;
	CounterValue value;

	// Start with NULL to get first key
	if (bpf_map__get_next_key(skel->maps.counters, NULL, &next_key, sizeof(next_key)) != 0) {
		printf("Map is empty\n");
		return num_entries;
	}

	// Iterate through the map using get_next_key until there are no more keys
	cur_key = next_key;
	while (1) {
		if (bpf_map__lookup_elem(skel->maps.counters, &cur_key, sizeof(cur_key), &value, sizeof(value), BPF_ANY) == 0) {
			// Store the key and value in the provided arrays
			keys[num_entries] = cur_key;
			values[num_entries] = value;
			num_entries++;
		}

		// Get next key
		if (bpf_map__get_next_key(skel->maps.counters, &cur_key, &next_key, sizeof(next_key)) != 0)
			break;

		cur_key = next_key;
	}

	return num_entries;
}

/* Function to get unique pid values from the counter keys
 * Receives an array of counter_key structs (CounterKey *keys), the number of entries in the array (int num_entries)
 * and an array of strings to store the unique pid values (char (*pid_list)[COMM_STR_LEN]) as arguments
 * Returns the number of unique pid values found
 */
int get_unique_pid_values(CounterKey *keys, int num_entries, char (*pid_list)[COMM_STR_LEN]) {

	int pid_count = 0;
	int found;
	char val[COMM_STR_LEN];

	for (int i = 0; i < num_entries; i++) {
		snprintf(val, sizeof(val), "%s-%d", keys[i].command, keys[i].pid);

		found = 0;
		for (int j = 0; j < pid_count && !found; j++) {
			if (strcmp(pid_list[j], val)==0) found = 1;
		}

		if (!found) {
			strcpy(pid_list[pid_count++], val);
		}
	}

	return pid_count;
}

/* Function to get unique op values from the counter keys
 * Receives an array of counter_key structs (CounterKey keys[MAX_PIDS * MAX_OPS]), the number of entries
 * in the array (int num_entries) and an array of integers to store the unique op values (int op_list[MAX_OPS]) as arguments
 * Returns the number of unique op values found
*/
int get_unique_op_values(CounterKey *keys, int num_entries, int *op_list) {

	int op_count = 0;
	int found;

	for (int i = 0; i < num_entries; i++) {
		found = 0;
		for (int j = 0; j < op_count; j++)
			if (op_list[j] == keys[i].op) found = 1;
		if (!found) op_list[op_count++] = keys[i].op;
	}

	return op_count;
}

/* Function to print the results in a table format
 * Receives an array of CounterKey (CounterKey *keys), an array of CounterValue (CounterValue *values),
 * the number of entries in the arrays (int num_entries), an array of strings with the unique pid values
 * (char (*pid_list)[COMM_STR_LEN]), the number of unique pid values (int pid_count), an array of integers with the unique
 * op values (int *op_list) and the number of unique op values (int op_count) as arguments.
 * Prints the results in a table format with the pids as rows and the ops as columns, and an aggregate row per op at the end
 */
void print_results(CounterKey *keys, CounterValue *values, int num_entries, char (*pid_list)[COMM_STR_LEN], int pid_count,
				   int *op_list, int op_count) {

	CounterValue total_op_list[MAX_OPS] = {0};

	printf("%-32s", "PID\\OP");
	for (int i = 0; i < op_count; i++)
		printf("%-20s", resolve_syscall(op_list[i]));
	printf("\n");

	int total_width = COMM_STR_LEN + op_count * 20;
	for (int i = 0; i < total_width; i++)
		putchar('-');
	printf("\n");

	char comm[COMM_STR_LEN];
	for (int j = 0; j < pid_count; j++) {
		printf("%-32s", pid_list[j]);
		for (int i = 0; i < op_count; i++) {
			CounterValue val = {0};
			for (int k = 0; k < num_entries; k++) {
				snprintf(comm, COMM_STR_LEN, "%s-%d", keys[k].command, keys[k].pid);
				if ((strcmp(comm,pid_list[j])==0) && keys[k].op == op_list[i]) {
					val = values[k];
					total_op_list[i].count += val.count;
					total_op_list[i].time_sum += val.time_sum;
				}
			}
			long avg_time_ns = val.count ? (val.time_sum / val.count) : 0;
			char cell[20];
			snprintf(cell, sizeof(cell), "%06ldx | %ldns", val.count, avg_time_ns);
			printf("%-20s", cell);
		}
		printf("\n");
	}

	for (int i = 0; i < total_width; i++)
		putchar('-');
	printf("\n");

	printf("%-32s", "Total");
	for (int i = 0; i < op_count; i++) {
		long avg_time_ns = total_op_list[i].count ? (total_op_list[i].time_sum / total_op_list[i].count) : 0;
		char cell[20];
		snprintf(cell, sizeof(cell), "%06ldx | %ldns", total_op_list[i].count, avg_time_ns);
		printf("%-20s", cell);
	}
	printf("\n");
}

/* Function to write the results in CSV format to a file
 * Receives an array of CounterKey (CounterKey *keys), an array of CounterValue (CounterValue *values),
 * the number of entries in the arrays (int num_entries), an array of strings with the unique pid values
 * (char (*pid_list)[COMM_STR_LEN]), the number of unique pid values (int pid_count), an array of integers with the unique
 * op values (int *op_list), the number of unique op values (int op_count), and the output filename (const char *filename)
 * as arguments. Writes the results in CSV format with columns: PID-Command, Syscall Name, Count, Avg Time (ns)
 */
void write_results_to_csv(CounterKey *keys, CounterValue *values, int num_entries, char (*pid_list)[COMM_STR_LEN], int pid_count,
						  int *op_list, int op_count, const char *filename) {

	FILE *file = fopen(filename, "w");
	if (!file) {
		fprintf(stderr, "Error: Failed to open file %s for writing\n", filename);
		return;
	}

	fprintf(file, "PID-Command,Syscall Name,Count,Avg Time (ns)\n");

	CounterValue total_by_op[MAX_OPS] = {0};
	char comm[COMM_STR_LEN];

	for (int j = 0; j < pid_count; j++) {
		for (int i = 0; i < op_count; i++) {
			CounterValue val = {0};
			for (int k = 0; k < num_entries; k++) {
				snprintf(comm, COMM_STR_LEN, "%s-%d", keys[k].command, keys[k].pid);
				if ((strcmp(comm, pid_list[j]) == 0) && keys[k].op == op_list[i]) {
					val = values[k];
					total_by_op[i].count += val.count;
					total_by_op[i].time_sum += val.time_sum;
				}
			}
			if (val.count > 0) {
				long avg_time_ns = val.time_sum / val.count;
				fprintf(file, "%s,%s,%ld,%ld\n", pid_list[j], resolve_syscall(op_list[i]), val.count, avg_time_ns);
			}
		}
	}

	fprintf(file, "TOTAL,");
	for (int i = 0; i < op_count; i++) {
		long avg_time_ns = total_by_op[i].count ? (total_by_op[i].time_sum / total_by_op[i].count) : 0;
		if (i == op_count - 1) {
			fprintf(file, "%ld,%ld\n", total_by_op[i].count, avg_time_ns);
		} else {
			fprintf(file, "%ld,%ld,", total_by_op[i].count, avg_time_ns);
		}
	}

	fclose(file);
	printf("Results written to %s\n", filename);
}


int main(int argc, char *argv[]) {
    struct syscounter_bpf *skel;
    int err;
	const char *csv_filename = NULL;
	int pid_argc = argc;

	/* Cleaner handling of Ctrl-C */
	signal(SIGINT, sig_handler);

	/* Load & verify BPF programs */
	skel = syscounter_bpf__open_and_load();
	if (!skel) {
		printf("Failed to open BPF object\n");
		goto cleanup;
	}

	/* Configure a map with the pid to filter */
	if (argc < 2) {
		printf("usage: ./syscounter <pid1> [pid2 ...] [output_file.csv]\n");
		goto cleanup;
	}

	/* Check if last argument is a CSV filename (contains .csv or starts with a path) */
	if (argc >= 3) {
		const char *last_arg = argv[argc - 1];
		/* Simple heuristic: if last arg doesn't parse as a number, treat it as filename */
		char *endptr;
		strtol(last_arg, &endptr, 10);
		if (*endptr != '\0') {
			/* Last argument is not purely numeric, treat as filename */
			csv_filename = last_arg;
			pid_argc = argc - 1;
		}
	}

	/* add the pids to filter to the my_config map. The key is the pid and the value is 1 (could be a char instead) */
	uint32_t key;
	for (int i = 1; i < pid_argc; i++) {
		key = atoi(argv[i]);
		uint32_t value = 1;
		bpf_map__update_elem(skel->maps.pids_to_consider, &key, sizeof(key), &value, sizeof(value), 0);
		printf("Tracing Pid %d\n", key);
	}

	/* Attach hooks */
	err = syscounter_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF skeleton: %d\n", err);
		syscounter_bpf__destroy(skel);
        goto cleanup;
	}

	/* Process events until interrupted */
	printf("Successfully started! Press Ctrl-C to stop and print results.\n");
	/* Use a very large sleep interval and rely on the signal handler to set exiting to true when Ctrl-C is pressed */
	while (!exiting) {
        sleep(99999999);
	}
	printf("\n");

	/* Print values stored in counter map */
	CounterKey *keys = malloc(sizeof(CounterKey) * MAX_PIDS * MAX_OPS);
	CounterValue *values = malloc(sizeof(CounterValue) * MAX_PIDS * MAX_OPS);
	char (*pid_list)[COMM_STR_LEN] = malloc(sizeof(char[COMM_STR_LEN]) * MAX_PIDS);
	int *op_list = malloc(sizeof(int) * MAX_OPS);

	int num_entries = get_values_from_counter_map(skel, keys, values);
	if (num_entries) {
		int pid_count = get_unique_pid_values(keys, num_entries, pid_list);
		int op_count = get_unique_op_values(keys, num_entries, op_list);
		print_results(keys, values, num_entries, pid_list, pid_count, op_list, op_count);
		
		/* Write CSV output if filename was provided */
		if (csv_filename) {
			printf("\n");
			write_results_to_csv(keys, values, num_entries, pid_list, pid_count, op_list, op_count, csv_filename);
		}
	}

	free(keys);
	free(values);
	free(pid_list);
	free(op_list);

	cleanup:
	syscounter_bpf__destroy(skel);

	return err < 0 ? -err : 0;
}
