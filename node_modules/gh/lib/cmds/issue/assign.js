"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.assign = void 0;
const index_1 = require("./index");
async function assign(options) {
    const issue = await index_1.getIssue(options);
    return index_1.editIssue(options, issue.title, index_1.STATE_OPEN);
}
exports.assign = assign;
//# sourceMappingURL=assign.js.map