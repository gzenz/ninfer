"use strict";
/**
 * © 2013 Liferay, Inc. <https://liferay.com> and Node GH contributors
 * (see file: README.md)
 * SPDX-License-Identifier: BSD-3-Clause
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.lf = exports.l = exports.isObject = exports.prepend = exports.maybeFnToEither = exports.safeReaddir = exports.safeImport = exports.safeRealpath = exports.safeWhich = void 0;
const S = require("sanctuary");
const Future = require("fluture");
const fs = require("fs");
const which = require("which");
const $ = require("sanctuary-def");
/* SAFE UTILS */
exports.safeWhich = Future.encase(which.sync);
exports.safeRealpath = Future.encase(fs.realpathSync);
function safeImport(fileName) {
    let moduleObj = null;
    try {
        moduleObj = require(fileName);
    }
    catch (e) {
        return e.code === 'MODULE_NOT_FOUND' ? Future.of(fileName) : Future.reject(e);
    }
    return Future.of(moduleObj);
}
exports.safeImport = safeImport;
exports.safeReaddir = Future.encaseN(fs.readdir);
/* TRANSFORMATIONS */
// Allows you to take a Maybe returning function and make it an Either returning function
function maybeFnToEither(monadReturningFunction) {
    return function convertMaybe(val) {
        const result = monadReturningFunction(val);
        return S.isJust(result) ? S.Right(result.value) : S.Left(null);
    };
}
exports.maybeFnToEither = maybeFnToEither;
/**
 * Concats two strings
 * @return {Future}
 */
exports.prepend = (a) => (b) => {
    const argsAreStrings = typeof a === 'string' && typeof b === 'string';
    return argsAreStrings ? Future.of(a + b) : Future.reject('Both args should be strings');
};
/* TYPE CHECKING UTILS */
exports.isObject = S.is($.Object);
/* LOGGING UTILS */
exports.l = x => {
    console.log('!!!!!!!', x);
    return x;
};
// returns a future
exports.lf = x => {
    console.log('-------', x);
    return Future.of(x);
};
//# sourceMappingURL=fp.js.map