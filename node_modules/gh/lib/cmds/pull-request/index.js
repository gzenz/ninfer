"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.updatePullRequest = exports.setMergeCommentRequiredOptions = exports.printPullInfo = exports.getPullRequest = exports.run = exports.STATE_OPEN = exports.FETCH_TYPE_SILENT = exports.DETAILS = exports.name = exports.STATUSES = exports.testing = void 0;
// -- Requires -------------------------------------------------------------------------------------
const lodash_1 = require("lodash");
const immer_1 = require("immer");
const list_1 = require("./list");
const browser_1 = require("./browser");
const close_1 = require("./close");
const open_1 = require("./open");
const comment_1 = require("./comment");
const submit_1 = require("./submit");
const fetch_1 = require("./fetch");
const forward_1 = require("./forward");
const info_1 = require("./info");
const utils_1 = require("../../utils");
const git = require("../../git");
const logger = require("../../logger");
exports.testing = process.env.NODE_ENV === 'testing';
exports.STATUSES = {
    error: logger.colors.red('!'),
    failure: logger.colors.red('✗'),
    pending: logger.colors.yellow('-'),
    success: logger.colors.green('✓'),
};
// -- Constants ------------------------------------------------------------------------------------
exports.name = 'PullRequest';
exports.DETAILS = {
    alias: 'pr',
    description: 'Provides a set of util commands to work with Pull Requests.',
    iterative: 'number',
    commands: [
        'browser',
        'close',
        'comment',
        'fetch',
        'fwd',
        'info',
        'list',
        'merge',
        'open',
        'rebase',
        'submit',
    ],
    options: {
        all: Boolean,
        branch: String,
        browser: Boolean,
        close: Boolean,
        comment: String,
        date: String,
        description: String,
        detailed: Boolean,
        direction: String,
        draft: Boolean,
        fetch: Boolean,
        fwd: String,
        issue: Number,
        info: Boolean,
        link: Boolean,
        list: Boolean,
        me: Boolean,
        merge: Boolean,
        number: [String, Array],
        open: Boolean,
        org: String,
        rebase: Boolean,
        remote: String,
        repo: String,
        sort: String,
        state: ['open', 'closed'],
        submit: String,
        title: String,
        user: String,
    },
    shorthands: {
        a: ['--all'],
        b: ['--branch'],
        B: ['--browser'],
        C: ['--close'],
        c: ['--comment'],
        D: ['--description'],
        d: ['--detailed'],
        f: ['--fetch'],
        i: ['--issue'],
        I: ['--info'],
        k: ['--link'],
        l: ['--list'],
        M: ['--merge'],
        m: ['--me'],
        n: ['--number'],
        o: ['--open'],
        O: ['--org'],
        R: ['--rebase'],
        r: ['--repo'],
        S: ['--state'],
        s: ['--submit'],
        t: ['--title'],
        u: ['--user'],
    },
};
exports.FETCH_TYPE_SILENT = 'silent';
exports.STATE_OPEN = 'open';
// -- Commands -------------------------------------------------------------------------------------
async function run(options, done) {
    if (!options.repo && !options.all) {
        logger.error('You must specify a Git repository with a GitHub remote to run this command');
    }
    if (!utils_1.userRanValidFlags(exports.DETAILS.commands, options)) {
        const payload = options.argv.remain && options.argv.remain.concat().slice(1);
        options = immer_1.produce(options, draft => {
            if (payload && payload[0]) {
                draft.fetch = true;
            }
            else {
                draft.list = true;
            }
        });
    }
    const numbers = [...options.number];
    for (const number of numbers) {
        await main(number, options);
    }
    done && done();
    // main logic to iterate on when number flag is passed in > 1
    async function main(number, options) {
        options = await immer_1.produce(options, async (draft) => {
            draft.number =
                number ||
                    getPullRequestNumberFromBranch(draft.currentBranch, options.config.pull_branch_name_prefix);
            draft.pullBranch = getBranchNameFromPullNumber(number, options.config.pull_branch_name_prefix);
            draft.state = draft.state || exports.STATE_OPEN;
        });
        if (!options.pullBranch && (options.close || options.fetch || options.merge)) {
            logger.error("You've invoked a method that requires an issue number.");
        }
        if (options.browser) {
            browser_1.browser(options.user, options.repo, number, options.config.github_host);
        }
        if (!options.list) {
            options = immer_1.produce(options, draft => {
                draft.branch = draft.branch || options.config.default_branch;
            });
        }
        if (options.close) {
            try {
                await close_1.closeHandler(options);
            }
            catch (err) {
                throw new Error(`Error closing PR\n${err}`);
            }
        }
        if (options.comment || options.comment === '') {
            await comment_1.commentHandler(options);
        }
        if (options.fetch) {
            await fetch_1.fetchHandler(options);
        }
        if (options.fwd === '') {
            options = immer_1.produce(options, draft => {
                draft.fwd = options.config.default_pr_forwarder;
            });
        }
        if (options.fwd) {
            await forward_1.fwdHandler(options);
            return;
        }
        if (options.info) {
            await info_1.infoHandler(options);
        }
        if (options.list) {
            await list_1.listHandler(options);
        }
        if (options.open) {
            await open_1.openHandler(options);
        }
        if (options.submit === '') {
            options = immer_1.produce(options, draft => {
                draft.submit = options.config.default_pr_reviewer;
            });
        }
        if (options.submit) {
            await submit_1.submitHandler(options);
        }
    }
}
exports.run = run;
function getPullRequest(options) {
    const payload = {
        pull_number: options.number,
        repo: options.repo,
        owner: options.user,
    };
    return options.GitHub.pulls.get(payload);
}
exports.getPullRequest = getPullRequest;
function getBranchNameFromPullNumber(number, pullBranchNamePrefix) {
    if (number && number[0] !== undefined) {
        return pullBranchNamePrefix + number;
    }
}
function getPullRequestNumberFromBranch(currentBranch, prefix) {
    if (currentBranch && lodash_1.startsWith(currentBranch, prefix)) {
        return currentBranch.replace(prefix, '');
    }
}
function printPullInfo(options, pull) {
    let status = '';
    switch (pull.combinedStatus) {
        case 'success':
        case 'failure':
        case 'error':
            status = ` ${exports.STATUSES[pull.combinedStatus]}`;
            break;
    }
    var headline = `${logger.colors.green(`#${pull.number}`)} ${pull.title} ${logger.colors.magenta(`@${pull.user.login}`)} (${logger.getDuration(pull.created_at, options.date)})${status}`;
    if (options.link) {
        headline += ` ${logger.colors.blue(pull.html_url)}`;
    }
    logger.log(headline);
    if (options.detailed && !options.link) {
        logger.log(logger.colors.blue(pull.html_url));
    }
    if (pull.mergeable_state === 'clean') {
        logger.log(`Mergeable (${pull.mergeable_state})`);
    }
    else if (pull.mergeable_state !== undefined) {
        logger.warn(`Not mergeable (${pull.mergeable_state})`);
    }
    if ((options.info || options.detailed) && pull.body) {
        logger.log(`${pull.body}\n`);
    }
}
exports.printPullInfo = printPullInfo;
function setMergeCommentRequiredOptions(options) {
    const lastCommitSHA = git.getLastCommitSHA();
    const changes = git.countUserAdjacentCommits();
    options = immer_1.produce(options, draft => {
        draft.currentSHA = lastCommitSHA;
        if (changes > 0) {
            draft.changes = changes;
        }
        draft.pullHeadSHA = `${lastCommitSHA}~${changes}`;
    });
    return options;
}
exports.setMergeCommentRequiredOptions = setMergeCommentRequiredOptions;
function updatePullRequest(options, title, optBody, state) {
    if (optBody) {
        optBody = logger.applyReplacements(optBody, options.config.replace);
    }
    const payload = Object.assign({ state,
        title, pull_number: options.number, repo: options.repo, owner: options.user }, (optBody ? { body: optBody } : {}));
    return options.GitHub.pulls.update(payload);
}
exports.updatePullRequest = updatePullRequest;
//# sourceMappingURL=index.js.map