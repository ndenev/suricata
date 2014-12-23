/* Copyright (C) 2007-2014 Open Information Security Foundation
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
 *
 * Packetpool queue handlers. Packet pool is implemented as a stack.
 */

#include "suricata.h"
#include "packet-queue.h"
#include "decode.h"
#include "detect.h"
#include "detect-uricontent.h"
#include "threads.h"
#include "threadvars.h"
#include "flow.h"
#include "flow-util.h"
#include "host.h"

#include "stream.h"
#include "stream-tcp-reassemble.h"

#include "tm-queuehandlers.h"

#include "pkt-var.h"

#include "tmqh-packetpool.h"

#include "util-debug.h"
#include "util-error.h"
#include "util-profiling.h"
#include "util-device.h"

/* Number of freed packet to save for one pool before freeing them. */
#define MAX_PENDING_RETURN_PACKETS 32

#ifdef TLS
__thread PktPool thread_pkt_pool;

static inline PktPool *GetThreadPacketPool(void)
{
    return &thread_pkt_pool;
}

static inline PktPool *ThreadPacketPoolCreate(void)
{
    /* Nothing to do since __thread statically allocates the memory. */
    return GetThreadPacketPool();
}
#else
/* __thread not supported. */
static pthread_key_t pkt_pool_thread_key;
static SCMutex pkt_pool_thread_key_mutex = SCMUTEX_INITIALIZER;
static int pkt_pool_thread_key_initialized = 0;

static inline PktPool *GetThreadPacketPool(void)
{
    return (PktPool*)pthread_getspecific(pkt_pool_thread_key);
}

static void PktPoolThreadDestroy(void * buf)
{
    free(buf);
}

static void TmqhPacketpoolInit(void)
{
    SCMutexLock(&pkt_pool_thread_key_mutex);
    if (pkt_pool_thread_key_initialized) {
        /* Key has already been created. */
        SCMutexUnlock(&pkt_pool_thread_key_mutex);
        return;
    }

    /* Create the pthread Key that is used to look up thread specific
     * data buffer. Needs to be created only once.
     */
    int r = pthread_key_create(&pkt_pool_thread_key, PktPoolThreadDestroy);
    if (r != 0) {
        SCLogError(SC_ERR_MEM_ALLOC, "pthread_key_create failed with %d", r);
        exit(EXIT_FAILURE);
    }

    pkt_pool_thread_key_initialized = 1;
    SCMutexUnlock(&pkt_pool_thread_key_mutex);
}

static inline PktPool *ThreadPacketPoolCreate(void)
{
    TmqhPacketpoolInit();

    /* Check that the pool is not already created */
    PktPool *pool = GetThreadPacketPool();
    if (pool)
        return pool;

    /* Create a new pool for this thread. */
    pool = (PktPool*)SCMallocAligned(sizeof(PktPool), CLS);
    if (pool == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "malloc failed");
        exit(EXIT_FAILURE);
    }
    int r = pthread_setspecific(pkt_pool_thread_key, pool);
    if (r != 0) {
        SCLogError(SC_ERR_MEM_ALLOC, "pthread_setspecific failed with %d", r);
        exit(EXIT_FAILURE);
    }

    return pool;
}
#endif

/**
 * \brief TmqhPacketpoolRegister
 * \initonly
 */
void TmqhPacketpoolRegister (void) {
    tmqh_table[TMQH_PACKETPOOL].name = "packetpool";
    tmqh_table[TMQH_PACKETPOOL].InHandler = TmqhInputPacketpool;
    tmqh_table[TMQH_PACKETPOOL].OutHandler = TmqhOutputPacketpool;
}

static int PacketPoolIsEmpty(PktPool *pool)
{
    /* Check local stack first. */
    if (pool->head || pool->return_stack.head)
        return 0;

    return 1;
}

void PacketPoolWait(void)
{
    PktPool *my_pool = GetThreadPacketPool();

    while(PacketPoolIsEmpty(my_pool))
        ;
}

/** \brief a initialized packet
 *
 *  \warning Use *only* at init, not at packet runtime
 */
static void PacketPoolStorePacket(Packet *p)
{
    /* Clear the PKT_ALLOC flag, since that indicates to push back
     * onto the ring buffer. */
    p->flags &= ~PKT_ALLOC;
    p->pool = GetThreadPacketPool();
    p->ReleasePacket = PacketPoolReturnPacket;
    PacketPoolReturnPacket(p);
}

/** \brief Get a new packet from the packet pool
 *
 * Only allocates from the thread's local stack, or mallocs new packets.
 * If the local stack is empty, first move all the return stack packets to
 * the local stack.
 *  \retval Packet pointer, or NULL on failure.
 */
Packet *PacketPoolGetPacket(void)
{
    PktPool *pool = GetThreadPacketPool();

    if (pool->head) {
        /* Stack is not empty. */
        Packet *p = pool->head;
        pool->head = p->next;
        p->pool = pool;
        return p;
    }

    /* Local Stack is empty, so check the return stack, which requires
     * locking. */
    SCMutexLock(&pool->return_stack.mutex);
    /* Move all the packets from the locked return stack to the local stack. */
    pool->head = pool->return_stack.head;
    pool->return_stack.head = NULL;
    SCMutexUnlock(&pool->return_stack.mutex);

    /* Try to allocate again. Need to check for not empty again, since the
     * return stack might have been empty too.
     */
    if (pool->head) {
        /* Stack is not empty. */
        Packet *p = pool->head;
        pool->head = p->next;
        p->pool = pool;
        return p;
    }

    /* Failed to allocate a packet, so return NULL. */
    /* Optionally, could allocate a new packet here. */
    return NULL;
}

/** \brief Return packet to Packet pool
 *
 */
void PacketPoolReturnPacket(Packet *p)
{
    PktPool *my_pool = GetThreadPacketPool();

    PktPool *pool = p->pool;
    if (pool == NULL) {
        free(p);
        return;
    }

    PACKET_RECYCLE(p);

    if (pool == my_pool) {
        /* Push back onto this thread's own stack, so no locking. */
        p->next = my_pool->head;
        my_pool->head = p;
    } else {
        PktPool *pending_pool = my_pool->pending_pool;
        if (pending_pool == NULL) {
            /* No pending packet, so store the current packet. */
            my_pool->pending_pool = pool;
            my_pool->pending_head = p;
            my_pool->pending_tail = p;
            my_pool->pending_count = 1;
        } else if (pending_pool == pool) {
            /* Another packet for the pending pool list. */
            p->next = my_pool->pending_head;
            my_pool->pending_head = p;
            my_pool->pending_count++;
            if (my_pool->pending_count > MAX_PENDING_RETURN_PACKETS) {
                /* Return the entire list of pending packets. */
                SCMutexLock(&pool->return_stack.mutex);
                my_pool->pending_tail->next = pool->return_stack.head;
                pool->return_stack.head = my_pool->pending_head;
                SCMutexUnlock(&pool->return_stack.mutex);
                /* Clear the list of pending packets to return. */
                my_pool->pending_pool = NULL;
            }
        } else {
            /* Push onto return stack for this pool */
            SCMutexLock(&pool->return_stack.mutex);
            p->next = pool->return_stack.head;
            pool->return_stack.head = p;
            SCMutexUnlock(&pool->return_stack.mutex);
        }
    }
}

void PacketPoolInit(void)
{
    extern intmax_t max_pending_packets;

    PktPool *my_pool = ThreadPacketPoolCreate();

    SCMutexInit(&my_pool->return_stack.mutex, NULL);

    /* pre allocate packets */
    SCLogDebug("preallocating packets... packet size %" PRIuMAX "",
               (uintmax_t)SIZE_OF_PACKET);
    int i = 0;
    for (i = 0; i < max_pending_packets; i++) {
        Packet *p = PacketGetFromAlloc();
        if (unlikely(p == NULL)) {
            SCLogError(SC_ERR_FATAL, "Fatal error encountered while allocating a packet. Exiting...");
            exit(EXIT_FAILURE);
        }
        PacketPoolStorePacket(p);
    }
    SCLogInfo("preallocated %"PRIiMAX" packets. Total memory %"PRIuMAX"",
            max_pending_packets, (uintmax_t)(max_pending_packets*SIZE_OF_PACKET));
}

void PacketPoolDestroy(void) {
#if 0
    Packet *p = NULL;
    while ((p = PacketPoolGetPacket()) != NULL) {
        PACKET_CLEANUP(p);
        SCFree(p);
    }
#endif
}

Packet *TmqhInputPacketpool(ThreadVars *tv)
{
    return PacketPoolGetPacket();
}

void TmqhOutputPacketpool(ThreadVars *t, Packet *p)
{
    int proot = 0;

    SCEnter();
    SCLogDebug("Packet %p, p->root %p, alloced %s", p, p->root, p->flags & PKT_ALLOC ? "true" : "false");

    /** \todo make this a callback
     *  Release tcp segments. Done here after alerting can use them. */
    if (p->flow != NULL && p->proto == IPPROTO_TCP) {
        SCMutexLock(&p->flow->m);
        StreamTcpPruneSession(p->flow, p->flowflags & FLOW_PKT_TOSERVER ?
                STREAM_TOSERVER : STREAM_TOCLIENT);
        SCMutexUnlock(&p->flow->m);
    }

    if (IS_TUNNEL_PKT(p)) {
        SCLogDebug("Packet %p is a tunnel packet: %s",
            p,p->root ? "upper layer" : "tunnel root");

        /* get a lock to access root packet fields */
        SCMutex *m = p->root ? &p->root->tunnel_mutex : &p->tunnel_mutex;
        SCMutexLock(m);

        if (IS_TUNNEL_ROOT_PKT(p)) {
            SCLogDebug("IS_TUNNEL_ROOT_PKT == TRUE");
            if (TUNNEL_PKT_TPR(p) == 0) {
                SCLogDebug("TUNNEL_PKT_TPR(p) == 0, no more tunnel packet "
                        "depending on this root");
                /* if this packet is the root and there are no
                 * more tunnel packets, return it to the pool */

                /* fall through */
            } else {
                SCLogDebug("tunnel root Packet %p: TUNNEL_PKT_TPR(p) > 0, so "
                        "packets are still depending on this root, setting "
                        "p->tunnel_verdicted == 1", p);
                /* if this is the root and there are more tunnel
                 * packets, return this to the pool. It's still referenced
                 * by the tunnel packets, and we will return it
                 * when we handle them */
                SET_TUNNEL_PKT_VERDICTED(p);

                PACKET_PROFILING_END(p);
                SCMutexUnlock(m);
                SCReturn;
            }
        } else {
            SCLogDebug("NOT IS_TUNNEL_ROOT_PKT, so tunnel pkt");

            /* the p->root != NULL here seems unnecessary: IS_TUNNEL_PKT checks
             * that p->tunnel_pkt == 1, IS_TUNNEL_ROOT_PKT checks that +
             * p->root == NULL. So when we are here p->root can only be
             * non-NULL, right? CLANG thinks differently. May be a FP, but
             * better safe than sorry. VJ */
            if (p->root != NULL && IS_TUNNEL_PKT_VERDICTED(p->root) &&
                    TUNNEL_PKT_TPR(p) == 1)
            {
                SCLogDebug("p->root->tunnel_verdicted == 1 && TUNNEL_PKT_TPR(p) == 1");
                /* the root is ready and we are the last tunnel packet,
                 * lets enqueue them both. */
                TUNNEL_DECR_PKT_TPR_NOLOCK(p);

                /* handle the root */
                SCLogDebug("setting proot = 1 for root pkt, p->root %p "
                        "(tunnel packet %p)", p->root, p);
                proot = 1;

                /* fall through */
            } else {
                /* root not ready yet, so get rid of the tunnel pkt only */

                SCLogDebug("NOT p->root->tunnel_verdicted == 1 && "
                        "TUNNEL_PKT_TPR(p) == 1 (%" PRIu32 ")", TUNNEL_PKT_TPR(p));

                TUNNEL_DECR_PKT_TPR_NOLOCK(p);

                 /* fall through */
            }
        }
        SCMutexUnlock(m);

        SCLogDebug("tunnel stuff done, move on (proot %d)", proot);
    }

    FlowDeReference(&p->flow);

    /* we're done with the tunnel root now as well */
    if (proot == 1) {
        SCLogDebug("getting rid of root pkt... alloc'd %s", p->root->flags & PKT_ALLOC ? "true" : "false");

        FlowDeReference(&p->root->flow);
        /* if p->root uses extended data, free them */
        if (p->root->ext_pkt) {
            if (!(p->root->flags & PKT_ZERO_COPY)) {
                SCFree(p->root->ext_pkt);
            }
            p->root->ext_pkt = NULL;
        }
        p->root->ReleasePacket(p->root);
        p->root = NULL;
    }

    /* if p uses extended data, free them */
    if (p->ext_pkt) {
        if (!(p->flags & PKT_ZERO_COPY)) {
            SCFree(p->ext_pkt);
        }
        p->ext_pkt = NULL;
    }

    PACKET_PROFILING_END(p);

    p->ReleasePacket(p);

    SCReturn;
}

/**
 *  \brief Release all the packets in the queue back to the packetpool.  Mainly
 *         used by threads that have failed, and wants to return the packets back
 *         to the packetpool.
 *
 *  \param pq Pointer to the packetqueue from which the packets have to be
 *            returned back to the packetpool
 *
 *  \warning this function assumes that the pq does not use locking
 */
void TmqhReleasePacketsToPacketPool(PacketQueue *pq)
{
    Packet *p = NULL;

    if (pq == NULL)
        return;

    while ( (p = PacketDequeue(pq)) != NULL)
        TmqhOutputPacketpool(NULL, p);

    return;
}
