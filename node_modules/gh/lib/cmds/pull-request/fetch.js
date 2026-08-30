"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.fetch = exports.fetchHandler = void 0;
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
const git = require("../../git");
const index_1 = require("./index");
const hooks_1 = require("../../hooks");
const logger = require("../../logger");
const FETCH_TYPE_CHECKOUT = 'checkout';
const FETCH_TYPE_MERGE = 'merge';
const FETCH_TYPE_REBASE = 'rebase';
async function fetchHandler(options) {
    let fetchType = FETCH_TYPE_CHECKOUT;
    if (options.merge) {
        fetchType = FETCH_TYPE_MERGE;
    }
    else if (options.rebase) {
        fetchType = FETCH_TYPE_REBASE;
    }
    await hooks_1.beforeHooks('pull-request.fetch', { options });
    let operation = '';
    let branch = options.pullBranch;
    if (options.merge) {
        operation = ' and merging';
        branch = options.currentBranch;
    }
    if (options.rebase) {
        operation = ' and rebasing';
        branch = options.currentBranch;
    }
    logger.log(`Fetching pull request ${logger.colors.green(`#${options.number}`)}${operation} into branch ${logger.colors.green(branch)}`);
    try {
        await fetch(options, fetchType);
    }
    catch (err) {
        throw new Error(`Can't fetch pull request ${options.number}.\n${err}`);
    }
    await hooks_1.afterHooks('pull-request.fetch', { options });
}
exports.fetchHandler = fetchHandler;
async function fetch(options, optType) {
    try {
        var { data: pull } = await index_1.getPullRequest(options);
    }
    catch (err) {
        throw new Error(`Error getting PR\n${err}`);
    }
    const headBranch = pull.head.ref;
    const repoUrl = options.config.ssh === false ? pull.head.repo.clone_url : pull.head.repo.ssh_url;
    git.fetch(repoUrl, headBranch, options.pullBranch);
    if (optType !== index_1.FETCH_TYPE_SILENT) {
        git[optType](options.pullBranch);
    }
    return pull;
}
exports.fetch = fetch;
//# sourceMappingURL=fetch.js.map