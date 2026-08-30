"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.search = void 0;
const index_1 = require("./index");
const logger = require("../../logger");
async function search(options, user, repo) {
    const query = ['type:issue'];
    if (!options.all && repo) {
        query.push(`repo:${repo}`);
    }
    if (user) {
        query.push(`user:${user}`);
    }
    query.push(options.search);
    const payload = {
        q: query.join(' '),
    };
    const { data } = await options.GitHub.search.issuesAndPullRequests(payload);
    if (data.items && data.items.length > 0) {
        const formattedIssues = index_1.formatIssues(data.items, options.detailed);
        logger.log(formattedIssues);
    }
    else {
        logger.log('Could not find any issues matching your query.');
    }
}
exports.search = search;
//# sourceMappingURL=search.js.map