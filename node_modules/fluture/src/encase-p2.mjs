import {Future} from './future';
import {noop, show, showf, partial1, partial2} from './internal/utils';
import {isThenable, isFunction} from './internal/predicates';
import {typeError} from './internal/error';
import {throwInvalidArgument} from './internal/throw';
import {makeError} from './internal/error';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

function invalidPromise(p, f, a, b){
  return typeError(
    'encaseP2() expects the function it\'s given to return a Promise/Thenable'
    + '\n  Actual: ' + (show(p)) + '\n  From calling: ' + (showf(f))
    + '\n  With 1: ' + (show(a))
    + '\n  With 2: ' + (show(b))
  );
}

export function EncaseP2(fn, a, b){
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
    rec(makeError(invalidPromise(p, fn, a, b), this, context));
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
  return function EncaseP2$cancel(){ open = false };
};

EncaseP2.prototype.toString = function EncaseP2$toString(){
  return 'encaseP2(' + showf(this._fn) + ', ' + show(this._a) + ', ' + show(this._b) + ')';
};

export function encaseP2(f, x, y){
  if(!isFunction(f)) throwInvalidArgument('encaseP2', 0, 'be a Function', f);

  switch(arguments.length){
    case 1: return partial1(encaseP2, f);
    case 2: return partial2(encaseP2, f, x);
    default: return new EncaseP2(f, x, y);
  }
}
