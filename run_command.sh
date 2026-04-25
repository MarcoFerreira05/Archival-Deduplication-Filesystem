#!/bin/bash
echo "PID = $$"
echo "Press any key to continue"
echo ""
read -n 1 -s key
cd benchmarks
time python3 dedupe_workload.py
cd ..

