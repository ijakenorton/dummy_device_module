TEST_DIR="./test_files"
DEVICE="/dev/asgn2"
RESULTS_FILE="./results_log"

multiple_writes() {
    echo "Testing multiple writes in a row..." | tee -a $RESULTS_FILE
    for file in "${TEST_DIR}"/*; do
        sudo ./data_generator "$file"
    done
}

multiple_reads() {
    rm ./device_output
    echo "Testing READS writes in a row..." | tee -a $RESULTS_FILE
    for file in "${TEST_DIR}"/*; do
        cat /dev/asgn2 >> ./device_output
    done

    if ! cmp ./combined.txt ./device_output; then
        echo "Error: Mismatch" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: $file matches device output" | tee -a $RESULTS_FILE
    return 0
}

multiple_writes
sleep 5

multiple_reads
