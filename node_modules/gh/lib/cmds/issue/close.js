"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.closeHandler = void 0;
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
const logger = require("../../logger");
const hooks_1 = require("../../hooks");
const index_1 = require("./index");
async function closeHandler(options) {
    await hooks_1.beforeHooks('issue.close', { options });
    for (const number of options.number) {
        logger.log(`Closing issue ${number} on ${options.userRepo}`);
        try {
            var { data } = await close(options, number);
        }
        catch (err) {
            throw new Error(`Can't close issue.\n${err}`);
        }
        logger.log(logger.colors.cyan(data.html_url));
    }
    await hooks_1.afterHooks('issue.close', { options });
}
exports.closeHandler = closeHandler;
async function close(options, number) {
    const issue = await index_1.getIssue(options, number);
    return index_1.editIssue(options, issue.title, index_1.STATE_CLOSED, number);
}
//# sourceMappingURL=close.js.map