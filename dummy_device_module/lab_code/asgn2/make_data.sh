#!/bin/bash

TEST_DIR="./test_files"
if [[ $# -eq 1 ]]; then
    iterations=$1
else
    iterations=5
fi 

cleanup() {
    rm -rf $TEST_DIR
    rm -f ./combined.txt
    rm -f ./device_output
}

cleanup
mkdir -p $TEST_DIR

for i in $(seq 1 $iterations); do
    < /dev/urandom tr -dc '~-' | head -c 10000 | fold -w 100 > "${TEST_DIR}/file${i}.txt"
done

> ./combined.txt
for file in "${TEST_DIR}"/*; do
    cat "$file" >> "./combined.txt"
done
