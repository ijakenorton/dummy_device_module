TEST_DIR="./test_files"
DEVICE="/dev/asgn2"
RESULTS_FILE="./results_log"

multiple_writes() {
    echo "Adding 5 files..." | tee -a $RESULTS_FILE
    for file in "${TEST_DIR}"/*; do
        sudo ./data_generator "$file"
    done
}

multiple_reads() {
    ./make_data.sh
    multiple_writes

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

multiple_reads_with_extra() {
    ./make_data.sh
    multiple_writes
    echo "Testing READS with extra cat in a row..." | tee -a $RESULTS_FILE
    for file in "${TEST_DIR}"/*; do
        cat /dev/asgn2 >> ./device_output
    done

        cat /dev/asgn2 >> ./device_output

    if ! cmp ./combined.txt ./device_output; then
        echo "Error: Mismatch with extra" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: $file matches device output" | tee -a $RESULTS_FILE
    return 0
}

multiple_reads_mixed_extra() {
    ./make_data.sh
    multiple_writes
    multiple_writes
    echo "Testing READS mixed..." | tee -a $RESULTS_FILE

    for file in "${TEST_DIR}"/*; do
        cat /dev/asgn2 >> ./device_output
    done

    if ! cmp ./combined.txt ./device_output; then
        echo "Error: Mismatch mixed" | tee -a $RESULTS_FILE
        return 1
    fi
    rm ./device_output

    multiple_writes

    for file in "${TEST_DIR}"/*; do
        cat /dev/asgn2 >> ./device_output
    done
    echo "Success: $file matches device output" | tee -a $RESULTS_FILE
    return 0
}

# multiple_reads_mixed() {
#     ./make_data.sh
#     echo "Testing READS writes in a row..." | tee -a $RESULTS_FILE
#     for file in "${TEST_DIR}"/*; do
#             sudo ./data_generator $file
#     done
#     for file in "${TEST_DIR}"/*; do
#         cat /dev/asgn2 >> ./device_output
#     done

#     if ! cmp ./combined.txt ./device_output; then
#         echo "Error: Mismatch mixed" | tee -a $RESULTS_FILE
#         return 1
#     fi
#     echo "Success: $file matches device output" | tee -a $RESULTS_FILE
#     return 0
# }

multiple_reads

multiple_reads_with_extra

multiple_reads_mixed_extra
