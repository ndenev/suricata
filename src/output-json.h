/* Copyright (C) 2007-2013 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Tom DeCanio <td@npulsetech.com>
 */

#ifndef __ALERT_JSON_H__
#define __ALERT_JSON_H__

void TmModuleOutputJsonRegister (void);

#ifdef HAVE_LIBJANSSON

json_t *CreateJSONHeader(Packet *p, int direction_sensative);
TmEcode OutputJSON(json_t *js, void *data, uint64_t *count);

OutputCtx *OutputJsonInitCtx(ConfNode *);

/* TODO: I think the following structures can be made private again */
/*
 * Global configuration context data
 */
typedef struct OutputJsonCtx_ {
    LogFileCtx *file_ctx;
    OutputCtx *drop_ctx;
    OutputCtx *files_ctx;
    OutputCtx *http_ctx;
    OutputCtx *tls_ctx;
} OutputJsonCtx;

typedef struct AlertJsonThread_ {
    /** LogFileCtx has the pointer to the file and a mutex to allow multithreading */
    LogFileCtx* file_ctx;

    void *buffer; /* pointer to MemBuffer */

    uint64_t alert_cnt;
    uint64_t dns_cnt;
    uint64_t drop_cnt;
    uint64_t files_cnt;
    uint64_t http_cnt;
    uint64_t tls_cnt;
    OutputCtx *drop_ctx;
    OutputCtx *files_ctx;
    OutputCtx *http_ctx;
    OutputCtx *tls_ctx;
} AlertJsonThread;

#endif /* HAVE_LIBJANSSON */

#endif /* __ALERT_JSON_H__ */
