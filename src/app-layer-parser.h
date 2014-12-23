/* Copyright (C) 2007-2010 Open Information Security Foundation
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

#ifndef __APP_LAYER_PARSER_H__
#define __APP_LAYER_PARSER_H__

#include "decode-events.h"

#include "util-file.h"

/** Mapping between local parser id's (e.g. HTTP_FIELD_REQUEST_URI) and
  * the dynamically assigned (at registration) global parser id. */
typedef struct AppLayerLocalMap_ {
    uint16_t parser_id;
} AppLayerLocalMap;

/** \brief Mapping between ALPROTO_* and L7Parsers
 *
 * Map the proto to the parsers for the to_client and to_server directions.
 */
typedef struct AppLayerProto_ {
    char *name; /**< name of the registered proto */

    uint16_t to_server;
    uint16_t to_client;
    uint16_t map_size;
    char logger; /**< does this proto have a logger enabled? */

    AppLayerLocalMap **map;

    void *(*StateAlloc)(void);
    void (*StateFree)(void *);
    void (*StateTransactionFree)(void *, uint64_t);
    void *(*LocalStorageAlloc)(void);
    void (*LocalStorageFree)(void *);

    /** truncate state after a gap/depth event */
    void (*Truncate)(void *, uint8_t);
    FileContainer *(*StateGetFiles)(void *, uint8_t);
    AppLayerDecoderEvents *(*StateGetEvents)(void *, uint64_t);
    /* bool indicating a state has decoder/parser events */
    int (*StateHasEvents)(void *);

    int (*StateGetAlstateProgress)(void *alstate, uint8_t direction);
    uint64_t (*StateGetTxCnt)(void *alstate);
    void *(*StateGetTx)(void *alstate, uint64_t tx_id);
    int (*StateGetAlstateProgressCompletionStatus)(uint8_t direction);
} AppLayerProto;

/** flags for the result elmts */
#define ALP_RESULT_ELMT_ALLOC 0x01

/** \brief Result elements for the parser */
typedef struct AppLayerParserResultElmt_ {
    uint16_t flags; /* flags. E.g. local alloc */
    uint16_t name_idx; /* idx for names like "http.request_line.uri" */

    uint32_t data_len; /* length of the data from the ptr */
    uint8_t  *data_ptr; /* point to the position in the "input" data
                          * or ptr to new mem if local alloc flag set */
    struct AppLayerParserResultElmt_ *next;
} AppLayerParserResultElmt;

/** \brief List head for parser result elmts */
typedef struct AppLayerParserResult_ {
    AppLayerParserResultElmt *head;
    AppLayerParserResultElmt *tail;
    uint32_t cnt;
} AppLayerParserResult;

#define APP_LAYER_PARSER_USE            0x01
#define APP_LAYER_PARSER_EOF            0x02
#define APP_LAYER_PARSER_DONE           0x04    /**< parser is done, ignore more
                                                     msgs */
#define APP_LAYER_PARSER_NO_INSPECTION  0x08    /**< Flag to indicate no more
                                                     packets payload inspection */
#define APP_LAYER_PARSER_NO_REASSEMBLY  0x10    /**< Flag to indicate no more
                                                     packets reassembly for this
                                                     session */

#define APP_LAYER_TRANSACTION_EOF       0x01    /**< Session done, last transaction
                                                     as well */
#define APP_LAYER_TRANSACTION_TOSERVER  0x02    /**< transaction has been inspected
                                                     in to server direction. */
#define APP_LAYER_TRANSACTION_TOCLIENT  0x04    /**< transaction has been inspected
                                                     in to server direction. */

typedef struct AppLayerParserState_ {
    uint8_t flags;
    uint16_t cur_parser; /**< idx of currently active parser */
    uint8_t *store;
    uint32_t store_len;
    uint16_t parse_field;
} AppLayerParserState;

typedef struct AppLayerParserStateStore_ {
    AppLayerParserState to_client;
    AppLayerParserState to_server;

    /** flags related to the id's */
    uint8_t id_flags;

    /* Indicates the current transaction that is being indicated.  We have
     * a var per direction. */
    uint64_t inspect_id[2];
    /* Indicates the current transaction being logged.  Unlike inspect_id,
     * we don't need a var per direction since we don't log a transaction
     * unless we have the entire transaction. */
    uint64_t log_id;
    uint16_t version;       /**< state version, incremented for each update,
                             *   can wrap around */

    /* Used to store decoder events */
    AppLayerDecoderEvents *decoder_events;
} AppLayerParserStateStore;

typedef struct AppLayerParserTableElement_ {
    int (*AppLayerParser)(Flow *f, void *protocol_state, AppLayerParserState
                          *parser_state, uint8_t *input, uint32_t input_len,
                          void *local_storage, AppLayerParserResult *output);

    char *name;

    uint16_t proto;
    uint16_t parser_local_id; /**< local id of the parser in the parser itself. */
} AppLayerParserTableElement;

typedef struct AppLayerProbingParserElement_ {
    const char *al_proto_name;
    uint16_t al_proto;
    uint16_t port;
    uint16_t ip_proto;
    uint8_t priority;
    uint8_t top;
    uint32_t al_proto_mask;
    /* the min length of data that has to be supplied to invoke the parser */
    uint32_t min_depth;
    /* the max length of data after which this parser won't be invoked */
    uint32_t max_depth;
    /* the probing parser function */
    uint16_t (*ProbingParser)(uint8_t *input, uint32_t input_len);

    struct AppLayerProbingParserElement_ *next;
} AppLayerProbingParserElement;

typedef struct AppLayerProbingParser_ {
    /* the port no for which probing parser(s) are invoked */
    uint16_t port;
    uint32_t toserver_al_proto_mask;
    uint32_t toclient_al_proto_mask;
    /* the max depth for all the probing parsers registered for this port */
    uint16_t toserver_max_depth;
    uint16_t toclient_max_depth;

    AppLayerProbingParserElement *toserver;
    AppLayerProbingParserElement *toclient;

    struct AppLayerProbingParser_ *next;
} AppLayerProbingParser;

typedef struct AppLayerProbingParserInfo_ {
    const char *al_proto_name;
    uint16_t ip_proto;
    uint16_t al_proto;
    uint16_t (*ProbingParser)(uint8_t *input, uint32_t input_len);
    struct AppLayerProbingParserInfo_ *next;
} AppLayerProbingParserInfo;

#define APP_LAYER_PROBING_PARSER_PRIORITY_HIGH   1
#define APP_LAYER_PROBING_PARSER_PRIORITY_MEDIUM 2
#define APP_LAYER_PROBING_PARSER_PRIORITY_LOW    3

extern AppLayerProto al_proto_table[];

static inline
AppLayerProbingParser *AppLayerGetProbingParsers(AppLayerProbingParser *probing_parsers,
                                                 uint16_t ip_proto,
                                                 uint16_t port)
{
    if (probing_parsers == NULL)
        return NULL;

    AppLayerProbingParser *pp = probing_parsers;
    while (pp != NULL) {
        if (pp->port == port || pp->port == 0) {
            break;
        }
        pp = pp->next;
    }

    return pp;
}

static inline
AppLayerProbingParserInfo *AppLayerGetProbingParserInfo(AppLayerProbingParserInfo *ppi,
                                                        const char *al_proto_name)
{
    while (ppi != NULL) {
        if (strcmp(ppi->al_proto_name, al_proto_name) == 0)
            return ppi;
        ppi = ppi->next;
    }

    return NULL;
}
struct AlpProtoDetectCtx_;

/* prototypes */
void AppLayerParsersInitPostProcess(void);
void RegisterAppLayerParsers(void);
void AppLayerParserRegisterTests(void);

/* registration */
int AppLayerRegisterProto(char *name, uint8_t proto, uint8_t flags,
                          int (*AppLayerParser)(Flow *f, void *protocol_state,
                                                AppLayerParserState *parser_state,
                                                uint8_t *input, uint32_t input_len,
                                                void *local_data,
                                                AppLayerParserResult *output));
int AppLayerRegisterParser(char *name, uint16_t proto, uint16_t parser_id,
                           int (*AppLayerParser)(Flow *f, void *protocol_state,
                                                 AppLayerParserState *parser_state,
                                                 uint8_t *input, uint32_t input_len,
                                                 void *local_data,
                                                 AppLayerParserResult *output),
                           char *dependency);
void AppLayerRegisterProbingParser(struct AlpProtoDetectCtx_ *, uint16_t, uint16_t,
                                   const char *, uint16_t,
                                   uint16_t, uint16_t, uint8_t, uint8_t,
                                   uint8_t,
                                   uint16_t (*ProbingParser)(uint8_t *, uint32_t));
void AppLayerRegisterStateFuncs(uint16_t proto, void *(*StateAlloc)(void),
                                void (*StateFree)(void *));
void AppLayerRegisterLocalStorageFunc(uint16_t proto,
                                      void *(*LocalStorageAlloc)(void),
                                      void (*LocalStorageFree)(void *));
void *AppLayerGetProtocolParserLocalStorage(uint16_t);
void AppLayerRegisterGetFilesFunc(uint16_t proto,
        FileContainer *(*StateGetFile)(void *, uint8_t));
void AppLayerRegisterGetEventsFunc(uint16_t proto,
        AppLayerDecoderEvents *(*StateGetEvents)(void *, uint64_t));
void AppLayerRegisterHasEventsFunc(uint16_t proto,
        int (*StateHasEvents)(void *));

void AppLayerRegisterLogger(uint16_t proto);
uint16_t AppLayerGetProtoByName(const char *);
const char *AppLayerGetProtoString(int proto);
void AppLayerRegisterTruncateFunc(uint16_t proto, void (*Truncate)(void *, uint8_t));
void AppLayerRegisterGetAlstateProgressFunc(uint16_t alproto,
                                            int (*StateGetAlstateProgress)(void *alstate, uint8_t direction));
void AppLayerRegisterTxFreeFunc(uint16_t proto,
        void (*StateTransactionFree)(void *, uint64_t));
void AppLayerRegisterGetTxCnt(uint16_t alproto,
                              uint64_t (*StateGetTxCnt)(void *alstate));
void AppLayerRegisterGetTx(uint16_t alproto,
                           void *(*StateGetTx)(void *alstate, uint64_t tx_id));
void AppLayerRegisterGetAlstateProgressCompletionStatus(uint16_t alproto,
    int (*StateProgressCompletionStatus)(uint8_t direction));

int AppLayerParse(void *, Flow *, uint8_t,
                  uint8_t, uint8_t *, uint32_t);

int AlpParseFieldBySize(AppLayerParserResult *, AppLayerParserState *, uint16_t,
                        uint32_t, uint8_t *, uint32_t, uint32_t *);
int AlpParseFieldByEOF(AppLayerParserResult *, AppLayerParserState *, uint16_t,
                       uint8_t *, uint32_t);
int AlpParseFieldByDelimiter(AppLayerParserResult *, AppLayerParserState *,
                             uint16_t, const uint8_t *, uint8_t, uint8_t *,
                             uint32_t, uint32_t *);


/***** transaction handling *****/

/**
 * \brief Update the current log id.  Does one step increments currently.
 *
 * \param f Flow.
 */
void AppLayerTransactionUpdateLogId(Flow *f);

/**
 * \brief Get the current log id.
 *
 * \param f Flow.
 */
uint64_t AppLayerTransactionGetLogId(Flow *f);

/**
 * \brief Updates the inspection id for the alstate.
 *
 * \param f         Pointer to the flow(LOCKED).
 * \param direction Direction.  0 - toserver, 1 - toclient.
 */
void AppLayerTransactionUpdateInspectId(Flow *f, uint8_t direction);

/**
 * \brief Get the current tx id to be inspected.
 *
 * \param f     Flow.
 * \param flags Flags.
 *
 * \retval A positive integer value.
 */
uint64_t AppLayerTransactionGetInspectId(Flow *f, uint8_t flags);



void AppLayerSetEOF(Flow *);



/***** cleanup *****/

void AppLayerParserCleanupState(Flow *);
void AppLayerFreeProbingParsers(AppLayerProbingParser *);
void AppLayerFreeProbingParsersInfo(AppLayerProbingParserInfo *);
void AppLayerPrintProbingParsers(AppLayerProbingParser *);

void AppLayerListSupportedProtocols(void);
AppLayerDecoderEvents *AppLayerGetDecoderEventsForFlow(Flow *);
AppLayerDecoderEvents *AppLayerGetEventsFromFlowByTx(Flow *f, uint64_t tx_id);
int AppLayerProtoIsTxEventAware(uint16_t alproto);
int AppLayerFlowHasDecoderEvents(Flow *f, uint8_t flags);

/***** Alproto param retrieval ******/

/**
 * \brief get the version of the state in a direction
 *
 * \param f Flow(LOCKED).
 * \param direction STREAM_TOSERVER or STREAM_TOCLIENT
 */
uint16_t AppLayerGetStateVersion(Flow *f);

FileContainer *AppLayerGetFilesFromFlow(Flow *, uint8_t);

/**
 * \brief Get the state progress.
 *
 *        This is a generic wrapper to each ALPROTO.  The value returned
 *        needs to be interpreted by the caller, based on the ALPROTO_*
 *        the caller supplies.
 *
 *        The state can be anything based on what the ALPROTO handler
 *        expects.  We have given a return value of int, although a range
 *        of -128 to 127 (int8_t) should be more than sufficient.
 *
 * \param alproto The app protocol.
 * \param state   App state.
 * \param dir     Directin. 0 - ts, 1 - tc.
 *
 * \retval An integer value indicating the current progress of "state".
 */
int AppLayerGetAlstateProgress(uint16_t alproto, void *state, uint8_t direction);

/**
 * \brief Get the no of txs.
 *
 * \param alproto The app protocol.
 * \param alstate App state.
 *
 * \retval A positive integer value indicating the no of txs.
 */
uint64_t AppLayerGetTxCnt(uint16_t alproto, void *alstate);

/**
 * \brief Get a tx referenced by the id.
 *
 * \param alproto The app protocol
 * \param alstate App state.
 * \param tx_id   The transaction id.
 *
 * \retval Tx instance.
 */
void *AppLayerGetTx(uint16_t alproto, void *alstate, uint64_t tx_id);

/**
 * \brief Get the state value for the following alproto, that corresponds to
 *        COMPLETE or DONE.
 *
 * \param alproto   The app protocol.
 * \param direction The direction.  0 - ts, 1 - tc.
 *
 * \retval An integer value indicating the state value.
 */
int AppLayerGetAlstateProgressCompletionStatus(uint16_t alproto, uint8_t direction);

/**
 * \brief Informs if the alproto supports transactions or not.
 *
 * \param alproto   The app protocol.
 * \param direction The direction.  0 - ts, 1 - tc.
 *
 * \retval 1 If true; 0 If false.
 */
int AppLayerAlprotoSupportsTxs(uint16_t alproto);

void AppLayerTriggerRawStreamReassembly(Flow *);

#endif /* __APP_LAYER_PARSER_H__ */
