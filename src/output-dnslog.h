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
 * \author Victor Julien <victor@inliniac.net>
 */

#ifndef __OUTPUT_DNSLOG_H__
#define __OUTPUT_DNSLOG_H__

void TmModuleDnsJsonRegister (void);
void TmModuleDnsJsonIPv4Register (void);
void TmModuleDnsJsonIPv6Register (void);
int OutputDnsNeedsLog(Packet *p);
TmEcode OutputDnsLog(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq);
OutputCtx *DnsJsonInitCtx(ConfNode *);

#endif /* __OUTPUT_DNSLOG_H__ */
