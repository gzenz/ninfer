"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.info = exports.infoHandler = void 0;
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
const index_1 = require("./index");
const logger = require("../../logger");
async function infoHandler(options) {
    try {
        await info(options, options.user, options.repo, options.number);
    }
    catch (err) {
        throw new Error(`Can't get pull requests.\n${err}`);
    }
}
exports.infoHandler = infoHandler;
async function info(options, user, repo, number) {
    const payload = {
        repo,
        pull_number: number,
        owner: user,
    };
    try {
        var { data: pull } = await options.GitHub.pulls.get(payload);
    }
    catch (err) {
        logger.warn(`Can't get pull request ${user}/${repo}/${number}`);
    }
    index_1.printPullInfo(options, pull);
}
exports.info = info;
//# sourceMappingURL=info.js.map