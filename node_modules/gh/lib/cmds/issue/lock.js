"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.lockHandler = void 0;
const _1 = require(".");
const logger = require("../../logger");
async function lockHandler(options) {
    const { data: { locked }, } = await _1.getIssue(options);
    if (!locked) {
        var { status } = await lock(options);
        logger.log(status === 204
            ? logger.colors.green('Success locking issue.')
            : logger.colors.green('Failed to lock issue.'));
    }
    else {
        logger.log('Issue is already locked.');
    }
}
exports.lockHandler = lockHandler;
function lock(options) {
    const { number, user, repo, 'lock-reason': lockReason, GitHub } = options;
    const payload = Object.assign(Object.assign({ repo, owner: user, issue_number: number }, (lockReason ? { lock_reason: lockReason } : {})), { mediaType: {
            previews: ['sailor-v-preview'],
        } });
    return GitHub.issues.lock(payload);
}
//# sourceMappingURL=lock.js.map