"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.pluginHasConfig = exports.getPlugins = exports.addPluginConfig = exports.writeGlobalConfigCredentials = exports.saveJsonConfig = exports.writeGlobalConfig = exports.createGlobalConfig = exports.removeGlobalConfig = exports.getGlobalConfig = exports.getConfig = exports.getUserHomePath = exports.getProjectConfigPath = exports.getDefaultConfigPath = exports.getGlobalPackageJson = exports.getNodeModulesGlobalPath = exports.getPlugin = exports.getPluginPath = exports.PLUGINS_PATH_KEY = void 0;
const fs = require("fs");
const lodash_1 = require("lodash");
const path = require("path");
const R = require("ramda");
const S = require("sanctuary");
const userhome = require("userhome");
const which = require("which");
const exec = require("./exec");
const logger = require("./logger");
const fp_1 = require("./fp");
exports.PLUGINS_PATH_KEY = 'plugins_path';
const testing = process.env.NODE_ENV === 'testing';
/* Refactored FP Functions */
const safeWhich = S.encase(which.sync);
const safeRealpath = S.encase(fs.realpathSync);
exports.getPluginPath = R.pipeK(safeWhich, safeRealpath);
// TODO merge with getPlugin fn in cmd.ts
exports.getPlugin = R.pipeK(S.prepend('gh-'), exports.getPluginPath, fp_1.safeImport);
/* ----------------------- */
// function concatError(leftMonad, errorMessage) {
//     const leftMonadContainsError = leftMonad && leftMonad.isLeft() && leftMonad.value
//     return leftMonadContainsError ? leftMonad.map(mapError) : Left(errorMessage)
//     function mapError(prevErrorMessage) {
//         return `${prevErrorMessage}\n${errorMessage}`
//     }
// }
// -- Config -------------------------------------------------------------------
function getNodeModulesGlobalPath() {
    try {
        var { stdout } = exec.spawnSync('npm', ['root', '-g']);
    }
    catch (err) {
        logger.warn(`Can't resolve plugins directory path.\n${err}`);
    }
    return stdout;
}
exports.getNodeModulesGlobalPath = getNodeModulesGlobalPath;
function getGlobalPackageJson() {
    const configFile = fs.readFileSync(path.join(__dirname, '../package.json'));
    return JSON.parse(configFile.toString());
}
exports.getGlobalPackageJson = getGlobalPackageJson;
function getDefaultConfigPath() {
    return path.join(__dirname, '../default.gh.json');
}
exports.getDefaultConfigPath = getDefaultConfigPath;
function getProjectConfigPath() {
    return path.join(process.cwd(), '.gh.json');
}
exports.getProjectConfigPath = getProjectConfigPath;
function getUserHomePath() {
    return userhome('.gh.json');
}
exports.getUserHomePath = getUserHomePath;
function resolveGHConfigs() {
    const globalConfig = getGlobalConfig();
    let projectConfig;
    const result = {};
    try {
        projectConfig = JSON.parse(fs.readFileSync(getProjectConfigPath()).toString());
        Object.keys(globalConfig).forEach(key => {
            result[key] = globalConfig[key];
        });
        Object.keys(projectConfig).forEach(key => {
            result[key] = projectConfig[key];
        });
        return result;
    }
    catch (e) {
        logger.debug(e.message);
        if (e.code !== 'MODULE_NOT_FOUND' && e.code !== 'ENOENT') {
            throw e;
        }
        return globalConfig;
    }
}
function getConfig() {
    const config = resolveGHConfigs();
    const protocol = `${config.api.protocol}://`;
    const is_enterprise = config.api.host !== 'api.github.com';
    if (config.github_host === undefined) {
        config.github_host = `${protocol}${is_enterprise ? config.api.host : 'github.com'}`;
    }
    if (config.github_gist_host === undefined) {
        config.github_gist_host = `${protocol}${is_enterprise ? `${config.api.host}/gist` : 'gist.github.com'}/`;
    }
    return config;
}
exports.getConfig = getConfig;
function getGlobalConfig() {
    const configPath = getUserHomePath();
    const defaultPath = getDefaultConfigPath();
    if (!fs.existsSync(configPath)) {
        createGlobalConfig();
    }
    return JSON.parse(fs.readFileSync(testing ? defaultPath : configPath).toString());
}
exports.getGlobalConfig = getGlobalConfig;
function removeGlobalConfig(key) {
    var config = getGlobalConfig();
    delete config[key];
    saveJsonConfig(getUserHomePath(), config);
}
exports.removeGlobalConfig = removeGlobalConfig;
function createGlobalConfig() {
    saveJsonConfig(getUserHomePath(), JSON.parse(fs.readFileSync(getDefaultConfigPath()).toString()));
}
exports.createGlobalConfig = createGlobalConfig;
function writeGlobalConfig(jsonPath, value) {
    const config = getGlobalConfig();
    let i;
    let output;
    let path;
    let pathLen;
    path = jsonPath.split('.');
    output = config;
    for (i = 0, pathLen = path.length; i < pathLen; i++) {
        output[path[i]] = config[path[i]] || (i + 1 === pathLen ? value : {});
        output = output[path[i]];
    }
    saveJsonConfig(getUserHomePath(), config);
}
exports.writeGlobalConfig = writeGlobalConfig;
function saveJsonConfig(path, object) {
    fs.writeFileSync(path, JSON.stringify(object, null, 4));
}
exports.saveJsonConfig = saveJsonConfig;
function writeGlobalConfigCredentials(user, token, path) {
    const configPath = path || getUserHomePath();
    let config;
    if (fs.existsSync(configPath)) {
        config = JSON.parse(fs.readFileSync(configPath).toString());
    }
    else {
        config = JSON.parse(fs.readFileSync(getDefaultConfigPath()).toString());
    }
    logger.log(`Writing GH config data: ${configPath}`);
    try {
        config.github_user = user;
        config.github_token = token;
        saveJsonConfig(configPath, config);
    }
    catch (err) {
        throw new Error(`Error writing credentials to global config\n${err}`);
    }
    logger.log('Authentication succeed. Token written to global config.');
}
exports.writeGlobalConfigCredentials = writeGlobalConfigCredentials;
// -- Plugins ------------------------------------------------------------------
function addPluginConfig(plugin) {
    try {
        const pluginConfig = require(path.join(getNodeModulesGlobalPath(), `gh-${plugin}`, 'gh-plugin.json'));
        const config = getGlobalConfig();
        const configHooks = lodash_1.cloneDeep(config.hooks);
        const pluginHooks = lodash_1.cloneDeep(pluginConfig.hooks);
        if (config.plugins[plugin] && !config.plugins[plugin]['hooks_installed']) {
            Object.keys(pluginHooks).forEach(cmd => {
                Object.keys(pluginHooks[cmd]).forEach(hook => {
                    configHooks[cmd][hook].before = [
                        ...configHooks[cmd][hook].before,
                        ...pluginHooks[cmd][hook].before,
                    ];
                    configHooks[cmd][hook].after = [
                        ...configHooks[cmd][hook].after,
                        ...pluginHooks[cmd][hook].after,
                    ];
                });
            });
            if (!testing) {
                logger.log(logger.colors.yellow(`Copying over ${plugin} plugin hooks to your .gh.json hooks.`));
                try {
                    config.hooks = configHooks;
                    config.plugins[plugin]['hooks_installed'] = true;
                    saveJsonConfig(getUserHomePath(), config);
                }
                catch (err) {
                    logger.error(`Error writing ${plugin} hooks to .gh.json config.\n${err}`);
                }
                logger.log(logger.colors.green('Copy successful.\n'));
            }
        }
    }
    catch (e) {
        if (e.code !== 'MODULE_NOT_FOUND') {
            throw e;
        }
    }
}
exports.addPluginConfig = addPluginConfig;
function getPlugins() {
    const pluginsPath = getNodeModulesGlobalPath();
    if (pluginsPath === '') {
        return [];
    }
    try {
        var plugins = fs.readdirSync(pluginsPath).filter(plugin => {
            return plugin.substring(0, 3) === 'gh-';
        });
    }
    catch (err) {
        logger.warn(`Can't read plugins directory.\n${err}`);
    }
    return plugins;
}
exports.getPlugins = getPlugins;
function pluginHasConfig(pluginName) {
    return Boolean(getConfig().plugins[pluginName]);
}
exports.pluginHasConfig = pluginHasConfig;
//# sourceMappingURL=configs.js.map