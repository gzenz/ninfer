"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: CONTRIBUTORS)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.run = exports.name = exports.DETAILS = void 0;
// -- Requires -------------------------------------------------------------------------------------
const base = require("../base");
const configs = require("../configs");
const logger = require("../logger");
const utils_1 = require("../utils");
const immer_1 = require("immer");
const config = base.getConfig();
// -- Constants ------------------------------------------------------------------------------------
exports.DETAILS = {
    alias: 'al',
    description: 'Create alias for a username.',
    commands: ['add', 'list', 'remove'],
    options: {
        add: String,
        list: Boolean,
        remove: String,
        user: String,
    },
    shorthands: {
        a: ['--add'],
        l: ['--list'],
        r: ['--remove'],
        u: ['--user'],
    },
};
// -- Commands -------------------------------------------------------------------------------------
exports.name = 'Alias';
async function run(options) {
    if (!utils_1.userRanValidFlags(exports.DETAILS.commands, options)) {
        const payload = options.argv.remain && options.argv.remain.concat().slice(1);
        options = immer_1.produce(options, draft => {
            if (payload[0]) {
                draft.add = payload[0];
                draft.user = payload[1];
            }
            else {
                draft.list = true;
            }
        });
    }
    if (options.add) {
        if (!options.user) {
            logger.error('You must specify an user, try --user username.');
        }
        logger.debug(`Creating alias ${options.add}`);
        add(options);
    }
    if (options.list) {
        list((_, data) => {
            let item;
            for (item in data) {
                if (data.hasOwnProperty(item)) {
                    logger.log(`${logger.colors.cyan(item)}: ${logger.colors.magenta(data[item])}`);
                }
            }
        });
    }
    if (options.remove) {
        logger.debug(`Removing alias ${options.remove}`);
        remove(options);
    }
}
exports.run = run;
function add(options) {
    configs.writeGlobalConfig(`alias.${options.add}`, options.user);
}
function list(opt_callback) {
    opt_callback(null, config.alias);
}
function remove(options) {
    delete config.alias[options.remove];
    configs.writeGlobalConfig('alias', config.alias);
}
//# sourceMappingURL=alias.js.map