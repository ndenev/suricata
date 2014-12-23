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
 *
 * Functions to expose to the lua scripts.
 */

#include "suricata-common.h"
#include "conf.h"

#include "threads.h"
#include "debug.h"
#include "decode.h"

#include "detect.h"
#include "detect-parse.h"
#include "detect-flowvar.h"

#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-state.h"

#include "flow.h"
#include "flow-var.h"
#include "flow-util.h"

#include "util-debug.h"
#include "util-spm-bm.h"
#include "util-print.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"

#include "app-layer.h"

#include "stream-tcp.h"

#include "detect-luajit.h"

#include "queue.h"
#include "util-cpu.h"

#ifdef HAVE_LUAJIT

static const char luaext_key_ld[] = "suricata:luajitdata";
static const char luaext_key_det_ctx[] = "suricata:det_ctx";
static const char luaext_key_flow[] = "suricata:flow";
static const char luaext_key_need_flow_lock[] = "suricata:need_flow_lock";

static int LuajitGetFlowvar(lua_State *luastate) {
    uint16_t idx;
    int id;
    Flow *f;
    FlowVar *fv;
    DetectLuajitData *ld;
    int need_flow_lock = 0;

    /* need luajit data for id -> idx conversion */
    lua_pushlightuserdata(luastate, (void *)&luaext_key_ld);
    lua_gettable(luastate, LUA_REGISTRYINDEX);
    ld = lua_touserdata(luastate, -1);
    SCLogDebug("ld %p", ld);
    if (ld == NULL) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "internal error: no ld");
        return 2;
    }

    /* need flow */
    lua_pushlightuserdata(luastate, (void *)&luaext_key_flow);
    lua_gettable(luastate, LUA_REGISTRYINDEX);
    f = lua_touserdata(luastate, -1);
    SCLogDebug("f %p", f);
    if (f == NULL) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "no flow");
        return 2;
    }

    /* need flow lock hint */
    lua_pushlightuserdata(luastate, (void *)&luaext_key_need_flow_lock);
    lua_gettable(luastate, LUA_REGISTRYINDEX);
    need_flow_lock = lua_toboolean(luastate, -1);

    /* need flowvar idx */
    if (!lua_isnumber(luastate, 1)) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "1st arg not a number");
        return 2;
    }
    id = lua_tonumber(luastate, 1);
    if (id < 0 || id >= DETECT_LUAJIT_MAX_FLOWVARS) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "flowvar id out of range");
        return 2;
    }
    idx = ld->flowvar[id];
    if (idx == 0) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "flowvar id uninitialized");
        return 2;
    }

    /* lookup var */
    if (need_flow_lock)
        FLOWLOCK_RDLOCK(f);

    fv = FlowVarGet(f, idx);
    if (fv == NULL) {
        if (need_flow_lock)
            FLOWLOCK_UNLOCK(f);

        lua_pushnil(luastate);
        lua_pushstring(luastate, "no flow var");
        return 2;
    }

    //SCLogInfo("returning:");
    //PrintRawDataFp(stdout,fv->data.fv_str.value,fv->data.fv_str.value_len);

    /* we're using a buffer sized at a multiple of 4 as lua_pushlstring generates
     * invalid read errors in valgrind otherwise. Adding in a nul to be sure.
     *
     * Buffer size = len + 1 (for nul) + whatever makes it a multiple of 4 */
    size_t buflen = fv->data.fv_str.value_len + 1 + ((fv->data.fv_str.value_len + 1) % 4);
    uint8_t buf[buflen];
    memset(buf, 0x00, buflen);
    memcpy(buf, fv->data.fv_str.value, fv->data.fv_str.value_len);
    buf[fv->data.fv_str.value_len] = '\0';

    if (need_flow_lock)
        FLOWLOCK_UNLOCK(f);

    /* return value through luastate, as a luastring */
    lua_pushlstring(luastate, (char *)buf, buflen);

    return 1;

}

int LuajitSetFlowvar(lua_State *luastate) {
    uint16_t idx;
    int id;
    Flow *f;
    const char *str;
    int len;
    uint8_t *buffer;
    DetectEngineThreadCtx *det_ctx;
    DetectLuajitData *ld;

    /* need luajit data for id -> idx conversion */
    lua_pushlightuserdata(luastate, (void *)&luaext_key_ld);
    lua_gettable(luastate, LUA_REGISTRYINDEX);
    ld = lua_touserdata(luastate, -1);
    SCLogDebug("ld %p", ld);
    if (ld == NULL) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "internal error: no ld");
        return 2;
    }

    /* need det_ctx */
    lua_pushlightuserdata(luastate, (void *)&luaext_key_det_ctx);
    lua_gettable(luastate, LUA_REGISTRYINDEX);
    det_ctx = lua_touserdata(luastate, -1);
    SCLogDebug("det_ctx %p", det_ctx);
    if (det_ctx == NULL) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "internal error: no det_ctx");
        return 2;
    }

    /* need flow */
    lua_pushlightuserdata(luastate, (void *)&luaext_key_flow);
    lua_gettable(luastate, LUA_REGISTRYINDEX);
    f = lua_touserdata(luastate, -1);
    SCLogDebug("f %p", f);
    if (f == NULL) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "no flow");
        return 2;
    }

    /* need flowvar idx */
    if (!lua_isnumber(luastate, 1)) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "1st arg not a number");
        return 2;
    }
    id = lua_tonumber(luastate, 1);
    if (id < 0 || id >= DETECT_LUAJIT_MAX_FLOWVARS) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "flowvar id out of range");
        return 2;
    }

    if (!lua_isstring(luastate, 2)) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "2nd arg not a string");
        return 2;
    }
    str = lua_tostring(luastate, 2);
    if (str == NULL) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "null string");
        return 2;
    }

    if (!lua_isnumber(luastate, 3)) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "3rd arg not a number");
        return 2;
    }
    len = lua_tonumber(luastate, 3);
    if (len < 0 || len > 0xffff) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "len out of range: max 64k");
        return 2;
    }

    idx = ld->flowvar[id];
    if (idx == 0) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "flowvar id uninitialized");
        return 2;
    }

    buffer = SCMalloc(len+1);
    if (buffer == NULL) {
        lua_pushnil(luastate);
        lua_pushstring(luastate, "out of memory");
        return 2;
    }
    memcpy(buffer, str, len);
    buffer[len] = '\0';

    if (DetectFlowvarStoreMatch(det_ctx, idx, buffer, len,
                DETECT_FLOWVAR_TYPE_ALWAYS) < 0) {
        SCLogInfo("store failed");
        SCFree(buffer);

        lua_pushnil(luastate);
        lua_pushstring(luastate, "store failed");
        return 2;
    }
    //SCLogInfo("stored:");
    //PrintRawDataFp(stdout,buffer,len);
    return 0;
}

void LuajitExtensionsMatchSetup(lua_State *lua_state, DetectLuajitData *ld, DetectEngineThreadCtx *det_ctx, Flow *f, int need_flow_lock) {
    SCLogDebug("det_ctx %p, f %p", det_ctx, f);

    /* luajit keyword data */
    lua_pushlightuserdata(lua_state, (void *)&luaext_key_ld);
    lua_pushlightuserdata(lua_state, (void *)ld);
    lua_settable(lua_state, LUA_REGISTRYINDEX);

    /* detection engine thread ctx */
    lua_pushlightuserdata(lua_state, (void *)&luaext_key_det_ctx);
    lua_pushlightuserdata(lua_state, (void *)det_ctx);
    lua_settable(lua_state, LUA_REGISTRYINDEX);

    /* flow */
    lua_pushlightuserdata(lua_state, (void *)&luaext_key_flow);
    lua_pushlightuserdata(lua_state, (void *)f);
    lua_settable(lua_state, LUA_REGISTRYINDEX);

    /* flow lock status hint */
    lua_pushlightuserdata(lua_state, (void *)&luaext_key_need_flow_lock);
    lua_pushboolean(lua_state, need_flow_lock);
    lua_settable(lua_state, LUA_REGISTRYINDEX);
}

/**
 *  \brief Register Suricata Lua functions
 */
int LuajitRegisterExtensions(lua_State *lua_state) {
    lua_pushcfunction(lua_state, LuajitGetFlowvar);
    lua_setglobal(lua_state, "ScFlowvarGet");

    lua_pushcfunction(lua_state, LuajitSetFlowvar);
    lua_setglobal(lua_state, "ScFlowvarSet");
    return 0;
}

#endif /* HAVE_LUAJIT */
