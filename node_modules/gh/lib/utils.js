"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.askUserToPaginate = exports.userRanValidFlags = exports.getCurrentFolderName = exports.cleanFileContents = exports.openFileInEditor = exports.userLeftMsgEmpty = exports.openUrl = exports.handlePagination = void 0;
const tmp = require("tmp");
const open = require("opn");
const exec_1 = require("./exec");
const fs_1 = require("fs");
const logger = require("./logger");
const inquirer = require("inquirer");
const testing = process.env.NODE_ENV === 'testing';
async function handlePagination({ options, listEndpoint, payload }) {
    let hasNextPage = false;
    try {
        // If no pageSize, assume user removed limit and fetch all prs
        var data = await (options.allPages
            ? options.GitHub.paginate(listEndpoint.endpoint.merge(payload))
            : listEndpoint(payload));
        hasNextPage = data.headers && data.headers.link && data.headers.link.includes('rel="next"');
        data = data.data || data;
    }
    catch (err) {
        if (err && err.status === '404') {
            // Some times a repo is found, but you can't list its prs
            // Due to the repo being disabled (e.g., private repo with debt)
            logger.warn(`Can't list pull requests for ${options.user}/${options.repo}`);
        }
        else {
            throw new Error(`Error listing data\n${err}`);
        }
    }
    return {
        data,
        hasNextPage,
    };
}
exports.handlePagination = handlePagination;
/**
 * Opens url in browser
 */
function openUrl(url) {
    testing ? console.log(url) : open(url, { wait: false });
}
exports.openUrl = openUrl;
/**
 * Checks if string has been merged with a common flag or is empty
 */
function userLeftMsgEmpty(string) {
    return (!string ||
        string === '--title' ||
        string === '-t' ||
        string === '--message' ||
        string === '-m' ||
        string === '--comment' ||
        string === '-c' ||
        string === '--description' ||
        string === '-D');
}
exports.userLeftMsgEmpty = userLeftMsgEmpty;
/**
 * Allows users to add text from their editor of choice rather than the terminal
 *
 * @example
 *   openFileInEditor('temp-gh-issue-title.txt', '# Add a pr title msg on the next line')
 */
function openFileInEditor(fileName, msg) {
    try {
        var { name: filePath, removeCallback } = tmp.fileSync({ postfix: `-${fileName}` });
        fs_1.writeFileSync(filePath, msg);
        const editor = process.env.EDITOR ||
            process.env.VISUAL ||
            exec_1.spawnSync('git', ['config', '--global', 'core.editor']).stdout;
        if (editor) {
            exec_1.execSyncInteractiveStream(`${editor} "${filePath}"`);
        }
        const newFileContents = fs_1.readFileSync(filePath).toString();
        const commentMark = fileName.endsWith('.md') ? '<!--' : '#';
        removeCallback();
        return cleanFileContents(newFileContents, commentMark);
    }
    catch (err) {
        logger.error('Could not use your editor to store a custom message\n', err);
    }
}
exports.openFileInEditor = openFileInEditor;
/**
 * Removes # comments and trims new lines
 * @param {string} commentMark - refers to the comment mark which is different for each file
 */
function cleanFileContents(fileContents, commentMark = '#') {
    return fileContents
        .split('\n')
        .filter(line => !line.startsWith(commentMark))
        .join('\n')
        .trim();
}
exports.cleanFileContents = cleanFileContents;
function getCurrentFolderName() {
    const cwdArr = process
        .cwd()
        .toString()
        .split('/');
    return cwdArr[cwdArr.length - 1];
}
exports.getCurrentFolderName = getCurrentFolderName;
/**
 * Checks to see if the cli arguments are one of the accepted flags
 */
function userRanValidFlags(commands, options) {
    if (commands) {
        return commands.some(c => {
            return options[c] !== undefined;
        });
    }
    return false;
}
exports.userRanValidFlags = userRanValidFlags;
async function askUserToPaginate(type) {
    logger.log('\n');
    const answers = await inquirer.prompt([
        {
            type: 'confirm',
            message: `Would you like to see the next batch of ${type}`,
            name: 'paginate',
        },
    ]);
    logger.log('\n');
    return answers.paginate;
}
exports.askUserToPaginate = askUserToPaginate;
//# sourceMappingURL=utils.js.map