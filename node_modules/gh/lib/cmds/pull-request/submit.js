"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.submitHandler = exports.submit = void 0;
const immer_1 = require("immer");
const git = require("../../git");
const utils_1 = require("../../utils");
const index_1 = require("./index");
const hooks_1 = require("../../hooks");
const logger = require("../../logger");
async function submit(options, user) {
    const useEditor = options.config.use_editor !== false;
    let description = options.description;
    let title = options.title;
    let pullBranch = options.pullBranch || options.currentBranch;
    if (index_1.testing) {
        pullBranch = 'test';
    }
    if (utils_1.userLeftMsgEmpty(title)) {
        title = useEditor
            ? utils_1.openFileInEditor('temp-gh-pr-title.txt', `# Add a pr title message on the next line\n`)
            : git.getLastCommitMessage(pullBranch);
    }
    /*
     * If user passes an empty title and description, --description will get merged into options.title
     * Need to reference the original title not the potentially modified one
     * Also check if user wants to user their editor to add a description
     */
    if (useEditor && (utils_1.userLeftMsgEmpty(options.title) || utils_1.userLeftMsgEmpty(description))) {
        description = utils_1.openFileInEditor('temp-gh-pr-body.md', '<!-- Add an pr body message in markdown format below -->');
    }
    const payload = Object.assign(Object.assign(Object.assign({ mediaType: {
            previews: ['shadow-cat'],
        }, owner: user, base: options.branch, head: `${options.user}:${pullBranch}`, repo: options.repo, title: title }, (options.issue ? { issue: options.issue } : {})), (options.draft ? { draft: options.draft } : {})), (description ? { body: description } : {}));
    try {
        git.push(options.config.default_remote, pullBranch);
        const method = payload.issue ? 'createFromIssue' : 'create';
        var { data } = await options.GitHub.pulls[method](payload);
    }
    catch (err) {
        var { originalError, pull } = await checkPullRequestIntegrity_(options, err);
        if (originalError) {
            throw new Error(`Error submitting PR\n${err}`);
        }
    }
    return data || pull;
}
exports.submit = submit;
async function submitHandler(options) {
    await hooks_1.beforeHooks('pull-request.submit', { options });
    logger.log(`Submitting pull request to ${logger.colors.magenta(`@${options.submit}`)}`);
    try {
        var pull = await submit(options, options.submit);
    }
    catch (err) {
        throw new Error(`Can't submit pull request\n${err}`);
    }
    if (pull.draft) {
        logger.log('Opened in draft state.');
    }
    if (pull) {
        options = immer_1.produce(options, draft => {
            draft.submittedPull = pull.number;
        });
    }
    logger.log(pull.html_url);
    options = index_1.setMergeCommentRequiredOptions(options);
    await hooks_1.afterHooks('pull-request.submit', { options });
}
exports.submitHandler = submitHandler;
async function checkPullRequestIntegrity_(options, originalError) {
    let pull;
    const payload = {
        owner: options.user,
        repo: options.repo,
        state: index_1.STATE_OPEN,
    };
    try {
        var pulls = await options.GitHub.paginate(options.GitHub.pulls.list.endpoint(payload));
    }
    catch (err) {
        throw new Error(`Error listings PRs\n${err}`);
    }
    pulls.forEach(data => {
        if (data.base.ref === options.branch &&
            data.head.ref === options.currentBranch &&
            data.base.sha === data.head.sha &&
            data.base.user.login === options.user &&
            data.head.user.login === options.user) {
            pull = data;
            originalError = null;
            return;
        }
    });
    return {
        originalError,
        pull,
    };
}
//# sourceMappingURL=submit.js.map