/**
 * Copyright (c) 2024, Advanced Micro Devices, Inc. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_IONIC_RC_H
#define UCT_IONIC_RC_H

#include <uct/ib/rc/base/rc_iface.h>
#include <uct/ib/rc/base/rc_ep.h>
#include <uct/ib/base/ib_md.h>
#include <ucs/type/class.h>


/**
 * Ionic PCI vendor ID (Pensando/AMD)
 */
#define UCT_IONIC_PCI_VENDOR_ID    0x1dd8


/**
 * Ionic RC TX producer/consumer count.
 * Same layout as uct_rc_verbs_txcnt_t but defined here to avoid
 * dependency on rc_verbs headers.
 */
typedef struct uct_ionic_rc_txcnt {
    uint16_t       pi;      /* producer (post_send) count */
    uint16_t       ci;      /* consumer (ibv_poll_cq) completion count */
} uct_ionic_rc_txcnt_t;


/**
 * Ionic RC EP address.
 */
typedef struct uct_ionic_rc_ep_addr {
    uct_ib_uint24_t  qp_num;
} UCS_S_PACKED uct_ionic_rc_ep_addr_t;


/**
 * Ionic RC interface configuration (extends uct_rc_iface_config_t).
 */
typedef struct uct_ionic_rc_iface_config {
    uct_rc_iface_config_t   super;
    unsigned                 tx_max_wr;
    unsigned                 num_planes;
    unsigned                 num_paths;
} uct_ionic_rc_iface_config_t;


/**
 * Ionic RC interface (extends uct_rc_iface_t).
 */
typedef struct uct_ionic_rc_iface {
    uct_rc_iface_t              super;
    struct ibv_srq             *srq;
    struct ibv_send_wr          inl_am_wr;
    struct ibv_send_wr          inl_rwrite_wr;
    struct ibv_sge              inl_sge[UCT_IB_MAX_IOV];
    uct_rc_am_short_hdr_t      am_inl_hdr;
    ucs_mpool_t                 short_desc_mp;
    uct_rc_iface_send_desc_t   *fc_desc;
    struct {
        size_t                  max_inline;
        size_t                  max_send_sge;
        size_t                  short_desc_size;
        unsigned                tx_max_wr;
        unsigned                num_planes;
        unsigned                num_paths;
    } config;
} uct_ionic_rc_iface_t;


/**
 * Ionic RC endpoint (extends uct_rc_ep_t).
 */
typedef struct uct_ionic_rc_ep {
    uct_rc_ep_t                 super;
    struct ibv_qp              *qp;
    uct_ionic_rc_txcnt_t        txcnt;
    uct_ib_fence_info_t         fi;
} uct_ionic_rc_ep_t;


/* Context for cleaning QP */
typedef struct {
    uct_rc_iface_qp_cleanup_ctx_t super;
    struct ibv_qp                 *qp;
} uct_ionic_rc_iface_qp_cleanup_ctx_t;


UCS_CLASS_DECLARE(uct_ionic_rc_ep_t, const uct_ep_params_t *);
UCS_CLASS_DECLARE_NEW_FUNC(uct_ionic_rc_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_ionic_rc_ep_t, uct_ep_t);

ucs_status_t uct_ionic_rc_ep_put_short(uct_ep_h tl_ep, const void *buffer,
                                       unsigned length, uint64_t remote_addr,
                                       uct_rkey_t rkey);

ssize_t uct_ionic_rc_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                                  void *arg, uint64_t remote_addr,
                                  uct_rkey_t rkey);

ucs_status_t uct_ionic_rc_ep_put_zcopy(uct_ep_h tl_ep,
                                       const uct_iov_t *iov, size_t iovcnt,
                                       uint64_t remote_addr, uct_rkey_t rkey,
                                       uct_completion_t *comp);

ucs_status_t uct_ionic_rc_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t hdr,
                                      const void *buffer, unsigned length);

ucs_status_t uct_ionic_rc_ep_am_short_iov(uct_ep_h ep, uint8_t id,
                                          const uct_iov_t *iov, size_t iovcnt);

ssize_t uct_ionic_rc_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                                 uct_pack_callback_t pack_cb, void *arg,
                                 unsigned flags);

ucs_status_t uct_ionic_rc_ep_am_zcopy(uct_ep_h tl_ep, uint8_t id,
                                      const void *header,
                                      unsigned header_length,
                                      const uct_iov_t *iov,
                                      size_t iovcnt, unsigned flags,
                                      uct_completion_t *comp);

ucs_status_t uct_ionic_rc_ep_flush(uct_ep_h tl_ep, unsigned flags,
                                   uct_completion_t *comp);

ucs_status_t uct_ionic_rc_ep_fence(uct_ep_h tl_ep, unsigned flags);

void uct_ionic_rc_ep_post_check(uct_ep_h tl_ep);

void uct_ionic_rc_ep_vfs_populate(uct_rc_ep_t *rc_ep);

ucs_status_t uct_ionic_rc_ep_fc_ctrl(uct_ep_t *tl_ep, unsigned op,
                                     uct_rc_pending_req_t *req);

ucs_status_t uct_ionic_rc_ep_get_address(uct_ep_h tl_ep, uct_ep_addr_t *addr);

int uct_ionic_rc_ep_is_connected(const uct_ep_h tl_ep,
                                 const uct_ep_is_connected_params_t *params);

ucs_status_t
uct_ionic_rc_ep_connect_to_ep_v2(uct_ep_h tl_ep,
                                 const uct_device_addr_t *dev_addr,
                                 const uct_ep_addr_t *ep_addr,
                                 const uct_ep_connect_to_ep_params_t *param);

extern ucs_config_field_t uct_ionic_rc_iface_config_table[];

#endif /* UCT_IONIC_RC_H */
