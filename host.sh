#!/bin/bash
echo 2 > /sys/devices/pci0000:74/0000:74:00.0/0000:75:00.0/sriov_numvfs
echo 0000:75:00.1 > /sys/bus/pci/drivers/hisi_zip/unbind
echo vfio-pci > /sys/devices/pci0000:74/0000:74:00.0/0000:75:00.1/driver_override
echo 0000:75:00.1 > /sys/bus/pci/drivers_probe

echo 0000:75:00.2 > /sys/bus/pci/drivers/hisi_zip/unbind
echo vfio-pci > /sys/devices/pci0000:74/0000:74:00.0/0000:75:00.2/driver_override
echo 0000:75:00.2 > /sys/bus/pci/drivers_probe

#sec
echo 1 > /sys/devices/pci0000:74/0000:74:01.0/0000:76:00.0/sriov_numvfs
echo 0000:76:00.1 > /sys/bus/pci/drivers/hisi_sec2/unbind
echo vfio-pci > /sys/devices/pci0000:74/0000:74:01.0/0000:76:00.1/driver_override
echo 0000:76:00.1 > /sys/bus/pci/drivers_probe

#hpre
echo 1 > /sys/devices/pci0000:78/0000:78:00.0/0000:79:00.0/sriov_numvfs
echo 0000:79:00.1 > /sys/bus/pci/drivers/hisi_hpre/unbind
echo vfio-pci > /sys/devices/pci0000:78/0000:78:00.0/0000:79:00.1/driver_override
echo 0000:79:00.1 > /sys/bus/pci/drivers_probe

#config network
#enp189s0f3 is the net port
ip link add link enp189s0f3 name macvtap0 type macvtap
ip link set macvtap0 up
ip link show macvtap0

#zip 0
# load zip/qm driver in host before running cmd below
# echo 1 > /sys/devices/pci0000:74/0000:74:00.0/0000:75:00.0/sriov_numvfs
# echo 0000:75:00.1 > /sys/bus/pci/drivers/hisi_zip/unbind
# echo vfio-pci > /sys/devices/pci0000:74/0000:74:00.0/0000:75:00.1/driver_override
# echo 0000:75:00.1 > /sys/bus/pci/drivers_probe

#zip1
#echo 1 > /sys/devices/pci0000:b4/0000:b4:00.0/0000:b5:00.0/sriov_numvfs
#echo 0000:b5:00.1 > /sys/bus/pci/drivers/hisi_zip/unbind
#echo vfio-pci > /sys/devices/pci0000:b4/0000:b4:00.0/0000:b5:00.1/driver_override
#echo 0000:b5:00.1 > /sys/bus/pci/drivers_probe


#-net nic,model=virtio,macaddr=$(cat /sys/class/net/macvtap0/address) \
#-net tap,fd=3 3<>/dev/tap$(cat /sys/class/net/macvtap0/ifindex) \

# Recover
# If want to reconfig vf, unbind pf, then bind pf again.
# echo 0000:75:00.0 > /sys/bus/pci/drivers/hisi_zip/unbind
# echo 0000:75:00.0 > /sys/bus/pci/drivers_probe

