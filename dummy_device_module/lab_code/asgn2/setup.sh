if [ $# -eq 0 ]; then
    echo "Usage: $0 <argument>"
   exit 1
fi

module_name="$1"

make && \
sudo dmesg -C
sudo insmod "./$module_name.ko" && \

echo "Setup $module_name successfully."

# sudo ./data_generator ./asgn2.c
sudo rmmod gpio
# sudo chown pi:pi /dev/"$module_name"

# Check the exit status of the last command
if [ $? -ne 0 ]; then
    echo "An error occurred during execution."
    exit 1
fi

