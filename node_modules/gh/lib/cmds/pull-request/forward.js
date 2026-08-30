"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.forward = exports.fwdHandler = void 0;
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
const immer_1 = require("immer");
const submit_1 = require("./submit");
const fetch_1 = require("./fetch");
const index_1 = require("./index");
const hooks_1 = require("../../hooks");
const logger = require("../../logger");
async function fwdHandler(options) {
    await hooks_1.beforeHooks('pull-request.fwd', { options });
    logger.log(`Forwarding pull request ${logger.colors.green(`#${options.number}`)} to ${logger.colors.magenta(`@${options.fwd}`)}`);
    try {
        var { options: updatedOptions, data: pull } = await forward(options);
    }
    catch (err) {
        throw new Error(`Can't forward pull request ${options.number} to ${options.fwd}.\n${err}`);
    }
    if (pull) {
        options = immer_1.produce(updatedOptions, draft => {
            draft.submittedPullNumber = pull.number;
            draft.forwardedPull = pull.number;
        });
    }
    logger.log(pull.html_url);
    options = index_1.setMergeCommentRequiredOptions(options);
    await hooks_1.afterHooks('pull-request.fwd', { options });
}
exports.fwdHandler = fwdHandler;
async function forward(options) {
    try {
        var pull = await fetch_1.fetch(options, index_1.FETCH_TYPE_SILENT);
    }
    catch (err) {
        throw new Error(`Error fetching PR\${err}`);
    }
    options = immer_1.produce(options, draft => {
        draft.title = pull.title;
        draft.description = pull.body;
        draft.submittedUser = pull.user.login;
    });
    const data = await submit_1.submit(options, options.fwd);
    return { data, options };
}
exports.forward = forward;
//# sourceMappingURL=forward.js.map