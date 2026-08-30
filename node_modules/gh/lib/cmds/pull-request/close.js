"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.close = exports.closeHandler = void 0;
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
const git = require("../../git");
const index_1 = require("./index");
const hooks_1 = require("../../hooks");
const logger = require("../../logger");
const STATE_CLOSED = 'closed';
async function closeHandler(options) {
    await hooks_1.beforeHooks('pull-request.close', { options });
    logger.log(`Closing pull request ${logger.colors.green(`#${options.number}`)}`);
    try {
        var { data } = await close(options);
    }
    catch (err) {
        throw new Error(`Can't close pull request ${options.number}.\n${err}`);
    }
    logger.log(data.html_url);
    options = index_1.setMergeCommentRequiredOptions(options);
    await hooks_1.afterHooks('pull-request.close', { options });
}
exports.closeHandler = closeHandler;
async function close(options) {
    const { data: pull } = await index_1.getPullRequest(options);
    const data = await index_1.updatePullRequest(options, pull.title, pull.body, STATE_CLOSED);
    if (options.pullBranch === options.currentBranch) {
        git.checkout(pull.base.ref);
    }
    if (options.pullBranch) {
        git.deleteBranch(options.pullBranch);
    }
    return data;
}
exports.close = close;
//# sourceMappingURL=close.js.map