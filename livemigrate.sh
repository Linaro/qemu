#!/bin/bash
echo 4 > /sys/devices/pci0000:74/0000:74:00.0/0000:75:00.0/sriov_numvfs

echo 0000:75:00.1 > /sys/bus/pci/drivers/hisi_zip/unbind
echo hisi_acc_vfio_pci > /sys/devices/pci0000:74/0000:74:00.0/0000:75:00.1/driver_override
echo 0000:75:00.1 > /sys/bus/pci/drivers_probe


echo 0000:75:00.2 > /sys/bus/pci/drivers/hisi_zip/unbind
echo hisi_acc_vfio_pci > /sys/devices/pci0000:74/0000:74:00.0/0000:75:00.2/driver_override
echo 0000:75:00.2 > /sys/bus/pci/drivers_probe

echo 0000:75:00.3 > /sys/bus/pci/drivers/hisi_zip/unbind
echo hisi_acc_vfio_pci > /sys/devices/pci0000:74/0000:74:00.0/0000:75:00.3/driver_override
echo 0000:75:00.3 > /sys/bus/pci/drivers_probe

echo 0000:75:00.4 > /sys/bus/pci/drivers/hisi_zip/unbind
echo hisi_acc_vfio_pci > /sys/devices/pci0000:74/0000:74:00.0/0000:75:00.4/driver_override
echo 0000:75:00.4 > /sys/bus/pci/drivers_probe
