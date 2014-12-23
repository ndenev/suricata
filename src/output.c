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
 * \author Endace Technology Limited, Jason Ish <jason.ish@endace.com>
 *
 * Output registration functions
 */

#include "suricata-common.h"
#include "flow.h"
#include "conf.h"
#include "tm-threads.h"
#include "util-error.h"
#include "util-debug.h"
#include "output.h"

static TAILQ_HEAD(, OutputModule_) output_modules =
    TAILQ_HEAD_INITIALIZER(output_modules);

/**
 * \brief Register an output module.
 *
 * This function will register an output module so it can be
 * configured with the configuration file.
 *
 * \retval Returns 0 on success, -1 on failure.
 */
void
OutputRegisterModule(char *name, char *conf_name,
    OutputCtx *(*InitFunc)(ConfNode *))
{
    OutputModule *module = SCCalloc(1, sizeof(*module));
    if (unlikely(module == NULL))
        goto error;

    module->name = SCStrdup(name);
    if (unlikely(module->name == NULL))
        goto error;
    module->conf_name = SCStrdup(conf_name);
    if (unlikely(module->conf_name == NULL))
        goto error;
    module->InitFunc = InitFunc;
    TAILQ_INSERT_TAIL(&output_modules, module, entries);

    SCLogDebug("Output module \"%s\" registered.", name);

    return;

error:
    SCLogError(SC_ERR_FATAL, "Fatal error encountered in OutputRegisterModule. Exiting...");
    exit(EXIT_FAILURE);
}

/**
 * \brief Register a packet output module.
 *
 * This function will register an output module so it can be
 * configured with the configuration file.
 *
 * \retval Returns 0 on success, -1 on failure.
 */
void
OutputRegisterPacketModule(char *name, char *conf_name,
    OutputCtx *(*InitFunc)(ConfNode *),
    PacketLogger PacketLogFunc, PacketLogCondition PacketConditionFunc)
{
    if (unlikely(PacketLogFunc == NULL || PacketConditionFunc == NULL)) {
        goto error;
    }

    OutputModule *module = SCCalloc(1, sizeof(*module));
    if (unlikely(module == NULL)) {
        goto error;
    }

    module->name = SCStrdup(name);
    if (unlikely(module->name == NULL))
        goto error;
    module->conf_name = SCStrdup(conf_name);
    if (unlikely(module->conf_name == NULL))
        goto error;
    module->InitFunc = InitFunc;
    module->PacketLogFunc = PacketLogFunc;
    module->PacketConditionFunc = PacketConditionFunc;
    TAILQ_INSERT_TAIL(&output_modules, module, entries);

    SCLogDebug("Packet logger \"%s\" registered.", name);
    return;
error:
    SCLogError(SC_ERR_FATAL, "Fatal error encountered. Exiting...");
    exit(EXIT_FAILURE);
}

/**
 * \brief Register a tx output module.
 *
 * This function will register an output module so it can be
 * configured with the configuration file.
 *
 * \retval Returns 0 on success, -1 on failure.
 */
void
OutputRegisterTxModule(char *name, char *conf_name,
    OutputCtx *(*InitFunc)(ConfNode *), uint16_t alproto,
    TxLogger TxLogFunc)
{
    if (unlikely(TxLogFunc == NULL)) {
        goto error;
    }

    OutputModule *module = SCCalloc(1, sizeof(*module));
    if (unlikely(module == NULL)) {
        goto error;
    }

    module->name = SCStrdup(name);
    if (unlikely(module->name == NULL))
        goto error;
    module->conf_name = SCStrdup(conf_name);
    if (unlikely(module->conf_name == NULL))
        goto error;
    module->InitFunc = InitFunc;
    module->TxLogFunc = TxLogFunc;
    module->alproto = alproto;
    TAILQ_INSERT_TAIL(&output_modules, module, entries);

    SCLogDebug("Tx logger \"%s\" registered.", name);
    return;
error:
    SCLogError(SC_ERR_FATAL, "Fatal error encountered. Exiting...");
    exit(EXIT_FAILURE);
}

void
OutputRegisterTxSubModule(const char *parent_name, char *name, char *conf_name,
    OutputCtx *(*InitFunc)(ConfNode *, OutputCtx *parent_ctx), uint16_t alproto,
    TxLogger TxLogFunc)
{
    if (unlikely(TxLogFunc == NULL)) {
        goto error;
    }

    OutputModule *module = SCCalloc(1, sizeof(*module));
    if (unlikely(module == NULL)) {
        goto error;
    }

    module->name = SCStrdup(name);
    if (unlikely(module->name == NULL))
        goto error;
    module->conf_name = SCStrdup(conf_name);
    if (unlikely(module->conf_name == NULL))
        goto error;
    module->parent_name = SCStrdup(parent_name);
    if (unlikely(module->parent_name == NULL))
        goto error;
    module->InitSubFunc = InitFunc;
    module->TxLogFunc = TxLogFunc;
    module->alproto = alproto;
    TAILQ_INSERT_TAIL(&output_modules, module, entries);

    SCLogDebug("Tx logger \"%s\" registered.", name);
    return;
error:
    SCLogError(SC_ERR_FATAL, "Fatal error encountered. Exiting...");
    exit(EXIT_FAILURE);
}

/**
 * \brief Register a file output module.
 *
 * This function will register an output module so it can be
 * configured with the configuration file.
 *
 * \retval Returns 0 on success, -1 on failure.
 */
void
OutputRegisterFileModule(char *name, char *conf_name,
    OutputCtx *(*InitFunc)(ConfNode *), FileLogger FileLogFunc)
{
    if (unlikely(FileLogFunc == NULL)) {
        goto error;
    }

    OutputModule *module = SCCalloc(1, sizeof(*module));
    if (unlikely(module == NULL)) {
        goto error;
    }

    module->name = SCStrdup(name);
    if (unlikely(module->name == NULL))
        goto error;
    module->conf_name = SCStrdup(conf_name);
    if (unlikely(module->conf_name == NULL))
        goto error;
    module->InitFunc = InitFunc;
    module->FileLogFunc = FileLogFunc;
    TAILQ_INSERT_TAIL(&output_modules, module, entries);

    SCLogDebug("File logger \"%s\" registered.", name);
    return;
error:
    SCLogError(SC_ERR_FATAL, "Fatal error encountered. Exiting...");
    exit(EXIT_FAILURE);
}

/**
 * \brief Register a file data output module.
 *
 * This function will register an output module so it can be
 * configured with the configuration file.
 *
 * \retval Returns 0 on success, -1 on failure.
 */
void
OutputRegisterFiledataModule(char *name, char *conf_name,
    OutputCtx *(*InitFunc)(ConfNode *), FiledataLogger FiledataLogFunc)
{
    if (unlikely(FiledataLogFunc == NULL)) {
        goto error;
    }

    OutputModule *module = SCCalloc(1, sizeof(*module));
    if (unlikely(module == NULL)) {
        goto error;
    }

    module->name = SCStrdup(name);
    if (unlikely(module->name == NULL))
        goto error;
    module->conf_name = SCStrdup(conf_name);
    if (unlikely(module->conf_name == NULL))
        goto error;
    module->InitFunc = InitFunc;
    module->FiledataLogFunc = FiledataLogFunc;
    TAILQ_INSERT_TAIL(&output_modules, module, entries);

    SCLogDebug("Filedata logger \"%s\" registered.", name);
    return;
error:
    SCLogError(SC_ERR_FATAL, "Fatal error encountered. Exiting...");
    exit(EXIT_FAILURE);
}

/**
 * \brief Get an output module by name.
 *
 * \retval The OutputModule with the given name or NULL if no output module
 * with the given name is registered.
 */
OutputModule *
OutputGetModuleByConfName(char *conf_name)
{
    OutputModule *module;

    TAILQ_FOREACH(module, &output_modules, entries) {
        if (strcmp(module->conf_name, conf_name) == 0)
            return module;
    }

    return NULL;
}

/**
 * \brief Deregister all modules.  Useful for a memory clean exit.
 */
void
OutputDeregisterAll(void)
{
    OutputModule *module;

    while ((module = TAILQ_FIRST(&output_modules))) {
        TAILQ_REMOVE(&output_modules, module, entries);
        SCFree(module->name);
        SCFree(module->conf_name);
        SCFree(module);
    }
}
