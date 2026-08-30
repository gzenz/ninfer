"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.browser = void 0;
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
const utils_1 = require("../../utils");
function browser({ config, user, repo, number }) {
    if (!number) {
        number = '';
    }
    utils_1.openUrl(`${config.github_host}/${user}/${repo}/issues/${number}`);
}
exports.browser = browser;
//# sourceMappingURL=browser.js.map