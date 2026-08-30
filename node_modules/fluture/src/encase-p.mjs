import {Future} from './future';
import {noop, show, showf, partial1} from './internal/utils';
import {isThenable, isFunction} from './internal/predicates';
import {typeError} from './internal/error';
import {throwInvalidArgument} from './internal/throw';
import {makeError} from './internal/error';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

function invalidPromise(p, f, a){
  return typeError(
    'encaseP() expects the function it\'s given to return a Promise/Thenable'
    + '\n  Actual: ' + (show(p)) + '\n  From calling: ' + (showf(f))
    + '\n  With: ' + (show(a))
  );
}

export function EncaseP(fn, a){
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
  return function EncaseP$cancel(){ open = false };
};

EncaseP.prototype.toString = function EncaseP$toString(){
  return 'encaseP(' + showf(this._fn) + ', ' + show(this._a) + ')';
};

export function encaseP(f, x){
  if(!isFunction(f)) throwInvalidArgument('encaseP', 0, 'be a Function', f);
  if(arguments.length === 1) return partial1(encaseP, f);
  return new EncaseP(f, x);
}
