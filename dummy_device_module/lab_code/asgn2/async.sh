#!/bin/bash

set -e  # Exit immediately if a command exits with a non-zero status

MODULE_NAME="gpio"
DEVICE="/dev/asgn2"
PROC_FILE="/proc/asgn2_proc"
TEST_DIR="./test_files"
RESULTS_FILE="test_results.log"

# Function to clean up
cleanup() {
    echo "Cleaning up..."
    sudo rmmod $MODULE_NAME || true
    rm -rf $TEST_DIR
    rm -f $RESULTS_FILE
}

# Set up trap to call cleanup function on script exit
trap cleanup EXIT

# Ensure the module is not already loaded
sudo rmmod $MODULE_NAME 2>/dev/null || true

# Compile and load the module
echo "Compiling and loading module..."
make || { echo "Compilation failed"; exit 1; }
sudo insmod "./${MODULE_NAME}.ko" || { echo "Module insertion failed"; exit 1; }

# Set permissions
sudo chown pi:pi $DEVICE
sudo chown pi:pi $PROC_FILE

# Create test directory and files
mkdir -p $TEST_DIR
for i in {1..5}; do
    dd if=/dev/urandom of="${TEST_DIR}/file${i}.bin" bs=1M count=1
done

# Function to write file to device and verify
test_file() {
    local file=$1
    echo "Testing file: $file"
    sudo ./data_generator "$file"
    if ! cmp "$file" $DEVICE; then
        echo "Error: Mismatch for $file" | tee -a $RESULTS_FILE
        return 1
    fi
    echo "Success: $file matches device output" | tee -a $RESULTS_FILE
    return 0
}

# Run tests
echo "Starting tests..." | tee $RESULTS_FILE
for file in "${TEST_DIR}"/*; do
    test_file "$file"
done

# Test removal while reading
echo "Testing removal while reading..." | tee -a $RESULTS_FILE
sudo ./data_generator "${TEST_DIR}/file1.bin" &
sleep 1
sudo rmmod $MODULE_NAME
sudo insmod "./${MODULE_NAME}.ko"
sudo chown pi:pi $DEVICE
sudo chown pi:pi $PROC_FILE
wait
if cmp "${TEST_DIR}/file1.bin" $DEVICE; then
    echo "Error: File should not match after module removal" | tee -a $RESULTS_FILE
else
    echo "Success: File does not match after module removal, as expected" | tee -a $RESULTS_FILE
fi

# Test concurrent writes
echo "Testing concurrent writes..." | tee -a $RESULTS_FILE
for file in "${TEST_DIR}"/*; do
    sudo ./data_generator "$file" &
done
wait
cat $DEVICE > "${TEST_DIR}/combined_output.bin"
if cmp "${TEST_DIR}/combined_output.bin" $DEVICE; then
    echo "Success: Concurrent writes successful" | tee -a $RESULTS_FILE
else
    echo "Error: Concurrent writes failed" | tee -a $RESULTS_FILE
fi

# Check proc file
echo "Checking proc file..." | tee -a $RESULTS_FILE
if [ -s $PROC_FILE ]; then
    echo "Success: Proc file is not empty" | tee -a $RESULTS_FILE
else
    echo "Error: Proc file is empty" | tee -a $RESULTS_FILE
fi

# Final results
if grep -q "Error:" $RESULTS_FILE; then
    echo "Some tests failed. Check $RESULTS_FILE for details."
    exit 1
else
    echo "All tests passed successfully!"
    exit 0
fi
