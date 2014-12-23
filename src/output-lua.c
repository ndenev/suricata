/* Copyright (C) 2014 Open Information Security Foundation
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
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "pkt-var.h"
#include "conf.h"

#include "threads.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "util-print.h"
#include "util-unittest.h"

#include "util-debug.h"

#include "output.h"
#include "app-layer-htp.h"
#include "app-layer.h"
#include "app-layer-parser.h"
#include "util-privs.h"
#include "util-buffer.h"
#include "util-proto-name.h"
#include "util-logopenfile.h"
#include "util-time.h"

#ifdef HAVE_LUA

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "output-lua-common.h"
#include "output-lua-http.h"

#define MODULE_NAME "LuaLog"

typedef struct LogLuaCtx_ {
    SCMutex m;
    lua_State *luastate;
    int deinit_once;
} LogLuaCtx;

typedef struct LogLuaThreadCtx_ {
    LogLuaCtx *lua_ctx;
} LogLuaThreadCtx;

const char lualog_ext_key_tx[] = "suricata:lualog:tx:ptr";
/* key for p (packet) pointer */
const char lualog_ext_key_p[] = "suricata:lualog:pkt:ptr";

static int LuaTxLogger(ThreadVars *tv, void *thread_data, const Packet *p, Flow *f, void *alstate, void *txptr, uint64_t tx_id)
{
    SCEnter();

    LogLuaThreadCtx *td = (LogLuaThreadCtx *)thread_data;

    SCMutexLock(&td->lua_ctx->m);

    /* we need the tx in our callbacks */
    lua_pushlightuserdata(td->lua_ctx->luastate, (void *)&lualog_ext_key_tx);
    lua_pushlightuserdata(td->lua_ctx->luastate, (void *)txptr);
    lua_settable(td->lua_ctx->luastate, LUA_REGISTRYINDEX);

    /* prepare data to pass to script */
    lua_getglobal(td->lua_ctx->luastate, "log");
    lua_newtable(td->lua_ctx->luastate); /* stack at -1 */
    lua_pushliteral (td->lua_ctx->luastate, "tx_id"); /* stack at -2 */
    lua_pushnumber (td->lua_ctx->luastate, (int)(tx_id));
    lua_settable(td->lua_ctx->luastate, -3);

    int retval = lua_pcall(td->lua_ctx->luastate, 1, 0, 0);
    if (retval != 0) {
        SCLogInfo("failed to run script: %s", lua_tostring(td->lua_ctx->luastate, -1));
    }

    SCMutexUnlock(&td->lua_ctx->m);
    SCReturnInt(0);
}

/** \internal
 *  \brief Packet Logger for lua scripts, for alerts
 *
 *  A single call to this function will run one script for a single
 *  packet. If it is called, it means that the registered condition
 *  function has returned TRUE.
 *
 *  The script is called once for each alert stored in the packet.
 *
 *  NOTE: p->flow is UNlocked
 */
static int LuaPacketLoggerAlerts(ThreadVars *tv, void *thread_data, const Packet *p)
{
    LogLuaThreadCtx *td = (LogLuaThreadCtx *)thread_data;

    char timebuf[64];
    CreateTimeString(&p->ts, timebuf, sizeof(timebuf));

    char srcip[46], dstip[46];
    if (PKT_IS_IPV4(p)) {
        PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), srcip, sizeof(srcip));
        PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), dstip, sizeof(dstip));
    } else if (PKT_IS_IPV6(p)) {
        PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), srcip, sizeof(srcip));
        PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), dstip, sizeof(dstip));
    } else {
        /* decoder event */
        goto not_supported;
    }

    char proto[16] = "";
    if (SCProtoNameValid(IP_GET_IPPROTO(p)) == TRUE) {
        strlcpy(proto, known_proto[IP_GET_IPPROTO(p)], sizeof(proto));
    } else {
        snprintf(proto, sizeof(proto), "PROTO:%03" PRIu32, IP_GET_IPPROTO(p));
    }

    /* loop through alerts stored in the packet */
    SCMutexLock(&td->lua_ctx->m);
    uint16_t cnt;
    for (cnt = 0; cnt < p->alerts.cnt; cnt++) {
        const PacketAlert *pa = &p->alerts.alerts[cnt];
        if (unlikely(pa->s == NULL)) {
            continue;
        }

        lua_getglobal(td->lua_ctx->luastate, "log");

        /* prepare data to pass to script */
        lua_newtable(td->lua_ctx->luastate);

        LogLuaPushTableKeyValueInt(td->lua_ctx->luastate, "sid", pa->s->id);
        LogLuaPushTableKeyValueInt(td->lua_ctx->luastate, "gid", pa->s->gid);
        LogLuaPushTableKeyValueInt(td->lua_ctx->luastate, "rev", pa->s->rev);
        LogLuaPushTableKeyValueInt(td->lua_ctx->luastate, "priority", pa->s->prio);

        if (p->proto == IPPROTO_TCP || p->proto == IPPROTO_UDP) {
            LogLuaPushTableKeyValueInt(td->lua_ctx->luastate, "sp", p->sp);
            LogLuaPushTableKeyValueInt(td->lua_ctx->luastate, "dp", p->dp);
        }

        LogLuaPushTableKeyValueString(td->lua_ctx->luastate, "msg", pa->s->msg);
        LogLuaPushTableKeyValueString(td->lua_ctx->luastate, "srcip", srcip);
        LogLuaPushTableKeyValueString(td->lua_ctx->luastate, "dstip", dstip);
        LogLuaPushTableKeyValueString(td->lua_ctx->luastate, "ts", timebuf);
        LogLuaPushTableKeyValueString(td->lua_ctx->luastate, "ipproto", proto);
        LogLuaPushTableKeyValueString(td->lua_ctx->luastate, "class", pa->s->class_msg);

        int retval = lua_pcall(td->lua_ctx->luastate, 1, 0, 0);
        if (retval != 0) {
            SCLogInfo("failed to run script: %s", lua_tostring(td->lua_ctx->luastate, -1));
        }
    }
    SCMutexUnlock(&td->lua_ctx->m);
not_supported:
    SCReturnInt(0);
}

static int LuaPacketConditionAlerts(ThreadVars *tv, const Packet *p)
{
    if (p->alerts.cnt > 0)
        return TRUE;
    return FALSE;
}

/** \internal
 *  \brief File API Logger function for Lua scripts
 *
 *  Executes a script once for one file.
 *
 * TODO non-http support
 *
 * NOTE p->flow is locked at this point
 */
static int LuaFileLogger(ThreadVars *tv, void *thread_data, const Packet *p, const File *ff)
{
    SCEnter();
    LogLuaThreadCtx *td = (LogLuaThreadCtx *)thread_data;

    if ((!(PKT_IS_IPV4(p))) && (!(PKT_IS_IPV6(p))))
        return 0;

    BUG_ON(ff->flags & FILE_LOGGED);

    SCLogDebug("ff %p", ff);

    /* Get the TX so the script can get more context about it.
     * TODO hardcoded to HTTP currently */
    void *txptr = NULL;
    if (p && p->flow && p->flow->alstate)
        txptr = AppLayerParserGetTx(p->proto, ALPROTO_HTTP, p->flow->alstate, ff->txid);

    SCMutexLock(&td->lua_ctx->m);

    /* we need the p in our callbacks */
    lua_pushlightuserdata(td->lua_ctx->luastate, (void *)&lualog_ext_key_p);
    lua_pushlightuserdata(td->lua_ctx->luastate, (void *)p);
    lua_settable(td->lua_ctx->luastate, LUA_REGISTRYINDEX);
    /* we need the tx in our callbacks */
    lua_pushlightuserdata(td->lua_ctx->luastate, (void *)&lualog_ext_key_tx);
    lua_pushlightuserdata(td->lua_ctx->luastate, (void *)txptr);
    lua_settable(td->lua_ctx->luastate, LUA_REGISTRYINDEX);

    /* get the lua function to call */
    lua_getglobal(td->lua_ctx->luastate, "log");

    /* prepare data to pass to script */
    lua_newtable(td->lua_ctx->luastate);

    LogLuaPushTableKeyValueArray(td->lua_ctx->luastate, "filename", ff->name, ff->name_len);
    LogLuaPushTableKeyValueString(td->lua_ctx->luastate, "filemagic", ff->magic);
    LogLuaPushTableKeyValueArray(td->lua_ctx->luastate, "filemd5", ff->md5, sizeof(ff->md5));

    //LuaPrintStack(td->lua_ctx->luastate);
    int retval = lua_pcall(td->lua_ctx->luastate, 1, 0, 0);
    if (retval != 0) {
        SCLogInfo("failed to run script: %s", lua_tostring(td->lua_ctx->luastate, -1));
    }
    SCMutexUnlock(&td->lua_ctx->m);
    return 0;
}

typedef struct LogLuaScriptOptions_ {
    AppProto alproto;
    int packet;
    int alerts;
    int file;
} LogLuaScriptOptions;

/** \brief load and evaluate the script
 *
 *  This function parses the script, checks if all the required functions
 *  are defined and runs the 'init' function. The init function will inform
 *  us what the scripts needs are.
 */
static int LuaScriptInit(const char *filename, LogLuaScriptOptions *options) {
    int status;

    lua_State *luastate = luaL_newstate();
    if (luastate == NULL)
        goto error;
    luaL_openlibs(luastate);

    /* hackish, needed to allow unittests to pass buffers as scripts instead of files */
#if 0//def UNITTESTS
    if (ut_script != NULL) {
        status = luaL_loadbuffer(luastate, ut_script, strlen(ut_script), "unittest");
        if (status) {
            SCLogError(SC_ERR_LUAJIT_ERROR, "couldn't load file: %s", lua_tostring(luastate, -1));
            goto error;
        }
    } else {
#endif
        status = luaL_loadfile(luastate, filename);
        if (status) {
            SCLogError(SC_ERR_LUAJIT_ERROR, "couldn't load file: %s", lua_tostring(luastate, -1));
            goto error;
        }
#if 0//def UNITTESTS
    }
#endif

    /* prime the script (or something) */
    if (lua_pcall(luastate, 0, 0, 0) != 0) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "couldn't prime file: %s", lua_tostring(luastate, -1));
        goto error;
    }

    lua_getglobal(luastate, "init");
    if (lua_type(luastate, -1) != LUA_TFUNCTION) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "no init function in script");
        goto error;
    }

    lua_newtable(luastate); /* stack at -1 */
    if (lua_gettop(luastate) == 0 || lua_type(luastate, 2) != LUA_TTABLE) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "no table setup");
        goto error;
    }

    lua_pushliteral(luastate, "script_api_ver"); /* stack at -2 */
    lua_pushnumber (luastate, 1); /* stack at -3 */
    lua_settable(luastate, -3);

    if (lua_pcall(luastate, 1, 1, 0) != 0) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "couldn't run script 'init' function: %s", lua_tostring(luastate, -1));
        goto error;
    }

    /* process returns from script */
    if (lua_gettop(luastate) == 0) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "init function in script should return table, nothing returned");
        goto error;
    }
    if (lua_type(luastate, 1) != LUA_TTABLE) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "init function in script should return table, returned is not table");
        goto error;
    }

    lua_pushnil(luastate);
    const char *k, *v;
    while (lua_next(luastate, -2)) {
        k = lua_tostring(luastate, -2);
        if (k == NULL)
            continue;

        v = lua_tostring(luastate, -1);
        lua_pop(luastate, 1);
        if (v == NULL)
            continue;

        SCLogDebug("k='%s', v='%s'", k, v);

        if (strcmp(k,"protocol") == 0 && strcmp(v, "http") == 0)
            options->alproto = ALPROTO_HTTP;
        else if (strcmp(k, "type") == 0 && strcmp(v, "packet") == 0)
            options->packet = 1;
        else if (strcmp(k, "filter") == 0 && strcmp(v, "alerts") == 0)
            options->alerts = 1;
        else if (strcmp(k, "type") == 0 && strcmp(v, "file") == 0)
            options->file = 1;
        else
            SCLogInfo("unknown key and/or value: k='%s', v='%s'", k, v);
    }

    if (options->alproto + options->packet + options->file > 1) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "invalid combination of 'needs' in the script");
        goto error;
    }

    lua_getglobal(luastate, "setup");
    if (lua_type(luastate, -1) != LUA_TFUNCTION) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "no setup function in script");
        goto error;
    }

    lua_getglobal(luastate, "log");
    if (lua_type(luastate, -1) != LUA_TFUNCTION) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "no log function in script");
        goto error;
    }

    lua_getglobal(luastate, "deinit");
    if (lua_type(luastate, -1) != LUA_TFUNCTION) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "no deinit function in script");
        goto error;
    }

    /* pop the table */
    lua_pop(luastate, 1);
    lua_close(luastate);
    return 0;
error:
    lua_close(luastate);
    return -1;
}

/** \brief setup a luastate for use at runtime
 *
 *  This loads the script, primes it and then runs the 'setup' function.
 */
static lua_State *LuaScriptSetup(const char *filename)
{
    lua_State *luastate = luaL_newstate();
    if (luastate == NULL) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "luaL_newstate failed");
        goto error;
    }

    luaL_openlibs(luastate);

    int status;
    /* hackish, needed to allow unittests to pass buffers as scripts instead of files */
#if 0//def UNITTESTS
    if (ut_script != NULL) {
        status = luaL_loadbuffer(t->luastate, ut_script, strlen(ut_script), "unittest");
        if (status) {
            SCLogError(SC_ERR_LUAJIT_ERROR, "couldn't load file: %s", lua_tostring(t->luastate, -1));
            goto error;
        }
    } else {
#endif
        status = luaL_loadfile(luastate, filename);
        if (status) {
            SCLogError(SC_ERR_LUAJIT_ERROR, "couldn't load file: %s", lua_tostring(luastate, -1));
            goto error;
        }
#if 0//def UNITTESTS
    }
#endif

    /* prime the script (or something) */
    if (lua_pcall(luastate, 0, 0, 0) != 0) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "couldn't prime file: %s", lua_tostring(luastate, -1));
        goto error;
    }

    lua_getglobal(luastate, "setup");
    if (lua_type(luastate, -1) != LUA_TFUNCTION) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "no init function in script");
        goto error;
    }

    //LuaPrintStack(luastate);
    if (lua_pcall(luastate, 0, 0, 0) != 0) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "couldn't run script 'setup' function: %s", lua_tostring(luastate, -1));
        goto error;
    }
    //LuaPrintStack(luastate);

    /* register functions common to all */
    LogLuaRegisterFunctions(luastate);
    /* unconditionally register http function. They will only work
     * if the tx is registered in the state at runtime though. */
    LogLuaRegisterHttpFunctions(luastate);

    SCLogDebug("lua_State %p is set up", luastate);
    return luastate;
error:
    lua_close(luastate);
    return NULL;
}

static OutputCtx *OutputLuaLogInitSub(ConfNode *conf, OutputCtx *parent_ctx)
{
    if (conf == NULL)
        return NULL;

    LogLuaCtx *lua_ctx = SCMalloc(sizeof(LogLuaCtx));
    if (unlikely(lua_ctx == NULL))
        return NULL;
    memset(lua_ctx, 0x00, sizeof(*lua_ctx));

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        SCFree(lua_ctx);
        return NULL;
    }

    SCMutexInit(&lua_ctx->m, NULL);

    //    SCLogInfo("script %s", conf->val);

    SCMutexLock(&lua_ctx->m);
    lua_ctx->luastate = LuaScriptSetup(conf->val);
    SCMutexUnlock(&lua_ctx->m);
    if (lua_ctx->luastate == NULL)
        goto error;

    SCLogDebug("lua_ctx %p", lua_ctx);

    output_ctx->data = lua_ctx;
    output_ctx->DeInit = NULL;

    return output_ctx;
error:
    SCMutexDestroy(&lua_ctx->m);
    SCFree(lua_ctx);
    SCFree(output_ctx);
    return NULL;
}

static OutputCtx *OutputLuaLogInit(ConfNode *conf)
{
    ConfNode *scripts = ConfNodeLookupChild(conf, "scripts");
    if (scripts == NULL) {
        /* No "outputs" section in the configuration. */
        SCLogInfo("scripts not defined");
        return NULL;
    }

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        return NULL;
    }
    TAILQ_INIT(&output_ctx->submodules);

    ConfNode *script;
    TAILQ_FOREACH(script, &scripts->head, next) {
        SCLogInfo("script %s", script->val);
        LogLuaScriptOptions opts;
        memset(&opts, 0x00, sizeof(opts));

        int r = LuaScriptInit(script->val, &opts);
        if (r != 0) {
            SCLogInfo("script init failed (%d)", r);
            continue;
        }

        /* create an OutputModule for this script, based
         * on it's needs. */
        OutputModule *om = SCCalloc(1, sizeof(*om));
        BUG_ON(om == NULL); //TODO

        om->name = MODULE_NAME;
        om->conf_name = script->val;
        om->InitSubFunc = OutputLuaLogInitSub;

        if (opts.alproto == ALPROTO_HTTP) {
            om->TxLogFunc = LuaTxLogger;
            om->alproto = ALPROTO_HTTP;
        } else if (opts.packet && opts.alerts) {
            om->PacketLogFunc = LuaPacketLoggerAlerts;
            om->PacketConditionFunc = LuaPacketConditionAlerts;
        } else if (opts.file) {
            om->FileLogFunc = LuaFileLogger;
        }

        TAILQ_INSERT_TAIL(&output_ctx->submodules, om, entries);
    }

    return output_ctx;
}

static void OutputLuaLogDoDeinit(LogLuaCtx *lua_ctx)
{
    lua_State *luastate = lua_ctx->luastate;

    lua_getglobal(luastate, "deinit");
    if (lua_type(luastate, -1) != LUA_TFUNCTION) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "no deinit function in script");
        return;
    }
    //LuaPrintStack(luastate);

    if (lua_pcall(luastate, 0, 0, 0) != 0) {
        SCLogError(SC_ERR_LUAJIT_ERROR, "couldn't run script 'deinit' function: %s", lua_tostring(luastate, -1));
        return;
    }
}

static TmEcode LuaLogThreadInit(ThreadVars *t, void *initdata, void **data)
{
    LogLuaThreadCtx *td = SCMalloc(sizeof(*td));
    if (unlikely(td == NULL))
        return TM_ECODE_FAILED;
    memset(td, 0, sizeof(*td));

    if (initdata == NULL) {
        SCLogDebug("Error getting context for LuaLog. \"initdata\" argument NULL");
        SCFree(td);
        return TM_ECODE_FAILED;
    }

    LogLuaCtx *lua_ctx = ((OutputCtx *)initdata)->data;
    SCLogDebug("lua_ctx %p", lua_ctx);
    td->lua_ctx = lua_ctx;
    *data = (void *)td;
    return TM_ECODE_OK;
}

static TmEcode LuaLogThreadDeinit(ThreadVars *t, void *data)
{
    LogLuaThreadCtx *td = (LogLuaThreadCtx *)data;
    if (td == NULL) {
        return TM_ECODE_OK;
    }

    SCMutexLock(&td->lua_ctx->m);
    if (td->lua_ctx->deinit_once == 0) {
        OutputLuaLogDoDeinit(td->lua_ctx);
        td->lua_ctx->deinit_once = 1;
    }
    SCMutexUnlock(&td->lua_ctx->m);

    /* clear memory */
    memset(td, 0, sizeof(*td));

    SCFree(td);
    return TM_ECODE_OK;
}

void TmModuleLuaLogRegister (void) {
    tmm_modules[TMM_LUALOG].name = MODULE_NAME;
    tmm_modules[TMM_LUALOG].ThreadInit = LuaLogThreadInit;
    tmm_modules[TMM_LUALOG].ThreadDeinit = LuaLogThreadDeinit;
    tmm_modules[TMM_LUALOG].RegisterTests = NULL;
    tmm_modules[TMM_LUALOG].cap_flags = 0;
    tmm_modules[TMM_LUALOG].flags = TM_FLAG_LOGAPI_TM;

    /* register as separate module */
    OutputRegisterModule(MODULE_NAME, "lua", OutputLuaLogInit);
    SCLogInfo("registered");
}

#else

void TmModuleLuaLogRegister (void) {
    /* no-op */
}

#endif
