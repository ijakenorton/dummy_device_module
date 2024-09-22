#!/bin/bash

TEST_DIR="./test_files"
DEVICE="/dev/asgn2"
RESULTS_FILE="./results_log"

multiple_writes() {
    echo "Adding files..." | tee -a $RESULTS_FILE
    for file in "${TEST_DIR}"/*; do
        sudo ./data_generator "$file"
    done
}

multiple_reads() {
    local iterations=$1
    echo "Testing writing and reading $iterations files"
    ./make_data.sh $iterations
    multiple_writes
    echo "Testing READS writes in a row..." | tee -a $RESULTS_FILE
    > ./device_output
    for file in "${TEST_DIR}"/*; do
        cat $DEVICE >> ./device_output
    done
    if ! cmp ./combined.txt ./device_output; then
        echo "Error: Mismatch" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: Files match device output" | tee -a $RESULTS_FILE
    return 0
}

run_tests() {
    echo "Starting tests with multiple iterations" | tee -a $RESULTS_FILE
    for i in {25..100}; do
        echo "Running test with $i iterations" | tee -a $RESULTS_FILE
        start_time=$(date +%s.%N)
        multiple_reads $i
        end_time=$(date +%s.%N)
        execution_time=$(echo "$end_time - $start_time" | bc)
        echo "Test with $i iterations completed in $execution_time seconds" | tee -a $RESULTS_FILE
        echo "----------------------------------------" | tee -a $RESULTS_FILE
    done
}

# Run the tests
run_tests
