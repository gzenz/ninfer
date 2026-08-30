/*eslint consistent-return: 0, no-cond-assign: 0*/

import {Future, isFuture} from './future';
import {isFunction, isIterator} from './internal/predicates';
import {isIteration} from './internal/iteration';
import {show, showf, noop} from './internal/utils';
import {typeError, invalidFuture, invalidArgument, makeError} from './internal/error';
import {throwInvalidArgument} from './internal/throw';
import {Undetermined, Synchronous, Asynchronous} from './internal/timing';
import {nil, cat} from './internal/list';
import {captureContext} from './internal/debug';

export function invalidIteration(o){
  return typeError(
    'The iterator did not return a valid iteration from iterator.next()\n' +
    '  Actual: ' + show(o)
  );
}

export function invalidState(x){
  return invalidFuture(
    'go',
    'the iterator to produce only valid Futures',
    x,
    '\n  Tip: If you\'re using a generator, make sure you always yield a Future'
  );
}

export function Go(generator){
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

  return function Go$cancel(){ cancel() };

};

Go.prototype.toString = function Go$toString(){
  return 'go(' + showf(this._generator) + ')';
};

export function go(generator){
  if(!isFunction(generator)) throwInvalidArgument('go', 0, 'be a Function', generator);
  return new Go(generator);
}
