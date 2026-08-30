"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.run = exports.DETAILS = exports.name = void 0;
const configs_1 = require("../configs");
const logger = require("../logger");
function Version() { }
exports.default = Version;
exports.name = 'Version';
exports.DETAILS = {
    alias: 'v',
    description: 'Print gh version.',
};
function run() {
    printVersion(configs_1.getGlobalPackageJson());
}
exports.run = run;
function printVersion(pkg) {
    logger.log(`${pkg.name} ${pkg.version}`);
}
//# sourceMappingURL=version.js.map