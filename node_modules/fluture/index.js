(function (global, factory) {
  typeof exports === 'object' && typeof module !== 'undefined' ? module.exports = factory(require('concurrify'), require('sanctuary-type-identifiers'), require('sanctuary-show')) :
  typeof define === 'function' && define.amd ? define(['concurrify', 'sanctuary-type-identifiers', 'sanctuary-show'], factory) :
  (global = global || self, global.Fluture = factory(global.concurrify, global.sanctuaryTypeIdentifiers, global.sanctuaryShow));
}(this, function (concurrify, type, show) { 'use strict';

  concurrify = concurrify && concurrify.hasOwnProperty('default') ? concurrify['default'] : concurrify;
  type = type && type.hasOwnProperty('default') ? type['default'] : type;
  show = show && show.hasOwnProperty('default') ? show['default'] : show;

  /* istanbul ignore next: non v8 compatibility */
  var setImmediate = typeof setImmediate === 'undefined' ? setImmediateFallback : setImmediate;
  function noop(){}
  function moop(){ return this }
  function padf(sf, s){ return s.replace(/^/gm, sf).replace(sf, '') }
  function showf(f){ return padf('  ', show(f)) }

  function partial1(f, a){
    return function bound1(b, c, d){
      switch(arguments.length){
        case 1: return f(a, b);
        case 2: return f(a, b, c);
        default: return f(a, b, c, d);
      }
    };
  }

  function partial2(f, a, b){
    return function bound2(c, d){
      return arguments.length === 1 ? f(a, b, c) : f(a, b, c, d);
    };
  }

  function partial3(f, a, b, c){
    return function bound3(d){
      return f(a, b, c, d);
    };
  }

  function setImmediateFallback(f, x){
    return setTimeout(f, 0, x);
  }

  function raise(x){
    setImmediate(function rethrowErrorDelayedToEscapePromiseCatch(){
      throw x;
    });
  }

  var FL = {
    alt: 'fantasy-land/alt',
    ap: 'fantasy-land/ap',
    bimap: 'fantasy-land/bimap',
    chain: 'fantasy-land/chain',
    chainRec: 'fantasy-land/chainRec',
    map: 'fantasy-land/map',
    of: 'fantasy-land/of',
    zero: 'fantasy-land/zero'
  };

  var ordinal = ['first', 'second', 'third', 'fourth', 'fifth'];

  var namespace = 'fluture';
  var name = 'Future';
  var version = 4;

  var $$type = namespace + '/' + name + '@' + version;

  var nil = {head: null};
  nil.tail = nil;

  function isNil(list){
    return list.tail === list;
  }

  // cons :: (a, List a) -> List a
  //      -- O(1) append operation
  function cons(head, tail){
    return {head: head, tail: tail};
  }

  // reverse :: List a -> List a
  //         -- O(n) list reversal
  function reverse(xs){
    var ys = nil, tail = xs;
    while(!isNil(tail)){
      ys = cons(tail.head, ys);
      tail = tail.tail;
    }
    return ys;
  }

  // cat :: (List a, List a) -> List a
  //     -- O(n) list concatenation
  function cat(xs, ys){
    var zs = ys, tail = reverse(xs);
    while(!isNil(tail)){
      zs = cons(tail.head, zs);
      tail = tail.tail;
    }
    return zs;
  }

  /* istanbul ignore next: non v8 compatibility */
  var captureStackTrace = Error.captureStackTrace || captureStackTraceFallback;
  var _debug = noop;

  function debugMode(debug){
    _debug = debug ? debugHandleAll : noop;
  }

  function debugHandleAll(fn){
    return fn();
  }

  function debug(fn){
    return _debug(fn);
  }

  function captureContext(previous, tag, fn){
    return debug(function debugCaptureContext(){
      var context = {
        tag: tag,
        name: ' from ' + tag + ':',
      };
      captureStackTrace(context, fn);
      return cat(previous, cons(context, nil));
    }) || previous;
  }

  function captureStackTraceFallback(x){
    var e = new Error;
    /* istanbul ignore else: non v8 compatibility */
    if(typeof e.stack === 'string'){
      x.stack = x.name + '\n' + e.stack.split('\n').slice(1).join('\n');
    }else{
      x.stack = x.name;
    }
  }

  function error(message){
    return new Error(message);
  }

  function typeError(message){
    return new TypeError(message);
  }

  function invalidArgument(it, at, expected, actual){
    return typeError(
      it + '() expects its ' + ordinal[at] + ' argument to ' + expected + '.' +
      '\n  Actual: ' + show(actual) + ' :: ' + type.parse(type(actual)).name
    );
  }

  function invalidContext(it, actual){
    return typeError(
      it + '() was invoked outside the context of a Future. You might want to use'
    + ' a dispatcher instead\n  Called on: ' + show(actual)
    );
  }

  function invalidNamespace(m, x){
    return (
      'The Future was not created by ' + namespace + '. '
    + 'Make sure you transform other Futures to ' + namespace + ' Futures. '
    + 'Got ' + (x ? ('a Future from ' + x) : 'an unscoped Future') + '.'
    + '\n  See: https://github.com/fluture-js/Fluture#casting-futures'
    );
  }

  function invalidVersion(m, x){
    return (
      'The Future was created by ' + (x < version ? 'an older' : 'a newer')
    + ' version of ' + namespace + '. '
    + 'This means that one of the sources which creates Futures is outdated. '
    + 'Update this source, or transform its created Futures to be compatible.'
    + '\n  See: https://github.com/fluture-js/Fluture#casting-futures'
    );
  }

  function invalidFuture(it, at, m, s){
    var id = type.parse(type(m));
    var info = id.name === name ? '\n' + (
      id.namespace !== namespace ? invalidNamespace(m, id.namespace)
    : id.version !== version ? invalidVersion(m, id.version)
    : 'Nothing seems wrong. Contact the Fluture maintainers.') : '';
    return typeError(
      it + '() expects ' +
      (ordinal[at] ? 'its ' + ordinal[at] + ' argument to be a valid Future' : at) +
      '.' + info + '\n  Actual: ' + show(m) + ' :: ' + id.name + (s || '')
    );
  }

  function ensureError(value, fn){
    var message;
    try{
      if(value instanceof Error) return value;
      message = 'A Non-Error was thrown from a Future: ' + show(value);
    }catch (_){
      message = 'Something was thrown from a Future, but it could not be converted to String';
    }
    var e = error(message);
    captureStackTrace(e, fn);
    return e;
  }

  function makeError(caught, callingFuture, extraContext){
    var origin = ensureError(caught, makeError);
    var e = error(origin.message);
    e.context = cat(origin.context || nil, extraContext || nil);
    e.future = origin.future || callingFuture;
    e.reason = origin.reason || origin;
    e.stack = e.reason.stack + (isNil(e.context) ? '' : '\n' + contextToStackTrace(e.context));
    return e;
  }

  function contextToStackTrace(context){
    var stack = '', tail = context;
    while(tail !== nil){
      stack += tail.head.stack + '\n';
      tail = tail.tail;
    }
    return stack;
  }

  function throwInvalidArgument(it, at, expected, actual){
    throw invalidArgument(it, at, expected, actual);
  }

  function throwInvalidContext(it, actual){
    throw invalidContext(it, actual);
  }

  function throwInvalidFuture(it, at, m, s){
    throw invalidFuture(it, at, m, s);
  }

  function isFunction(f){
    return typeof f === 'function';
  }

  function isThenable(m){
    return m instanceof Promise || m != null && isFunction(m.then);
  }

  function isBoolean(f){
    return typeof f === 'boolean';
  }

  function isNumber(f){
    return typeof f === 'number';
  }

  function isUnsigned(n){
    return (n === Infinity || isNumber(n) && n > 0 && n % 1 === 0);
  }

  function isObject(o){
    return o !== null && typeof o === 'object';
  }

  function isIterator(i){
    return isObject(i) && isFunction(i.next);
  }

  function isArray(x){
    return Array.isArray(x);
  }

  function hasMethod(method, x){
    return x != null && isFunction(x[method]);
  }

  function isFunctor(x){
    return hasMethod(FL.map, x);
  }

  function isAlt(x){
    return isFunctor(x) && hasMethod(FL.alt, x);
  }

  function isApply(x){
    return isFunctor(x) && hasMethod(FL.ap, x);
  }

  function isBifunctor(x){
    return isFunctor(x) && hasMethod(FL.bimap, x);
  }

  function isChain(x){
    return isApply(x) && hasMethod(FL.chain, x);
  }

  /*eslint no-cond-assign:0, no-constant-condition:0 */

  function Future(computation){
    if(!isFunction(computation)) throwInvalidArgument('Future', 0, 'be a Function', computation);
    return new Computation(computation);
  }

  function isFuture(x){
    return x instanceof Future || type(x) === $$type;
  }

  Future['@@type'] = $$type;

  Future.prototype['@@show'] = function Future$show(){
    return this.toString();
  };

  Future.prototype[FL.ap] = function Future$FL$ap(other){
    return other._ap(this);
  };

  Future.prototype[FL.map] = function Future$FL$map(mapper){
    return this._map(mapper);
  };

  Future.prototype[FL.bimap] = function Future$FL$bimap(lmapper, rmapper){
    return this._bimap(lmapper, rmapper);
  };

  Future.prototype[FL.chain] = function Future$FL$chain(mapper){
    return this._chain(mapper);
  };

  Future.prototype[FL.alt] = function Future$FL$alt(other){
    return this._alt(other);
  };

  Future.prototype.pipe = function Future$pipe(f){
    if(!isFuture(this)) throwInvalidContext('Future#pipe', this);
    if(!isFunction(f)) throwInvalidArgument('Future#pipe', 0, 'be a Function', f);
    return f(this);
  };

  Future.prototype.fork = function Future$fork(rej, res){
    if(!isFuture(this)) throwInvalidContext('Future#fork', this);
    if(!isFunction(rej)) throwInvalidArgument('Future#fork', 0, 'be a Function', rej);
    if(!isFunction(res)) throwInvalidArgument('Future#fork', 1, 'be a Function', res);
    return this._interpret(raise, rej, res);
  };

  Future.prototype.forkCatch = function Future$forkCatch(rec, rej, res){
    if(!isFuture(this)) throwInvalidContext('Future#forkCatch', this);
    if(!isFunction(rec)) throwInvalidArgument('Future#forkCatch', 0, 'be a Function', rec);
    if(!isFunction(rej)) throwInvalidArgument('Future#forkCatch', 1, 'be a Function', rej);
    if(!isFunction(res)) throwInvalidArgument('Future#forkCatch', 2, 'be a Function', res);
    return this._interpret(rec, rej, res);
  };

  Future.prototype.value = function Future$value(res){
    if(!isFuture(this)) throwInvalidContext('Future#value', this);
    if(!isFunction(res)) throwInvalidArgument('Future#value', 0, 'be a Function', res);
    var _this = this;
    return _this._interpret(raise, function Future$value$rej(x){
      raise(error(
        'Future#value was called on a rejected Future\n' +
        '  Rejection: ' + show(x) + '\n' +
        '  Future: ' + _this.toString()
      ));
    }, res);
  };

  Future.prototype.done = function Future$done(callback){
    if(!isFuture(this)) throwInvalidContext('Future#done', this);
    if(!isFunction(callback)) throwInvalidArgument('Future#done', 0, 'be a Function', callback);
    return this._interpret(raise,
                           function Future$done$rej(x){ callback(x); },
                           function Future$done$res(x){ callback(null, x); });
  };

  Future.prototype.promise = function Future$promise(){
    if(!isFuture(this)) throwInvalidContext('Future#promise', this);
    var _this = this;
    return new Promise(function Future$promise$computation(res, rej){
      _this._interpret(raise, rej, res);
    });
  };

  Future.prototype.extractLeft = function Future$extractLeft(){
    return [];
  };

  Future.prototype.extractRight = function Future$extractRight(){
    return [];
  };

  Future.prototype._transform = function Future$transform(action){
    return new Transformation(this, cons(action, nil));
  };

  Future.prototype.context = nil;

  function Computation(computation){
    this._computation = computation;
    this.context = captureContext(nil, 'a Future created with the Future constructor', Future);
  }

  Computation.prototype = Object.create(Future.prototype);

  Computation.prototype._interpret = function Computation$interpret(rec, rej, res){
    var open = false, cancel = noop, cont = function(){ open = true; };
    var context = captureContext(this.context, 'consuming a Future', Computation$interpret);
    try{
      cancel = this._computation(function Computation$rej(x){
        cont = function Computation$rej$cont(){
          open = false;
          rej(x);
        };
        if(open){
          cont();
        }
      }, function Computation$res(x){
        cont = function Computation$res$cont(){
          open = false;
          res(x);
        };
        if(open){
          cont();
        }
      }) || noop;
    }catch(e){
      rec(makeError(e, this, context));
      return noop;
    }
    if(!(isFunction(cancel) && cancel.length === 0)){
      rec(makeError(typeError(
        'The computation was expected to return a nullary function or void\n' +
        '  Actual: ' + show(cancel)
      ), this, context));
      return noop;
    }
    cont();
    return function Computation$cancel(){
      if(open){
        open = false;
        cancel && cancel();
      }
    };
  };

  Computation.prototype.toString = function Computation$toString(){
    return 'Future(' + showf(this._computation) + ')';
  };

  function Transformation(spawn, actions){
    this._spawn = spawn;
    this._actions = actions;
  }

  Transformation.prototype = Object.create(Future.prototype);

  Transformation.prototype._transform = function Transformation$_transform(action){
    return new Transformation(this._spawn, cons(action, this._actions));
  };

  Transformation.prototype._interpret = function Transformation$interpret(rec, rej, res){

    //These are the cold, and hot, action stacks. The cold actions are those that
    //have yet to run parallel computations, and hot are those that have.
    var cold = nil, hot = nil;

    //A linked list of stack traces, tracking context across ticks.
    var context = captureContext(nil, 'consuming a transformed Future', Transformation$interpret);

    //The context of the last action to run.
    var asyncContext = nil;

    //These combined variables define our current state.
    // future  = the future we are currently forking
    // action  = the action to be informed when the future settles
    // cancel  = the cancel function of the current future
    // settled = a boolean indicating whether a new tick should start
    // async   = a boolean indicating whether we are awaiting a result asynchronously
    var future, action, cancel = noop, settled, async = true, it;

    //Takes an action from the top of the hot stack and returns it.
    function nextHot(){
      var x = hot.head;
      hot = hot.tail;
      return x;
    }

    //Takes an action from the top of the cold stack and returns it.
    function nextCold(){
      var x = cold.head;
      cold = cold.tail;
      return x;
    }

    //This function is called with a future to use in the next tick.
    //Here we "flatten" the actions of another Sequence into our own actions,
    //this is the magic that allows for infinitely stack safe recursion because
    //actions like ChainAction will return a new Sequence.
    //If we settled asynchronously, we call drain() directly to run the next tick.
    function settle(m){
      settled = true;
      future = m;
      if(future._spawn){
        var tail = future._actions;
        while(!isNil(tail)){
          cold = cons(tail.head, cold);
          tail = tail.tail;
        }
        future = future._spawn;
      }
      if(async) drain();
    }

    //This function serves as a rejection handler for our current future.
    //It will tell the current action that the future rejected, and it will
    //settle the current tick with the action's answer to that.
    function rejected(x){
      if(async) context = cat(future.context, cat(asyncContext, context));
      settle(action.rejected(x));
    }

    //This function serves as a resolution handler for our current future.
    //It will tell the current action that the future resolved, and it will
    //settle the current tick with the action's answer to that.
    function resolved(x){
      if(async) context = cat(future.context, cat(asyncContext, context));
      settle(action.resolved(x));
    }

    //This function is passed into actions when they are "warmed up".
    //If the action decides that it has its result, without the need to await
    //anything else, then it can call this function to force "early termination".
    //When early termination occurs, all actions which were stacked prior to the
    //terminator will be skipped. If they were already hot, they will also be
    //sent a cancel signal so they can cancel their own concurrent computations,
    //as their results are no longer needed.
    function early(m, terminator){
      context = cat(terminator.context, context);
      cancel();
      cold = nil;
      if(async && action !== terminator){
        action.cancel();
        while((it = nextHot()) && it !== terminator) it.cancel();
      }
      settle(m);
    }

    //This will cancel the current Future, the current action, and all stacked hot actions.
    function Sequence$cancel(){
      cancel();
      action && action.cancel();
      while(it = nextHot()) it.cancel();
    }

    //This function is called when an exception is caught.
    function exception(e){
      Sequence$cancel();
      settled = true;
      cold = hot = nil;
      var error = makeError(e, future, context);
      future = never;
      rec(error);
    }

    //This function serves to kickstart concurrent computations.
    //Takes all actions from the cold stack in reverse order, and calls run() on
    //each of them, passing them the "early" function. If any of them settles (by
    //calling early()), we abort. After warming up all actions in the cold queue,
    //we warm up the current action as well.
    function warmupActions(){
      cold = reverse(cold);
      while(cold !== nil){
        it = cold.head.run(early);
        if(settled) return;
        hot = cons(it, hot);
        cold = cold.tail;
      }
      action = action.run(early);
    }

    //This function represents our main execution loop. By "tick", we've been
    //referring to the execution of one iteration in the while-loop below.
    function drain(){
      async = false;
      while(true){
        settled = false;
        if(action) asyncContext = action.context;
        if(action = nextCold()){
          cancel = future._interpret(exception, rejected, resolved);
          if(!settled) warmupActions();
        }else if(action = nextHot()){
          cancel = future._interpret(exception, rejected, resolved);
        }else break;
        if(settled) continue;
        async = true;
        return;
      }
      cancel = future._interpret(exception, rej, res);
    }

    //Start the execution loop.
    settle(this);

    //Return the cancellation function.
    return Sequence$cancel;

  };

  Transformation.prototype.toString = function Transformation$toString(){
    var str = '', tail = this._actions;

    while(!isNil(tail)){
      str = '.' + tail.head.toString() + str;
      tail = tail.tail;
    }

    return this._spawn.toString() + str;
  };

  function Crashed(exception){
    this._exception = exception;
  }

  Crashed.prototype = Object.create(Future.prototype);

  Crashed.prototype._interpret = function Crashed$interpret(rec){
    rec(this._exception);
    return noop;
  };

  Crashed.prototype.toString = function Crashed$toString(){
    return 'Future(function crash(){ throw ' + show(this._exception) + ' })';
  };

  function Rejected(value){
    this._value = value;
  }

  Rejected.prototype = Object.create(Future.prototype);

  Rejected.prototype._interpret = function Rejected$interpret(rec, rej){
    rej(this._value);
    return noop;
  };

  Rejected.prototype.extractLeft = function Rejected$extractLeft(){
    return [this._value];
  };

  Rejected.prototype.toString = function Rejected$toString(){
    return 'reject(' + show(this._value) + ')';
  };

  function reject(x){
    return new Rejected(x);
  }

  function Resolved(value){
    this._value = value;
  }

  Resolved.prototype = Object.create(Future.prototype);

  Resolved.prototype._interpret = function Resolved$interpret(rec, rej, res){
    res(this._value);
    return noop;
  };

  Resolved.prototype.extractRight = function Resolved$extractRight(){
    return [this._value];
  };

  Resolved.prototype.toString = function Resolved$toString(){
    return 'Future.of(' + show(this._value) + ')';
  };

  function resolve(x){
    return new Resolved(x);
  }

  function Never(){
    this._isNever = true;
  }

  Never.prototype = Object.create(Future.prototype);

  Never.prototype._interpret = function Never$interpret(){
    return noop;
  };

  Never.prototype.toString = function Never$toString(){
    return 'never';
  };

  var never = new Never();

  function isNever(x){
    return isFuture(x) && x._isNever === true;
  }

  function Eager(future){
    var _this = this;
    _this.rec = noop;
    _this.rej = noop;
    _this.res = noop;
    _this.crashed = false;
    _this.rejected = false;
    _this.resolved = false;
    _this.value = null;
    _this.cancel = future._interpret(function Eager$crash(x){
      _this.value = x;
      _this.crashed = true;
      _this.cancel = noop;
      _this.rec(x);
    }, function Eager$reject(x){
      _this.value = x;
      _this.rejected = true;
      _this.cancel = noop;
      _this.rej(x);
    }, function Eager$resolve(x){
      _this.value = x;
      _this.resolved = true;
      _this.cancel = noop;
      _this.res(x);
    });
  }

  Eager.prototype = Object.create(Future.prototype);

  Eager.prototype._interpret = function Eager$interpret(rec, rej, res){
    if(this.crashed) rec(this.value);
    else if(this.rejected) rej(this.value);
    else if(this.resolved) res(this.value);
    else{
      this.rec = rec;
      this.rej = rej;
      this.res = res;
    }
    return this.cancel;
  };

  var Action = {
    rejected: function Action$rejected(x){ this.cancel(); return new Rejected(x) },
    resolved: function Action$resolved(x){ this.cancel(); return new Resolved(x) },
    run: moop,
    cancel: noop
  };

  function captureActionContext(name, fn){
    return captureContext(nil, 'a Future transformed with ' + name, fn);
  }

  function nullaryActionToString(){
    return this.name + '()';
  }

  function defineNullaryAction(name, prototype){
    var _name = '_' + name;
    function NullaryAction(context){ this.context = context; }
    NullaryAction.prototype = Object.assign(Object.create(Action), prototype);
    NullaryAction.prototype.name = name;
    NullaryAction.prototype.toString = nullaryActionToString;
    Future.prototype[name] = function checkedNullaryTransformation(){
      if(!isFuture(this)) throwInvalidContext('Future#' + name, this);
      return this[_name]();
    };
    Future.prototype[_name] = function uncheckedNullaryTransformation(){
      return this._transform(new NullaryAction(
        captureActionContext(name, uncheckedNullaryTransformation)
      ));
    };
    return NullaryAction;
  }

  function mapperActionToString(){
    return this.name + '(' + showf(this.mapper) + ')';
  }

  function defineMapperAction(name, prototype){
    var _name = '_' + name;
    function MapperAction(mapper, context){ this.mapper = mapper; this.context = context; }
    MapperAction.prototype = Object.assign(Object.create(Action), prototype);
    MapperAction.prototype.name = name;
    MapperAction.prototype.toString = mapperActionToString;
    Future.prototype[name] = function checkedMapperTransformation(mapper){
      if(!isFuture(this)) throwInvalidContext('Future#' + name, this);
      if(!isFunction(mapper)) throwInvalidArgument('Future#' + name, 0, 'be a Function', mapper);
      return this[_name](mapper);
    };
    Future.prototype[_name] = function uncheckedMapperTransformation(mapper){
      return this._transform(new MapperAction(
        mapper,
        captureActionContext(name, uncheckedMapperTransformation)
      ));
    };
    return MapperAction;
  }

  function bimapperActionToString(){
    return this.name + '(' + showf(this.lmapper) + ', ' + showf(this.rmapper) + ')';
  }

  function defineBimapperAction(name, prototype){
    var _name = '_' + name;
    function BimapperAction(lmapper, rmapper, context){
      this.lmapper = lmapper;
      this.rmapper = rmapper;
      this.context = context;
    }
    BimapperAction.prototype = Object.assign(Object.create(Action), prototype);
    BimapperAction.prototype.name = name;
    BimapperAction.prototype.toString = bimapperActionToString;
    Future.prototype[name] = function checkedBimapperTransformation(lm, rm){
      if(!isFuture(this)) throwInvalidContext('Future#' + name, this);
      if(!isFunction(lm)) throwInvalidArgument('Future#' + name, 0, 'be a Function', lm);
      if(!isFunction(rm)) throwInvalidArgument('Future#' + name, 1, 'be a Function', rm);
      return this[_name](lm, rm);
    };
    Future.prototype[_name] = function uncheckedBimapperTransformation(lmapper, rmapper){
      return this._transform(new BimapperAction(
        lmapper,
        rmapper,
        captureActionContext(name, uncheckedBimapperTransformation)
      ));
    };
    return BimapperAction;
  }

  function otherActionToString(){
    return this.name + '(' + this.other.toString() + ')';
  }

  function defineOtherAction(name, prototype){
    var _name = '_' + name;
    function OtherAction(other, context){ this.other = other; this.context = context; }
    OtherAction.prototype = Object.assign(Object.create(Action), prototype);
    OtherAction.prototype.name = name;
    OtherAction.prototype.toString = otherActionToString;
    Future.prototype[name] = function checkedOtherTransformation(other){
      if(!isFuture(this)) throwInvalidContext('Future#' + name, this);
      if(!isFuture(other)) throwInvalidFuture('Future#' + name, 0, other);
      return this[_name](other);
    };
    Future.prototype[_name] = function uncheckedOtherTransformation(other){
      return this._transform(new OtherAction(
        other,
        captureActionContext(name, uncheckedOtherTransformation)
      ));
    };
    return OtherAction;
  }

  function defineParallelAction(name, rec, rej, res, prototype){
    var ParallelAction = defineOtherAction(name, prototype);
    ParallelAction.prototype.run = function ParallelAction$run(early){
      var eager = new Eager(this.other);
      var action = new ParallelAction(eager);
      function ParallelAction$early(m){ early(m, action); }
      action.context = captureContext(
        this.context,
        name + ' triggering a parallel Future',
        ParallelAction$run
      );
      action.cancel = eager._interpret(
        function ParallelAction$rec(x){ rec(ParallelAction$early, x); },
        function ParallelAction$rej(x){ rej(ParallelAction$early, x); },
        function ParallelAction$res(x){ res(ParallelAction$early, x); }
      );
      return action;
    };
    return ParallelAction;
  }

  function apActionHandler(f){
    return isFunction(f) ?
           this.other._map(function ApAction$resolved$mapper(x){ return f(x) }) :
           new Crashed(makeError(typeError(
             'Future#' + this.name + '() expects its first argument to be a Future of a Function\n' +
             '  Actual: Future.of(' + show(f) + ')'
           ), null, this.context));
  }

  function chainActionHandler(x){
    var m;
    try{ m = this.mapper(x); }catch(e){ return new Crashed(makeError(e, null, this.context)) }
    return isFuture(m) ? m : new Crashed(makeError(invalidFuture(
      'Future#' + this.name,
      'the function it\'s given to return a Future',
      m,
      '\n  From calling: ' + showf(this.mapper) + '\n  With: ' + show(x)
    ), null, this.context));
  }

  function returnOther(){
    return this.other;
  }

  function mapWith(mapper, create, value, context){
    var m;
    try{ m = create(mapper(value)); }catch(e){ m = new Crashed(makeError(e, null, context)); }
    return m;
  }

  function mapRight(value){
    return mapWith(this.rmapper, resolve, value, this.context);
  }

  function earlyCrash(early, x){
    early(new Crashed(x));
  }

  function earlyReject(early, x){
    early(new Rejected(x));
  }

  function earlyResolve(early, x){
    early(new Resolved(x));
  }

  defineOtherAction('ap', {
    resolved: apActionHandler
  });

  defineMapperAction('map', {
    resolved: function MapAction$resolved(x){
      return mapWith(this.mapper, resolve, x, this.context);
    }
  });

  defineBimapperAction('bimap', {
    resolved: mapRight,
    rejected: function BimapAction$rejected(x){
      return mapWith(this.lmapper, reject, x, this.context);
    }
  });

  defineMapperAction('chain', {
    resolved: chainActionHandler
  });

  defineMapperAction('mapRej', {
    rejected: function MapRejAction$rejected(x){
      return mapWith(this.mapper, reject, x, this.context);
    }
  });

  defineMapperAction('chainRej', {
    rejected: chainActionHandler
  });

  defineNullaryAction('swap', {
    rejected: Action.resolved,
    resolved: Action.rejected
  });

  defineBimapperAction('fold', {
    resolved: mapRight,
    rejected: function FoldAction$rejected(x){
      return mapWith(this.lmapper, resolve, x, this.context);
    }
  });

  var finallyAction = {
    rejected: function FinallyAction$rejected(x){ return this.other._and(new Rejected(x)) },
    resolved: function FinallyAction$resolved(x){ return this.other._and(new Resolved(x)) }
  };

  defineOtherAction('finally', finallyAction);
  defineOtherAction('lastly', finallyAction);

  defineOtherAction('and', {
    resolved: returnOther
  });

  var altAction = {
    rejected: returnOther
  };

  defineOtherAction('or', altAction);
  defineOtherAction('alt', altAction);

  defineParallelAction('_parallelAp', earlyCrash, earlyReject, noop, {
    resolved: apActionHandler
  });

  defineParallelAction('race', earlyCrash, earlyReject, earlyResolve, {});

  defineParallelAction('both', earlyCrash, earlyReject, noop, {
    resolved: function BothAction$resolved(x){
      return this.other._map(function BothAction$resolved$mapper(y){ return [x, y] });
    }
  });

  function Next(x){
    return {done: false, value: x};
  }

  function Done(x){
    return {done: true, value: x};
  }

  function isIteration(x){
    return isObject(x) && isBoolean(x.done);
  }

  function chainRec(step, init){
    return resolve(Next(init))._chain(function chainRec$recur(o){
      return o.done ? resolve(o.value) : step(Next, Done, o.value)._chain(chainRec$recur);
    });
  }

  function ap$mval(mval, mfunc){
    if(!isApply(mfunc)) throwInvalidArgument('ap', 1, 'be an Apply', mfunc);
    return mfunc[FL.ap](mval);
  }

  function ap(mval, mfunc){
    if(!isApply(mval)) throwInvalidArgument('ap', 0, 'be an Apply', mval);
    if(arguments.length === 1) return partial1(ap$mval, mval);
    return ap$mval(mval, mfunc);
  }

  function alt$left(left, right){
    if(!isAlt(right)) throwInvalidArgument('alt', 1, 'be an Alt', right);
    return left[FL.alt](right);
  }

  function alt(left, right){
    if(!isAlt(left)) throwInvalidArgument('alt', 0, 'be an Alt', left);
    if(arguments.length === 1) return partial1(alt$left, left);
    return alt$left(left, right);
  }

  function map$mapper(mapper, m){
    if(!isFunctor(m)) throwInvalidArgument('map', 1, 'be a Functor', m);
    return m[FL.map](mapper);
  }

  function map(mapper, m){
    if(!isFunction(mapper)) throwInvalidArgument('map', 0, 'be a Function', mapper);
    if(arguments.length === 1) return partial1(map$mapper, mapper);
    return map$mapper(mapper, m);
  }

  function bimap$lmapper$rmapper(lmapper, rmapper, m){
    if(!isBifunctor(m)) throwInvalidArgument('bimap', 2, 'be a Bifunctor', m);
    return m[FL.bimap](lmapper, rmapper);
  }

  function bimap$lmapper(lmapper, rmapper, m){
    if(!isFunction(rmapper)) throwInvalidArgument('bimap', 1, 'be a Function', rmapper);
    if(arguments.length === 2) return partial2(bimap$lmapper$rmapper, lmapper, rmapper);
    return bimap$lmapper$rmapper(lmapper, rmapper, m);
  }

  function bimap(lmapper, rmapper, m){
    if(!isFunction(lmapper)) throwInvalidArgument('bimap', 0, 'be a Function', lmapper);
    if(arguments.length === 1) return partial1(bimap$lmapper, lmapper);
    if(arguments.length === 2) return bimap$lmapper(lmapper, rmapper);
    return bimap$lmapper(lmapper, rmapper, m);
  }

  function chain$chainer(chainer, m){
    if(!isChain(m)) throwInvalidArgument('chain', 1, 'be a Chain', m);
    return m[FL.chain](chainer);
  }

  function chain(chainer, m){
    if(!isFunction(chainer)) throwInvalidArgument('chain', 0, 'be a Function', chainer);
    if(arguments.length === 1) return partial1(chain$chainer, chainer);
    return chain$chainer(chainer, m);
  }

  function mapRej$mapper(mapper, m){
    if(!isFuture(m)) throwInvalidFuture('mapRej', 1, m);
    return m.mapRej(mapper);
  }

  function mapRej(mapper, m){
    if(!isFunction(mapper)) throwInvalidArgument('mapRej', 0, 'be a Function', mapper);
    if(arguments.length === 1) return partial1(mapRej$mapper, mapper);
    return mapRej$mapper(mapper, m);
  }

  function chainRej$chainer(chainer, m){
    if(!isFuture(m)) throwInvalidFuture('chainRej', 1, m);
    return m.chainRej(chainer);
  }

  function chainRej(chainer, m){
    if(!isFunction(chainer)) throwInvalidArgument('chainRej', 0, 'be a Function', chainer);
    if(arguments.length === 1) return partial1(chainRej$chainer, chainer);
    return chainRej$chainer(chainer, m);
  }

  function lastly$right(right, left){
    if(!isFuture(left)) throwInvalidFuture('lastly', 1, left);
    return left.lastly(right);
  }

  function lastly(right, left){
    if(!isFuture(right)) throwInvalidFuture('lastly', 0, right);
    if(arguments.length === 1) return partial1(lastly$right, right);
    return lastly$right(right, left);
  }

  function and$left(left, right){
    if(!isFuture(right)) throwInvalidFuture('and', 1, right);
    return left.and(right);
  }

  function and(left, right){
    if(!isFuture(left)) throwInvalidFuture('and', 0, left);
    if(arguments.length === 1) return partial1(and$left, left);
    return and$left(left, right);
  }

  function both$left(left, right){
    if(!isFuture(right)) throwInvalidFuture('both', 1, right);
    return left.both(right);
  }

  function both(left, right){
    if(!isFuture(left)) throwInvalidFuture('both', 0, left);
    if(arguments.length === 1) return partial1(both$left, left);
    return both$left(left, right);
  }

  function race$right(right, left){
    if(!isFuture(left)) throwInvalidFuture('race', 1, left);
    return left.race(right);
  }

  function race(right, left){
    if(!isFuture(right)) throwInvalidFuture('race', 0, right);
    if(arguments.length === 1) return partial1(race$right, right);
    return race$right(right, left);
  }

  function swap(m){
    if(!isFuture(m)) throwInvalidFuture('swap', 0, m);
    return m.swap();
  }

  function fold$f$g(f, g, m){
    if(!isFuture(m)) throwInvalidFuture('fold', 2, m);
    return m.fold(f, g);
  }

  function fold$f(f, g, m){
    if(!isFunction(g)) throwInvalidArgument('fold', 1, 'be a Function', g);
    if(arguments.length === 2) return partial2(fold$f$g, f, g);
    return fold$f$g(f, g, m);
  }

  function fold(f, g, m){
    if(!isFunction(f)) throwInvalidArgument('fold', 0, 'be a Function', f);
    if(arguments.length === 1) return partial1(fold$f, f);
    if(arguments.length === 2) return fold$f(f, g);
    return fold$f(f, g, m);
  }

  function done$callback(callback, m){
    if(!isFuture(m)) throwInvalidFuture('done', 1, m);
    return m.done(callback);
  }

  function done(callback, m){
    if(!isFunction(callback)) throwInvalidArgument('done', 0, 'be a Function', callback);
    if(arguments.length === 1) return partial1(done$callback, callback);
    return done$callback(callback, m);
  }

  function fork$f$g(f, g, m){
    if(!isFuture(m)) throwInvalidFuture('fork', 2, m);
    return m._interpret(raise, f, g);
  }

  function fork$f(f, g, m){
    if(!isFunction(g)) throwInvalidArgument('fork', 1, 'be a Function', g);
    if(arguments.length === 2) return partial2(fork$f$g, f, g);
    return fork$f$g(f, g, m);
  }

  function fork(f, g, m){
    if(!isFunction(f)) throwInvalidArgument('fork', 0, 'be a Function', f);
    if(arguments.length === 1) return partial1(fork$f, f);
    if(arguments.length === 2) return fork$f(f, g);
    return fork$f(f, g, m);
  }

  function forkCatch(f, g, h, m){
    if(!isFunction(f)) throwInvalidArgument('forkCatch', 0, 'be a Function', f);
    if(arguments.length === 1) return partial1(forkCatch, f);
    if(!isFunction(g)) throwInvalidArgument('forkCatch', 1, 'be a Function', g);
    if(arguments.length === 2) return partial2(forkCatch, f, g);
    if(!isFunction(h)) throwInvalidArgument('forkCatch', 2, 'be a Function', h);
    if(arguments.length === 3) return partial3(forkCatch, f, g, h);
    if(!isFuture(m)) throwInvalidFuture('forkCatch', 3, m);
    return m._interpret(f, g, h);
  }

  function promise(m){
    if(!isFuture(m)) throwInvalidFuture('promise', 0, m);
    return m.promise();
  }

  function value$cont(cont, m){
    if(!isFuture(m)) throwInvalidFuture('value', 1, m);
    return m.value(cont);
  }

  function value(cont, m){
    if(!isFunction(cont)) throwInvalidArgument('value', 0, 'be a Function', cont);
    if(arguments.length === 1) return partial1(value$cont, cont);
    return value$cont(cont, m);
  }

  function extractLeft(m){
    if(!isFuture(m)) throwInvalidFuture('extractLeft', 0, m);
    return m.extractLeft();
  }

  function extractRight(m){
    if(!isFuture(m)) throwInvalidFuture('extractRight', 0, m);
    return m.extractRight();
  }

  function After(time, value){
    this._time = time;
    this._value = value;
    this.context = captureContext(nil, 'a Future created with after', After);
  }

  After.prototype = Object.create(Future.prototype);

  After.prototype._interpret = function After$interpret(rec, rej, res){
    var id = setTimeout(res, this._time, this._value);
    return function After$cancel(){ clearTimeout(id); };
  };

  After.prototype.extractRight = function After$extractRight(){
    return [this._value];
  };

  After.prototype.toString = function After$toString(){
    return 'after(' + show(this._time) + ', ' + show(this._value) + ')';
  };

  function RejectAfter(time, value){
    this._time = time;
    this._value = value;
    this.context = captureContext(nil, 'a Future created with rejectAfter', After);
  }

  RejectAfter.prototype = Object.create(Future.prototype);

  RejectAfter.prototype._interpret = function RejectAfter$interpret(rec, rej){
    var id = setTimeout(rej, this._time, this._value);
    return function RejectAfter$cancel(){ clearTimeout(id); };
  };

  RejectAfter.prototype.extractLeft = function RejectAfter$extractLeft(){
    return [this._value];
  };

  RejectAfter.prototype.toString = function RejectAfter$toString(){
    return 'rejectAfter(' + show(this._time) + ', ' + show(this._value) + ')';
  };

  function after$time(time, value){
    return time === Infinity ? never : new After(time, value);
  }

  function after(time, value){
    if(!isUnsigned(time)) throwInvalidArgument('after', 0, 'be a positive Integer', time);
    if(arguments.length === 1) return partial1(after$time, time);
    return after$time(time, value);
  }

  function rejectAfter$time(time, reason){
    return time === Infinity ? never : new RejectAfter(time, reason);
  }

  function rejectAfter(time, reason){
    if(!isUnsigned(time)){
      throwInvalidArgument('rejectAfter', 0, 'be a positive Integer', time);
    }
    if(arguments.length === 1) return partial1(rejectAfter$time, time);
    return rejectAfter$time(time, reason);
  }

  function Attempt(fn){
    this._fn = fn;
    this.context = captureContext(nil, 'a Future created with attempt/try', Attempt);
  }

  Attempt.prototype = Object.create(Future.prototype);

  Attempt.prototype._interpret = function Attempt$interpret(rec, rej, res){
    var r;
    try{ r = this._fn(); }catch(e){ rej(e); return noop }
    res(r);
    return noop;
  };

  Attempt.prototype.toString = function Attempt$toString(){
    return 'attempt(' + showf(this._fn) + ')';
  };

  function attempt(f){
    if(!isFunction(f)) throwInvalidArgument('attempt', 0, 'be a Function', f);
    return new Attempt(f);
  }

  var Cold = Cached.Cold = 0;
  var Pending = Cached.Pending = 1;
  var Crashed$1 = Cached.Crashed = 2;
  var Rejected$1 = Cached.Rejected = 3;
  var Resolved$1 = Cached.Resolved = 4;

  function Queued(rec, rej, res){
    this[Crashed$1] = rec;
    this[Rejected$1] = rej;
    this[Resolved$1] = res;
  }

  function Cached(pure){
    this._pure = pure;
    this.reset();
  }

  Cached.prototype = Object.create(Future.prototype);

  Cached.prototype.extractLeft = function Cached$extractLeft(){
    return this._state === Rejected$1 ? [this._value] : [];
  };

  Cached.prototype.extractRight = function Cached$extractRight(){
    return this._state === Resolved$1 ? [this._value] : [];
  };

  Cached.prototype._addToQueue = function Cached$addToQueue(rec, rej, res){
    var _this = this;
    if(_this._state > Pending) return noop;
    var i = _this._queue.push(new Queued(rec, rej, res)) - 1;
    _this._queued = _this._queued + 1;

    return function Cached$removeFromQueue(){
      if(_this._state > Pending) return;
      _this._queue[i] = undefined;
      _this._queued = _this._queued - 1;
      if(_this._queued === 0) _this.reset();
    };
  };

  Cached.prototype._drainQueue = function Cached$drainQueue(){
    if(this._state <= Pending) return;
    if(this._queued === 0) return;
    var queue = this._queue;
    var length = queue.length;
    var state = this._state;
    var value = this._value;

    for(var i = 0; i < length; i++){
      queue[i] && queue[i][state](value);
      queue[i] = undefined;
    }

    this._queue = undefined;
    this._queued = 0;
  };

  Cached.prototype.crash = function Cached$crash(error){
    if(this._state > Pending) return;
    this._value = error;
    this._state = Crashed$1;
    this._drainQueue();
  };

  Cached.prototype.reject = function Cached$reject(reason){
    if(this._state > Pending) return;
    this._value = reason;
    this._state = Rejected$1;
    this._drainQueue();
  };

  Cached.prototype.resolve = function Cached$resolve(value){
    if(this._state > Pending) return;
    this._value = value;
    this._state = Resolved$1;
    this._drainQueue();
  };

  Cached.prototype.run = function Cached$run(){
    var _this = this;
    if(_this._state > Cold) return;
    _this._state = Pending;
    _this._cancel = _this._pure._interpret(
      function Cached$fork$rec(x){ _this.crash(x); },
      function Cached$fork$rej(x){ _this.reject(x); },
      function Cached$fork$res(x){ _this.resolve(x); }
    );
  };

  Cached.prototype.reset = function Cached$reset(){
    if(this._state === Cold) return;
    if(this._state === Pending) this._cancel();
    this._cancel = noop;
    this._queue = [];
    this._queued = 0;
    this._value = undefined;
    this._state = Cold;
  };

  Cached.prototype._interpret = function Cached$interpret(rec, rej, res){
    var cancel = noop;

    switch(this._state){
      case Pending: cancel = this._addToQueue(rec, rej, res); break;
      case Crashed$1: rec(this._value); break;
      case Rejected$1: rej(this._value); break;
      case Resolved$1: res(this._value); break;
      default: cancel = this._addToQueue(rec, rej, res); this.run();
    }

    return cancel;
  };

  Cached.prototype.toString = function Cached$toString(){
    return 'cache(' + this._pure.toString() + ')';
  };

  function cache(m){
    if(!isFuture(m)) throwInvalidFuture('cache', 0, m);
    return new Cached(m);
  }

  function Encase(fn, a){
    this._fn = fn;
    this._a = a;
    this.context = captureContext(nil, 'a Future created with encase', Encase);
  }

  Encase.prototype = Object.create(Future.prototype);

  Encase.prototype._interpret = function Encase$interpret(rec, rej, res){
    var r;
    try{ r = this._fn(this._a); }catch(e){ rej(e); return noop }
    res(r);
    return noop;
  };

  Encase.prototype.toString = function Encase$toString(){
    return 'encase(' + showf(this._fn) + ', ' + show(this._a) + ')';
  };

  function encase(f, x){
    if(!isFunction(f)) throwInvalidArgument('encase', 0, 'be a Function', f);
    if(arguments.length === 1) return partial1(encase, f);
    return new Encase(f, x);
  }

  function Encase2(fn, a, b){
    this._fn = fn;
    this._a = a;
    this._b = b;
    this.context = captureContext(nil, 'a Future created with encase2', Encase2);
  }

  Encase2.prototype = Object.create(Future.prototype);

  Encase2.prototype._interpret = function Encase2$interpret(rec, rej, res){
    var r;
    try{ r = this._fn(this._a, this._b); }catch(e){ rej(e); return noop }
    res(r);
    return noop;
  };

  Encase2.prototype.toString = function Encase2$toString(){
    return 'encase2(' + showf(this._fn) + ', ' + show(this._a) + ', ' + show(this._b) + ')';
  };

  function encase2(f, x, y){
    if(!isFunction(f)) throwInvalidArgument('encase2', 0, 'be a Function', f);

    switch(arguments.length){
      case 1: return partial1(encase2, f);
      case 2: return partial2(encase2, f, x);
      default: return new Encase2(f, x, y);
    }
  }

  function Encase3(fn, a, b, c){
    this._fn = fn;
    this._a = a;
    this._b = b;
    this._c = c;
    this.context = captureContext(nil, 'a Future created with encase3', Encase3);
  }

  Encase3.prototype = Object.create(Future.prototype);

  Encase3.prototype._interpret = function Encase3$interpret(rec, rej, res){
    var r;
    try{ r = this._fn(this._a, this._b, this._c); }catch(e){ rej(e); return noop }
    res(r);
    return noop;
  };

  Encase3.prototype.toString = function Encase3$toString(){
    return 'encase3('
         + showf(this._fn)
         + ', '
         + show(this._a)
         + ', '
         + show(this._b)
         + ', '
         + show(this._c)
         + ')';
  };

  function encase3(f, x, y, z){
    if(!isFunction(f)) throwInvalidArgument('encase3', 0, 'be a Function', f);

    switch(arguments.length){
      case 1: return partial1(encase3, f);
      case 2: return partial2(encase3, f, x);
      case 3: return partial3(encase3, f, x, y);
      default: return new Encase3(f, x, y, z);
    }
  }

  function EncaseN(fn, a){
    this._fn = fn;
    this._a = a;
    this.context = captureContext(nil, 'a Future created with encaseN', EncaseN);
  }

  EncaseN.prototype = Object.create(Future.prototype);

  EncaseN.prototype._interpret = function EncaseN$interpret(rec, rej, res){
    var open = false, cont = function(){ open = true; };
    var context = captureContext(this.context, 'consuming an encased Future', EncaseN$interpret);
    try{
      this._fn(this._a, function EncaseN$done(err, val){
        cont = err ? function EncaseN3$rej(){
          open = false;
          rej(err);
        } : function EncaseN3$res(){
          open = false;
          res(val);
        };
        if(open){
          cont();
        }
      });
    }catch(e){
      rec(makeError(e, this, context));
      open = false;
      return noop;
    }
    cont();
    return function EncaseN$cancel(){ open = false; };
  };

  EncaseN.prototype.toString = function EncaseN$toString(){
    return 'encaseN(' + showf(this._fn) + ', ' + show(this._a) + ')';
  };

  function encaseN(f, x){
    if(!isFunction(f)) throwInvalidArgument('encaseN', 0, 'be a Function', f);
    if(arguments.length === 1) return partial1(encaseN, f);
    return new EncaseN(f, x);
  }

  function EncaseN2(fn, a, b){
    this._fn = fn;
    this._a = a;
    this._b = b;
    this.context = captureContext(nil, 'a Future created with encaseN2', EncaseN2);
  }

  EncaseN2.prototype = Object.create(Future.prototype);

  EncaseN2.prototype._interpret = function EncaseN2$interpret(rec, rej, res){
    var open = false, cont = function(){ open = true; };
    var context = captureContext(this.context, 'consuming an encased Future', EncaseN2$interpret);
    try{
      this._fn(this._a, this._b, function EncaseN2$done(err, val){
        cont = err ? function EncaseN2$rej(){
          open = false;
          rej(err);
        } : function EncaseN2$res(){
          open = false;
          res(val);
        };
        if(open){
          cont();
        }
      });
    }catch(e){
      rec(makeError(e, this, context));
      open = false;
      return noop;
    }
    cont();
    return function EncaseN2$cancel(){ open = false; };
  };

  EncaseN2.prototype.toString = function EncaseN2$toString(){
    return 'encaseN2(' + showf(this._fn) + ', ' + show(this._a) + ', ' + show(this._b) + ')';
  };

  function encaseN2(f, x, y){
    if(!isFunction(f)) throwInvalidArgument('encaseN2', 0, 'be a Function', f);

    switch(arguments.length){
      case 1: return partial1(encaseN2, f);
      case 2: return partial2(encaseN2, f, x);
      default: return new EncaseN2(f, x, y);
    }
  }

  function EncaseN3(fn, a, b, c){
    this._fn = fn;
    this._a = a;
    this._b = b;
    this._c = c;
    this.context = captureContext(nil, 'a Future created with encaseN3', EncaseN3);
  }

  EncaseN3.prototype = Object.create(Future.prototype);

  EncaseN3.prototype._interpret = function EncaseN3$interpret(rec, rej, res){
    var open = false, cont = function(){ open = true; };
    var context = captureContext(this.context, 'consuming an encased Future', EncaseN3$interpret);
    try{
      this._fn(this._a, this._b, this._c, function EncaseN3$done(err, val){
        cont = err ? function EncaseN3$rej(){
          open = false;
          rej(err);
        } : function EncaseN3$res(){
          open = false;
          res(val);
        };
        if(open){
          cont();
        }
      });
    }catch(e){
      rec(makeError(e, this, context));
      open = false;
      return noop;
    }
    cont();
    return function EncaseN3$cancel(){ open = false; };
  };

  EncaseN3.prototype.toString = function EncaseN3$toString(){
    return 'encaseN3('
         + showf(this._fn)
         + ', '
         + show(this._a)
         + ', '
         + show(this._b)
         + ', '
         + show(this._c)
         + ')';
  };

  function encaseN3(f, x, y, z){
    if(!isFunction(f)) throwInvalidArgument('encaseN3', 0, 'be a Function', f);

    switch(arguments.length){
      case 1: return partial1(encaseN3, f);
      case 2: return partial2(encaseN3, f, x);
      case 3: return partial3(encaseN3, f, x, y);
      default: return new EncaseN3(f, x, y, z);
    }
  }

  function invalidPromise(p, f, a){
    return typeError(
      'encaseP() expects the function it\'s given to return a Promise/Thenable'
      + '\n  Actual: ' + (show(p)) + '\n  From calling: ' + (showf(f))
      + '\n  With: ' + (show(a))
    );
  }

  function EncaseP(fn, a){
    this._fn = fn;
    this._a = a;
    this.context = captureContext(nil, 'a Future created with encaseP', EncaseP);
  }

  EncaseP.prototype = Object.create(Future.prototype);

  EncaseP.prototype._interpret = function EncaseP$interpret(rec, rej, res){
    var open = true, fn = this._fn, a = this._a, p;
    var context = captureContext(this.context, 'consuming an encased Future', EncaseP$interpret);
    try{
      p = fn(a);
    }catch(e){
      rec(makeError(e, this, context));
      return noop;
    }
    if(!isThenable(p)){
      rec(makeError(invalidPromise(p, fn, a), this, context));
      return noop;
    }
    p.then(function EncaseP$res(x){
      if(open){
        open = false;
        res(x);
      }
    }, function EncaseP$rej(x){
      if(open){
        open = false;
        rej(x);
      }
    });
    return function EncaseP$cancel(){ open = false; };
  };

  EncaseP.prototype.toString = function EncaseP$toString(){
    return 'encaseP(' + showf(this._fn) + ', ' + show(this._a) + ')';
  };

  function encaseP(f, x){
    if(!isFunction(f)) throwInvalidArgument('encaseP', 0, 'be a Function', f);
    if(arguments.length === 1) return partial1(encaseP, f);
    return new EncaseP(f, x);
  }

  function invalidPromise$1(p, f, a, b){
    return typeError(
      'encaseP2() expects the function it\'s given to return a Promise/Thenable'
      + '\n  Actual: ' + (show(p)) + '\n  From calling: ' + (showf(f))
      + '\n  With 1: ' + (show(a))
      + '\n  With 2: ' + (show(b))
    );
  }

  function EncaseP2(fn, a, b){
    this._fn = fn;
    this._a = a;
    this._b = b;
    this.context = captureContext(nil, 'a Future created with encaseP2', EncaseP2);
  }

  EncaseP2.prototype = Object.create(Future.prototype);

  EncaseP2.prototype._interpret = function EncaseP2$interpret(rec, rej, res){
    var open = true, fn = this._fn, a = this._a, b = this._b, p;
    var context = captureContext(this.context, 'consuming an encased Future', EncaseP2$interpret);
    try{
      p = fn(a, b);
    }catch(e){
      rec(makeError(e, this, context));
      return noop;
    }
    if(!isThenable(p)){
      rec(makeError(invalidPromise$1(p, fn, a, b), this, context));
      return noop;
    }
    p.then(function EncaseP2$res(x){
      if(open){
        open = false;
        res(x);
      }
    }, function EncaseP2$rej(x){
      if(open){
        open = false;
        rej(x);
      }
    });
    return function EncaseP2$cancel(){ open = false; };
  };

  EncaseP2.prototype.toString = function EncaseP2$toString(){
    return 'encaseP2(' + showf(this._fn) + ', ' + show(this._a) + ', ' + show(this._b) + ')';
  };

  function encaseP2(f, x, y){
    if(!isFunction(f)) throwInvalidArgument('encaseP2', 0, 'be a Function', f);

    switch(arguments.length){
      case 1: return partial1(encaseP2, f);
      case 2: return partial2(encaseP2, f, x);
      default: return new EncaseP2(f, x, y);
    }
  }

  function invalidPromise$2(p, f, a, b, c){
    return typeError(
      'encaseP3() expects the function it\'s given to return a Promise/Thenable'
      + '\n  Actual: ' + (show(p)) + '\n  From calling: ' + (showf(f))
      + '\n  With 1: ' + (show(a))
      + '\n  With 2: ' + (show(b))
      + '\n  With 3: ' + (show(c))
    );
  }

  function EncaseP3(fn, a, b, c){
    this._fn = fn;
    this._a = a;
    this._b = b;
    this._c = c;
    this.context = captureContext(nil, 'a Future created with encaseP3', EncaseP3);
  }

  EncaseP3.prototype = Object.create(Future.prototype);

  EncaseP3.prototype._interpret = function EncaseP3$interpret(rec, rej, res){
    var open = true, fn = this._fn, a = this._a, b = this._b, c = this._c, p;
    var context = captureContext(this.context, 'consuming an encased Future', EncaseP3$interpret);
    try{
      p = fn(a, b, c);
    }catch(e){
      rec(makeError(e, this, context));
      return noop;
    }
    if(!isThenable(p)){
      rec(makeError(invalidPromise$2(p, fn, a, b, c), this, context));
      return noop;
    }
    p.then(function EncaseP3$res(x){
      if(open){
        open = false;
        res(x);
      }
    }, function EncaseP3$rej(x){
      if(open){
        open = false;
        rej(x);
      }
    });
    return function EncaseP3$cancel(){ open = false; };
  };

  EncaseP3.prototype.toString = function EncaseP3$toString(){
    return 'encaseP3('
         + showf(this._fn)
         + ', '
         + show(this._a)
         + ', '
         + show(this._b)
         + ', '
         + show(this._c)
         + ')';
  };

  function encaseP3(f, x, y, z){
    if(!isFunction(f)) throwInvalidArgument('encaseP3', 0, 'be a Function', f);

    switch(arguments.length){
      case 1: return partial1(encaseP3, f);
      case 2: return partial2(encaseP3, f, x);
      case 3: return partial3(encaseP3, f, x, y);
      default: return new EncaseP3(f, x, y, z);
    }
  }

  var Undetermined = 0;
  var Synchronous = 1;
  var Asynchronous = 2;

  /*eslint consistent-return: 0, no-cond-assign: 0*/

  function invalidIteration(o){
    return typeError(
      'The iterator did not return a valid iteration from iterator.next()\n' +
      '  Actual: ' + show(o)
    );
  }

  function invalidState(x){
    return invalidFuture(
      'go',
      'the iterator to produce only valid Futures',
      x,
      '\n  Tip: If you\'re using a generator, make sure you always yield a Future'
    );
  }

  function Go(generator){
    this._generator = generator;
    this.context = captureContext(nil, 'a Future created with do-notation', Go);
  }

  Go.prototype = Object.create(Future.prototype);

  Go.prototype._interpret = function Go$interpret(rec, rej, res){

    var _this = this, timing = Undetermined, cancel = noop, state, value, iterator;

    var context = captureContext(
      _this.context,
      'interpreting a Future created with do-notation',
      Go$interpret
    );

    try{
      iterator = _this._generator();
    }catch(e){
      rec(makeError(e, _this, context));
      return noop;
    }

    if(!isIterator(iterator)){
      rec(makeError(
        invalidArgument('go', 0, 'return an iterator, maybe you forgot the "*"', iterator),
        _this,
        context
      ));
      return noop;
    }

    function resolved(x){
      value = x;
      if(timing === Asynchronous){
        context = cat(state.value.context, context);
        return drain();
      }
      timing = Synchronous;
    }

    function crash(e){
      rec(makeError(e, state.value, cat(state.value.context, context)));
    }

    function drain(){
      //eslint-disable-next-line no-constant-condition
      while(true){
        try{
          state = iterator.next(value);
        }catch(e){
          return rec(makeError(e, _this, context));
        }
        if(!isIteration(state)) return rec(makeError(invalidIteration(state), _this, context));
        if(state.done) break;
        if(!isFuture(state.value)) return rec(makeError(invalidState(state.value), _this, context));
        timing = Undetermined;
        cancel = state.value._interpret(crash, rej, resolved);
        if(timing === Undetermined) return timing = Asynchronous;
      }
      res(state.value);
    }

    drain();

    return function Go$cancel(){ cancel(); };

  };

  Go.prototype.toString = function Go$toString(){
    return 'go(' + showf(this._generator) + ')';
  };

  function go(generator){
    if(!isFunction(generator)) throwInvalidArgument('go', 0, 'be a Function', generator);
    return new Go(generator);
  }

  function invalidDisposal(m, f, x){
    return invalidFuture(
      'hook',
      'the first function it\'s given to return a Future',
      m,
      '\n  From calling: ' + showf(f) + '\n  With: ' + show(x)
    );
  }

  function invalidConsumption(m, f, x){
    return invalidFuture(
      'hook',
      'the second function it\'s given to return a Future',
      m,
      '\n  From calling: ' + showf(f) + '\n  With: ' + show(x)
    );
  }

  function Hook(acquire, dispose, consume){
    this._acquire = acquire;
    this._dispose = dispose;
    this._consume = consume;
    this.context = captureContext(nil, 'a Future created with hook', Hook);
  }

  Hook.prototype = Object.create(Future.prototype);

  Hook.prototype._interpret = function Hook$interpret(rec, rej, res){

    var _this = this, _acquire = this._acquire, _dispose = this._dispose, _consume = this._consume;
    var cancel, cancelConsume = noop, resource, value, cont = noop;
    var context = captureContext(_this.context, 'interpreting a hooked Future', Hook$interpret);

    function Hook$done(){
      cont(value);
    }

    function Hook$dispose(){
      var disposal;
      try{
        disposal = _dispose(resource);
      }catch(e){
        return rec(makeError(e, _this, context));
      }
      if(!isFuture(disposal)){
        return rec(makeError(invalidDisposal(disposal, _dispose, resource), _this, context));
      }
      cancel = Hook$cancelDisposal;
      disposal._interpret(Hook$disposalCrashed, Hook$disposalRejected, Hook$done);
    }

    function Hook$cancelConsumption(){
      cancelConsume();
      Hook$dispose();
      Hook$cancelDisposal();
    }

    function Hook$cancelDisposal(){
      cont = noop;
    }

    function Hook$disposalCrashed(x){
      rec(makeError(x, _this, context));
    }

    function Hook$disposalRejected(x){
      rec(makeError(new Error('The disposal Future rejected with ' + show(x)), _this, context));
    }

    function Hook$consumptionException(x){
      context = captureContext(context, 'resource consumption crashing', Hook$dispose);
      cont = rec;
      value = x;
      Hook$dispose();
    }

    function Hook$consumptionRejected(x){
      context = captureContext(context, 'resource consumption failing', Hook$consumptionRejected);
      cont = rej;
      value = x;
      Hook$dispose();
    }

    function Hook$consumptionResolved(x){
      context = captureContext(context, 'resource consumption', Hook$consumptionResolved);
      cont = res;
      value = x;
      Hook$dispose();
    }

    function Hook$consume(x){
      context = captureContext(context, 'hook acquiring a resource', Hook$consume);
      resource = x;
      var consumption;
      try{
        consumption = _consume(resource);
      }catch(e){
        return Hook$consumptionException(makeError(e, _this, context));
      }
      if(!isFuture(consumption)){
        return Hook$consumptionException(makeError(
          invalidConsumption(consumption, _consume, resource),
          _this,
          context
        ));
      }
      cancel = Hook$cancelConsumption;
      cancelConsume = consumption._interpret(
        Hook$consumptionException,
        Hook$consumptionRejected,
        Hook$consumptionResolved
      );
    }

    var cancelAcquire = _acquire._interpret(rec, rej, Hook$consume);
    cancel = cancel || cancelAcquire;

    return function Hook$fork$cancel(){
      rec = raise;
      cancel();
    };

  };

  Hook.prototype.toString = function Hook$toString(){
    return 'hook('
         + this._acquire.toString()
         + ', '
         + showf(this._dispose)
         + ', '
         + showf(this._consume)
         + ')';
  };

  function hook$acquire$cleanup(acquire, cleanup, consume){
    if(!isFunction(consume)) throwInvalidArgument('hook', 2, 'be a Function', consume);
    return new Hook(acquire, cleanup, consume);
  }

  function hook$acquire(acquire, cleanup, consume){
    if(!isFunction(cleanup)) throwInvalidArgument('hook', 1, 'be a Function', cleanup);
    if(arguments.length === 2) return partial2(hook$acquire$cleanup, acquire, cleanup);
    return hook$acquire$cleanup(acquire, cleanup, consume);
  }

  function hook(acquire, cleanup, consume){
    if(!isFuture(acquire)) throwInvalidFuture('hook', 0, acquire);
    if(arguments.length === 1) return partial1(hook$acquire, acquire);
    if(arguments.length === 2) return hook$acquire(acquire, cleanup);
    return hook$acquire(acquire, cleanup, consume);
  }

  function Node(fn){
    this._fn = fn;
    this.context = captureContext(nil, 'a Future created with node');
  }

  Node.prototype = Object.create(Future.prototype);

  Node.prototype._interpret = function Node$interpret(rec, rej, res){
    var open = false, cont = function(){ open = true; };
    try{
      this._fn(function Node$done(err, val){
        cont = err ? function Node$rej(){
          open = false;
          rej(err);
        } : function Node$res(){
          open = false;
          res(val);
        };
        if(open){
          cont();
        }
      });
    }catch(e){
      rec(makeError(e, this, this.context));
      open = false;
      return noop;
    }
    cont();
    return function Node$cancel(){ open = false; };
  };

  Node.prototype.toString = function Node$toString(){
    return 'node(' + showf(this._fn) + ')';
  };

  function node(f){
    if(!isFunction(f)) throwInvalidArgument('node', 0, 'be a Function', f);
    return new Node(f);
  }

  function Parallel(max, futures){
    this._futures = futures;
    this._length = futures.length;
    this._max = Math.min(this._length, max);
    this.context = captureContext(nil, 'a Future created with parallel', Parallel);
  }

  Parallel.prototype = Object.create(Future.prototype);

  Parallel.prototype._interpret = function Parallel$interpret(rec, rej, res){

    var _futures = this._futures, _length = this._length, _max = this._max;
    var cancels = new Array(_length), out = new Array(_length);
    var cursor = 0, running = 0, blocked = false, cont = noop;
    var context = captureContext(this.context, 'consuming a parallel Future', Parallel$interpret);

    function Parallel$cancel(){
      rec = noop;
      rej = noop;
      res = noop;
      cursor = _length;
      for(var n = 0; n < _length; n++) cancels[n] && cancels[n]();
    }

    function Parallel$run(idx){
      running++;
      cancels[idx] = _futures[idx]._interpret(function Parallel$rec(e){
        cont = rec;
        cancels[idx] = noop;
        Parallel$cancel();
        cont(makeError(e, _futures[idx], context));
      }, function Parallel$rej(reason){
        cont = rej;
        cancels[idx] = noop;
        Parallel$cancel();
        cont(reason);
      }, function Parallel$res(value){
        cancels[idx] = noop;
        out[idx] = value;
        running--;
        if(cursor === _length && running === 0) res(out);
        else if(blocked) Parallel$drain();
      });
    }

    function Parallel$drain(){
      blocked = false;
      while(cursor < _length && running < _max) Parallel$run(cursor++);
      blocked = true;
    }

    Parallel$drain();

    return Parallel$cancel;

  };

  Parallel.prototype.toString = function Parallel$toString(){
    return 'parallel(' + this._max + ', ' + show(this._futures) + ')';
  };

  var emptyArray = new Resolved([]);

  function validateNthFuture(m, xs){
    if(!isFuture(m)) throwInvalidFuture(
      'parallel',
      'its second argument to be an Array of valid Futures',
      xs
    );
  }

  function parallel$max(max, xs){
    if(!isArray(xs)) throwInvalidArgument('parallel', 1, 'be an Array of valid Futures', xs);
    for(var idx = 0; idx < xs.length; idx++) validateNthFuture(xs[idx], xs);
    return xs.length === 0 ? emptyArray : new Parallel(max, xs);
  }

  function parallel(max, xs){
    if(!isUnsigned(max)) throwInvalidArgument('parallel', 0, 'be a positive Integer', max);
    if(arguments.length === 1) return partial1(parallel$max, max);
    return parallel$max(max, xs);
  }

  function invalidPromise$3(p, f){
    return typeError(
      'tryP() expects the function it\'s given to return a Promise/Thenable'
      + '\n  Actual: ' + show(p) + '\n  From calling: ' + showf(f)
    );
  }

  function TryP(fn){
    this._fn = fn;
    this.context = captureContext(nil, 'a Future created with tryP', TryP);
  }

  TryP.prototype = Object.create(Future.prototype);

  TryP.prototype._interpret = function TryP$interpret(rec, rej, res){
    var open = true, fn = this._fn, p;
    try{
      p = fn();
    }catch(e){
      rec(makeError(e, this, this.context));
      return noop;
    }
    if(!isThenable(p)){
      rec(makeError(invalidPromise$3(p, fn), this, this.context));
      return noop;
    }
    p.then(function TryP$res(x){
      if(open){
        open = false;
        res(x);
      }
    }, function TryP$rej(x){
      if(open){
        open = false;
        rej(x);
      }
    });
    return function TryP$cancel(){ open = false; };
  };

  TryP.prototype.toString = function TryP$toString(){
    return 'tryP(' + show(this._fn) + ')';
  };

  function tryP(f){
    if(!isFunction(f)) throwInvalidArgument('tryP', 0, 'be a Function', f);
    return new TryP(f);
  }

  Future.resolve = Future.of = Future[FL.of] = resolve;
  Future.chainRec = Future[FL.chainRec] = chainRec;
  Future.reject = reject;
  Future.ap = ap;
  Future.alt = alt;
  Future.map = map;
  Future.bimap = bimap;
  Future.chain = chain;

  var Par = concurrify(Future, never, race, function parallelAp(a, b){ return b._parallelAp(a) });
  Par.of = Par[FL.of];
  Par.zero = Par[FL.zero];
  Par.map = map;
  Par.ap = ap;
  Par.alt = alt;

  function isParallel(x){
    return x instanceof Par || type(x) === Par['@@type'];
  }

  function seq(par){
    if(!isParallel(par)) throwInvalidArgument('seq', 0, 'be a ConcurrentFuture', par);
    return par.sequential;
  }

  var Fluture = /*#__PURE__*/Object.freeze({
    Future: Future,
    'default': Future,
    Par: Par,
    isParallel: isParallel,
    seq: seq,
    isFuture: isFuture,
    reject: reject,
    resolve: resolve,
    of: resolve,
    never: never,
    isNever: isNever,
    after: after,
    rejectAfter: rejectAfter,
    attempt: attempt,
    'try': attempt,
    cache: cache,
    encase: encase,
    encase2: encase2,
    encase3: encase3,
    encaseN: encaseN,
    encaseN2: encaseN2,
    encaseN3: encaseN3,
    encaseP: encaseP,
    encaseP2: encaseP2,
    encaseP3: encaseP3,
    go: go,
    'do': go,
    hook: hook,
    node: node,
    parallel: parallel,
    tryP: tryP,
    debugMode: debugMode,
    ap: ap,
    alt: alt,
    or: alt,
    map: map,
    bimap: bimap,
    chain: chain,
    mapRej: mapRej,
    chainRej: chainRej,
    lastly: lastly,
    'finally': lastly,
    and: and,
    both: both,
    race: race,
    swap: swap,
    fold: fold,
    done: done,
    fork: fork,
    forkCatch: forkCatch,
    promise: promise,
    value: value,
    extractLeft: extractLeft,
    extractRight: extractRight
  });

  var index_cjs = Object.assign(Future, Fluture);

  return index_cjs;

}));
