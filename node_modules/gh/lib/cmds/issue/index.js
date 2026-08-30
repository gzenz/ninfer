"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.formatIssues = exports.getIssue = exports.editIssue = exports.run = exports.name = exports.STATE_OPEN = exports.STATE_CLOSED = exports.DETAILS = exports.testing = void 0;
// -- Requires -------------------------------------------------------------------------------------
const lodash_1 = require("lodash");
const immer_1 = require("immer");
const utils_1 = require("../../utils");
const hooks_1 = require("../../hooks");
const logger = require("../../logger");
const browser_1 = require("./browser");
const new_1 = require("./new");
const comment_1 = require("./comment");
const list_1 = require("./list");
const assign_1 = require("./assign");
const close_1 = require("./close");
const open_1 = require("./open");
const search_1 = require("./search");
const lock_1 = require("./lock");
// -- Constants ------------------------------------------------------------------------------------
exports.testing = process.env.NODE_ENV === 'testing';
exports.DETAILS = {
    alias: 'is',
    description: 'Provides a set of util commands to work with Issues.',
    iterative: 'number',
    commands: ['assign', 'browser', 'close', 'comment', 'list', 'new', 'open', 'search'],
    options: {
        all: Boolean,
        assign: Boolean,
        assignee: String,
        browser: Boolean,
        close: Boolean,
        comment: String,
        date: String,
        detailed: Boolean,
        labels: [String],
        list: Boolean,
        link: Boolean,
        lock: Boolean,
        'lock-reason': ['off-topic', 'too heated', 'resolved', 'spam'],
        message: String,
        milestone: [Number, String],
        'no-milestone': Boolean,
        new: Boolean,
        number: [String, Array],
        open: Boolean,
        remote: String,
        repo: String,
        search: String,
        state: ['open', 'closed'],
        title: String,
        user: String,
    },
    shorthands: {
        a: ['--all'],
        A: ['--assignee'],
        B: ['--browser'],
        C: ['--close'],
        c: ['--comment'],
        d: ['--detailed'],
        L: ['--labels'],
        k: ['--link'],
        l: ['--list'],
        m: ['--message'],
        M: ['--milestone'],
        N: ['--new'],
        n: ['--number'],
        o: ['--open'],
        r: ['--repo'],
        s: ['--search'],
        S: ['--state'],
        t: ['--title'],
        u: ['--user'],
    },
};
exports.STATE_CLOSED = 'closed';
exports.STATE_OPEN = 'open';
// -- Command-------------------------------------------------------------------------------------
exports.name = 'Issue';
async function run(options, done) {
    if (!options.repo && !options.all) {
        logger.error('You must specify a Git repository with a GitHub remote to run this command');
    }
    const number = logger.colors.green(`#${options.number}`);
    options = immer_1.produce(options, draft => {
        draft.state = draft.state || exports.STATE_OPEN;
        if (!utils_1.userRanValidFlags(exports.DETAILS.commands, draft)) {
            const payload = draft.argv.remain && draft.argv.remain.slice(1);
            if (payload && payload[0]) {
                if (options.argv.original.length === 2) {
                    draft.browser = true;
                    draft.number = payload[0];
                }
                else {
                    draft.new = true;
                    draft.title = draft.title || payload[0];
                    draft.message = draft.message || payload[1];
                }
            }
            else {
                draft.list = true;
            }
        }
    });
    if (options.assign) {
        await hooks_1.beforeHooks('issue.assign', { options });
        logger.log(`Assigning issue ${number} on ${options.userRepo} to ${logger.colors.magenta(options.assignee)}`);
        try {
            var { data } = await assign_1.assign(options);
        }
        catch (err) {
            throw new Error(`Can't assign issue.\n${err}`);
        }
        logger.log(logger.colors.cyan(data.html_url));
        await hooks_1.afterHooks('issue.assign', { options });
    }
    else if (options.browser) {
        browser_1.browser(options);
    }
    else if (options.comment || options.comment === '') {
        logger.log(`Adding comment on issue ${number} on ${options.userRepo}`);
        try {
            var { data } = await comment_1.comment(options);
        }
        catch (err) {
            throw new Error(`Can't add comment.\n${err}`);
        }
        logger.log(logger.colors.cyan(data.html_url));
    }
    else if (options.list) {
        try {
            if (options.all) {
                logger.log(`Listing ${logger.colors.green(options.state)} issues for ${logger.colors.green(options.user)}`);
                await list_1.listFromAllRepositories(options);
            }
            else {
                logger.log(`Listing ${logger.colors.green(options.state)} issues on ${options.userRepo}`);
                await list_1.list(options);
            }
        }
        catch (err) {
            throw new Error(`Error listing issues\n${err}`);
        }
    }
    else if (options.lock) {
        logger.log(`Locking issue ${options.number} on ${options.userRepo}`);
        try {
            await lock_1.lockHandler(options);
        }
        catch (err) {
            throw new Error(`Error locking issue\n${err}`);
        }
    }
    else if (options.new) {
        await hooks_1.beforeHooks('issue.new', { options });
        logger.log(`Creating a new issue on ${options.userRepo}`);
        try {
            var { data } = await new_1.newIssue(options);
        }
        catch (err) {
            throw new Error(`Can't create issue.\n${err}`);
        }
        if (data) {
            options = immer_1.produce(options, draft => {
                draft.number = data.number;
            });
            logger.log(data.html_url);
        }
        await hooks_1.afterHooks('issue.new', { options });
    }
    else if (options.open) {
        await open_1.openHandler(options);
    }
    else if (options.close) {
        await close_1.closeHandler(options);
    }
    else if (options.search) {
        let { repo, user } = options;
        const query = logger.colors.green(options.search);
        if (options.all) {
            repo = undefined;
            logger.log(`Searching for ${query} in issues for ${logger.colors.green(user)}\n`);
        }
        else {
            logger.log(`Searching for ${query} in issues for ${options.userRepo}\n`);
        }
        try {
            await search_1.search(options, user, repo);
        }
        catch (err) {
            throw new Error(`Can't search issues for ${options.userRepo}: \n${err}`);
        }
    }
    done && done();
}
exports.run = run;
// -- HELPERS -------------------------------------------------------------------------------------
function editIssue(options, title, state, number) {
    let payload;
    payload = {
        state,
        title,
        assignee: options.assignee,
        labels: options.labels || [],
        milestone: options.milestone,
        issue_number: number || options.number,
        owner: options.user,
        repo: options.repo,
    };
    return options.GitHub.issues.update(payload);
}
exports.editIssue = editIssue;
function getIssue(options, number) {
    const payload = {
        issue_number: number || options.number,
        repo: options.repo,
        owner: options.user,
    };
    return options.GitHub.issues.get(payload);
}
exports.getIssue = getIssue;
function formatIssues(issues, showDetailed, dateFormatter) {
    issues.sort((a, b) => {
        return a.number > b.number ? -1 : 1;
    });
    if (issues && issues.length > 0) {
        const formattedIssuesArr = issues.map(issue => {
            const issueNumber = logger.colors.green(`#${issue.number}`);
            const issueUser = logger.colors.magenta(`@${issue.user.login}`);
            const issueDate = `(${logger.getDuration(issue.created_at, dateFormatter)})`;
            let formattedIssue = `${issueNumber} ${issue.title} ${issueUser} ${issueDate}`;
            if (showDetailed) {
                if (issue.body) {
                    formattedIssue = `
                        ${formattedIssue}
                        ${issue.body}
                    `;
                }
                if (lodash_1.isArray(issue.labels) && issue.labels.length > 0) {
                    const labels = issue.labels.map(label => label.name);
                    const labelHeading = labels.length > 1 ? 'labels: ' : 'label: ';
                    formattedIssue = `
                        ${formattedIssue}
                        ${logger.colors.yellow(labelHeading) + labels.join(', ')}
                    `;
                }
                if (issue.milestone) {
                    const { number, title } = issue.milestone;
                    formattedIssue = `
                        ${formattedIssue}
                        ${`${logger.colors.green('milestone: ')} ${title} - ${number}`}
                    `;
                }
                formattedIssue = `
                    ${formattedIssue}
                    ${logger.colors.blue(issue.html_url)}
                `;
            }
            return trim(formattedIssue);
        });
        return formattedIssuesArr.join('\n\n');
    }
    return null;
}
exports.formatIssues = formatIssues;
function trim(str) {
    return str
        .replace(/^[ ]+/gm, '')
        .replace(/[\r\n]+/g, '\n')
        .trim();
}
//# sourceMappingURL=index.js.map