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
 */

#ifndef __UTIL_LUA_H__
#define __UTIL_LUA_H__

#ifdef HAVE_LUA

#define LUA_FLOW_LOCKED_BY_PARENT       0
#define LUA_FLOW_NOT_LOCKED_BY_PARENT   1

/* gets */

/** \brief get tv pointer from the lua state */
ThreadVars *LuaStateGetThreadVars(lua_State *luastate);

Packet *LuaStateGetPacket(lua_State *luastate);
void *LuaStateGetTX(lua_State *luastate);

/** \brief get flow pointer from lua state
 *
 *  \param locked_by_parent[out] bool indicating if flow is locked
 *                          (LUA_FLOW_LOCKED_BY_PARENT) or unlocked
 *                          (LUA_FLOW_NOT_LOCKED_BY_PARENT)
 *
 *  \retval f flow poiner or NULL if it was not set
 */
Flow *LuaStateGetFlow(lua_State *luastate, int *locked_by_parent);

PacketAlert *LuaStateGetPacketAlert(lua_State *luastate);

/** \brief get file pointer from the lua state */
File *LuaStateGetFile(lua_State *luastate);

/* sets */

void LuaStateSetPacket(lua_State *luastate, Packet *p);
void LuaStateSetTX(lua_State *luastate, void *tx);

/** \brief set a flow pointer in the lua state
 *
 *  \param f flow pointer
 *  \param locked_by_parent bool indicating if flow is locked
 *                          (LUA_FLOW_LOCKED_BY_PARENT) or unlocked
 *                          (LUA_FLOW_NOT_LOCKED_BY_PARENT)
 */
void LuaStateSetFlow(lua_State *luastate, Flow *f, int locked_by_parent);

void LuaStateSetPacketAlert(lua_State *luastate, PacketAlert *pa);

void LuaStateSetFile(lua_State *luastate, File *file);

void LuaStateSetThreadVars(lua_State *luastate, ThreadVars *tv);

void LuaPrintStack(lua_State *state);

#endif /* HAVE_LUA */

#endif /* __UTIL_LUA_H__ */
