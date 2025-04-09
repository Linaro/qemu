build/qemu-system-aarch64 -object iommufd,id=iommufd0 \
-machine virt,accel=kvm,gic-version=3,default-bus-bypass-iommu=on \
-cpu host -smp cpus=61 -m size=16G,slots=4,maxmem=256G -nographic \
-object memory-backend-ram,size=8G,id=m0 \
-object memory-backend-ram,size=8G,id=m1 \
-numa node,memdev=m0,cpus=0-60,nodeid=0  -numa node,memdev=m1,nodeid=1 \
-device pxb-pcie,id=pcie.1,bus_nr=1,bus=pcie.0 \
-device arm-smmuv3-accel,bus=pcie.1 \
-device pcie-root-port,id=pcie.port1,bus=pcie.1,chassis=1,pref64-reserve=2M,io-reserve=1K \
-device vfio-pci,host=0000:75:00.2,bus=pcie.port1,iommufd=iommufd0,x-pre-copy-dirty-page-tracking=off \
-device pcie-root-port,id=pcie.port2,bus=pcie.1,chassis=2,pref64-reserve=2M,io-reserve=1K \
-device vfio-pci,host=0000:76:00.2,bus=pcie.port2,iommufd=iommufd0,x-pre-copy-dirty-page-tracking=off \
-device pxb-pcie,id=pcie.2,bus_nr=8,bus=pcie.0 \
-device arm-smmuv3-accel,bus=pcie.2 \
-device pcie-root-port,id=pcie.port3,bus=pcie.2,chassis=3,pref64-reserve=2M,io-reserve=1K \
-device vfio-pci,host=0000:79:00.2,bus=pcie.port3,iommufd=iommufd0,x-pre-copy-dirty-page-tracking=off \
-bios /home/linaro/virtual/QEMU_EFI.fd \
-device virtio-blk-device,drive=image \
-drive if=none,file=/home/linaro/virtual/openEuler-22.03-LTS-SP3-aarch64.qcow2,id=image \
-kernel /home/linaro/Image \
-append "console=ttyAMA0,115200  root=/dev/vda2" \
-incoming tcp:0:4444 \
-nographic

#-device vfio-pci-nohotplug,host=0000:75:00.1,iommufd=iommufd0,enable-migration=on,x-pre-copy-dirty-page-tracking=off \
