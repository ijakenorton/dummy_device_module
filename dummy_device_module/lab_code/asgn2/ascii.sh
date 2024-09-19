#!/bin/bash

set -e  # Exit immediately if a command exits with a non-zero status

MODULE_NAME="gpio"
DEVICE="/dev/asgn2"
PROC_FILE="/proc/asgn2_proc"
TEST_DIR="./test_files"
RESULTS_FILE="test_results.log"
# PAGE_LIMIT=10  # Set the page limit to 10

# Function to clean up
cleanup() {
    echo "Cleaning up..."
    sudo rmmod $MODULE_NAME || true
    rm -rf $TEST_DIR
    mv $RESULTS_FILE "$RESULTS_FILE.bak"
}

# Set up trap to call cleanup function on script exit
trap cleanup EXIT

# Ensure the module is not already loaded
sudo rmmod $MODULE_NAME 2>/dev/null || true

# Compile and load the module with page limit
echo "Compiling and loading module..."
make || { echo "Compilation failed"; exit 1; }
sudo insmod "./${MODULE_NAME}.ko" page_limit=$PAGE_LIMIT || { echo "Module insertion failed"; exit 1; }

# Set permissions
sudo chown pi:pi $DEVICE
sudo chown pi:pi $PROC_FILE

# Create test directory and ASCII files
mkdir -p $TEST_DIR
for i in {1..5}; do
    < /dev/urandom tr -dc ' -~' | head -c 10000 > "${TEST_DIR}/file${i}.txt"
done

test_file() {
    local file=$1
    echo "Testing file: $file"
    sudo ./data_generator "$file"
    local temp_read="${TEST_DIR}/temp_read.txt"
    cat $DEVICE > "$temp_read"
    if ! cmp "$file" "$temp_read"; then
        echo "Error: Mismatch for $file" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: $file matches device output" | tee -a $RESULTS_FILE
    # Verify that the device is now empty
    if [ -s $DEVICE ]; then
        echo "Error: Device not empty after read for $file" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: Device empty after read for $file" | tee -a $RESULTS_FILE
    return 0
}

test_multiple_writes() {
    echo "Testing multiple writes in a row..." | tee -a $RESULTS_FILE
    local combined_file="${TEST_DIR}/combined.txt"
    > "$combined_file"  # Clear the file
    for file in "${TEST_DIR}"/*; do
        sudo ./data_generator "$file"
        cat "$file" >> "$combined_file"
    done
    
    local read_result="${TEST_DIR}/read_result.txt"
    
    for file in {1..5}/*; do
        cat $DEVICE >> "$read_result"
    done
    if ! cmp "$combined_file" "$read_result"; then
        echo "Error: Mismatch after multiple writes" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: Combined files match device output after multiple writes" | tee -a $RESULTS_FILE
    
    # Verify that the device is now empty
    if [ -s $DEVICE ]; then
        echo "Error: Device not empty after read for multiple writes" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: Device empty after read for multiple writes" | tee -a $RESULTS_FILE
    return 0
}

# Function to test partial reads
test_partial_reads() {
    echo "Testing partial reads..." | tee -a $RESULTS_FILE
    local test_file="${TEST_DIR}/partial_test.txt"
    < /dev/urandom tr -dc ' -~' | head -c 10000 > "$test_file"
    sudo ./data_generator "$test_file"
    
    local partial_read="${TEST_DIR}/partial_read.txt"
    head -c 512 $DEVICE > "$partial_read"
    if ! cmp -n 512 "$test_file" "$partial_read"; then
        echo "Error: Partial read mismatch" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: Partial read matches first 512 bytes" | tee -a $RESULTS_FILE
    
    local remaining_read="${TEST_DIR}/remaining_read.txt"
    cat $DEVICE > "$remaining_read"
    if ! cmp -i 512 "$test_file" "$remaining_read"; then
        echo "Error: Remaining data after partial read mismatch" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: Remaining data after partial read matches" | tee -a $RESULTS_FILE
    
    # Verify that the device is now empty
    if [ -s $DEVICE ]; then
        echo "Error: Device not empty after complete read" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: Device empty after complete read" | tee -a $RESULTS_FILE
    return 0
}

# Run tests
echo "Starting tests..." | tee $RESULTS_FILE


test_multiple_writes

# Test multiple reads in a row
test_partial_reads

for file in "${TEST_DIR}"/*; do
    test_file "$file"
done
# Test removal and reinsertion
echo "Testing removal and reinsertion..." | tee -a $RESULTS_FILE
sudo rmmod $MODULE_NAME
sudo insmod "./${MODULE_NAME}.ko" page_limit=$PAGE_LIMIT
sudo chown pi:pi $DEVICE
sudo chown pi:pi $PROC_FILE

# Test writing after reinsertion
test_file "${TEST_DIR}/file1.txt"

# Test partial reads
echo "Testing partial reads..." | tee -a $RESULTS_FILE
sudo ./data_generator "${TEST_DIR}/file2.txt"
head -c 512 $DEVICE > "${TEST_DIR}/partial_read.txt"
if cmp -n 512 "${TEST_DIR}/file2.txt" "${TEST_DIR}/partial_read.txt"; then
    echo "Success: Partial read matches first 512 bytes" | tee -a $RESULTS_FILE
else
    echo "Error: Partial read mismatch" | tee -a $RESULTS_FILE
fi

# Final results
if grep -q "Error:" $RESULTS_FILE; then
    echo "Some tests failed. Check $RESULTS_FILE for details."
    exit 1
else
    echo "All tests passed successfully!"
    exit 0
fi
