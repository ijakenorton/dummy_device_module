module_name="gpio"

make && \
sudo insmod "./$module_name.ko" && \
sudo chown pi:pi /dev/asgn2
sudo chown pi:pi /proc/asgn2_proc

echo "Setup $module_name successfully."


# Check the exit status of the last command
if [ $? -ne 0 ]; then
    echo "An error occurred during execution."
    exit 1
fi

