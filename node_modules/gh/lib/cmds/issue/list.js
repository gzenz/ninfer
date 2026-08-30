"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.listFromAllRepositories = exports.list = void 0;
const logger = require("../../logger");
const index_1 = require("./index");
const utils_1 = require("../../utils");
const ora = require("ora");
const spinner = ora({ text: 'Fetching Issues', discardStdin: false });
async function list(options) {
    const { user, repo, page = 1, pageSize } = options;
    spinner.start();
    let payload;
    payload = {
        repo,
        owner: user,
        state: options.state,
        page,
        per_page: pageSize,
    };
    if (options.labels) {
        payload.labels = options.labels;
    }
    if (options['no-milestone']) {
        payload.milestone = 'none';
    }
    if (options.milestone) {
        const data = await options.GitHub.paginate(options.GitHub.issues.listMilestonesForRepo.endpoint({
            repo,
            owner: user,
        }));
        const milestoneNumber = data
            .filter(milestone => options.milestone === milestone.title)
            .map(milestone => milestone.number)[0];
        if (!milestoneNumber) {
            logger.log(`No issues found with milestone title: ${logger.colors.red(options.milestone)}`);
            return;
        }
        payload.milestone = `${milestoneNumber}`;
    }
    if (options.assignee) {
        payload.assignee = options.assignee;
    }
    const { data, hasNextPage } = await utils_1.handlePagination({
        options,
        listEndpoint: options.GitHub.issues.listForRepo,
        payload,
    });
    const issues = data.filter(result => Boolean(result));
    spinner.stop();
    if (issues && issues.length > 0) {
        const formattedIssues = index_1.formatIssues(issues, options.detailed);
        options.all
            ? logger.log(`\n${options.user}/${options.repo}:\n${formattedIssues}`)
            : logger.log(formattedIssues);
    }
    else {
        logger.log(`\nNo issues on ${options.user}/${options.repo}`);
    }
    if (hasNextPage && !index_1.testing) {
        const continuePaginating = await utils_1.askUserToPaginate(`Issues for ${options.user}/${options.repo}`);
        continuePaginating && (await list(Object.assign(Object.assign({}, options), { page: page + 1 })));
    }
}
exports.list = list;
async function listFromAllRepositories(options) {
    const payload = {
        type: 'all',
        username: options.user,
    };
    const repositories = await options.GitHub.paginate(options.GitHub.repos.listForUser.endpoint(payload));
    for (const repo of repositories) {
        await list(Object.assign(Object.assign({}, options), { user: repo.owner.login, repo: repo.name }));
    }
}
exports.listFromAllRepositories = listFromAllRepositories;
//# sourceMappingURL=list.js.map