"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.openHandler = void 0;
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
const logger = require("../../logger");
const hooks_1 = require("../../hooks");
const index_1 = require("./index");
async function openHandler(options) {
    await hooks_1.beforeHooks('issue.open', { options });
    for (const number of options.number) {
        logger.log(`Opening issue ${number} on ${options.userRepo}`);
        try {
            var { data } = await open(options, number);
        }
        catch (err) {
            throw new Error(`Can't close issue.\n${err}`);
        }
        logger.log(logger.colors.cyan(data.html_url));
    }
    await hooks_1.afterHooks('issue.open', { options });
}
exports.openHandler = openHandler;
async function open(options, number) {
    const issue = await index_1.getIssue(options, number);
    return index_1.editIssue(options, issue.title, index_1.STATE_OPEN, number);
}
//# sourceMappingURL=open.js.map