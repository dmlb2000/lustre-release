/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

/* Intramodule declarations for ptlrpc. */

#ifndef PTLRPC_INTERNAL_H
#define PTLRPC_INTERNAL_H

#include "../ldlm/ldlm_internal.h"

struct ldlm_namespace;
struct obd_import;
struct ldlm_res_id;
struct ptlrpc_request_set;
extern int test_req_buffer_pressure;
extern struct mutex ptlrpc_all_services_mutex;

int ptlrpc_start_thread(struct ptlrpc_service_part *svcpt, int wait);
/* ptlrpcd.c */
int ptlrpcd_start(int index, int max, const char *name, struct ptlrpcd_ctl *pc);

/* client.c */
struct ptlrpc_bulk_desc *new_bulk(int npages, int type, int portal);
void ptlrpc_init_xid(void);

/* events.c */
int ptlrpc_init_portals(void);
void ptlrpc_exit_portals(void);

void ptlrpc_request_handle_notconn(struct ptlrpc_request *);
void lustre_assert_wire_constants(void);
int ptlrpc_import_in_recovery(struct obd_import *imp);
int ptlrpc_set_import_discon(struct obd_import *imp, __u32 conn_cnt);
void ptlrpc_handle_failed_import(struct obd_import *imp);
int ptlrpc_replay_next(struct obd_import *imp, int *inflight);
void ptlrpc_initiate_recovery(struct obd_import *imp);

int lustre_unpack_req_ptlrpc_body(struct ptlrpc_request *req, int offset);
int lustre_unpack_rep_ptlrpc_body(struct ptlrpc_request *req, int offset);

#ifdef LPROCFS
void ptlrpc_lprocfs_register_service(struct proc_dir_entry *proc_entry,
                                     struct ptlrpc_service *svc);
void ptlrpc_lprocfs_unregister_service(struct ptlrpc_service *svc);
void ptlrpc_lprocfs_rpc_sent(struct ptlrpc_request *req, long amount);
void ptlrpc_lprocfs_do_request_stat (struct ptlrpc_request *req,
                                     long q_usec, long work_usec);
#else
#define ptlrpc_lprocfs_register_service(params...) do{}while(0)
#define ptlrpc_lprocfs_unregister_service(params...) do{}while(0)
#define ptlrpc_lprocfs_rpc_sent(params...) do{}while(0)
#define ptlrpc_lprocfs_do_request_stat(params...) do{}while(0)
#endif /* LPROCFS */

/* NRS */

/**
 * NRS core object.
 *
 * Holds NRS core fields.
 */
struct nrs_core {
	/**
	 * Protects nrs_core::nrs_heads, nrs_core::nrs_policies, serializes
	 * external policy registration/unregistration, and NRS core lprocfs
	 * operations.
	 */
	struct mutex nrs_mutex;
	/* XXX: This is just for liblustre. Remove the #if defined directive
	 * when the * "cfs_" prefix is dropped from cfs_list_head. */
#if defined (__linux__) && defined(__KERNEL__)
	/**
	 * List of all NRS heads on all service partitions of all services;
	 * protected by nrs_core::nrs_mutex.
	 */
	struct list_head nrs_heads;
	/**
	 * List of all policy descriptors registered with NRS core; protected
	 * by nrs_core::nrs_mutex.
	 */
	struct list_head nrs_policies;
#else
	struct cfs_list_head nrs_heads;
	struct cfs_list_head nrs_policies;
#endif

};

int ptlrpc_service_nrs_setup(struct ptlrpc_service *svc);
void ptlrpc_service_nrs_cleanup(struct ptlrpc_service *svc);

void ptlrpc_nrs_req_initialize(struct ptlrpc_service_part *svcpt,
			       struct ptlrpc_request *req, bool hp);
void ptlrpc_nrs_req_finalize(struct ptlrpc_request *req);
void ptlrpc_nrs_req_start_nolock(struct ptlrpc_request *req);
void ptlrpc_nrs_req_stop_nolock(struct ptlrpc_request *req);
void ptlrpc_nrs_req_add(struct ptlrpc_service_part *svcpt,
			struct ptlrpc_request *req, bool hp);
struct ptlrpc_request *
ptlrpc_nrs_req_poll_nolock(struct ptlrpc_service_part *svcpt,
						  bool hp);
void ptlrpc_nrs_req_del_nolock(struct ptlrpc_request *req);
bool ptlrpc_nrs_req_pending_nolock(struct ptlrpc_service_part *svcpt, bool hp);

int ptlrpc_nrs_policy_control(struct ptlrpc_service *svc,
			      enum ptlrpc_nrs_queue_type queue, char *name,
			      enum ptlrpc_nrs_ctl opc, bool single, void *arg);

int ptlrpc_nrs_init(void);
void ptlrpc_nrs_fini(void);

static inline int
nrs_svcpt_has_hp(struct ptlrpc_service_part *svcpt)
{
	return svcpt->scp_nrs_hp != NULL;
}

static inline int
nrs_svc_has_hp(struct ptlrpc_service *svc)
{
	/**
	 * If the first service partition has an HP NRS head, all service
	 * partitions will.
	 */
	return nrs_svcpt_has_hp(svc->srv_parts[0]);
}

static inline struct ptlrpc_nrs *
nrs_svcpt2nrs(struct ptlrpc_service_part *svcpt, bool hp)
{
	LASSERT(ergo(hp, nrs_svcpt_has_hp(svcpt)));
	return hp ? svcpt->scp_nrs_hp : &svcpt->scp_nrs_reg;
}

static inline int
nrs_pol2cptid(struct ptlrpc_nrs_policy *policy)
{
	return policy->pol_nrs->nrs_svcpt->scp_cpt;
}

static inline struct ptlrpc_service *
nrs_pol2svc(struct ptlrpc_nrs_policy *policy)
{
	return policy->pol_nrs->nrs_svcpt->scp_service;
}

static inline struct ptlrpc_service_part *
nrs_pol2svcpt(struct ptlrpc_nrs_policy *policy)
{
	return policy->pol_nrs->nrs_svcpt;
}

static inline struct cfs_cpt_table *
nrs_pol2cptab(struct ptlrpc_nrs_policy *policy)
{
	return nrs_pol2svc(policy)->srv_cptable;
}

static inline struct ptlrpc_nrs_resource *
nrs_request_resource(struct ptlrpc_nrs_request *nrq)
{
	LASSERT(nrq->nr_initialized);
	LASSERT(!nrq->nr_finalized);

	return nrq->nr_res_ptrs[nrq->nr_res_idx];
}

static inline struct ptlrpc_nrs_policy *
nrs_request_policy(struct ptlrpc_nrs_request *nrq)
{
	return nrs_request_resource(nrq)->res_policy;
}

/* recovd_thread.c */

int ptlrpc_expire_one_request(struct ptlrpc_request *req, int async_unlink);

/* pers.c */
void ptlrpc_fill_bulk_md(lnet_md_t *md, struct ptlrpc_bulk_desc *desc);
void ptlrpc_add_bulk_page(struct ptlrpc_bulk_desc *desc, cfs_page_t *page,
                          int pageoffset, int len);

/* pack_generic.c */
struct ptlrpc_reply_state *
lustre_get_emerg_rs(struct ptlrpc_service_part *svcpt);
void lustre_put_emerg_rs(struct ptlrpc_reply_state *rs);

/* pinger.c */
int ptlrpc_start_pinger(void);
int ptlrpc_stop_pinger(void);
void ptlrpc_pinger_sending_on_import(struct obd_import *imp);
void ptlrpc_pinger_commit_expected(struct obd_import *imp);
void ptlrpc_pinger_wake_up(void);
void ptlrpc_ping_import_soon(struct obd_import *imp);
#ifdef __KERNEL__
int ping_evictor_wake(struct obd_export *exp);
#else
#define ping_evictor_wake(exp)     1
#endif

/* sec_null.c */
int  sptlrpc_null_init(void);
void sptlrpc_null_fini(void);

/* sec_plain.c */
int  sptlrpc_plain_init(void);
void sptlrpc_plain_fini(void);

/* sec_bulk.c */
int  sptlrpc_enc_pool_init(void);
void sptlrpc_enc_pool_fini(void);
int sptlrpc_proc_read_enc_pool(char *page, char **start, off_t off, int count,
                               int *eof, void *data);

/* sec_lproc.c */
int  sptlrpc_lproc_init(void);
void sptlrpc_lproc_fini(void);

/* sec_gc.c */
int sptlrpc_gc_init(void);
void sptlrpc_gc_fini(void);

/* sec_config.c */
void sptlrpc_conf_choose_flavor(enum lustre_sec_part from,
                                enum lustre_sec_part to,
                                struct obd_uuid *target,
                                lnet_nid_t nid,
                                struct sptlrpc_flavor *sf);
int  sptlrpc_conf_init(void);
void sptlrpc_conf_fini(void);

/* sec.c */
int  sptlrpc_init(void);
void sptlrpc_fini(void);

/* recov_thread.c */
int llog_recov_init(void);
void llog_recov_fini(void);

static inline int ll_rpc_recoverable_error(int rc)
{
        return (rc == -ENOTCONN || rc == -ENODEV);
}

#ifdef HAVE_SERVER_SUPPORT
int tgt_mod_init(void);
void tgt_mod_exit(void);
#else
static inline int tgt_mod_init(void)
{
	return 0;
}

static inline void tgt_mod_exit(void)
{
	return;
}
#endif

static inline void ptlrpc_reqset_put(struct ptlrpc_request_set *set)
{
        if (cfs_atomic_dec_and_test(&set->set_refcount))
                OBD_FREE_PTR(set);
}
#endif /* PTLRPC_INTERNAL_H */
