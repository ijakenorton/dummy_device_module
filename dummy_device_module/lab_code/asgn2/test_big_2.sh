make && \
sudo insmod "./gpio.ko" && \
sudo chown pi:pi /dev/asgn2
sudo chown pi:pi /proc/asgn2_proc
sudo ./data_generator ./asgn2.c ./asgn2.c
cat /dev/asgn2
cat /dev/asgn2
# cat /dev/asgn2
sudo rmmod gpio
# sudo chown pi:pi /dev/"$module_name"

# Check the exit status of the last command
if [ $? -ne 0 ]; then
    echo "An error occurred during execution."
    exit 1
fi

