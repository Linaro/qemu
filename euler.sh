build/aarch64-softmmu/qemu-system-aarch64 -machine virt,gic-version=3,iommu=nested-smmuv3,iommufd=iommufd0 \
--trace events=events \
-enable-kvm -cpu host -m 4G -smp cpus=8,maxcpus=8 \
-object iommufd,id=iommufd0 \
-kernel /home/linaro/Image \
-device virtio-blk-device,drive=image \
-drive if=none,file=/home/linaro/virtual/openEuler-22.03-LTS-SP3-aarch64.qcow2,id=image \
-device vfio-pci,host=0000:76:00.1,iommufd=iommufd0 \
-device virtio-9p-pci,fsdev=p9fs,mount_tag=p9 \
-fsdev local,id=p9fs,path=/home/linaro/p9root,security_model=mapped \
-bios /home/linaro/virtual/QEMU_EFI.fd \
-append "console=ttyAMA0,115200  root=/dev/vda2" \
-nographic \
-netdev user,id=user0,hostfwd=tcp::5000-:22 \
-device virtio-net-device,netdev=user0 \
#-mem-prealloc -mem-path /dev/hugepages \

#sec
#-device vfio-pci,host=0000:76:00.1,iommufd=iommufd0 \

#zip0
#-device vfio-pci,host=0000:75:00.1,iommufd=iommufd0 \

#zip1
#-device vfio-pci,host=0000:b5:00.1,iommufd=iommufd0 \
