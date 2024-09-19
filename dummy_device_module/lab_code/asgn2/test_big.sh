make && \
sudo insmod "./gpio.ko" && \
sudo chown pi:pi /dev/asgn2
sudo chown pi:pi /proc/asgn2_proc
sudo ./data_generator ./asgn2.c
cat /dev/asgn2 | diff .asgn2.c
if [ $? -ne 0 ]; then
    echo "An error occurred during execution."
    exit 1
fi

