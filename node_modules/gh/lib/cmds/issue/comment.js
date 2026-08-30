"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.comment = void 0;
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
const utils_1 = require("../../utils");
const logger = require("../../logger");
function comment(options) {
    const useEditor = options.config.use_editor !== false;
    let body = logger.applyReplacements(options.comment, options.config.replace) + options.config.signature;
    if (useEditor && utils_1.userLeftMsgEmpty(options.comment)) {
        body = utils_1.openFileInEditor('temp-gh-issue-comment.md', '<!-- Add an issue comment message in markdown format below -->');
    }
    const payload = {
        body,
        issue_number: options.number,
        repo: options.repo,
        owner: options.user,
    };
    return options.GitHub.issues.createComment(payload);
}
exports.comment = comment;
//# sourceMappingURL=comment.js.map