import sys
import re
import json
from statistics import mean, stdev

argc = len(sys.argv)
input_jsons: list[str] = []
output_json: str = ""
aggregator: dict[str, list[int]] = {}


def parse_args() -> bool:
    global argc, input_jsons, output_json

    if argc < 2:
        print("Usage: python3 json_aggregator_cachestat.py <list_input_JSONs> <output_JSON>")
        return False

    for i in range(1,argc-1):
        input_jsons.append(sys.argv[i])
    output_json = sys.argv[argc-1]
    
    return True


def process_file(file: str):
    global aggregator
    with open(file, mode='r') as f:
        data = json.load(f)
        for field in data:
            acc_values = aggregator.get(field)
            data_value = data[field]
            if acc_values is None:
                aggregator[field] = [data_value]
            else:
                acc_values.append(data_value)


def prepare_output() -> dict[str, int]:
    global aggregator
    output_dict = {}

    for field in aggregator:
        acc_values = aggregator[field]
        avg = mean(acc_values)
        st_dev = stdev(acc_values)
        output_dict[f"{field}_avg"] = avg
        output_dict[f"{field}_stdev"] = st_dev

    return output_dict


if not parse_args():
    exit()

for file in input_jsons:
    process_file(file)

output_data = prepare_output()
with open(output_json, "w") as out:
    json.dump(output_data, out, indent=2)
