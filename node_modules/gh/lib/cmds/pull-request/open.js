"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.open = exports.openHandler = void 0;
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
const index_1 = require("./index");
const hooks_1 = require("../../hooks");
const logger = require("../../logger");
async function openHandler(options) {
    await hooks_1.beforeHooks('pull-request.open', { options });
    logger.log(`Opening pull request ${logger.colors.green(`#${options.number}`)}`);
    try {
        var { data } = await open(options);
    }
    catch (err) {
        logger.error(`Can't open pull request ${options.number}.`);
    }
    logger.log(data.html_url);
    await hooks_1.afterHooks('pull-request.open', { options });
}
exports.openHandler = openHandler;
async function open(options) {
    const { data: pull } = await index_1.getPullRequest(options);
    return index_1.updatePullRequest(options, pull.title, pull.body, index_1.STATE_OPEN);
}
exports.open = open;
//# sourceMappingURL=open.js.map