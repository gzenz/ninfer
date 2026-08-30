import {Future} from './future';
import {noop, show, showf} from './internal/utils';
import {isThenable, isFunction} from './internal/predicates';
import {typeError} from './internal/error';
import {throwInvalidArgument} from './internal/throw';
import {makeError} from './internal/error';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

function invalidPromise(p, f){
  return typeError(
    'tryP() expects the function it\'s given to return a Promise/Thenable'
    + '\n  Actual: ' + show(p) + '\n  From calling: ' + showf(f)
  );
}

export function TryP(fn){
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
    rec(makeError(invalidPromise(p, fn), this, this.context));
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
  return function TryP$cancel(){ open = false };
};

TryP.prototype.toString = function TryP$toString(){
  return 'tryP(' + show(this._fn) + ')';
};

export function tryP(f){
  if(!isFunction(f)) throwInvalidArgument('tryP', 0, 'be a Function', f);
  return new TryP(f);
}
