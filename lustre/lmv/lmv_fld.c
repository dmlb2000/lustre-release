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
 * Copyright (c) 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_SUBSYSTEM S_LMV
#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <asm/div64.h>
#include <linux/seq_file.h>
#else
#include <liblustre.h>
#endif

#include <obd_support.h>
#include <lustre/lustre_idl.h>
#include <lustre_fid.h>
#include <lustre_lib.h>
#include <lustre_net.h>
#include <lustre_dlm.h>
#include <obd_class.h>
#include <lprocfs_status.h>
#include "lmv_internal.h"

int lmv_fld_lookup(struct lmv_obd *lmv,
                   const struct lu_fid *fid,
                   mdsno_t *mds)
{
	int rc;
	ENTRY;

	LASSERTF(fid_is_sane(fid), DFID" is insane!\n", PFID(fid));

	/* FIXME: Because ZFS still use LOCAL fid sequence for root,
	 * and root will always be in MDT0, for local fid, it will
	 * return 0 directly. And it should be removed once the root
	 * FID has been assigned with special sequence */
	if (fid_seq(fid) == FID_SEQ_LOCAL_FILE) {
		*mds = 0;
		RETURN(0);
	}

	rc = fld_client_lookup(&lmv->lmv_fld, fid_seq(fid), mds,
                               LU_SEQ_RANGE_MDT, NULL);
        if (rc) {
                CERROR("Error while looking for mds number. Seq "LPX64
                       ", err = %d\n", fid_seq(fid), rc);
                RETURN(rc);
        }

        CDEBUG(D_INODE, "FLD lookup got mds #%x for fid="DFID"\n",
               *mds, PFID(fid));

        if (*mds >= lmv->desc.ld_tgt_count) {
                CERROR("FLD lookup got invalid mds #%x (max: %x) "
                       "for fid="DFID"\n", *mds, lmv->desc.ld_tgt_count,
                       PFID(fid));
                rc = -EINVAL;
        }
        RETURN(rc);
}
