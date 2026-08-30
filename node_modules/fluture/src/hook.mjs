import {Future, isFuture} from './future';
import {noop, show, showf, partial1, partial2, raise} from './internal/utils';
import {isFunction} from './internal/predicates';
import {invalidFuture, makeError} from './internal/error';
import {throwInvalidArgument, throwInvalidFuture} from './internal/throw';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

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

export function Hook(acquire, dispose, consume){
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

export function hook(acquire, cleanup, consume){
  if(!isFuture(acquire)) throwInvalidFuture('hook', 0, acquire);
  if(arguments.length === 1) return partial1(hook$acquire, acquire);
  if(arguments.length === 2) return hook$acquire(acquire, cleanup);
  return hook$acquire(acquire, cleanup, consume);
}
