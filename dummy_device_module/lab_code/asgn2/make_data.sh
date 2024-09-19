TEST_DIR="./test_files"


cleanup() {
    echo "Cleaning up..."
    sudo rmmod gpio || true
    rm -rf $TEST_DIR
    rm ./combined.txt
    rm ./device_output
}
cleanup

mkdir -p $TEST_DIR
for i in {1..5}; do
    < /dev/urandom tr -dc 'A-Za-z0-9 ' | head -c 10000 | fold -w 100 > "${TEST_DIR}/file${i}.txt"
done

rm ./combined.txt
for file in "${TEST_DIR}"/*; do
        cat "$file" >> "./combined.txt"
done

