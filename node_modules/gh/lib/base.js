"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.writeGlobalConfig = exports.getConfig = exports.getUser = exports.find = exports.clone = void 0;
const configs = require("./configs");
const fp_1 = require("./fp");
// -- Config -------------------------------------------------------------------
function clone(o) {
    return JSON.parse(JSON.stringify(o));
}
exports.clone = clone;
// -- Utils --------------------------------------------------------------------
/**
 * Returns files in a folder path that match a given patter
 */
function find(dirPath, optPattern = /.*/) {
    return fp_1.safeReaddir(dirPath).map(dirs => {
        return dirs.filter(file => {
            return optPattern.test(file);
        });
    });
}
exports.find = find;
function getUser() {
    return configs.getConfig().github_user || process.env.GH_USER;
}
exports.getUser = getUser;
// Export some config methods to allow plugins to access them
exports.getConfig = configs.getConfig;
exports.writeGlobalConfig = configs.writeGlobalConfig;
//# sourceMappingURL=base.js.map