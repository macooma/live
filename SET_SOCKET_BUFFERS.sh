#!/bin/bash

echo "Current..."
echo -n "net.core.wmem_max = "
cat /proc/sys/net/core/wmem_max 

echo -n "net.core.rmem_max = "
cat /proc/sys/net/core/rmem_max

echo
echo "Setting..."
sudo sysctl -w net.core.wmem_max=524288
sudo sysctl -w net.core.rmem_max=524288
echo "Done."

# echo 'net.core.wmem_max=524288' >> /etc/sysctl.conf
# echo 'net.core.rmem_max=524288' >> /etc/sysctl.conf

# Refer to https://segmentfault.com/a/1190000000473365
