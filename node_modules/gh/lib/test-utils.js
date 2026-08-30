"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.prepareTestFixtures = exports.runCmd = void 0;
const child_process_1 = require("child_process");
const lodash_1 = require("lodash");
const nock = require("nock");
const zlib = require("zlib");
function runCmd(cmd, env) {
    try {
        const customEnv = env ? { env: Object.assign({}, process.env) } : {};
        var result = child_process_1.execSync(cmd, Object.assign({ cwd: process.cwd() }, customEnv));
    }
    catch (error) {
        throw new Error(error.output.toString());
    }
    return result.toString();
}
exports.runCmd = runCmd;
const nockBack = nock.back;
function prepareTestFixtures(cmdName, argv) {
    let id = 0;
    // These should only include the flags that you need for e2e tests
    const cmds = [
        {
            name: 'Help',
            flags: [],
        },
        {
            name: 'Issue',
            flags: ['--comment', '--lock', '--new', '--open', '--close', '--search', '--assign'],
        },
        {
            name: 'PullRequest',
            flags: [
                '--detailed',
                '--info',
                '--fetch',
                '--fwd',
                '--comment',
                '--open',
                '--close',
                '--draft',
                '--submit',
            ],
        },
        {
            name: 'Gists',
            flags: ['--new', '--fork', '--delete'],
        },
        {
            name: 'Milestone',
            flags: ['--list'],
        },
        {
            name: 'Notifications',
        },
        {
            name: 'Repo',
            flags: ['--label', '--list', '--new', '--fork', '--delete', '--search'],
        },
        {
            name: 'User',
            flags: ['--login', '--logout', '--whoami'],
        },
        {
            name: 'Version',
            flags: ['--version'],
        },
    ].filter(cmd => cmd.name === cmdName);
    const newCmdName = formatCmdName(cmds[0], argv);
    if (!newCmdName) {
        return () => { };
    }
    nock.disableNetConnect();
    nockBack.fixtures = `${process.cwd()}/__tests__/nockFixtures`;
    nockBack.setMode('record');
    const nockPromise = nockBack(`${newCmdName}.json`, {
        before,
        afterRecord,
    });
    return () => nockPromise
        .then(({ nockDone }) => nockDone())
        .catch(err => {
        throw new Error(`Nock ==> ${err}`);
    });
    /* --- Normalization Functions --- */
    function normalize(value, key) {
        if (!value)
            return value;
        if (lodash_1.isPlainObject(value)) {
            return lodash_1.mapValues(value, normalize);
        }
        if (lodash_1.isArray(value) && lodash_1.isPlainObject(value[0])) {
            return lodash_1.map(value, normalize);
        }
        if (key.includes('token')) {
            return '234lkj23l4kj234lkj234lkj234lkj23l4kj234l';
        }
        if (key.includes('_at')) {
            return '2017-10-10T16:00:00Z';
        }
        if (key.includes('_count')) {
            return 42;
        }
        if (key.includes('id')) {
            return 1000 + id++;
        }
        if (key.includes('node_id')) {
            return 'MDA6RW50aXR5MQ==';
        }
        if (key.includes('url')) {
            return value.replace(/[1-9][0-9]{2,10}/, '000000001');
        }
        return value;
    }
    function decodeBuffer(fixture) {
        const response = lodash_1.isArray(fixture.response) ? fixture.response.join('') : fixture.response;
        if (!lodash_1.isObject(response)) {
            try {
                // Decode the hex buffer that nock made
                const decoded = Buffer.from(response, 'hex');
                var unzipped = zlib.gunzipSync(decoded).toString('utf-8');
            }
            catch (err) {
                throw new Error(`Error decoding nock hex:\n${err}`);
            }
        }
        return JSON.parse(unzipped);
    }
    // This only executes when first recording the request, but not on subsequent requests
    function afterRecord(fixtures) {
        const normalizedFixtures = fixtures.map(fixture => {
            const isGzipped = fixture.rawHeaders.includes('gzip');
            let res = fixture.response;
            if (fixture.body.note) {
                fixture.body.note = 'Hello from the inside!';
            }
            fixture.path = stripAccessToken(fixture.path);
            fixture.rawHeaders = fixture.rawHeaders.map(header => stripAccessToken(header));
            if (isGzipped) {
                res = decodeBuffer(fixture);
            }
            if (lodash_1.isArray(res)) {
                res = res.slice(0, 3).map(res => {
                    return lodash_1.mapValues(res, normalize);
                });
            }
            else {
                res = lodash_1.mapValues(res, normalize);
            }
            if (isGzipped) {
                try {
                    // Re-gzip to keep the octokittens happy
                    const stringified = JSON.stringify(res);
                    var zipped = zlib.gzipSync(stringified);
                }
                catch (err) {
                    throw new Error(`Error re-gzipping nock ==> ${err}`);
                }
            }
            fixture.response = zipped || res;
            return fixture;
        });
        return normalizedFixtures;
    }
    function stripAccessToken(header) {
        return header.includes('access_token')
            ? header.replace(/access_token(.*?)(&|$)/gi, '')
            : header;
    }
    function before(scope) {
        scope.filteringPath = () => stripAccessToken(scope.path);
        scope.filteringRequestBody = (body, aRecordedBody) => {
            if (body.includes('note')) {
                body = JSON.parse(body);
                body.note = aRecordedBody.note;
                return JSON.stringify(body);
            }
            return body;
        };
    }
}
exports.prepareTestFixtures = prepareTestFixtures;
function formatCmdName(cmd, argv) {
    if (argv.length === 1) {
        return cmd.name;
    }
    return cmd.flags.reduce((flagName, current) => {
        if (flagName) {
            return flagName;
        }
        if (argv.includes(current)) {
            return concatUpper(cmd.name, current.slice(2));
        }
    }, null);
}
function concatUpper(one, two) {
    return `${one}${lodash_1.upperFirst(two)}`;
}
//# sourceMappingURL=test-utils.js.map