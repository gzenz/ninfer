"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.run = exports.buildOptions = exports.loadCommand = exports.tryResolvingByAlias = exports.tryResolvingByPlugin = exports.tryResolvingByHelpOrVersion = void 0;
// -- Requires -------------------------------------------------------------------------------------
const Future = require("fluture");
const fluture_sanctuary_types_1 = require("fluture-sanctuary-types");
const fs = require("fs");
const immer_1 = require("immer");
const nopt = require("nopt");
const path = require("path");
const R = require("ramda");
const sanctuary_1 = require("sanctuary");
const updateNotifier = require("update-notifier");
const base_1 = require("./base");
const logger = require("./logger");
const configs_1 = require("./configs");
const fp_1 = require("./fp");
const git = require("./git");
const github_1 = require("./github");
const testing = process.env.NODE_ENV === 'testing';
// Make Fluture Play nicely with Sanctuary
const S = sanctuary_1.create({ checkTypes: true, env: sanctuary_1.env.concat(fluture_sanctuary_types_1.env) });
// Allow mutation of options when not testing
// https://immerjs.github.io/immer/docs/freezing
immer_1.setAutoFreeze(testing);
Future.debugMode(testing);
/**
 * Figure out if cmd is either the Version of Help cmd
 */
function tryResolvingByHelpOrVersion({ cooked, remain } = {}) {
    let cmdName = null;
    const isVersionCmd = cooked[0] === '--version' || cooked[0] === '-v';
    const isHelpCmd = !remain.length || cooked.includes('-h') || cooked.includes('--help');
    if (isVersionCmd) {
        cmdName = 'version';
    }
    else if (isHelpCmd) {
        cmdName = 'help';
    }
    return cmdName ? Future.of(cmdName) : Future.reject(remain[0]);
}
exports.tryResolvingByHelpOrVersion = tryResolvingByHelpOrVersion;
/**
 * Builds out the absolute path of the non plugin cmd
 */
function buildFilePath(filename) {
    const commandDir = path.join(__dirname, 'cmds');
    const fullFileName = filename.includes('.') ? filename : `${filename}`;
    const absolutePath = path.join(commandDir, fullFileName);
    return absolutePath;
}
/**
 * Try to determine if cmd passed in is a plugin
 */
exports.tryResolvingByPlugin = R.pipeK(fp_1.prepend('gh-'), fp_1.safeWhich, fp_1.safeRealpath);
/**
 * Checks if cmd is a valid alias
 */
function tryResolvingByAlias(name) {
    const cmdDir = path.join(__dirname, 'cmds');
    return fp_1.safeReaddir(cmdDir)
        .chain(filterFiles)
        .chainRej(() => Future.reject(name));
    function filterFiles(files) {
        const cmdFileName = files.filter((file) => {
            return file.startsWith(name[0]) && file.includes(name[1]);
        })[0];
        return cmdFileName ? Future.of(cmdFileName) : Future.reject(name);
    }
}
exports.tryResolvingByAlias = tryResolvingByAlias;
// Some plugins have the Impl prop housing the main class
// For backwards compat, we will flatten it if it exists
function flattenIfImpl(obj) {
    return obj.Impl ? obj.Impl : obj;
}
function loadCommand(args) {
    return tryResolvingByHelpOrVersion(args)
        .chainRej(tryResolvingByAlias)
        .map(buildFilePath)
        .chainRej(exports.tryResolvingByPlugin)
        .chain(fp_1.safeImport)
        .map(flattenIfImpl);
}
exports.loadCommand = loadCommand;
function getCommand(args) {
    /**
     * nopt function returns:
     *
     * remain: The remaining args after all the parsing has occurred.
     * original: The args as they originally appeared.
     * cooked: The args after flags and shorthands are expanded.
     */
    const parsed = nopt(args);
    const remain = parsed.argv.remain;
    const module = remain[0];
    const Command = loadCommand(parsed.argv);
    return Command.fold(() => S.Left(`Cannot find module ${module}`), S.Right);
}
function notifyVersion() {
    const notifier = updateNotifier({ pkg: configs_1.getGlobalPackageJson() });
    if (notifier.update) {
        notifier.notify();
    }
}
async function buildOptions(args, cmdName) {
    const options = immer_1.default(args, async (draft) => {
        const config = base_1.getConfig();
        // Gets 2nd positional arg (`gh pr 1` will return 1)
        const secondArg = [draft.argv.remain[1]];
        const remote = draft.remote || config.default_remote;
        const remoteUrl = git.getRemoteUrl(remote);
        if (cmdName !== 'Help' && cmdName !== 'Version') {
            // We don't want to boot up Ocktokit if user just wants help or version
            draft.GitHub = await github_1.getGitHubInstance();
        }
        // default the page size to 30
        draft.allPages = config.page_size === '';
        draft.pageSize = config.page_size || 30;
        draft.config = config;
        draft.remote = remote;
        draft.number = draft.number || secondArg;
        draft.loggedUser = base_1.getUser();
        draft.remoteUser = git.getUserFromRemoteUrl(remoteUrl);
        draft.repo = draft.repo || git.getRepoFromRemoteURL(remoteUrl);
        draft.currentBranch = git.getCurrentBranch();
        draft.github_host = config.github_host;
        draft.github_gist_host = config.github_gist_host;
        if (!draft.user) {
            if (args.repo || args.all) {
                draft.user = draft.loggedUser;
            }
            else {
                draft.user = process.env.GH_USER || draft.remoteUser || draft.loggedUser;
            }
        }
        /**
         * Checks if there are aliases in your .gh.json file.
         * If there are aliases in your .gh.json file, we will attempt to resolve the user, PR forwarder or PR submitter to your alias.
         */
        if (config.alias) {
            draft.fwd = config.alias[draft.fwd] || draft.fwd;
            draft.submit = config.alias[draft.submit] || draft.submit;
            draft.user = config.alias[draft.user] || draft.user;
        }
        draft.userRepo = logger.colors.green(`${draft.user}/${draft.repo}`);
    });
    return options;
}
exports.buildOptions = buildOptions;
/* !! IMPURE CALLING CODE !! */
async function run() {
    process.env.GH_PATH = path.join(__dirname, '../');
    if (!fs.existsSync(configs_1.getUserHomePath())) {
        configs_1.createGlobalConfig();
    }
    notifyVersion();
    getCommand(process.argv).fork(errMsg => console.log(errMsg), async ({ value: Command }) => {
        const args = getAvailableArgsOnCmd(Command);
        let cmdDoneRunning = null;
        if (testing) {
            const { prepareTestFixtures } = await Promise.resolve().then(() => require('./test-utils'));
            // Enable mock apis for e2e's
            cmdDoneRunning = prepareTestFixtures(Command.name, args.argv.cooked);
        }
        const options = await buildOptions(args, Command.name);
        // Maintain backwards compat with plugins implemented as classes
        if (typeof Command === 'function') {
            const Plugin = Command;
            configs_1.addPluginConfig(Plugin.name);
            await new Plugin(options).run(cmdDoneRunning);
        }
        else {
            await Command.run(options, cmdDoneRunning);
        }
    });
}
exports.run = run;
/**
 * If you run `gh pr 1 -s node-gh --remote=origin --user protoEvangelion`, nopt will return
 *
 *   {
 *     remote: 'origin',
 *     submit: 'node-gh',
 *     user: 'protoEvangelion',
 *     argv: {
 *         original: ['pr', '1', '-s', 'pr', 'node-gh', '--remote', 'origin', '--user', 'protoEvangelion'],
 *         remain: ['pr', '1'],
 *         cooked: ['pr', '1', '--submit', 'node-gh', '--remote', 'origin', '--user', 'protoEvangelion'],
 *     },
 *   }
 *
 * Historically we passed every arg after 2nd arg (gh pr 1 -s user; everything after 'pr')
 * and all parsed options to each cmd's payload function to figure out positional args and allow for neat shortcuts like:
 * gh is 'new issue' 'new issue description'
 */
function getAvailableArgsOnCmd(Command) {
    return nopt(Command.DETAILS.options, Command.DETAILS.shorthands, process.argv, 2);
}
//# sourceMappingURL=cmd.js.map