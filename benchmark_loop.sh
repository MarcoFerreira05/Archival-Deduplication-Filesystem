for i in $(seq 1 $1); do
	echo ""
	echo "===== Iteration $i ====="
	./benchmark.sh
	sleep 10
done

