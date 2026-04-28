/**
 * Copyright (c) 2024, Advanced Micro Devices, Inc. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ionic_rc.h"

#include <uct/ib/base/ib_md.h>
#include <uct/ib/base/ib_device.h>
#include <ucs/type/status.h>
#include <ucs/debug/log.h>
#include <ucs/sys/math.h>


static uct_ib_md_ops_t uct_ib_ionic_md_ops;

static ucs_status_t uct_ib_ionic_md_open(struct ibv_device *ibv_device,
                                         const uct_ib_md_config_t *md_config,
                                         uct_ib_md_t **p_md)
{
    struct ibv_context *ctx;
    struct ibv_device_attr dev_attr;
    uct_ib_md_t *md;
    uct_ib_device_t *dev;
    ucs_status_t status;
    int ret;

    ucs_info("ionic_rc: md_open probing device %s",
             ibv_get_device_name(ibv_device));

    ctx = ibv_open_device(ibv_device);
    if (ctx == NULL) {
        ucs_diag("ionic_rc: ibv_open_device(%s) failed: %m",
                 ibv_get_device_name(ibv_device));
        return UCS_ERR_IO_ERROR;
    }

    /* Detect Ionic device by PCI vendor ID (Pensando/AMD = 0x1dd8) */
    ret = ibv_query_device(ctx, &dev_attr);
    if (ret != 0) {
        ucs_info("ionic_rc: ibv_query_device(%s) failed: %d",
                 ibv_get_device_name(ibv_device), ret);
        status = UCS_ERR_UNSUPPORTED;
        goto err_free_context;
    }

    if (dev_attr.vendor_id != UCT_IONIC_PCI_VENDOR_ID) {
        ucs_info("ionic_rc: %s vendor_id 0x%x != Ionic (0x%x), skipping",
                 ibv_get_device_name(ibv_device), dev_attr.vendor_id,
                 UCT_IONIC_PCI_VENDOR_ID);
        status = UCS_ERR_UNSUPPORTED;
        goto err_free_context;
    }

    ucs_info("ionic_rc: %s matched ionic vendor 0x%x: max_qp=%d max_srq=%d "
             "max_sge=%d max_cqe=%d phys_port_cnt=%u",
             ibv_get_device_name(ibv_device), dev_attr.vendor_id,
             dev_attr.max_qp, dev_attr.max_srq, dev_attr.max_sge,
             dev_attr.max_cqe, dev_attr.phys_port_cnt);

    md = ucs_derived_of(uct_ib_md_alloc(sizeof(*md), "ib_ionic_md", ctx),
                        uct_ib_md_t);
    if (md == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err_free_context;
    }

    md->super.ops = &uct_ib_ionic_md_ops.super;
    md->name      = UCT_IB_MD_NAME(ionic);

    status = uct_ib_device_query(&md->dev, ibv_device);
    if (status != UCS_OK) {
        goto err_md_free;
    }

    dev                        = &md->dev;
    dev->mr_access_flags       = IBV_ACCESS_LOCAL_WRITE |
                                 IBV_ACCESS_REMOTE_WRITE |
                                 IBV_ACCESS_REMOTE_READ;
    dev->ordered_send_comp     = 0;
    /* ionic does not support ibv_req_notify_cq() with comp_channel for the
     * QP types we create. Leave req_notify_cq_support=0 (no comp_channel),
     * and override the event_flags in iface_query to drop EVENT_FD. */
    dev->req_notify_cq_support = 0;
    dev->max_inline_data       = 4 * UCS_KBYTE;

    status = uct_ib_md_open_common(md, ibv_device, md_config);
    if (status != UCS_OK) {
        ucs_info("ionic_rc: uct_ib_md_open_common(%s) failed: %s",
                 ibv_get_device_name(ibv_device), ucs_status_string(status));
        goto err_md_free;
    }

    ucs_info("ionic_rc: md_open SUCCESS for %s, MD=%p",
             ibv_get_device_name(ibv_device), md);

    *p_md = md;
    return UCS_OK;

err_md_free:
    uct_ib_md_free(md);
err_free_context:
    uct_ib_md_device_context_close(ctx);
    return status;
}

static uct_ib_md_ops_t uct_ib_ionic_md_ops = {
    .super = {
        .close              = uct_ib_md_close,
        .query              = uct_ib_md_query,
        .mem_alloc          = (uct_md_mem_alloc_func_t)ucs_empty_function_return_unsupported,
        .mem_free           = (uct_md_mem_free_func_t)ucs_empty_function_return_unsupported,
        .mem_advise         = uct_ib_mem_advise,
        .mem_reg            = uct_ib_verbs_mem_reg,
        .mem_dereg          = uct_ib_verbs_mem_dereg,
        .mem_query          = (uct_md_mem_query_func_t)ucs_empty_function_return_unsupported,
        .mkey_pack          = uct_ib_verbs_mkey_pack,
        .mem_attach         = (uct_md_mem_attach_func_t)ucs_empty_function_return_unsupported,
        .detect_memory_type = (uct_md_detect_memory_type_func_t)ucs_empty_function_return_unsupported,
        .mem_elem_pack      = (uct_md_mem_elem_pack_func_t)ucs_empty_function_return_unsupported
    },
    .open = uct_ib_ionic_md_open,
};

UCT_IB_MD_DEFINE_ENTRY(ionic, uct_ib_ionic_md_ops);

extern uct_tl_t UCT_TL_NAME(ionic_rc);

void UCS_F_CTOR uct_ionic_init(void)
{
    ucs_info("ionic_rc: module CTOR - registering ionic MD ops and ionic_rc TL");
    ucs_list_add_head(&uct_ib_ops, &UCT_IB_MD_OPS_NAME(ionic).list);
    uct_tl_register(&uct_ib_component, &UCT_TL_NAME(ionic_rc));
}

void UCS_F_DTOR uct_ionic_cleanup(void)
{
    ucs_info("ionic_rc: module DTOR - unregistering");
    uct_tl_unregister(&UCT_TL_NAME(ionic_rc));
    ucs_list_del(&UCT_IB_MD_OPS_NAME(ionic).list);
}
