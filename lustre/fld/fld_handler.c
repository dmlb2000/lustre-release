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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/fld/fld_handler.c
 *
 * FLD (Fids Location Database)
 *
 * Author: Yury Umanets <umka@clusterfs.com>
 * Author: WangDi <wangdi@clusterfs.com>
 * Author: Pravin Shelar <pravin.shelar@sun.com>
 */

#define DEBUG_SUBSYSTEM S_FLD

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
# include <linux/module.h>
# include <linux/jbd.h>
# include <asm/div64.h>
#else /* __KERNEL__ */
# include <liblustre.h>
# include <libcfs/list.h>
#endif

#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <lprocfs_status.h>

#include <md_object.h>
#include <lustre_fid.h>
#include <lustre_req_layout.h>
#include "fld_internal.h"
#include <lustre_fid.h>

#ifdef __KERNEL__

/* context key constructor/destructor: fld_key_init, fld_key_fini */
LU_KEY_INIT_FINI(fld, struct fld_thread_info);

/* context key: fld_thread_key */
LU_CONTEXT_KEY_DEFINE(fld, LCT_MD_THREAD|LCT_DT_THREAD);

cfs_proc_dir_entry_t *fld_type_proc_dir = NULL;

static int __init fld_mod_init(void)
{
        fld_type_proc_dir = lprocfs_register(LUSTRE_FLD_NAME,
                                             proc_lustre_root,
                                             NULL, NULL);
        if (IS_ERR(fld_type_proc_dir))
                return PTR_ERR(fld_type_proc_dir);

        LU_CONTEXT_KEY_INIT(&fld_thread_key);
        lu_context_key_register(&fld_thread_key);
        return 0;
}

static void __exit fld_mod_exit(void)
{
        lu_context_key_degister(&fld_thread_key);
        if (fld_type_proc_dir != NULL && !IS_ERR(fld_type_proc_dir)) {
                lprocfs_remove(&fld_type_proc_dir);
                fld_type_proc_dir = NULL;
        }
}

int fld_declare_server_create(const struct lu_env *env,
			      struct lu_server_fld *fld,
			      struct lu_seq_range *range,
			      struct thandle *th)
{
	int rc;

	rc = fld_declare_index_create(env, fld, range, th);
	RETURN(rc);
}
EXPORT_SYMBOL(fld_declare_server_create);

/**
 * Insert FLD index entry and update FLD cache.
 *
 * This function is called from the sequence allocator when a super-sequence
 * is granted to a server.
 */
int fld_server_create(const struct lu_env *env, struct lu_server_fld *fld,
		      struct lu_seq_range *range, struct thandle *th)
{
	int rc;

	mutex_lock(&fld->lsf_lock);
	rc = fld_index_create(env, fld, range, th);
	mutex_unlock(&fld->lsf_lock);

	RETURN(rc);
}
EXPORT_SYMBOL(fld_server_create);

/**
 *  Lookup mds by seq, returns a range for given seq.
 *
 *  If that entry is not cached in fld cache, request is sent to super
 *  sequence controller node (MDT0). All other MDT[1...N] and client
 *  cache fld entries, but this cache is not persistent.
 */

int fld_server_lookup(const struct lu_env *env, struct lu_server_fld *fld,
		      seqno_t seq, struct lu_seq_range *range)
{
        struct lu_seq_range *erange;
        struct fld_thread_info *info;
        int rc;
        ENTRY;

        info = lu_context_key_get(&env->le_ctx, &fld_thread_key);
        erange = &info->fti_lrange;

        /* Lookup it in the cache. */
        rc = fld_cache_lookup(fld->lsf_cache, seq, erange);
        if (rc == 0) {
		if (unlikely(erange->lsr_flags != range->lsr_flags) &&
		    range->lsr_flags != -1) {
                        CERROR("FLD cache found a range "DRANGE" doesn't "
                               "match the requested flag %x\n",
                               PRANGE(erange), range->lsr_flags);
                        RETURN(-EIO);
                }
                *range = *erange;
                RETURN(0);
        }

	if (fld->lsf_obj) {
		/* On server side, all entries should be in cache.
		 * If we can not find it in cache, just return error */
		CERROR("%s: Can not found the seq "LPX64"\n",
			fld->lsf_name, seq);
		RETURN(-EIO);
	} else {
		LASSERT(fld->lsf_control_exp);
		/* send request to mdt0 i.e. super seq. controller.
		 * This is temporary solution, long term solution is fld
		 * replication on all mdt servers.
		 */
		range->lsr_start = seq;
		rc = fld_client_rpc(fld->lsf_control_exp,
				    range, FLD_LOOKUP);
		if (rc == 0)
			fld_cache_insert(fld->lsf_cache, range);
	}
	RETURN(rc);
}
EXPORT_SYMBOL(fld_server_lookup);

/**
 * All MDT server handle fld lookup operation. But only MDT0 has fld index.
 * if entry is not found in cache we need to forward lookup request to MDT0
 */

static int fld_server_handle(struct lu_server_fld *fld,
                             const struct lu_env *env,
                             __u32 opc, struct lu_seq_range *range,
                             struct fld_thread_info *info)
{
        int rc;
        ENTRY;

        switch (opc) {
        case FLD_LOOKUP:
		rc = fld_server_lookup(env, fld, range->lsr_start, range);
                break;
        default:
                rc = -EINVAL;
                break;
        }

        CDEBUG(D_INFO, "%s: FLD req handle: error %d (opc: %d, range: "
               DRANGE"\n", fld->lsf_name, rc, opc, PRANGE(range));

        RETURN(rc);

}

static int fld_req_handle(struct ptlrpc_request *req,
                          struct fld_thread_info *info)
{
        struct obd_export *exp = req->rq_export;
        struct lu_site *site = exp->exp_obd->obd_lu_dev->ld_site;
        struct lu_seq_range *in;
        struct lu_seq_range *out;
        int rc;
        __u32 *opc;
        ENTRY;

        rc = req_capsule_server_pack(info->fti_pill);
        if (rc)
                RETURN(err_serious(rc));

        opc = req_capsule_client_get(info->fti_pill, &RMF_FLD_OPC);
        if (opc != NULL) {
                in = req_capsule_client_get(info->fti_pill, &RMF_FLD_MDFLD);
                if (in == NULL)
                        RETURN(err_serious(-EPROTO));
                out = req_capsule_server_get(info->fti_pill, &RMF_FLD_MDFLD);
                if (out == NULL)
                        RETURN(err_serious(-EPROTO));
                *out = *in;

		/* For old 2.0 client, the 'lsr_flags' is uninitialized.
		 * Set it as 'LU_SEQ_RANGE_MDT' by default. */
		if (!(exp_connect_flags(exp) & OBD_CONNECT_64BITHASH) &&
		    !(exp_connect_flags(exp) & OBD_CONNECT_MDS_MDS) &&
		    !(exp_connect_flags(exp) & OBD_CONNECT_LIGHTWEIGHT) &&
		    !exp->exp_libclient)
			out->lsr_flags = LU_SEQ_RANGE_MDT;

		rc = fld_server_handle(lu_site2seq(site)->ss_server_fld,
				       req->rq_svc_thread->t_env,
				       *opc, out, info);
        } else
                rc = err_serious(-EPROTO);

        RETURN(rc);
}

static void fld_thread_info_init(struct ptlrpc_request *req,
                                 struct fld_thread_info *info)
{
        info->fti_pill = &req->rq_pill;
        /* Init request capsule. */
        req_capsule_init(info->fti_pill, req, RCL_SERVER);
        req_capsule_set(info->fti_pill, &RQF_FLD_QUERY);
}

static void fld_thread_info_fini(struct fld_thread_info *info)
{
        req_capsule_fini(info->fti_pill);
}

static int fld_handle(struct ptlrpc_request *req)
{
        struct fld_thread_info *info;
        const struct lu_env *env;
        int rc;

        env = req->rq_svc_thread->t_env;
        LASSERT(env != NULL);

        info = lu_context_key_get(&env->le_ctx, &fld_thread_key);
        LASSERT(info != NULL);

        fld_thread_info_init(req, info);
        rc = fld_req_handle(req, info);
        fld_thread_info_fini(info);

        return rc;
}

/*
 * Entry point for handling FLD RPCs called from MDT.
 */
int fld_query(struct com_thread_info *info)
{
        return fld_handle(info->cti_pill->rc_req);
}
EXPORT_SYMBOL(fld_query);

/*
 * Returns true, if fid is local to this server node.
 *
 * WARNING: this function is *not* guaranteed to return false if fid is
 * remote: it makes an educated conservative guess only.
 *
 * fid_is_local() is supposed to be used in assertion checks only.
 */
int fid_is_local(const struct lu_env *env,
                 struct lu_site *site, const struct lu_fid *fid)
{
	int result;
	struct seq_server_site *ss_site;
	struct lu_seq_range *range;
	struct fld_thread_info *info;
	ENTRY;

	info = lu_context_key_get(&env->le_ctx, &fld_thread_key);
	range = &info->fti_lrange;

	result = 1; /* conservatively assume fid is local */
	ss_site = lu_site2seq(site);
	if (ss_site->ss_client_fld != NULL) {
		int rc;

		rc = fld_cache_lookup(ss_site->ss_client_fld->lcf_cache,
				      fid_seq(fid), range);
		if (rc == 0)
			result = (range->lsr_index == ss_site->ss_node_id);
	}
	return result;
}
EXPORT_SYMBOL(fid_is_local);

static void fld_server_proc_fini(struct lu_server_fld *fld);

#ifdef LPROCFS
static int fld_server_proc_init(struct lu_server_fld *fld)
{
        int rc = 0;
        ENTRY;

        fld->lsf_proc_dir = lprocfs_register(fld->lsf_name,
                                             fld_type_proc_dir,
                                             fld_server_proc_list, fld);
        if (IS_ERR(fld->lsf_proc_dir)) {
                rc = PTR_ERR(fld->lsf_proc_dir);
                RETURN(rc);
        }

	rc = lprocfs_seq_create(fld->lsf_proc_dir, "fldb", 0444,
				&fld_proc_seq_fops, fld);
	if (rc) {
		lprocfs_remove(&fld->lsf_proc_dir);
		fld->lsf_proc_dir = NULL;
	}

	RETURN(rc);
}

static void fld_server_proc_fini(struct lu_server_fld *fld)
{
        ENTRY;
        if (fld->lsf_proc_dir != NULL) {
                if (!IS_ERR(fld->lsf_proc_dir))
                        lprocfs_remove(&fld->lsf_proc_dir);
                fld->lsf_proc_dir = NULL;
        }
        EXIT;
}
#else
static int fld_server_proc_init(struct lu_server_fld *fld)
{
        return 0;
}

static void fld_server_proc_fini(struct lu_server_fld *fld)
{
        return;
}
#endif

int fld_server_init(const struct lu_env *env, struct lu_server_fld *fld,
		    struct dt_device *dt, const char *prefix, int mds_node_id,
		    __u32 lsr_flags)
{
        int cache_size, cache_threshold;
        struct lu_seq_range range;
        int rc;
        ENTRY;

        snprintf(fld->lsf_name, sizeof(fld->lsf_name),
                 "srv-%s", prefix);

        cache_size = FLD_SERVER_CACHE_SIZE /
                sizeof(struct fld_cache_entry);

        cache_threshold = cache_size *
                FLD_SERVER_CACHE_THRESHOLD / 100;

	mutex_init(&fld->lsf_lock);
        fld->lsf_cache = fld_cache_init(fld->lsf_name,
                                        cache_size, cache_threshold);
        if (IS_ERR(fld->lsf_cache)) {
                rc = PTR_ERR(fld->lsf_cache);
                fld->lsf_cache = NULL;
                GOTO(out, rc);
        }

	if (!mds_node_id && lsr_flags == LU_SEQ_RANGE_MDT) {
		rc = fld_index_init(env, fld, dt);
                if (rc)
                        GOTO(out, rc);
        } else
                fld->lsf_obj = NULL;

        rc = fld_server_proc_init(fld);
        if (rc)
                GOTO(out, rc);

        fld->lsf_control_exp = NULL;

	if (lsr_flags == LU_SEQ_RANGE_MDT) {
		/* Insert reserved sequence of "ROOT" and ".lustre"
		 * into fld cache. */
		range.lsr_start = FID_SEQ_LOCAL_FILE;
		range.lsr_end = FID_SEQ_DOT_LUSTRE + 1;
		range.lsr_index = 0;
		range.lsr_flags = lsr_flags;
		fld_cache_insert(fld->lsf_cache, &range);
	}
        EXIT;
out:
	if (rc)
		fld_server_fini(env, fld);
	return rc;
}
EXPORT_SYMBOL(fld_server_init);

void fld_server_fini(const struct lu_env *env, struct lu_server_fld *fld)
{
	ENTRY;

	fld_server_proc_fini(fld);
	fld_index_fini(env, fld);

	if (fld->lsf_cache != NULL) {
		if (!IS_ERR(fld->lsf_cache))
			fld_cache_fini(fld->lsf_cache);
		fld->lsf_cache = NULL;
	}

	EXIT;
}
EXPORT_SYMBOL(fld_server_fini);

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre FLD");
MODULE_LICENSE("GPL");

cfs_module(mdd, "0.1.0", fld_mod_init, fld_mod_exit);
#endif
