"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.browser = void 0;
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
const utils_1 = require("../../utils");
function browser(user, repo, number, githubHost) {
    if (number) {
        utils_1.openUrl(`${githubHost}/${user}/${repo}/pull/${number}`);
    }
    else {
        utils_1.openUrl(`${githubHost}/${user}/${repo}/pulls`);
    }
}
exports.browser = browser;
//# sourceMappingURL=browser.js.map