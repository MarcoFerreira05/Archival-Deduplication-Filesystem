for i in $(seq 1 $1); do
	echo ""
	echo "===== Iteration $i ====="
	./benchmark.sh
	sleep 10
done

echo ""
echo "WARNING: Statistics will be calculated using every result sub-directory inside results/"

cd results/
python3 ../benchmarks/csv_aggregator_syscounter.py "passthrough.*" 2026-*/passthrough_results.csv passthrough_results.json
python3 ../benchmarks/csv_aggregator_syscounter.py "python3.*" 2026-*/workload_results.csv workload_results.json
python3 ../benchmarks/json_aggregator_cachestat.py 2026-*/cachestat_passthrough.json cachestat_passthrough.json
cd ..

echo "Statistics successfully calculated!"
