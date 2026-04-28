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

#include <ucs/vfs/base/vfs_cb.h>
#include <ucs/vfs/base/vfs_obj.h>
#include <ucs/arch/bitops.h>
#include <uct/ib/base/ib_log.h>


void uct_ionic_rc_txcnt_init(uct_ionic_rc_txcnt_t *txcnt)
{
    txcnt->pi = txcnt->ci = 0;
}


static UCS_F_ALWAYS_INLINE void
uct_ionic_rc_ep_fence_put(uct_ionic_rc_iface_t *iface, uct_ionic_rc_ep_t *ep,
                          uct_rkey_t *rkey, uint64_t *addr)
{
    uct_rc_ep_fence_put(&iface->super, &ep->fi, rkey, addr,
                        ep->super.atomic_mr_offset);
}

static UCS_F_ALWAYS_INLINE void
uct_ionic_rc_ep_post_send(uct_ionic_rc_iface_t *iface, uct_ionic_rc_ep_t *ep,
                          struct ibv_send_wr *wr, int send_flags, int max_log_sge)
{
    static int post_send_first_log = 0;
    struct ibv_send_wr *bad_wr;
    int ret;

    if (!post_send_first_log) {
        post_send_first_log = 1;
        ucs_info("ionic_rc: FIRST post_send qpn=0x%x opcode=%d send_flags=0x%x "
                 "(ionic_rc IS being used for data!)",
                 ep->qp->qp_num, wr->opcode, send_flags);
    }

    ucs_assertv(ep->qp->state == IBV_QPS_RTS, "QP 0x%x state is %d",
                ep->qp->qp_num, ep->qp->state);

    if (!(send_flags & IBV_SEND_SIGNALED)) {
        send_flags |= uct_rc_iface_tx_moderation(&iface->super, &ep->super.txqp,
                                                 IBV_SEND_SIGNALED);
    }
    if (wr->opcode == IBV_WR_RDMA_READ) {
        send_flags |= uct_rc_ep_fm(&iface->super, &ep->fi, IBV_SEND_FENCE);
    }

    wr->send_flags = send_flags;
    wr->wr_id      = ep->txcnt.pi + 1;

    uct_ib_log_post_send(&iface->super.super, ep->qp, wr, max_log_sge,
                         (wr->opcode == IBV_WR_SEND) ? uct_rc_ep_packet_dump : NULL);

    ret = ibv_post_send(ep->qp, wr, &bad_wr);
    if (ret != 0) {
        ucs_fatal("ibv_post_send() returned %d (%m)", ret);
    }

    uct_ionic_rc_txqp_posted(&ep->super.txqp, &ep->txcnt, &iface->super,
                             send_flags & IBV_SEND_SIGNALED);
}

/*
 * Helper function for posting sends with a descriptor.
 */
static UCS_F_ALWAYS_INLINE void
uct_ionic_rc_ep_post_send_desc(uct_ionic_rc_ep_t *ep, struct ibv_send_wr *wr,
                               uct_rc_iface_send_desc_t *desc, int send_flags,
                               int max_log_sge)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(ep->super.super.super.iface,
                                                 uct_ionic_rc_iface_t);
    UCT_IONIC_RC_FILL_DESC_WR(wr, desc);
    uct_ionic_rc_ep_post_send(iface, ep, wr, send_flags, max_log_sge);
    uct_rc_txqp_add_send_op_sn(&ep->super.txqp, &desc->super, ep->txcnt.pi);
}

static inline ucs_status_t
uct_ionic_rc_ep_rdma_zcopy(uct_ionic_rc_ep_t *ep, const uct_iov_t *iov,
                           size_t iovcnt, size_t iov_total_length,
                           uint64_t remote_addr, uint32_t rkey,
                           uct_completion_t *comp,
                           uct_rc_send_handler_t handler, uint16_t op_flags,
                           int opcode)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(ep->super.super.super.iface,
                                                 uct_ionic_rc_iface_t);
    struct ibv_sge sge[UCT_IB_MAX_IOV];
    struct ibv_send_wr wr;
    size_t sge_cnt;

    ucs_assertv(iovcnt <= ucs_min(UCT_IB_MAX_IOV, iface->config.max_send_sge),
                "iovcnt %zu, maxcnt (%zu, %zu)",
                iovcnt, UCT_IB_MAX_IOV, iface->config.max_send_sge);

    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    sge_cnt = uct_ib_verbs_sge_fill_iov(sge, iov, iovcnt);
    /* cppcheck-suppress syntaxError */
    UCT_SKIP_ZERO_LENGTH(sge_cnt);
    UCT_IONIC_RC_FILL_RDMA_WR_IOV(wr, wr.opcode, (enum ibv_wr_opcode)opcode,
                                  sge, sge_cnt, remote_addr, rkey);
    wr.next = NULL;

    uct_ionic_rc_ep_post_send(iface, ep, &wr, IBV_SEND_SIGNALED, INT_MAX);
    uct_rc_txqp_add_send_comp(&iface->super, &ep->super.txqp, handler, comp,
                              ep->txcnt.pi,
                              op_flags | UCT_RC_IFACE_SEND_OP_FLAG_ZCOPY,
                              iov, iovcnt, iov_total_length);
    return UCS_INPROGRESS;
}


ucs_status_t uct_ionic_rc_ep_put_short(uct_ep_h tl_ep, const void *buffer,
                                       unsigned length, uint64_t remote_addr,
                                       uct_rkey_t rkey)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ionic_rc_iface_t);
    uct_ionic_rc_ep_t *ep       = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);

    UCT_CHECK_LENGTH(length, 0, iface->config.max_inline, "put_short");

    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    uct_ionic_rc_ep_fence_put(iface, ep, &rkey, &remote_addr);
    UCT_IONIC_RC_FILL_INL_PUT_WR(iface, remote_addr, rkey, buffer, length);
    UCT_TL_EP_STAT_OP(&ep->super.super, PUT, SHORT, length);
    uct_ionic_rc_ep_post_send(iface, ep, &iface->inl_rwrite_wr,
                              IBV_SEND_INLINE | IBV_SEND_SIGNALED, INT_MAX);
    uct_rc_ep_enable_flush_remote(&ep->super);
    return UCS_OK;
}

ssize_t uct_ionic_rc_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                                  void *arg, uint64_t remote_addr, uct_rkey_t rkey)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ionic_rc_iface_t);
    uct_ionic_rc_ep_t *ep       = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);
    uct_rc_iface_send_desc_t *desc;
    struct ibv_send_wr wr;
    struct ibv_sge sge;
    size_t length;

    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    UCT_RC_IFACE_GET_TX_PUT_BCOPY_DESC(&iface->super, &iface->super.tx.mp, desc,
                                       pack_cb, arg, length);
    uct_ionic_rc_ep_fence_put(iface, ep, &rkey, &remote_addr);
    UCT_IONIC_RC_FILL_RDMA_WR(wr, wr.opcode, IBV_WR_RDMA_WRITE, sge,
                              length, remote_addr, rkey);
    UCT_TL_EP_STAT_OP(&ep->super.super, PUT, BCOPY, length);
    uct_ionic_rc_ep_post_send_desc(ep, &wr, desc, IBV_SEND_SIGNALED, INT_MAX);
    uct_rc_ep_enable_flush_remote(&ep->super);
    return length;
}

ucs_status_t uct_ionic_rc_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                                       size_t iovcnt, uint64_t remote_addr,
                                       uct_rkey_t rkey, uct_completion_t *comp)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                 uct_ionic_rc_iface_t);
    uct_ionic_rc_ep_t *ep       = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);
    ucs_status_t status;

    UCT_CHECK_IOV_SIZE(iovcnt, iface->config.max_send_sge,
                       "uct_ionic_rc_ep_put_zcopy");
    uct_ionic_rc_ep_fence_put(iface, ep, &rkey, &remote_addr);
    status = uct_ionic_rc_ep_rdma_zcopy(ep, iov, iovcnt, 0ul, remote_addr, rkey,
                                        comp, uct_rc_ep_send_op_completion_handler,
                                        0, IBV_WR_RDMA_WRITE);
    UCT_TL_EP_STAT_OP_IF_SUCCESS(status, &ep->super.super, PUT, ZCOPY,
                                 uct_iov_total_length(iov, iovcnt));
    uct_rc_ep_enable_flush_remote(&ep->super);
    return status;
}


ucs_status_t uct_ionic_rc_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t hdr,
                                      const void *buffer, unsigned length)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ionic_rc_iface_t);
    uct_ionic_rc_ep_t *ep       = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);

    UCT_RC_CHECK_AM_SHORT(id, length, uct_rc_am_short_hdr_t, iface->config.max_inline);
    UCT_RC_CHECK_RES_AND_FC(&iface->super, &ep->super, id);
    uct_ionic_rc_iface_fill_inl_am_sge(iface, id, hdr, buffer, length);
    UCT_TL_EP_STAT_OP(&ep->super.super, AM, SHORT, sizeof(hdr) + length);
    uct_ionic_rc_ep_post_send(iface, ep, &iface->inl_am_wr,
                              IBV_SEND_INLINE | IBV_SEND_SOLICITED, INT_MAX);
    UCT_RC_UPDATE_FC(&ep->super, id);

    return UCS_OK;
}

ucs_status_t uct_ionic_rc_ep_am_short_iov(uct_ep_h tl_ep, uint8_t id,
                                          const uct_iov_t *iov, size_t iovcnt)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ionic_rc_iface_t);
    uct_ionic_rc_ep_t *ep       = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);

    UCT_RC_CHECK_AM_SHORT(id, uct_iov_total_length(iov, iovcnt), uct_rc_hdr_t,
                          iface->config.max_inline);
    UCT_RC_CHECK_RES_AND_FC(&iface->super, &ep->super, id);
    UCT_CHECK_IOV_SIZE(iovcnt, UCT_IB_MAX_IOV - 1, "uct_ionic_rc_ep_am_short_iov");
    uct_ionic_rc_iface_fill_inl_am_sge_iov(iface, id, iov, iovcnt);
    UCT_TL_EP_STAT_OP(&ep->super.super, AM, SHORT, uct_iov_total_length(iov, iovcnt));
    uct_ionic_rc_ep_post_send(iface, ep, &iface->inl_am_wr,
                              IBV_SEND_INLINE | IBV_SEND_SOLICITED, INT_MAX);
    UCT_RC_UPDATE_FC(&ep->super, id);

    return UCS_OK;
}

ssize_t uct_ionic_rc_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                                 uct_pack_callback_t pack_cb, void *arg,
                                 unsigned flags)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ionic_rc_iface_t);
    uct_ionic_rc_ep_t *ep       = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);
    uct_rc_iface_send_desc_t *desc;
    struct ibv_send_wr wr;
    struct ibv_sge sge;
    size_t length;

    UCT_CHECK_AM_ID(id);

    UCT_RC_CHECK_RES_AND_FC(&iface->super, &ep->super, id);
    UCT_RC_IFACE_GET_TX_AM_BCOPY_DESC(&iface->super, &iface->super.tx.mp, desc,
                                      id, uct_rc_am_hdr_fill, uct_rc_hdr_t,
                                      pack_cb, arg, &length);
    UCT_IONIC_RC_FILL_AM_BCOPY_WR(wr, sge, length + sizeof(uct_rc_hdr_t),
                                  wr.opcode);
    UCT_TL_EP_STAT_OP(&ep->super.super, AM, BCOPY, length);
    uct_ionic_rc_ep_post_send_desc(ep, &wr, desc, IBV_SEND_SOLICITED, INT_MAX);
    UCT_RC_UPDATE_FC(&ep->super, id);

    return length;
}

ucs_status_t uct_ionic_rc_ep_am_zcopy(uct_ep_h tl_ep, uint8_t id,
                                      const void *header,
                                      unsigned header_length,
                                      const uct_iov_t *iov,
                                      size_t iovcnt, unsigned flags,
                                      uct_completion_t *comp)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ionic_rc_iface_t);
    uct_ionic_rc_ep_t *ep       = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);
    uct_rc_iface_send_desc_t *desc  = NULL;
    struct ibv_sge sge[UCT_IB_MAX_IOV]; /* First sge is reserved for the header */
    struct ibv_send_wr wr;
    int send_flags;
    size_t sge_cnt;

    /* 1 iov consumed by am header */
    UCT_CHECK_IOV_SIZE(iovcnt, iface->config.max_send_sge - 1,
                       "uct_ionic_rc_ep_am_zcopy");
    UCT_RC_CHECK_AM_ZCOPY(id, header_length, uct_iov_total_length(iov, iovcnt),
                          iface->config.short_desc_size,
                          iface->super.super.config.seg_size);
    UCT_RC_CHECK_RES_AND_FC(&iface->super, &ep->super, id);

    UCT_RC_IFACE_GET_TX_AM_ZCOPY_DESC(&iface->super, &iface->short_desc_mp,
                                      desc, id, header, header_length, comp,
                                      &send_flags);
    sge[0].length = sizeof(uct_rc_hdr_t) + header_length;
    sge_cnt = uct_ib_verbs_sge_fill_iov(sge + 1, iov, iovcnt);
    UCT_IONIC_RC_FILL_AM_ZCOPY_WR_IOV(wr, sge, (sge_cnt + 1), wr.opcode);
    UCT_TL_EP_STAT_OP(&ep->super.super, AM, ZCOPY,
                      (header_length + uct_iov_total_length(iov, iovcnt)));

    uct_ionic_rc_ep_post_send_desc(ep, &wr, desc, send_flags | IBV_SEND_SOLICITED,
                                   UCT_IB_MAX_ZCOPY_LOG_SGE(&iface->super.super));
    UCT_RC_UPDATE_FC(&ep->super, id);

    return UCS_INPROGRESS;
}


static void uct_ionic_rc_ep_post_flush(uct_ionic_rc_ep_t *ep, int send_flags)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(ep->super.super.super.iface,
                                                 uct_ionic_rc_iface_t);
    struct ibv_send_wr wr;
    struct ibv_sge sge;
    int inl_flag;

    if (iface->config.max_inline == 0) {
        /* Flush by flow control pure grant if inline not supported */
        sge.addr   = (uintptr_t)(iface->fc_desc + 1);
        sge.length = sizeof(uct_rc_hdr_t);
        sge.lkey   = iface->fc_desc->lkey;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode  = IBV_WR_SEND;
        inl_flag   = 0;
    } else {
        /* Flush by empty RDMA_WRITE */
        wr.sg_list             = NULL;
        wr.num_sge             = 0;
        wr.opcode              = IBV_WR_RDMA_WRITE;
        wr.wr.rdma.remote_addr = 0;
        wr.wr.rdma.rkey        = 0;
        inl_flag               = IBV_SEND_INLINE;
    }
    wr.next = NULL;

    uct_ionic_rc_ep_post_send(iface, ep, &wr, inl_flag | send_flags, 1);
}


ucs_status_t uct_ionic_rc_ep_flush(uct_ep_h tl_ep, unsigned flags,
                                   uct_completion_t *comp)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ionic_rc_iface_t);
    uct_ionic_rc_ep_t *ep       = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);
    int already_canceled        = ep->super.flags & UCT_RC_EP_FLAG_FLUSH_CANCEL;
    ucs_status_t status;

    UCT_CHECK_PARAM(!ucs_test_all_flags(flags, UCT_FLUSH_FLAG_CANCEL |
                                               UCT_FLUSH_FLAG_REMOTE),
                    "flush flags CANCEL and REMOTE are mutually exclusive");

    status = uct_rc_ep_flush(&ep->super, iface->config.tx_max_wr, flags);
    if (status != UCS_INPROGRESS) {
        return status;
    }

    if (uct_rc_txqp_unsignaled(&ep->super.txqp) != 0) {
        UCT_RC_CHECK_RES(&iface->super, &ep->super);
        uct_ionic_rc_ep_post_flush(ep, IBV_SEND_SIGNALED);
    }

    if (ucs_unlikely((flags & UCT_FLUSH_FLAG_CANCEL) && !already_canceled)) {
        status = uct_ib_modify_qp(ep->qp, IBV_QPS_ERR);
        if (status != UCS_OK) {
            return status;
        }
    }

    return uct_rc_txqp_add_flush_comp(&iface->super, &ep->super.super,
                                      &ep->super.txqp, comp, ep->txcnt.pi);
}

ucs_status_t uct_ionic_rc_ep_fence(uct_ep_h tl_ep, unsigned flags)
{
    uct_ionic_rc_ep_t *ep = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);

    return uct_rc_ep_fence(tl_ep, &ep->fi);
}

void uct_ionic_rc_ep_post_check(uct_ep_h tl_ep)
{
    uct_ionic_rc_ep_t *ep = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);

    uct_ionic_rc_ep_post_flush(ep, 0);
}

void uct_ionic_rc_ep_vfs_populate(uct_rc_ep_t *rc_ep)
{
    uct_rc_iface_t *rc_iface = ucs_derived_of(rc_ep->super.super.iface,
                                              uct_rc_iface_t);
    uct_ionic_rc_ep_t *ep    = ucs_derived_of(rc_ep, uct_ionic_rc_ep_t);

    ucs_vfs_obj_add_dir(rc_iface, ep, "ep/%p", ep);
    ucs_vfs_obj_add_ro_file(ep, ucs_vfs_show_primitive, &ep->qp->qp_num,
                            UCS_VFS_TYPE_U32_HEX, "qp_num");
    uct_rc_txqp_vfs_populate(&ep->super.txqp, ep);
}

ucs_status_t uct_ionic_rc_ep_fc_ctrl(uct_ep_t *tl_ep, unsigned op,
                                     uct_rc_pending_req_t *req)
{
    struct ibv_send_wr fc_wr;
    uct_ionic_rc_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                 uct_ionic_rc_iface_t);
    uct_ionic_rc_ep_t *ep = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);
    uct_rc_hdr_t *hdr;
    struct ibv_sge sge;
    int flags;

    if (!iface->fc_desc) {
        hdr                      = &iface->am_inl_hdr.rc_hdr;
        hdr->am_id               = UCT_RC_EP_FC_PURE_GRANT;
        fc_wr.sg_list            = iface->inl_sge;
        iface->inl_sge[0].addr   = (uintptr_t)hdr;
        iface->inl_sge[0].length = sizeof(*hdr);
        flags                    = IBV_SEND_INLINE;
    } else {
        hdr           = (uct_rc_hdr_t*)(iface->fc_desc + 1);
        sge.addr      = (uintptr_t)hdr;
        sge.length    = sizeof(*hdr);
        sge.lkey      = iface->fc_desc->lkey;
        fc_wr.sg_list = &sge;
        flags         = 0;
    }

    /* In RC only PURE grant is sent as a separate message. Other FC
     * messages are bundled with AM. */
    ucs_assert(op == UCT_RC_EP_FC_PURE_GRANT);

    /* Do not check FC WND here to avoid head-to-head deadlock.
     * Credits grant should be sent regardless of FC wnd state. */
    UCT_RC_CHECK_TX_CQ_RES(&iface->super, &ep->super);

    fc_wr.opcode  = IBV_WR_SEND;
    fc_wr.next    = NULL;
    fc_wr.num_sge = 1;

    uct_ionic_rc_ep_post_send(iface, ep, &fc_wr, flags, INT_MAX);
    return UCS_OK;
}

ucs_status_t uct_ionic_rc_ep_get_address(uct_ep_h tl_ep, uct_ep_addr_t *addr)
{
    uct_ionic_rc_ep_t *ep                 = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);
    uct_ionic_rc_ep_addr_t *rc_addr       = (uct_ionic_rc_ep_addr_t *)addr;

    uct_ib_pack_uint24(rc_addr->qp_num, ep->qp->qp_num);
    return UCS_OK;
}

int uct_ionic_rc_ep_is_connected(const uct_ep_h tl_ep,
                                 const uct_ep_is_connected_params_t *params)
{
    uct_ionic_rc_ep_t *ep = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);
    uint32_t addr_qp      = 0;
    const uct_ionic_rc_ep_addr_t *rc_addr;
    ucs_status_t status;
    struct ibv_ah_attr ah_attr;
    uint32_t qp_num;

    status = uct_ib_query_qp_peer_info(ep->qp, &ah_attr, &qp_num);
    if (status != UCS_OK) {
        return 0;
    }

    if (params->field_mask & UCT_EP_IS_CONNECTED_FIELD_EP_ADDR) {
        rc_addr = (const uct_ionic_rc_ep_addr_t*)params->ep_addr;
        addr_qp = uct_ib_unpack_uint24(rc_addr->qp_num);
    }

    return uct_rc_ep_is_connected(&ep->super, &ah_attr, params, qp_num,
                                  addr_qp);
}


/* No custom QP connect needed — pUEC is activated by the libionic provider
 * via IONIC_PRIVATE_SERVICE_FORCE=1 environment variable. Standard RC QP
 * transitions (INIT/RTR/RTS) are used; libionic internally creates MRC QPs. */


ucs_status_t
uct_ionic_rc_ep_connect_to_ep_v2(uct_ep_h tl_ep,
                                 const uct_device_addr_t *dev_addr,
                                 const uct_ep_addr_t *ep_addr,
                                 const uct_ep_connect_to_ep_params_t *param)
{
    uct_ionic_rc_ep_t *ep           = ucs_derived_of(tl_ep, uct_ionic_rc_ep_t);
    uct_ionic_rc_iface_t *iface     = ucs_derived_of(tl_ep->iface,
                                                      uct_ionic_rc_iface_t);
    const uct_ib_address_t *ib_addr = (const uct_ib_address_t *)dev_addr;
    const uct_ionic_rc_ep_addr_t *rc_addr =
            (const uct_ionic_rc_ep_addr_t *)ep_addr;
    ucs_status_t status;
    uint32_t qp_num;
    struct ibv_ah_attr ah_attr;
    enum ibv_mtu path_mtu;

    ucs_info("ionic_rc: connect_to_ep_v2 ep=%p local_qpn=0x%x path_index=%u",
             ep, ep->qp->qp_num, ep->super.path_index);

    status = uct_ib_iface_fill_ah_attr_from_addr(&iface->super.super, ib_addr,
                                                 ep->super.path_index, &ah_attr,
                                                 &path_mtu);
    if (status != UCS_OK) {
        ucs_error("ionic_rc: connect_to_ep_v2 fill_ah_attr_from_addr FAILED: %s",
                  ucs_status_string(status));
        return status;
    }

    ucs_assert(path_mtu != UCT_IB_ADDRESS_INVALID_PATH_MTU);

    qp_num = uct_ib_unpack_uint24(rc_addr->qp_num);
    ucs_info("ionic_rc: connect_to_ep_v2 connecting local_qpn=0x%x to "
             "remote_qpn=0x%x mtu=%d sl=%u port=%u",
             ep->qp->qp_num, qp_num, path_mtu, ah_attr.sl, ah_attr.port_num);

    status = uct_rc_iface_qp_connect(&iface->super, ep->qp, qp_num, &ah_attr,
                                     path_mtu);
    if (status != UCS_OK) {
        ucs_error("ionic_rc: connect_to_ep_v2 uct_rc_iface_qp_connect FAILED: "
                  "%s (local_qpn=0x%x remote_qpn=0x%x)",
                  ucs_status_string(status), ep->qp->qp_num, qp_num);
        return status;
    }

    ep->super.atomic_mr_offset = 0;
    ep->super.flush_rkey       = UCT_IB_MD_INVALID_FLUSH_RKEY;

    ep->super.flags |= UCT_RC_EP_FLAG_CONNECTED;
    ucs_info("ionic_rc: connect_to_ep_v2 SUCCESS local_qpn=0x%x remote_qpn=0x%x",
             ep->qp->qp_num, qp_num);
    return UCS_OK;
}


UCS_CLASS_INIT_FUNC(uct_ionic_rc_ep_t, const uct_ep_params_t *params)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(params->iface, uct_ionic_rc_iface_t);
    uct_ib_md_t *md             = uct_ib_iface_md(&iface->super.super);
    uct_ib_qp_attr_t attr = {};
    ucs_status_t status = UCS_OK;

    ucs_info("ionic_rc: ep_init ENTER iface=%p path_index=%u",
             iface,
             (params->field_mask & UCT_EP_PARAM_FIELD_PATH_INDEX) ?
                     params->path_index : 0);

    /* Use ibv_create_qp() (not ibv_create_qp_ex) because libionic requires
     * ionic-specific bits in sq_sig_all upper bits which only work through
     * the legacy ibv_create_qp() path. */
    {
        struct ibv_qp_init_attr qp_init_attr;
        memset(&qp_init_attr, 0, sizeof(qp_init_attr));
        /* libionic + pUEC rejects SRQ-attached QPs with BAD_ATTR. amd-anp
         * also uses srq=NULL with per-QP receive queue. Use single CQ.
         * Per-QP recv WRs would need to be posted (not yet implemented). */
        qp_init_attr.send_cq             = iface->super.super.cq[UCT_IB_DIR_TX];
        qp_init_attr.recv_cq             = iface->super.super.cq[UCT_IB_DIR_TX];
        qp_init_attr.srq                 = NULL;
        qp_init_attr.qp_type             = IBV_QPT_RC;
        qp_init_attr.cap.max_send_wr     = 256;
        qp_init_attr.cap.max_recv_wr     = 128;
        qp_init_attr.cap.max_send_sge    = 1;
        qp_init_attr.cap.max_recv_sge    = 1;
        qp_init_attr.cap.max_inline_data = iface->config.max_inline;
        /* libionic uses upper bits of sq_sig_all as ionic-private flags:
         *   bit 16 = ionic marker (required to be recognized as ionic QP)
         *   bit 17 = data QP (vs control QP)
         *   bit 18 = ionic flag
         * These bits match what amd-anp/ROCm NCCL plugin uses. */
        qp_init_attr.sq_sig_all          = 0;
        qp_init_attr.sq_sig_all         |= (1 << 16);  /* ionic marker */
        qp_init_attr.sq_sig_all         |= (1 << 17);  /* data QP */
        qp_init_attr.sq_sig_all         |= (1 << 18);  /* ionic flag */

        ucs_info("ionic_rc: ep_init calling ibv_create_qp pd=%p send_cq=%p "
                 "recv_cq=%p srq=%p max_send_wr=%u max_send_sge=%u "
                 "max_inline=%u sq_sig_all=0x%x",
                 uct_ib_iface_md(&iface->super.super)->pd,
                 qp_init_attr.send_cq, qp_init_attr.recv_cq,
                 qp_init_attr.srq, qp_init_attr.cap.max_send_wr,
                 qp_init_attr.cap.max_send_sge,
                 qp_init_attr.cap.max_inline_data, qp_init_attr.sq_sig_all);

        self->qp = ibv_create_qp(uct_ib_iface_md(&iface->super.super)->pd,
                                 &qp_init_attr);
        if (self->qp == NULL) {
            ucs_error("ionic_rc: ibv_create_qp() failed: %m");
            status = UCS_ERR_IO_ERROR;
            goto err;
        }

        attr.cap.max_send_wr     = qp_init_attr.cap.max_send_wr;
        attr.cap.max_send_sge    = qp_init_attr.cap.max_send_sge;
        attr.cap.max_inline_data = qp_init_attr.cap.max_inline_data;
        attr.cap.max_recv_wr     = qp_init_attr.cap.max_recv_wr;
        attr.cap.max_recv_sge    = qp_init_attr.cap.max_recv_sge;

        ucs_info("ionic_rc: ep_init created QP 0x%x on %s max_send_wr=%d "
                 "max_send_sge=%d max_inline=%d",
                 self->qp->qp_num,
                 uct_ib_device_name(&uct_ib_iface_md(&iface->super.super)->dev),
                 attr.cap.max_send_wr, attr.cap.max_send_sge,
                 attr.cap.max_inline_data);
    }
    if (status != UCS_OK) {
        goto err;
    }

    UCS_CLASS_CALL_SUPER_INIT(uct_rc_ep_t, &iface->super, self->qp->qp_num,
                              params);

    ucs_info("ionic_rc: ep_init SUPER_INIT done, calling qp_init->INIT state");

    status = uct_rc_iface_qp_init(&iface->super, self->qp);
    if (status != UCS_OK) {
        ucs_error("ionic_rc: ep_init uct_rc_iface_qp_init FAILED: %s",
                  ucs_status_string(status));
        goto err_destroy_qp;
    }

    ucs_info("ionic_rc: ep_init qp_init->INIT state OK qpn=0x%x",
             self->qp->qp_num);

    status = uct_ib_device_async_event_register(&md->dev,
                                                IBV_EVENT_QP_LAST_WQE_REACHED,
                                                self->qp->qp_num);
    if (status != UCS_OK) {
        ucs_error("ionic_rc: ep_init async_event_register FAILED: %s",
                  ucs_status_string(status));
        goto err_destroy_qp;
    }

    status = uct_rc_iface_add_qp(&iface->super, &self->super, self->qp->qp_num);
    if (status != UCS_OK) {
        ucs_error("ionic_rc: ep_init add_qp FAILED: %s",
                  ucs_status_string(status));
        goto err_event_unreg;
    }

    status = uct_ionic_rc_iface_common_prepost_recvs(iface);
    if (status != UCS_OK) {
        ucs_error("ionic_rc: ep_init prepost_recvs FAILED: %s",
                  ucs_status_string(status));
        goto err_remove_qp;
    }

    uct_rc_txqp_available_set(&self->super.txqp, iface->config.tx_max_wr);
    uct_ionic_rc_txcnt_init(&self->txcnt);
    uct_ib_fence_info_init(&self->fi);

    ucs_info("ionic_rc: ep_init COMPLETE ep=%p qpn=0x%x", self, self->qp->qp_num);

    return UCS_OK;

err_remove_qp:
    uct_rc_iface_remove_qp(&iface->super, self->qp->qp_num);
err_event_unreg:
    uct_ib_device_async_event_unregister(&md->dev,
                                         IBV_EVENT_QP_LAST_WQE_REACHED,
                                         self->qp->qp_num);
err_destroy_qp:
    uct_ib_destroy_qp(self->qp);
err:
    return status;
}

UCS_CLASS_CLEANUP_FUNC(uct_ionic_rc_ep_t)
{
    uct_ionic_rc_iface_t *iface = ucs_derived_of(self->super.super.super.iface,
                                                 uct_ionic_rc_iface_t);
    uct_ionic_rc_iface_qp_cleanup_ctx_t *cleanup_ctx;

    uct_rc_txqp_purge_outstanding(&iface->super, &self->super.txqp,
                                  UCS_ERR_CANCELED, self->txcnt.pi, 1);
    uct_ib_modify_qp(self->qp, IBV_QPS_ERR);

    cleanup_ctx = ucs_malloc(sizeof(*cleanup_ctx), "ionic_qp_cleanup_ctx");
    ucs_assert_always(cleanup_ctx != NULL);
    cleanup_ctx->qp = self->qp;
    ucs_assert(UCS_CIRCULAR_COMPARE16(self->txcnt.pi, >=, self->txcnt.ci));
    uct_rc_ep_cleanup_qp(&self->super, &cleanup_ctx->super, self->qp->qp_num,
                         self->txcnt.pi - self->txcnt.ci);
}

UCS_CLASS_DEFINE(uct_ionic_rc_ep_t, uct_rc_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_ionic_rc_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_ionic_rc_ep_t, uct_ep_t);
