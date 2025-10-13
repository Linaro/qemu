/*
 * Copyright (c) 2025 Huawei Technologies R & D (UK) Ltd
 * Copyright (C) 2025 NVIDIA
 * Written by Nicolin Chen, Shameer Kolothum
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_SMMUV3_ACCEL_H
#define HW_ARM_SMMUV3_ACCEL_H

#include "hw/arm/smmu-common.h"
#include "system/iommufd.h"
#include <linux/iommufd.h>
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif
#include CONFIG_DEVICES

typedef struct SMMUViommu {
    IOMMUFDBackend *iommufd;
    IOMMUFDViommu core;
    uint32_t bypass_hwpt_id;
    uint32_t abort_hwpt_id;
    QLIST_HEAD(, SMMUv3AccelDevice) device_list;
} SMMUViommu;

typedef struct PendFaultEntry {
    struct iommu_hwpt_pgfault fault;
    QTAILQ_ENTRY(PendFaultEntry) entry;
} PendFaultEntry;

typedef struct PageRespEntry {
    struct iommu_hwpt_page_response resp;
    QTAILQ_ENTRY(PageRespEntry) entry;
} PageRespEntry;

typedef struct SMMUS1Hwpt {
    void  *sdev;
    IOMMUFDBackend *iommufd;
    uint32_t hwpt_id;
    uint32_t out_fault_fd;
    /* fault handling */
    struct io_uring fault_ring;
    QemuThread read_fault_thread;
    QemuThread write_fault_thread;
    QemuMutex fault_mutex;
    QemuCond fault_cond;
    QTAILQ_HEAD(, PageRespEntry) pageresp;
    QTAILQ_HEAD(, PendFaultEntry) pendfault;
    bool exiting;
} SMMUS1Hwpt;

typedef struct SMMUv3AccelDevice {
    SMMUDevice  sdev;
    HostIOMMUDeviceIOMMUFD *idev;
    SMMUS1Hwpt *s1_hwpt;
    IOMMUFDVdev *vdev;
    SMMUViommu *viommu;
    QLIST_ENTRY(SMMUv3AccelDevice) next;
} SMMUv3AccelDevice;

typedef struct SMMUv3AccelState {
    SMMUViommu *viommu;
} SMMUv3AccelState;

#ifdef CONFIG_ARM_SMMUV3_ACCEL
void smmuv3_accel_init(SMMUv3State *s);
bool smmuv3_accel_install_nested_ste(SMMUv3State *s, SMMUDevice *sdev, int sid,
                                     Error **errp);
bool smmuv3_accel_install_nested_ste_range(SMMUv3State *s, SMMUSIDRange *range,
                                           Error **errp);
bool smmuv3_accel_issue_inv_cmd(SMMUv3State *s, void *cmd, SMMUDevice *sdev,
                                Error **errp);
void smmuv3_accel_attach_bypass_hwpt(SMMUv3State *s);
void smmuv3_accel_idr_override(SMMUv3State *s);
void smmuv3_notify_stall_resume(SMMUState *bs, uint32_t sid,
                                uint32_t stag, uint32_t code);
#else
static inline void smmuv3_accel_init(SMMUv3State *s)
{
}
static inline bool
smmuv3_accel_install_nested_ste(SMMUv3State *s, SMMUDevice *sdev, int sid,
                                Error **errp)
{
    return true;
}
static inline bool
smmuv3_accel_install_nested_ste_range(SMMUv3State *s, SMMUSIDRange *range,
                                      Error **errp)
{
    return true;
}
static inline bool
smmuv3_accel_issue_inv_cmd(SMMUv3State *s, void *cmd, SMMUDevice *sdev,
                           Error **errp)
{
    return true;
}
static inline void smmuv3_accel_attach_bypass_hwpt(SMMUv3State *s)
{
}
static inline void smmuv3_accel_idr_override(SMMUv3State *s)
{
}
#endif

#endif /* HW_ARM_SMMUV3_ACCEL_H */
