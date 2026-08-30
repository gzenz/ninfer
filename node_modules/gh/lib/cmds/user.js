"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.run = exports.name = exports.DETAILS = void 0;
// -- Requires -------------------------------------------------------------------------------------
const configs = require("../configs");
const github_1 = require("../github");
const logger = require("../logger");
const utils_1 = require("../utils");
const testing = process.env.NODE_ENV === 'testing';
// -- Constants ------------------------------------------------------------------------------------
exports.DETAILS = {
    alias: 'us',
    description: 'Provides the ability to login and logout if needed.',
    commands: ['login', 'logout', 'whoami'],
    options: {
        login: Boolean,
        logout: Boolean,
        whoami: Boolean,
    },
    shorthands: {
        l: ['--login'],
        L: ['--logout'],
        w: ['--whoami'],
    },
};
// -- Commands -------------------------------------------------------------------------------------
exports.name = 'User';
async function run(options, done) {
    let login = options.login;
    if (!utils_1.userRanValidFlags(exports.DETAILS.commands, options)) {
        login = true;
    }
    if (login) {
        const { github_token: token, github_user: user } = configs.getConfig();
        if (github_1.tokenExists({ token, user })) {
            logger.log(`You're logged in as ${logger.colors.green(options.user)}`);
        }
        else {
            done && done();
        }
    }
    if (options.logout) {
        logger.log(`Logging out of user ${logger.colors.green(options.user)}`);
        !testing && logout();
    }
    if (options.whoami) {
        logger.log(options.user);
    }
}
exports.run = run;
// -- Static ---------------------------------------------------------------------------------------
function logout() {
    configs.removeGlobalConfig('github_user');
    configs.removeGlobalConfig('github_token');
}
//# sourceMappingURL=user.js.map