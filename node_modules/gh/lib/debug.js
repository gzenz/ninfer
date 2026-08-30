#!/usr/bin/env node
"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
const cmd_1 = require("./cmd");
const verbose = process.argv.indexOf('--verbose') !== -1;
const insane = process.argv.indexOf('--insane') !== -1;
process.on('unhandledRejection', r => console.log(r));
if (verbose || insane) {
    process.env.GH_VERBOSE = 'true';
}
if (insane) {
    process.env.GH_VERBOSE_INSANE = 'true';
}
cmd_1.run();
//# sourceMappingURL=debug.js.map