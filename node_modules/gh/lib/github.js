"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.tokenExists = exports.getToken = exports.getGitHubInstance = void 0;
const Octokit = require("@octokit/rest");
const fs = require("fs");
const inquirer = require("inquirer");
const moment = require("moment");
const path_1 = require("path");
const userhome = require("userhome");
const configs_1 = require("./configs");
const logger = require("./logger");
const url_1 = require("url");
async function getGitHubInstance() {
    const config = configs_1.getConfig();
    const { github_token: token, github_user: user, api: { protocol, pathPrefix, host }, } = config;
    const isEnterprise = host !== 'api.github.com';
    const apiUrl = `${protocol}://${isEnterprise ? host : 'api.github.com'}`;
    const { href } = new url_1.URL(`${apiUrl}${pathPrefix || ''}`);
    // trim trailing slash for Octokit
    const baseUrl = href.replace(/\/+$/, '');
    const throttlePlugin = await Promise.resolve().then(() => require('@octokit/plugin-throttling'));
    Octokit.plugin(throttlePlugin);
    return new Octokit({
        // log: console,
        baseUrl,
        auth: await getToken({ token, user }),
        throttle: {
            onRateLimit: (retryAfter, options) => {
                console.warn(`Request quota exhausted for request ${options.method} ${options.url}`);
                if (options.request.retryCount === 0) {
                    // only retries once
                    console.log(`Retrying after ${retryAfter} seconds!`);
                    return true;
                }
            },
            onAbuseLimit: (_, options) => {
                // does not retry, only logs a warning
                console.warn(`Abuse detected for request ${options.method} ${options.url}`);
            },
        },
    });
}
exports.getGitHubInstance = getGitHubInstance;
async function getToken(tokenAndUser) {
    let token;
    if (!tokenExists(tokenAndUser)) {
        token = await createNewOathToken();
    }
    else {
        token = getSavedToken(tokenAndUser.token);
    }
    if (token) {
        return token;
    }
    process.exit(1);
}
exports.getToken = getToken;
function tokenExists({ token, user }) {
    if (process.env.GENERATE_NEW_TOKEN) {
        return false;
    }
    return (token && user) || (process.env.GH_TOKEN && process.env.GH_USER);
}
exports.tokenExists = tokenExists;
function getSavedToken(token) {
    if (process.env.CONTINUOUS_INTEGRATION) {
        return process.env.GH_TOKEN;
    }
    if (process.env.NODE_ENV === 'testing') {
        // Load your local token when generating test fixtures
        return JSON.parse(fs.readFileSync(userhome('.gh.json')).toString()).github_token;
    }
    return token;
}
async function createNewOathToken() {
    logger.log(`In order to use GitHub's API, you will need to login with your GitHub account.`);
    const answers = await inquirer.prompt([
        {
            type: 'input',
            message: 'Enter your GitHub user',
            name: 'username',
        },
        {
            type: 'password',
            message: 'Enter your GitHub password',
            name: 'password',
        },
    ]);
    const octokit = new Octokit({
        auth: {
            username: answers.username,
            password: answers.password,
            async on2fa() {
                const { code } = await inquirer.prompt([
                    {
                        type: 'input',
                        message: 'Enter your Two-factor GitHub authenticator code',
                        name: 'code',
                    },
                ]);
                return code;
            },
        },
    });
    const payload = {
        note: `Node GH (${moment().format('MMMM Do YYYY, h:mm:ss a')})`,
        note_url: 'https://github.com/node-gh/gh',
        scopes: ['user', 'public_repo', 'repo', 'repo:status', 'delete_repo', 'gist'],
    };
    try {
        var { data } = await octokit.oauthAuthorizations.createAuthorization(payload);
    }
    catch (err) {
        throw new Error(`Error creating GitHub token\n${err.message}`);
    }
    if (data.token) {
        configs_1.writeGlobalConfigCredentials(answers.username, data.token, process.env.GENERATE_NEW_TOKEN && path_1.join(__dirname, '../__tests__/auth.json'));
        return data.token;
    }
    logger.log(`Was not able to retrieve token from GitHub's api`);
}
//# sourceMappingURL=github.js.map