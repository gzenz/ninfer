"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.wrapCommand_ = exports.beforeHooks = exports.afterHooks = exports.getHooksFromPath = exports.createContext = void 0;
const truncate = require("truncate");
const configs = require("./configs");
const exec = require("./exec");
const logger = require("./logger");
const cmd_1 = require("./cmd");
const fp_1 = require("./fp");
const testing = process.env.NODE_ENV === 'testing';
function createContext(scope) {
    return {
        options: scope.options,
        signature: scope.options.config.signature,
    };
}
exports.createContext = createContext;
function getHooksFromPath(path, config) {
    const keys = path.split('.');
    let key = keys.shift();
    let hooks;
    hooks = config.hooks || {};
    while (hooks[key]) {
        hooks = hooks[key];
        key = keys.shift();
    }
    return Array.isArray(hooks) ? hooks : [];
}
exports.getHooksFromPath = getHooksFromPath;
async function afterHooks(path, scope) {
    const after = getHooksFromPath(`${path}.after`, scope.options.config);
    const options = scope.options;
    if (options.hooks === false || process.env.NODEGH_HOOK_IS_LOCKED) {
        return;
    }
    let context = createContext(scope);
    if (!testing) {
        const pluginContext = await setupPlugins_(context, 'setupAfterHooks');
        if (pluginContext) {
            context = Object.assign(Object.assign({}, context), pluginContext);
        }
    }
    after.forEach(cmd => {
        wrapCommand_(cmd, context, 'after');
    });
    process.env.NODEGH_HOOK_IS_LOCKED = 'true';
}
exports.afterHooks = afterHooks;
async function beforeHooks(path, scope) {
    const before = getHooksFromPath(`${path}.before`, scope.options.config);
    const options = scope.options;
    if (options.hooks === false || process.env.NODEGH_HOOK_IS_LOCKED) {
        return;
    }
    let context = createContext(scope);
    if (!testing) {
        const pluginContext = await setupPlugins_(context, 'setupBeforeHooks');
        if (pluginContext) {
            context = Object.assign(Object.assign({}, context), pluginContext);
        }
    }
    before.forEach(cmd => {
        wrapCommand_(cmd, context, 'before');
    });
}
exports.beforeHooks = beforeHooks;
async function setupPlugins_(context, setupFn) {
    const plugins = configs.getPlugins();
    return Promise.all(plugins.map(async (pluginName) => {
        // Slice off extra 'gh-' so it isn't 'gh-gh-'
        const name = pluginName.slice(3);
        try {
            var pluginFile = await cmd_1.tryResolvingByPlugin(name)
                .chain(fp_1.safeImport)
                .promise();
        }
        catch (e) {
            logger.warn(`Can't get ${name} plugin.`);
        }
        if (pluginFile && configs.pluginHasConfig(pluginName) && pluginFile[setupFn]) {
            // TODO: find a better state sharing mechanism than mutation
            // Currently our approach is to give each plugin a chance to
            // update the main options object for the before & after hooks
            pluginFile[setupFn](context);
        }
    }));
}
function wrapCommand_(cmd, context, when) {
    const raw = logger.compileTemplate(cmd, context);
    if (!raw) {
        return;
    }
    logger.log(logger.colors.cyan('[hook]'), truncate(raw.trim(), 120));
    if (testing)
        return;
    try {
        exec.execSyncInteractiveStream(raw, { cwd: process.cwd() });
    }
    catch (e) {
        logger.debug(`[${when} hook failure]`);
    }
    finally {
        logger.debug(logger.colors.cyan(`[end of ${when} hook]`));
    }
}
exports.wrapCommand_ = wrapCommand_;
//# sourceMappingURL=hooks.js.map