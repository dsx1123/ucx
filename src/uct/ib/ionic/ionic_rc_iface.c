/**
 * Copyright (c) 2024, Advanced Micro Devices, Inc. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ionic_rc.h"
#include "ionic_rc_impl.h"

#include <uct/api/uct.h>
#include <uct/ib/rc/base/rc_iface.h>
#include <uct/ib/base/ib_device.h>
#include <uct/ib/base/ib_log.h>
#include <uct/base/uct_md.h>
#include <ucs/arch/bitops.h>
#include <ucs/arch/cpu.h>
#include <ucs/debug/log.h>
#include <string.h>


#define UCT_IONIC_RC_IFACE_OVERHEAD  75e-9


static uct_rc_iface_ops_t uct_ionic_rc_iface_ops;
static uct_iface_ops_t uct_ionic_rc_iface_tl_ops;


ucs_config_field_t uct_ionic_rc_iface_config_table[] = {
    {"RC_", UCT_IB_SEND_OVERHEAD_DEFAULT(UCT_IONIC_RC_IFACE_OVERHEAD), NULL,
     ucs_offsetof(uct_ionic_rc_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_rc_iface_config_table)},

    {"TX_MAX_WR", "-1",
     "Limits the number of outstanding posted work requests. The actual limit is\n"
     "a minimum between this value and the TX queue length. -1 means no limit.",
     ucs_offsetof(uct_ionic_rc_iface_config_t, tx_max_wr), UCS_CONFIG_TYPE_UINT},

    {"NUM_PLANES", "1",
     "Number of planes for pUEC multiplane routing.\n"
     "1 = single-plane pUEC (default), 2 or 4 = multiplane pUEC.\n"
     "When >1, per-plane AH headers with distinct GIDs are configured.",
     ucs_offsetof(uct_ionic_rc_iface_config_t, num_planes), UCS_CONFIG_TYPE_UINT},

    {"NUM_PATHS", "32",
     "Number of source port paths for pUEC load balancing.\n"
     "Encoded in pkey_index during RTR transition.",
     ucs_offsetof(uct_ionic_rc_iface_config_t, num_paths), UCS_CONFIG_TYPE_UINT},

    {NULL}
};


static unsigned uct_ionic_rc_get_tx_res_count(uct_ionic_rc_ep_t *ep,
                                              struct ibv_wc *wc)
{
    return wc->wr_id - ep->txcnt.ci;
}

static UCS_F_ALWAYS_INLINE void
uct_ionic_rc_update_tx_res(uct_rc_iface_t *iface, uct_ionic_rc_ep_t *ep,
                           unsigned count)
{
    ep->txcnt.ci += count;
    uct_rc_txqp_available_add(&ep->super.txqp, count);
    uct_rc_iface_update_reads(iface);
    uct_rc_iface_add_cq_credits(iface, count);
}

static void uct_ionic_rc_handle_failure(uct_ib_iface_t *ib_iface, void *arg,
                                        ucs_status_t ep_status)
{
    struct ibv_wc *wc       = arg;
    uct_rc_iface_t *iface   = ucs_derived_of(ib_iface, uct_rc_iface_t);
    ucs_log_level_t log_lvl = UCS_LOG_LEVEL_FATAL;
    char peer_info[128]     = {};
    unsigned dest_qpn;
    uct_ionic_rc_ep_t *ep;
    ucs_status_t status;
    unsigned count;
    struct ibv_ah_attr ah_attr;

    ep = ucs_derived_of(uct_rc_iface_lookup_ep(iface, wc->qp_num),
                        uct_ionic_rc_ep_t);
    if (!ep) {
        return;
    }

    count = uct_ionic_rc_get_tx_res_count(ep, wc);
    uct_rc_txqp_purge_outstanding(iface, &ep->super.txqp, ep_status,
                                  ep->txcnt.ci + count, 0);
    ucs_arbiter_group_purge(&iface->tx.arbiter, &ep->super.arb_group,
                            uct_rc_ep_arbiter_purge_internal_cb, NULL);
    uct_ionic_rc_update_tx_res(iface, ep, count);

    if (ep->super.flags & (UCT_RC_EP_FLAG_ERR_HANDLER_INVOKED |
                           UCT_RC_EP_FLAG_FLUSH_CANCEL)) {
        goto out;
    }

    ep->super.flags |= UCT_RC_EP_FLAG_ERR_HANDLER_INVOKED;
    uct_rc_fc_restore_wnd(iface, &ep->super.fc);

    status  = uct_iface_handle_ep_err(&iface->super.super.super,
                                      &ep->super.super.super, ep_status);
    log_lvl = uct_base_iface_failure_log_level(&ib_iface->super, status,
                                               ep_status);
    status  = uct_ib_query_qp_peer_info(ep->qp, &ah_attr, &dest_qpn);
    if (status == UCS_OK) {
        uct_ib_log_dump_qp_peer_info(ib_iface, &ah_attr, dest_qpn, peer_info,
                                     sizeof(peer_info));
    }

    ucs_log(log_lvl,
            "send completion with error: %s [qpn 0x%x wrid 0x%lx "
            "vendor_err 0x%x]\n%s",
            ibv_wc_status_str(wc->status), wc->qp_num, wc->wr_id,
            wc->vendor_err, peer_info);

out:
    uct_rc_iface_arbiter_dispatch(iface);
}


static UCS_F_ALWAYS_INLINE unsigned
uct_ionic_rc_iface_poll_tx(uct_ionic_rc_iface_t *iface)
{
    uct_ionic_rc_ep_t *ep;
    uint16_t count;
    int i;
    unsigned num_wcs = iface->super.super.config.tx_max_poll;
    struct ibv_wc wc[num_wcs];
    ucs_status_t status;

    UCT_IONIC_RC_IFACE_FOREACH_TXWQE(&iface->super, i, wc, num_wcs) {
        ep = ucs_derived_of(uct_rc_iface_lookup_ep(&iface->super, wc[i].qp_num),
                            uct_ionic_rc_ep_t);
        if (ucs_unlikely((wc[i].status != IBV_WC_SUCCESS) || (ep == NULL))) {
            status = uct_ib_wc_to_ucs_status(wc[i].status);
            iface->super.super.ops->handle_failure(&iface->super.super, &wc[i],
                                                   status);
            continue;
        }

        count = uct_ionic_rc_get_tx_res_count(ep, &wc[i]);
        ucs_trace_poll("ionic_rc iface %p tx_wc wrid 0x%lx ep %p qpn 0x%x count %d",
                       iface, wc[i].wr_id, ep, wc[i].qp_num, count);

        uct_rc_txqp_completion_desc(&ep->super.txqp, ep->txcnt.ci + count);
        ucs_arbiter_group_schedule(&iface->super.tx.arbiter,
                                   &ep->super.arb_group);
        uct_ionic_rc_update_tx_res(&iface->super, ep, count);
        ucs_arbiter_dispatch(&iface->super.tx.arbiter, 1, uct_rc_ep_process_pending,
                             NULL);
    }

    return num_wcs;
}

static unsigned uct_ionic_rc_iface_progress(void *arg)
{
    uct_ionic_rc_iface_t *iface = arg;
    unsigned count;

    count = uct_ionic_rc_iface_poll_rx_common(iface);
    if (!uct_rc_iface_poll_tx(&iface->super, count)) {
        return count;
    }

    return count + uct_ionic_rc_iface_poll_tx(iface);
}

static void uct_ionic_rc_iface_init_inl_wrs(uct_ionic_rc_iface_t *iface)
{
    memset(&iface->inl_am_wr, 0, sizeof(iface->inl_am_wr));
    iface->inl_am_wr.sg_list        = iface->inl_sge;
    iface->inl_am_wr.opcode         = IBV_WR_SEND;
    iface->inl_am_wr.send_flags     = IBV_SEND_INLINE;

    memset(&iface->inl_rwrite_wr, 0, sizeof(iface->inl_rwrite_wr));
    iface->inl_rwrite_wr.sg_list    = iface->inl_sge;
    iface->inl_rwrite_wr.num_sge    = 1;
    iface->inl_rwrite_wr.opcode     = IBV_WR_RDMA_WRITE;
    iface->inl_rwrite_wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
}

static ucs_status_t
uct_ionic_rc_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_iface,
                                                  uct_ionic_rc_iface_t);
    ucs_status_t status;

    ucs_info("ionic_rc: iface_query iface=%p max_inline=%zu max_send_sge=%zu",
             iface, iface->config.max_inline, iface->config.max_send_sge);

    status = uct_rc_iface_query(&iface->super, iface_attr,
                                iface->config.max_inline,
                                iface->config.max_inline,
                                iface->config.short_desc_size,
                                iface->config.max_send_sge - 1,
                                sizeof(uct_rc_hdr_t),
                                iface->config.max_send_sge);
    if (status != UCS_OK) {
        ucs_info("ionic_rc: uct_rc_iface_query failed: %s",
                 ucs_status_string(status));
        return status;
    }

    iface_attr->cap.flags |= UCT_IFACE_FLAG_EP_CHECK;
    iface_attr->latency.m += 1e-9;  /* 1 ns per each extra QP */

    /* Software overhead */
    iface_attr->overhead = UCT_IONIC_RC_IFACE_OVERHEAD;

    iface_attr->ep_addr_len = sizeof(uct_ionic_rc_ep_addr_t);

    /* Drop ALL event flags: ionic does not support comp_channel-based
     * notifications and ibv_req_notify_cq()-style arming. UCP would otherwise
     * call event_fd_get / iface_event_arm and fail. The iface still works
     * via active polling. */
    iface_attr->cap.event_flags = 0;

    /* Drop AM capabilities: libionic+pUEC rejects SRQ-attached QPs with
     * BAD_ATTR, and we don't yet implement per-EP QP recv WRs. Without a
     * working recv path we can't consume AM messages. UCP will pick a
     * different transport (e.g. tcp) for the AM lane.
     *
     * Drop GET (RDMA READ) capabilities: libionic+pUEC supports only
     * RDMA WRITE (like amd-anp/NCCL). UCP would otherwise pick rndv/get/zcopy
     * which fails with "Unsupported operation".
     *
     * Keep PUT (RDMA WRITE) — that's what ionic_rc is for. */
    iface_attr->cap.flags        &= ~(UCT_IFACE_FLAG_AM_SHORT |
                                      UCT_IFACE_FLAG_AM_BCOPY |
                                      UCT_IFACE_FLAG_AM_ZCOPY |
                                      UCT_IFACE_FLAG_GET_SHORT |
                                      UCT_IFACE_FLAG_GET_BCOPY |
                                      UCT_IFACE_FLAG_GET_ZCOPY |
                                      UCT_IFACE_FLAG_ATOMIC_CPU |
                                      UCT_IFACE_FLAG_ATOMIC_DEVICE);
    iface_attr->cap.am.max_short  = 0;
    iface_attr->cap.am.max_bcopy  = 0;
    iface_attr->cap.am.max_zcopy  = 0;
    iface_attr->cap.get.max_short = 0;
    iface_attr->cap.get.max_bcopy = 0;
    iface_attr->cap.get.max_zcopy = 0;

    ucs_info("ionic_rc: iface_query OK caps.flags=0x%lx event_flags=0x%lx "
             "put.max_short=%zu put.max_bcopy=%zu put.max_zcopy=%zu "
             "am.max_short=%zu am.max_bcopy=%zu am.max_zcopy=%zu "
             "ep_addr_len=%zu dev_num_paths=%u max_num_eps=%zu "
             "iface_addr_len=%zu device_addr_len=%zu",
             iface_attr->cap.flags, iface_attr->cap.event_flags,
             iface_attr->cap.put.max_short, iface_attr->cap.put.max_bcopy,
             iface_attr->cap.put.max_zcopy, iface_attr->cap.am.max_short,
             iface_attr->cap.am.max_bcopy, iface_attr->cap.am.max_zcopy,
             iface_attr->ep_addr_len,
             (unsigned)iface_attr->dev_num_paths,
             (size_t)iface_attr->max_num_eps,
             iface_attr->iface_addr_len, iface_attr->device_addr_len);

    return UCS_OK;
}

static ucs_status_t
uct_ionic_rc_iface_init_rx(uct_rc_iface_t *rc_iface,
                           const uct_rc_iface_common_config_t *config)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(rc_iface, uct_ionic_rc_iface_t);
    ucs_status_t status;

    ucs_info("ionic_rc: init_rx ENTER iface=%p", iface);
    status = uct_rc_iface_init_rx(rc_iface, config, &iface->srq);
    if (status != UCS_OK) {
        ucs_info("ionic_rc: init_rx FAILED: %s (likely SRQ creation issue)",
                 ucs_status_string(status));
        return status;
    }
    ucs_info("ionic_rc: init_rx SUCCESS srq=%p", iface->srq);
    return UCS_OK;
}

static void uct_ionic_rc_iface_cleanup_rx(uct_rc_iface_t *rc_iface)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(rc_iface, uct_ionic_rc_iface_t);

    /* TODO flush RX buffers */
    uct_ib_destroy_srq(iface->srq);
}

static void
uct_ionic_rc_iface_qp_cleanup(uct_rc_iface_qp_cleanup_ctx_t *rc_cleanup_ctx)
{
    uct_ionic_rc_iface_qp_cleanup_ctx_t *cleanup_ctx =
            ucs_derived_of(rc_cleanup_ctx, uct_ionic_rc_iface_qp_cleanup_ctx_t);
    uct_ib_destroy_qp(cleanup_ctx->qp);
}

static UCS_CLASS_INIT_FUNC(uct_ionic_rc_iface_t, uct_md_h tl_md,
                           uct_worker_h worker, const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_ionic_rc_iface_config_t *config =
                    ucs_derived_of(tl_config, uct_ionic_rc_iface_config_t);
    uct_ib_iface_config_t *ib_config    = &config->super.super.super;
    uct_ib_md_t *ib_md                  = ucs_derived_of(tl_md, uct_ib_md_t);
    uct_ib_iface_init_attr_t init_attr  = {};
    ucs_status_t status;
    uct_rc_hdr_t *hdr;

    ucs_info("ionic_rc: iface_init ENTER md=%p worker=%p dev_name=%s",
             tl_md, worker,
             (params->field_mask & UCT_IFACE_PARAM_FIELD_DEVICE) ?
                     params->mode.device.dev_name : "(none)");

    init_attr.fc_req_size           = sizeof(uct_rc_pending_req_t);
    init_attr.rx_hdr_len            = sizeof(uct_rc_hdr_t);
    /* Use IBV_QPT_RC. pUEC is activated by the libionic provider when
     * IONIC_PRIVATE_SERVICE_FORCE=1 is set in the environment. */
    init_attr.qp_type               = IBV_QPT_RC;
    init_attr.cq_len[UCT_IB_DIR_RX] = ib_config->rx.queue_len;
    init_attr.cq_len[UCT_IB_DIR_TX] = config->super.tx_cq_len;
    init_attr.seg_size              = ib_config->seg_size;
    init_attr.xport_hdr_len         = ucs_max(sizeof(uct_rc_hdr_t), UCT_IB_RETH_LEN);
    init_attr.max_rd_atomic         = IBV_DEV_ATTR(&ib_md->dev, max_qp_rd_atom);
    init_attr.tx_moderation         = config->super.tx_cq_moderation;
    init_attr.dev_name              = params->mode.device.dev_name;

    ucs_info("ionic_rc: iface_init calling rc_iface SUPER_INIT qp_type=RC "
             "cq_rx=%u cq_tx=%u seg_size=%zu max_rd_atomic=%u",
             init_attr.cq_len[UCT_IB_DIR_RX], init_attr.cq_len[UCT_IB_DIR_TX],
             init_attr.seg_size, init_attr.max_rd_atomic);

    UCS_CLASS_CALL_SUPER_INIT(uct_rc_iface_t, &uct_ionic_rc_iface_tl_ops,
                              &uct_ionic_rc_iface_ops, tl_md, worker, params,
                              &config->super.super, &init_attr);

    ucs_info("ionic_rc: iface_init SUPER_INIT returned OK iface=%p", self);

    self->config.tx_max_wr               = ucs_min(config->tx_max_wr,
                                                   self->super.config.tx_qp_len);
    self->super.config.tx_moderation     = ucs_min(self->super.config.tx_moderation,
                                                   self->config.tx_max_wr / 4);
    self->super.config.fence_mode        = (uct_rc_fence_mode_t)config->super.super.fence_mode;
    self->super.progress                 = uct_ionic_rc_iface_progress;
    self->super.super.config.sl          = uct_ib_iface_config_select_sl(ib_config);
    uct_ib_iface_set_reverse_sl(&self->super.super, ib_config);

    self->config.num_planes              = config->num_planes;
    self->config.num_paths               = config->num_paths;

    if ((config->super.super.fence_mode == UCT_RC_FENCE_MODE_WEAK) ||
        (config->super.super.fence_mode == UCT_RC_FENCE_MODE_AUTO)) {
        self->super.config.fence_mode = UCT_RC_FENCE_MODE_WEAK;
    } else if (config->super.super.fence_mode == UCT_RC_FENCE_MODE_NONE) {
        self->super.config.fence_mode = UCT_RC_FENCE_MODE_NONE;
    } else {
        ucs_error("incorrect fence value: %d", self->super.config.fence_mode);
        status = UCS_ERR_INVALID_PARAM;
        goto err;
    }

    memset(self->inl_sge, 0, sizeof(self->inl_sge));
    uct_rc_am_hdr_fill(&self->am_inl_hdr.rc_hdr, 0);

    /* Configuration */
    self->config.short_desc_size = ucs_max(sizeof(uct_rc_hdr_t),
                                           sizeof(uct_rc_hdr_t) + 128);
    self->config.short_desc_size = ucs_max(UCT_IB_MAX_ATOMIC_SIZE,
                                           self->config.short_desc_size);

    /* Create AM headers and Atomic mempool */
    status = uct_iface_mpool_init(&self->super.super.super,
                                  &self->short_desc_mp,
                                  sizeof(uct_rc_iface_send_desc_t) +
                                      self->config.short_desc_size,
                                  sizeof(uct_rc_iface_send_desc_t),
                                  UCS_SYS_CACHE_LINE_SIZE,
                                  &ib_config->tx.mp,
                                  self->super.config.tx_qp_len,
                                  uct_rc_iface_send_desc_init,
                                  "ionic_rc_short_desc");
    if (status != UCS_OK) {
        goto err;
    }

    uct_ionic_rc_iface_init_inl_wrs(self);

    /* Check FC parameters correctness */
    status = uct_rc_init_fc_thresh(&config->super, &self->super);
    if (status != UCS_OK) {
        goto err_common_cleanup;
    }

    /* Cannot create a standard probe QP because ionic requires special
     * sq_sig_all bits (set in EP init). Use device attributes for max_send_sge.
     * Set max_inline to 224 (same as rc_verbs gets on ionic after QP creation).
     * The device actually supports up to 4096 bytes inline but the QP cap
     * reports a negotiated value after creation. */
    self->config.max_inline   = 224;
    self->config.max_send_sge = ucs_min(UCT_IB_MAX_IOV,
                                        (size_t)IBV_DEV_ATTR(&ib_md->dev, max_sge));

    if (self->config.max_inline < sizeof(*hdr)) {
        self->fc_desc = ucs_mpool_get(&self->short_desc_mp);
        ucs_assert_always(self->fc_desc != NULL);
        hdr        = (uct_rc_hdr_t*)(self->fc_desc + 1);
        hdr->am_id = UCT_RC_EP_FC_PURE_GRANT;
    } else {
        self->fc_desc = NULL;
    }

    ucs_info("ionic_rc: iface_init COMPLETE iface=%p num_planes=%u num_paths=%u "
             "max_inline=%zu max_send_sge=%zu tx_max_wr=%u tx_qp_len=%u "
             "fence_mode=%d sl=%u srq=%p",
             self, self->config.num_planes, self->config.num_paths,
             self->config.max_inline, self->config.max_send_sge,
             self->config.tx_max_wr, self->super.config.tx_qp_len,
             self->super.config.fence_mode, self->super.super.config.sl,
             self->srq);

    return UCS_OK;

err_common_cleanup:
    ucs_info("ionic_rc: iface_init FAILED at fc_thresh check: %s",
             ucs_status_string(status));
    ucs_mpool_cleanup(&self->short_desc_mp, 1);
err:
    ucs_info("ionic_rc: iface_init FAILED status=%s", ucs_status_string(status));
    return status;
}


ucs_status_t
uct_ionic_rc_iface_common_prepost_recvs(uct_ionic_rc_iface_t *iface)
{
    if (iface->super.rx.srq.quota == 0) {
        return UCS_OK;
    }

    iface->super.rx.srq.available = iface->super.rx.srq.quota;
    iface->super.rx.srq.quota     = 0;
    while (iface->super.rx.srq.available > 0) {
        if (uct_ionic_rc_iface_post_recv_common(iface, 1) == 0) {
            ucs_error("failed to post receives");
            return UCS_ERR_NO_MEMORY;
        }
    }

    return UCS_OK;
}

void uct_ionic_rc_iface_common_progress_enable(uct_iface_h tl_iface, unsigned flags)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_iface, uct_ionic_rc_iface_t);

    if (flags & UCT_PROGRESS_RECV) {
        uct_ionic_rc_iface_common_prepost_recvs(iface);
    }

    uct_base_iface_progress_enable_cb(&iface->super.super.super,
                                      iface->super.progress,
                                      flags);
}

unsigned uct_ionic_rc_iface_post_recv_always(uct_ionic_rc_iface_t *iface, unsigned max)
{
    struct ibv_recv_wr *bad_wr;
    uct_ib_recv_wr_t *wrs;
    unsigned count;
    int ret;

    wrs  = ucs_alloca(sizeof *wrs  * max);

    count = uct_ib_iface_prepare_rx_wrs(&iface->super.super, &iface->super.rx.mp,
                                        wrs, max);
    if (ucs_unlikely(count == 0)) {
        return 0;
    }

    ret = ibv_post_srq_recv(iface->srq, &wrs[0].ibwr, &bad_wr);
    if (ret != 0) {
        ucs_fatal("ibv_post_srq_recv() returned %d: %m", ret);
    }
    iface->super.rx.srq.available -= count;

    return count;
}

static UCS_CLASS_CLEANUP_FUNC(uct_ionic_rc_iface_t)
{
    uct_base_iface_progress_disable(&self->super.super.super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);

    uct_rc_iface_cleanup_qps(&self->super);

    if (self->fc_desc != NULL) {
        ucs_mpool_put(self->fc_desc);
    }
    ucs_mpool_cleanup(&self->short_desc_mp, 1);
}

UCS_CLASS_DEFINE(uct_ionic_rc_iface_t, uct_rc_iface_t);
static UCS_CLASS_DEFINE_NEW_FUNC(uct_ionic_rc_iface_t, uct_iface_t, uct_md_h,
                                 uct_worker_h, const uct_iface_params_t*,
                                 const uct_iface_config_t*);
static UCS_CLASS_DEFINE_DELETE_FUNC(uct_ionic_rc_iface_t, uct_iface_t);


static uct_iface_ops_t uct_ionic_rc_iface_tl_ops = {
    .ep_am_short              = uct_ionic_rc_ep_am_short,
    .ep_am_short_iov          = uct_ionic_rc_ep_am_short_iov,
    .ep_am_bcopy              = uct_ionic_rc_ep_am_bcopy,
    .ep_am_zcopy              = uct_ionic_rc_ep_am_zcopy,
    .ep_put_short             = uct_ionic_rc_ep_put_short,
    .ep_put_bcopy             = uct_ionic_rc_ep_put_bcopy,
    .ep_put_zcopy             = uct_ionic_rc_ep_put_zcopy,
    .ep_get_bcopy             = (uct_ep_get_bcopy_func_t)ucs_empty_function_return_unsupported,
    .ep_get_zcopy             = (uct_ep_get_zcopy_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic_cswap64        = (uct_ep_atomic_cswap64_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic64_post         = (uct_ep_atomic64_post_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic64_fetch        = (uct_ep_atomic64_fetch_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic_cswap32        = (uct_ep_atomic_cswap32_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic32_post         = (uct_ep_atomic32_post_func_t)ucs_empty_function_return_unsupported,
    .ep_atomic32_fetch        = (uct_ep_atomic32_fetch_func_t)ucs_empty_function_return_unsupported,
    .ep_pending_add           = uct_rc_ep_pending_add,
    .ep_pending_purge         = uct_rc_ep_pending_purge,
    .ep_flush                 = uct_ionic_rc_ep_flush,
    .ep_fence                 = uct_ionic_rc_ep_fence,
    .ep_check                 = uct_rc_ep_check,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_ionic_rc_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_ionic_rc_ep_t),
    .ep_get_address           = uct_ionic_rc_ep_get_address,
    .ep_connect_to_ep         = uct_base_ep_connect_to_ep,
    .iface_flush              = uct_rc_iface_flush,
    .iface_fence              = uct_rc_iface_fence,
    .iface_progress_enable    = uct_ionic_rc_iface_common_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_rc_iface_do_progress,
    .iface_event_fd_get       = uct_ib_iface_event_fd_get,
    .iface_event_arm          = uct_rc_iface_event_arm,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_ionic_rc_iface_t),
    .iface_query              = uct_ionic_rc_iface_query,
    .iface_get_address        = (uct_iface_get_address_func_t)ucs_empty_function_return_success,
    .iface_get_device_address = uct_ib_iface_get_device_address,
    .iface_is_reachable       = uct_base_iface_is_reachable,
};

static uct_rc_iface_ops_t uct_ionic_rc_iface_ops = {
    .super = {
        .super = {
            .iface_query_v2         = uct_iface_base_query_v2,
            .iface_estimate_perf    = uct_rc_iface_estimate_perf,
            .iface_vfs_refresh      = uct_rc_iface_vfs_refresh,
            .ep_query               = (uct_ep_query_func_t)ucs_empty_function_return_unsupported,
            .ep_invalidate          = (uct_ep_invalidate_func_t)ucs_empty_function_return_unsupported,
            .ep_connect_to_ep_v2    = uct_ionic_rc_ep_connect_to_ep_v2,
            .iface_is_reachable_v2  = uct_ib_iface_is_reachable_v2,
            .ep_is_connected        = uct_ionic_rc_ep_is_connected,
            .ep_get_device_ep       = (uct_ep_get_device_ep_func_t)ucs_empty_function_return_unsupported
        },
        .create_cq      = uct_ib_verbs_create_cq,
        .destroy_cq     = uct_ib_verbs_destroy_cq,
        .event_cq       = (uct_ib_iface_event_cq_func_t)ucs_empty_function,
        .handle_failure  = uct_ionic_rc_handle_failure,
    },
    .init_rx         = uct_ionic_rc_iface_init_rx,
    .cleanup_rx      = uct_ionic_rc_iface_cleanup_rx,
    .fc_ctrl         = uct_ionic_rc_ep_fc_ctrl,
    .fc_handler      = uct_rc_iface_fc_handler,
    .cleanup_qp      = uct_ionic_rc_iface_qp_cleanup,
    .ep_post_check   = uct_ionic_rc_ep_post_check,
    .ep_vfs_populate = uct_ionic_rc_ep_vfs_populate
};


static ucs_status_t
uct_ionic_rc_query_tl_devices(uct_md_h md,
                              uct_tl_device_resource_t **tl_devices_p,
                              unsigned *num_tl_devices_p)
{
    uct_ib_md_t *ib_md = ucs_derived_of(md, uct_ib_md_t);
    ucs_status_t status;
    unsigned i;

    ucs_info("ionic_rc: query_tl_devices md=%p name=%s", md, ib_md->name);

    status = uct_ib_device_query_ports(&ib_md->dev, UCT_IB_DEVICE_FLAG_SRQ,
                                       tl_devices_p, num_tl_devices_p);
    if (status != UCS_OK) {
        ucs_info("ionic_rc: query_tl_devices FAILED: %s",
                 ucs_status_string(status));
        return status;
    }

    ucs_info("ionic_rc: query_tl_devices returned %u devices",
             *num_tl_devices_p);
    for (i = 0; i < *num_tl_devices_p; i++) {
        ucs_info("ionic_rc:   tl_device[%u]: name=%s type=%d sys_dev=%d", i,
                 (*tl_devices_p)[i].name, (*tl_devices_p)[i].type,
                 (*tl_devices_p)[i].sys_device);
    }

    return UCS_OK;
}

UCT_TL_DEFINE_ENTRY(&uct_ib_component, ionic_rc,
                    uct_ionic_rc_query_tl_devices,
                    uct_ionic_rc_iface_t, "IONIC_RC_",
                    uct_ionic_rc_iface_config_table,
                    uct_ionic_rc_iface_config_t);
