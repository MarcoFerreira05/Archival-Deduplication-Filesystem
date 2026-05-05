import sys
import re
import json
from csv import DictReader
from statistics import mean, stdev

argc = len(sys.argv)
command_regex: str = ""
input_csvs: list[str] = []
output_json: str = ""
aggregator_time: dict[str, list[int]] = {}
aggregator_count: dict[str, list[int]] = {}


def parse_args() -> bool:
    global argc, command_regex, input_csvs, output_json

    if argc < 3:
        print("Usage: python3 csv_aggregator_syscounter.py <command_regex> <list_input_CSVs> <output_JSON>")
        return False

    command_regex = sys.argv[1]
    for i in range(2,argc-1):
        input_csvs.append(sys.argv[i])
    output_json = sys.argv[argc-1]
    
    return True


def process_file(file: str):
    with open(file, mode='r') as f:
        csv_reader = DictReader(f)
        for row in csv_reader:
            if row["PID-Command"] == "TOTAL":
                continue
            if re.search(command_regex, row["PID-Command"]):
                syscall = row["Syscall Name"]
                time = int(row["Avg Time (ns)"])
                count = int(row["Count"])
                old_time = aggregator_time.get(syscall)
                old_count = aggregator_count.get(syscall)
                if old_time is None:
                    aggregator_time[syscall] = [time]
                else:
                    old_time.append(time)
                if old_count is None:
                    aggregator_count[syscall] = [count]
                else:
                    old_count.append(count)


def prepare_output() -> dict:
    output_dict = {}

    for syscall in aggregator_time:
        times = aggregator_time[syscall]
        average = mean(times)
        output_dict[f"{syscall}_avg_time"] = average
        st_dev = stdev(times) if len(times) > 1 else 0.0
        output_dict[f"{syscall}_stdev_time"] = st_dev
    
    for syscall in aggregator_count:
        counts = aggregator_count[syscall]
        average = mean(counts)
        output_dict[f"{syscall}_avg_count"] = average
        st_dev = stdev(counts) if len(counts) > 1 else 0.0
        output_dict[f"{syscall}_stdev_count"] = st_dev

    return output_dict


if not parse_args():
    exit()

for file in input_csvs:
    process_file(file)

output_data = prepare_output()
with open(output_json, "w") as out:
    json.dump(output_data, out, indent=2)
