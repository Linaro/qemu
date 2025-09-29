/*
 * Copyright (c) 2025 Huawei Technologies R & D (UK) Ltd
 * Copyright (C) 2025 NVIDIA
 * Written by Nicolin Chen, Shameer Kolothum
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "trace.h"

#include "hw/arm/smmuv3.h"
#include "hw/iommu.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci-host/gpex.h"
#include "hw/vfio/pci.h"

#include "smmuv3-internal.h"
#include "smmuv3-accel.h"

/*
 * The root region aliases the global system memory, and shared_as_sysmem
 * provides a shared Address Space referencing it. This Address Space is used
 * by all vfio-pci devices behind all accelerated SMMUv3 instances within a VM.
 */
MemoryRegion root;
MemoryRegion sysmem;
static AddressSpace *shared_as_sysmem;

static int smmuv3_oas_bits(uint32_t oas)
{
    static const int map[] = { 32, 36, 40, 42, 44, 48, 52, 56 };
    return (oas < ARRAY_SIZE(map)) ? map[oas] : -EINVAL;
}

static bool
smmuv3_accel_check_hw_compatible(SMMUv3State *s,
                                 struct iommu_hw_info_arm_smmuv3 *info,
                                 Error **errp)
{
    /* QEMU SMMUv3 supports both linear and 2-level stream tables */
    if (FIELD_EX32(info->idr[0], IDR0, STLEVEL) !=
                FIELD_EX32(s->idr[0], IDR0, STLEVEL)) {
        error_setg(errp, "Host SMMUv3 differs in Stream Table format");
        return false;
    }

    /* QEMU SMMUv3 supports only little-endian translation table walks */
    if (FIELD_EX32(info->idr[0], IDR0, TTENDIAN) >
                FIELD_EX32(s->idr[0], IDR0, TTENDIAN)) {
        error_setg(errp, "Host SMMUv3 doesn't support Little-endian "
                   "translation table");
        return false;
    }

    /* QEMU SMMUv3 supports only AArch64 translation table format */
    if (FIELD_EX32(info->idr[0], IDR0, TTF) <
                FIELD_EX32(s->idr[0], IDR0, TTF)) {
        error_setg(errp, "Host SMMUv3 doesn't support AArch64 translation "
                   "table format");
        return false;
    }

    /* QEMU SMMUv3 supports SIDSIZE 16 */
    if (FIELD_EX32(info->idr[1], IDR1, SIDSIZE) <
                FIELD_EX32(s->idr[1], IDR1, SIDSIZE)) {
        error_setg(errp, "Host SMMUv3 SIDSIZE not compatible");
        return false;
    }

    /* User can disable QEMU SMMUv3 Range Invalidation support */
    if (FIELD_EX32(info->idr[3], IDR3, RIL) !=
                FIELD_EX32(s->idr[3], IDR3, RIL)) {
        error_setg(errp, "Host SMMUv3 differs in Range Invalidation support");
        return false;
    }

    /*
     * TODO: OAS is not something Linux kernel doc says meaningful for user.
     * But looks like OAS needs to be compatible for accelerator support. Please
     * check.
     */
    if (FIELD_EX32(info->idr[5], IDR5, OAS) <
                FIELD_EX32(s->idr[5], IDR5, OAS)) {
        error_setg(errp, "Host SMMUv3 OAS(%d) bits not compatible",
                   smmuv3_oas_bits(FIELD_EX32(info->idr[5], IDR5, OAS)));
        return false;
    }

    /* QEMU SMMUv3 supports GRAN4K/GRAN16K/GRAN64K translation granules */
    if (FIELD_EX32(info->idr[5], IDR5, GRAN4K) !=
                FIELD_EX32(s->idr[5], IDR5, GRAN4K)) {
        error_setg(errp, "Host SMMUv3 doesn't support 4K translation granule");
        return false;
    }
    if (FIELD_EX32(info->idr[5], IDR5, GRAN16K) !=
                FIELD_EX32(s->idr[5], IDR5, GRAN16K)) {
        error_setg(errp, "Host SMMUv3 doesn't support 16K translation granule");
        return false;
    }
    if (FIELD_EX32(info->idr[5], IDR5, GRAN64K) !=
                FIELD_EX32(s->idr[5], IDR5, GRAN64K)) {
        error_setg(errp, "Host SMMUv3 doesn't support 64K translation granule");
        return false;
    }

    /* QEMU SMMUv3 supports architecture version 3.1 */
    if (info->aidr < s->aidr) {
        error_setg(errp, "Host SMMUv3 architecture version not compatible");
        return false;
    }
    return true;
}

static bool
smmuv3_accel_hw_compatible(SMMUv3State *s, HostIOMMUDeviceIOMMUFD *idev,
                           Error **errp)
{
    struct iommu_hw_info_arm_smmuv3 info;
    uint32_t data_type;
    uint64_t caps;

    if (!iommufd_backend_get_device_info(idev->iommufd, idev->devid, &data_type,
                                         &info, sizeof(info), &caps, errp)) {
        return false;
    }

    if (data_type != IOMMU_HW_INFO_TYPE_ARM_SMMUV3) {
        error_setg(errp, "Wrong data type (%d) for Host SMMUv3 device info",
                     data_type);
        return false;
    }

    if (!smmuv3_accel_check_hw_compatible(s, &info, errp)) {
        return false;
    }
    return true;
}

static bool
smmuv3_accel_alloc_vdev(SMMUv3AccelDevice *accel_dev, int sid, Error **errp)
{
    SMMUViommu *vsmmu = accel_dev->vsmmu;
    IOMMUFDVdev *vdev;
    uint32_t vdevice_id;

    if (!accel_dev->idev || accel_dev->vdev) {
        return true;
    }

    if (!iommufd_backend_alloc_vdev(vsmmu->iommufd, accel_dev->idev->devid,
                                    vsmmu->viommu.viommu_id, sid,
                                    &vdevice_id, errp)) {
            return false;
    }
    if (!host_iommu_device_iommufd_attach_hwpt(accel_dev->idev,
                                               vsmmu->bypass_hwpt_id, errp)) {
        iommufd_backend_free_id(vsmmu->iommufd, vdevice_id);
        return false;
    }

    vdev = g_new(IOMMUFDVdev, 1);
    vdev->vdevice_id = vdevice_id;
    vdev->virt_id = sid;
    accel_dev->vdev = vdev;
    return true;
}

static bool
smmuv3_accel_dev_uninstall_nested_ste(SMMUv3AccelDevice *accel_dev, bool abort,
                                      Error **errp)
{
    HostIOMMUDeviceIOMMUFD *idev = accel_dev->idev;
    SMMUS1Hwpt *s1_hwpt = accel_dev->s1_hwpt;
    uint32_t hwpt_id;

    if (!s1_hwpt || !accel_dev->vsmmu) {
        return true;
    }

    if (abort) {
        hwpt_id = accel_dev->vsmmu->abort_hwpt_id;
    } else {
        hwpt_id = accel_dev->vsmmu->bypass_hwpt_id;
    }

    if (!host_iommu_device_iommufd_attach_hwpt(idev, hwpt_id, errp)) {
        return false;
    }
    trace_smmuv3_accel_uninstall_nested_ste(smmu_get_sid(&accel_dev->sdev),
                                            abort ? "abort" : "bypass",
                                            hwpt_id);

    iommufd_backend_free_id(s1_hwpt->iommufd, s1_hwpt->hwpt_id);
    accel_dev->s1_hwpt = NULL;
    g_free(s1_hwpt);
    return true;
}

static bool
smmuv3_accel_dev_install_nested_ste(SMMUv3AccelDevice *accel_dev,
                                    uint32_t data_type, uint32_t data_len,
                                    void *data, Error **errp)
{
    SMMUViommu *vsmmu = accel_dev->vsmmu;
    SMMUS1Hwpt *s1_hwpt = accel_dev->s1_hwpt;
    HostIOMMUDeviceIOMMUFD *idev = accel_dev->idev;
    uint32_t flags = 0;

    if (!idev || !vsmmu) {
        error_setg(errp, "Device 0x%x has no associated IOMMU dev or vIOMMU",
                   smmu_get_sid(&accel_dev->sdev));
        return false;
    }

    if (s1_hwpt) {
        if (!smmuv3_accel_dev_uninstall_nested_ste(accel_dev, true, errp)) {
            return false;
        }
    }

    s1_hwpt = g_new0(SMMUS1Hwpt, 1);
    s1_hwpt->iommufd = idev->iommufd;
    if (!iommufd_backend_alloc_hwpt(idev->iommufd, idev->devid,
                                    vsmmu->viommu.viommu_id, flags,
                                    data_type, data_len, data,
                                    &s1_hwpt->hwpt_id, errp)) {
        return false;
    }

    if (!host_iommu_device_iommufd_attach_hwpt(idev, s1_hwpt->hwpt_id, errp)) {
        iommufd_backend_free_id(idev->iommufd, s1_hwpt->hwpt_id);
        return false;
    }
    accel_dev->s1_hwpt = s1_hwpt;
    return true;
}

bool
smmuv3_accel_install_nested_ste(SMMUv3State *s, SMMUDevice *sdev, int sid,
                                Error **errp)
{
    SMMUv3AccelDevice *accel_dev;
    SMMUEventInfo event = {.type = SMMU_EVT_NONE, .sid = sid,
                           .inval_ste_allowed = true};
    struct iommu_hwpt_arm_smmuv3 nested_data = {};
    uint64_t ste_0, ste_1;
    uint32_t config;
    STE ste;
    int ret;

    if (!s->accel) {
        return true;
    }

    accel_dev = container_of(sdev, SMMUv3AccelDevice, sdev);
    if (!accel_dev->vsmmu) {
        return true;
    }

    if (!smmuv3_accel_alloc_vdev(accel_dev, sid, errp)) {
        return false;
    }

    ret = smmu_find_ste(sdev->smmu, sid, &ste, &event);
    if (ret) {
        error_setg(errp, "Failed to find STE for Device 0x%x", sid);
        return true;
    }

    config = STE_CONFIG(&ste);
    if (!STE_VALID(&ste) || !STE_CFG_S1_ENABLED(config)) {
        if (!smmuv3_accel_dev_uninstall_nested_ste(accel_dev,
                                                   STE_CFG_ABORT(config),
                                                   errp)) {
            return false;
        }
        smmuv3_flush_config(sdev);
        return true;
    }

    ste_0 = (uint64_t)ste.word[0] | (uint64_t)ste.word[1] << 32;
    ste_1 = (uint64_t)ste.word[2] | (uint64_t)ste.word[3] << 32;
    nested_data.ste[0] = cpu_to_le64(ste_0 & STE0_MASK);
    nested_data.ste[1] = cpu_to_le64(ste_1 & STE1_MASK);

    if (!smmuv3_accel_dev_install_nested_ste(accel_dev,
                                             IOMMU_HWPT_DATA_ARM_SMMUV3,
                                             sizeof(nested_data),
                                             &nested_data, errp)) {
        error_append_hint(errp, "Unable to install sid=0x%x nested STE="
                          "0x%"PRIx64":=0x%"PRIx64"", sid,
                          (uint64_t)le64_to_cpu(nested_data.ste[1]),
                          (uint64_t)le64_to_cpu(nested_data.ste[0]));
        return false;
    }
    trace_smmuv3_accel_install_nested_ste(sid, nested_data.ste[1],
                                          nested_data.ste[0]);
    return true;
}

bool smmuv3_accel_install_nested_ste_range(SMMUv3State *s, SMMUSIDRange *range,
                                           Error **errp)
{
    SMMUv3AccelState *s_accel = s->s_accel;
    SMMUv3AccelDevice *accel_dev;

    if (!s_accel || !s_accel->vsmmu) {
        return true;
    }

    QLIST_FOREACH(accel_dev, &s_accel->vsmmu->device_list, next) {
        uint32_t sid = smmu_get_sid(&accel_dev->sdev);

        if (sid >= range->start && sid <= range->end) {
            if (!smmuv3_accel_install_nested_ste(s, &accel_dev->sdev,
                                                 sid, errp)) {
                return false;
            }
        }
    }
    return true;
}

/*
 * This issues the invalidation cmd to the host SMMUv3.
 * Note: sdev can be NULL for certain invalidation commands
 * e.g., SMMU_CMD_TLBI_NH_ASID, SMMU_CMD_TLBI_NH_VA etc.
 */
bool smmuv3_accel_issue_inv_cmd(SMMUv3State *bs, void *cmd, SMMUDevice *sdev,
                                Error **errp)
{
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUv3AccelState *s_accel = s->s_accel;
    IOMMUFDViommu *viommu;
    uint32_t entry_num = 1;

    /* No vIOMMU means no VFIO/IOMMUFD devices, nothing to invalidate. */
    if (!s_accel || !s_accel->vsmmu) {
        return true;
    }

    /*
     * Called for emulated bridges or root ports, but SID-based
     * invalidations (e.g. CFGI_CD) apply only to vfio-pci endpoints
     * with a valid vIOMMU vdev.
     */
    if (sdev && !container_of(sdev, SMMUv3AccelDevice, sdev)->vdev) {
        return true;
    }

    viommu = &s_accel->vsmmu->viommu;
    /* Single command (entry_num = 1); no need to check returned entry_num */
    return iommufd_backend_invalidate_cache(
                   viommu->iommufd, viommu->viommu_id,
                   IOMMU_VIOMMU_INVALIDATE_DATA_ARM_SMMUV3,
                   sizeof(Cmd), &entry_num, cmd, errp);
}

static SMMUv3AccelDevice *smmuv3_accel_get_dev(SMMUState *bs, SMMUPciBus *sbus,
                                               PCIBus *bus, int devfn)
{
    SMMUDevice *sdev = sbus->pbdev[devfn];
    SMMUv3AccelDevice *accel_dev;

    if (sdev) {
        return container_of(sdev, SMMUv3AccelDevice, sdev);
    }

    accel_dev = g_new0(SMMUv3AccelDevice, 1);
    sdev = &accel_dev->sdev;

    sbus->pbdev[devfn] = sdev;
    smmu_init_sdev(bs, sdev, bus, devfn);
    return accel_dev;
}

static bool
smmuv3_accel_dev_alloc_viommu(SMMUv3AccelDevice *accel_dev,
                              HostIOMMUDeviceIOMMUFD *idev, Error **errp)
{
    struct iommu_hwpt_arm_smmuv3 bypass_data = {
        .ste = { SMMU_STE_CFG_BYPASS | SMMU_STE_VALID, 0x0ULL },
    };
    struct iommu_hwpt_arm_smmuv3 abort_data = {
        .ste = { SMMU_STE_VALID, 0x0ULL },
    };
    SMMUDevice *sdev = &accel_dev->sdev;
    SMMUState *bs = sdev->smmu;
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUv3AccelState *s_accel = s->s_accel;
    uint32_t s2_hwpt_id = idev->hwpt_id;
    SMMUViommu *vsmmu;
    uint32_t viommu_id;

    if (s_accel->vsmmu) {
        accel_dev->vsmmu = s_accel->vsmmu;
        return true;
    }

    if (!iommufd_backend_alloc_viommu(idev->iommufd, idev->devid,
                                      IOMMU_VIOMMU_TYPE_ARM_SMMUV3,
                                      s2_hwpt_id, &viommu_id, errp)) {
        return false;
    }

    vsmmu = g_new0(SMMUViommu, 1);
    vsmmu->viommu.viommu_id = viommu_id;
    vsmmu->viommu.s2_hwpt_id = s2_hwpt_id;
    vsmmu->viommu.iommufd = idev->iommufd;

    /*
     * Pre-allocate HWPTs for S1 bypass and abort cases. These will be attached
     * later for guest STEs or GBPAs that require bypass or abort configuration.
     */
    if (!iommufd_backend_alloc_hwpt(idev->iommufd, idev->devid, viommu_id,
                                    0, IOMMU_HWPT_DATA_ARM_SMMUV3,
                                    sizeof(abort_data), &abort_data,
                                    &vsmmu->abort_hwpt_id, errp)) {
        goto free_viommu;
    }

    if (!iommufd_backend_alloc_hwpt(idev->iommufd, idev->devid, viommu_id,
                                    0, IOMMU_HWPT_DATA_ARM_SMMUV3,
                                    sizeof(bypass_data), &bypass_data,
                                    &vsmmu->bypass_hwpt_id, errp)) {
        goto free_abort_hwpt;
    }

    vsmmu->iommufd = idev->iommufd;
    s_accel->vsmmu = vsmmu;
    accel_dev->vsmmu = vsmmu;
    return true;

free_abort_hwpt:
    iommufd_backend_free_id(idev->iommufd, vsmmu->abort_hwpt_id);
free_viommu:
    iommufd_backend_free_id(idev->iommufd, vsmmu->viommu.viommu_id);
    g_free(vsmmu);
    return false;
}

static bool smmuv3_accel_set_iommu_device(PCIBus *bus, void *opaque, int devfn,
                                          HostIOMMUDevice *hiod, Error **errp)
{
    HostIOMMUDeviceIOMMUFD *idev = HOST_IOMMU_DEVICE_IOMMUFD(hiod);
    SMMUState *bs = opaque;
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUv3AccelState *s_accel = s->s_accel;
    SMMUPciBus *sbus = smmu_get_sbus(bs, bus);
    SMMUv3AccelDevice *accel_dev = smmuv3_accel_get_dev(bs, sbus, bus, devfn);
    SMMUDevice *sdev = &accel_dev->sdev;
    uint16_t sid = smmu_get_sid(sdev);

    if (!idev) {
        return true;
    }

    if (accel_dev->idev) {
        if (accel_dev->idev != idev) {
            error_setg(errp, "Device 0x%x already has an associated IOMMU dev",
                       sid);
            return false;
        }
        return true;
    }

    /*
     * Check the host SMMUv3 associated with the dev is compatible with the
     * QEMU SMMUv3 accel.
     */
    if (!smmuv3_accel_hw_compatible(s, idev, errp)) {
        return false;
    }

    if (!smmuv3_accel_dev_alloc_viommu(accel_dev, idev, errp)) {
        error_append_hint(errp, "Device 0x%x: Unable to alloc viommu", sid);
        return false;
    }

    accel_dev->idev = idev;
    QLIST_INSERT_HEAD(&s_accel->vsmmu->device_list, accel_dev, next);
    trace_smmuv3_accel_set_iommu_device(devfn, idev->devid);
    return true;
}

static void smmuv3_accel_unset_iommu_device(PCIBus *bus, void *opaque,
                                            int devfn)
{
    SMMUState *bs = opaque;
    SMMUv3State *s = ARM_SMMUV3(bs);
    SMMUPciBus *sbus = g_hash_table_lookup(bs->smmu_pcibus_by_busptr, bus);
    SMMUv3AccelDevice *accel_dev;
    IOMMUFDVdev *vdev;
    SMMUViommu *vsmmu;
    SMMUDevice *sdev;
    uint16_t sid;

    if (!sbus) {
        return;
    }

    sdev = sbus->pbdev[devfn];
    if (!sdev) {
        return;
    }

    sid = smmu_get_sid(sdev);
    accel_dev = container_of(sdev, SMMUv3AccelDevice, sdev);
    /* Re-attach the default s2 hwpt id */
    if (!host_iommu_device_iommufd_attach_hwpt(accel_dev->idev,
                                               accel_dev->idev->hwpt_id,
                                               NULL)) {
        error_report("Unable to attach dev 0x%x to the default HW pagetable",
                     sid);
    }

    accel_dev->idev = NULL;
    QLIST_REMOVE(accel_dev, next);
    trace_smmuv3_accel_unset_iommu_device(devfn, sid);

    vsmmu = s->s_accel->vsmmu;
    vdev = accel_dev->vdev;
    if (vdev) {
        iommufd_backend_free_id(vsmmu->iommufd, vdev->vdevice_id);
        g_free(vdev);
        accel_dev->vdev = NULL;
    }

    if (QLIST_EMPTY(&vsmmu->device_list)) {
        iommufd_backend_free_id(vsmmu->iommufd, vsmmu->bypass_hwpt_id);
        iommufd_backend_free_id(vsmmu->iommufd, vsmmu->abort_hwpt_id);
        iommufd_backend_free_id(vsmmu->iommufd, vsmmu->viommu.viommu_id);
        g_free(vsmmu);
        s->s_accel->vsmmu = NULL;
    }
}

static AddressSpace *smmuv3_accel_get_msi_as(PCIBus *bus, void *opaque,
                                             int devfn)
{
    SMMUState *bs = opaque;
    SMMUPciBus *sbus = smmu_get_sbus(bs, bus);
    SMMUv3AccelDevice *accel_dev = smmuv3_accel_get_dev(bs, sbus, bus, devfn);
    SMMUDevice *sdev = &accel_dev->sdev;

    /*
     * If the assigned vfio-pci dev has S1 translation enabled by Guest,
     * return IOMMU address space for MSI translation. Otherwise, return
     * system address space.
     */
    if (accel_dev->s1_hwpt) {
        return &sdev->as;
    } else {
        return &address_space_memory;
    }
}

static bool smmuv3_accel_pdev_allowed(PCIDevice *pdev, bool *vfio_pci)
{

    if (object_dynamic_cast(OBJECT(pdev), TYPE_PCI_BRIDGE) ||
        object_dynamic_cast(OBJECT(pdev), TYPE_PXB_PCIE_DEV) ||
        object_dynamic_cast(OBJECT(pdev), TYPE_GPEX_ROOT_DEVICE)) {
        return true;
    } else if ((object_dynamic_cast(OBJECT(pdev), TYPE_VFIO_PCI))) {
        *vfio_pci = true;
        if (object_property_get_link(OBJECT(pdev), "iommufd", NULL)) {
            return true;
        }
    }
    return false;
}

static bool smmuv3_accel_supports_as(PCIBus *bus, void *opaque, int devfn,
                                     Error **errp)
{
    PCIDevice *pdev = pci_find_device(bus, pci_bus_num(bus), devfn);
    bool vfio_pci = false;

    if (pdev && !smmuv3_accel_pdev_allowed(pdev, &vfio_pci)) {
        if (vfio_pci) {
            error_setg(errp, "vfio-pci endpoint devices without an iommufd "
                       "backend not allowed when using arm-smmuv3,accel=on");

        } else {
            error_setg(errp, "Emulated endpoint devices are not allowed when "
                       "using arm-smmuv3,accel=on");
        }
        return false;
    }
    return true;
}
/*
 * Find or add an address space for the given PCI device.
 *
 * If a device matching @bus and @devfn already exists, return its
 * corresponding address space. Otherwise, create a new device entry
 * and initialize address space for it.
 */
static AddressSpace *smmuv3_accel_find_add_as(PCIBus *bus, void *opaque,
                                              int devfn)
{
    PCIDevice *pdev = pci_find_device(bus, pci_bus_num(bus), devfn);
    SMMUState *bs = opaque;
    SMMUPciBus *sbus = smmu_get_sbus(bs, bus);
    SMMUv3AccelDevice *accel_dev = smmuv3_accel_get_dev(bs, sbus, bus, devfn);
    SMMUDevice *sdev = &accel_dev->sdev;
    bool vfio_pci = false;

    if (pdev && !smmuv3_accel_pdev_allowed(pdev, &vfio_pci)) {
        /* Should never be here: supports_address_space() filters these out */
        g_assert_not_reached();
    }

    /*
     * In the accelerated mode, a vfio-pci device attached via the iommufd
     * backend must remain in the system address space. Such a device is
     * always translated by its physical SMMU (using either a stage-2-only
     * STE or a nested STE), where the parent stage-2 page table is allocated
     * by the VFIO core to back the system address space.
     *
     * Return the shared_as_sysmem aliased to the global system memory in this
     * case. Sharing address_space_memory also allows devices under different
     * vSMMU instances in the same VM to reuse a single nesting parent HWPT in
     * the VFIO core.
     */
    if (vfio_pci) {
        return shared_as_sysmem;
    } else {
        return &sdev->as;
    }
}

static uint64_t smmuv3_accel_get_viommu_flags(void *opaque)
{
    /*
     * We return VIOMMU_FLAG_WANT_NESTING_PARENT to inform VFIO core to create a
     * nesting parent which is required for accelerated SMMUv3 support.
     * The real HW nested support should be reported from host SMMUv3 and if
     * it doesn't, the nesting parent allocation will fail anyway in VFIO core.
     */
    return VIOMMU_FLAG_WANT_NESTING_PARENT;
}

static const PCIIOMMUOps smmuv3_accel_ops = {
    .supports_address_space = smmuv3_accel_supports_as,
    .get_address_space = smmuv3_accel_find_add_as,
    .get_viommu_flags = smmuv3_accel_get_viommu_flags,
    .set_iommu_device = smmuv3_accel_set_iommu_device,
    .unset_iommu_device = smmuv3_accel_unset_iommu_device,
    .get_msi_address_space = smmuv3_accel_get_msi_as,
};

void smmuv3_accel_idr_override(SMMUv3State *s)
{
    if (!s->accel) {
        return;
    }

    /* By default QEMU SMMUv3 has RIL. Update IDR3 if user has disabled it */
    if (!s->ril) {
        s->idr[3] = FIELD_DP32(s->idr[3], IDR3, RIL, 0);
    }
    /* QEMU SMMUv3 has no ATS. Update IDR0 if user has enabled it */
    if (s->ats) {
        s->idr[0] = FIELD_DP32(s->idr[0], IDR0, ATS, 1); /* ATS */
    }
    /* QEMU SMMUv3 has OAS set 44. Update IDR5 if user has it set to 48 bits*/
    if (s->oas == 48) {
        s->idr[5] = FIELD_DP32(s->idr[5], IDR5, OAS, SMMU_IDR5_OAS_48);
    }
}

/* Based on SMUUv3 GBPA configuration, attach a corresponding HWPT */
void smmuv3_accel_gbpa_update(SMMUv3State *s)
{
    SMMUv3AccelDevice *accel_dev;
    Error *local_err = NULL;
    SMMUViommu *vsmmu;
    uint32_t hwpt_id;

    if (!s->accel || !s->s_accel->vsmmu) {
        return;
    }

    vsmmu = s->s_accel->vsmmu;
    /*
     * The Linux kernel does not allow configuring GBPA MemAttr, MTCFG,
     * ALLOCCFG, SHCFG, PRIVCFG, or INSTCFG fields for a vSTE. Host kernel
     * has final control over these parameters. Hence, use one of the
     * pre-allocated HWPTs depending on GBPA.ABORT value.
     */
    if (s->gbpa & SMMU_GBPA_ABORT) {
        hwpt_id = vsmmu->abort_hwpt_id;
    } else {
        hwpt_id = vsmmu->bypass_hwpt_id;
    }

    QLIST_FOREACH(accel_dev, &vsmmu->device_list, next) {
        if (!host_iommu_device_iommufd_attach_hwpt(accel_dev->idev, hwpt_id,
                                                   &local_err)) {
            error_append_hint(&local_err, "Failed to attach GBPA hwpt id %u "
                              "for dev id %u", hwpt_id, accel_dev->idev->devid);
            error_report_err(local_err);
        }
    }
}

void smmuv3_accel_reset(SMMUv3State *s)
{
     /* Attach a HWPT based on GBPA reset value */
     smmuv3_accel_gbpa_update(s);
}

static void smmuv3_accel_as_init(SMMUv3State *s)
{

    if (shared_as_sysmem) {
        return;
    }

    memory_region_init(&root, OBJECT(s), "root", UINT64_MAX);
    memory_region_init_alias(&sysmem, OBJECT(s), "smmuv3-accel-sysmem",
                             get_system_memory(), 0,
                             memory_region_size(get_system_memory()));
    memory_region_add_subregion(&root, 0, &sysmem);

    shared_as_sysmem = g_new0(AddressSpace, 1);
    address_space_init(shared_as_sysmem, &root, "smmuv3-accel-as-sysmem");
}

void smmuv3_accel_init(SMMUv3State *s)
{
    SMMUState *bs = ARM_SMMU(s);

    bs->iommu_ops = &smmuv3_accel_ops;
    smmuv3_accel_as_init(s);
    s->s_accel = g_new0(SMMUv3AccelState, 1);
}
