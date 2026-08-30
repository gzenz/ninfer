(function (global, factory) {
  typeof exports === 'object' && typeof module !== 'undefined' ? factory(exports, require('sanctuary-def'), require('sanctuary-type-identifiers'), require('fluture')) :
  typeof define === 'function' && define.amd ? define(['exports', 'sanctuary-def', 'sanctuary-type-identifiers', 'fluture'], factory) :
  (global = global || self, factory(global.flutureSanctuaryTypes = {}, global.sanctuaryDef, global.sanctuaryTypeIdentifiers, global.Fluture));
}(this, function (exports, $, type, fluture) { 'use strict';

  //. # fluture-sanctuary-types

  //  $$type :: String
  var $$type = '@@type';

  //# FutureType :: Type -> Type -> Type
  //.
  //. The binary type constructor for members of Future.
  //.
  //. ```js
  //. > $.test (env) (FutureType ($.String) ($.Number)) (Future.of (1));
  //. true
  //. ```
  var FutureType = $.BinaryType
    (type.parse (fluture.Future[$$type]).name)
    ('https://github.com/fluture-js/Fluture#readme')
    ([])
    (fluture.isFuture)
    (fluture.extractLeft)
    (fluture.extractRight);

  //# ConcurrentFutureType :: Type -> Type -> Type
  //.
  //. The binary type constructor for members of ConcurrentFuture.
  //.
  //. ```js
  //. > $.test
  //. .   (env)
  //. .   (ConcurrentFutureType ($.String) ($.Number))
  //. .   (Future.Par.of (1));
  //. true
  //. ```
  var ConcurrentFutureType = $.BinaryType
    (type.parse (fluture.Par[$$type]).name)
    ('https://github.com/fluture-js/Fluture#concurrentfuture')
    ([])
    (function(x) { return type (x) === fluture.Par[$$type]; })
    (function(f) { return (fluture.seq (f)).extractLeft (); })
    (function(f) { return (fluture.seq (f)).extractRight (); });

  //# env :: Array Type
  //.
  //. An Array containing all types applied to [`$.Unknown`][Unknown] for
  //. direct use as a Sanctuary environment, as shown in [Usage](#usage).
  var env = [
    FutureType ($.Unknown) ($.Unknown),
    ConcurrentFutureType ($.Unknown) ($.Unknown)
  ];

  //. [Fluture]:    https://github.com/fluture-js/Fluture
  //. [Sanctuary]:  https://sanctuary.js.org/
  //. [Unknown]:    https://github.com/sanctuary-js/sanctuary-def#Unknown

  exports.ConcurrentFutureType = ConcurrentFutureType;
  exports.FutureType = FutureType;
  exports.env = env;

  Object.defineProperty(exports, '__esModule', { value: true });

}));
