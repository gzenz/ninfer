import {Future} from './future';
import {noop, showf} from './internal/utils';
import {isFunction} from './internal/predicates';
import {throwInvalidArgument} from './internal/throw';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

export function Attempt(fn){
  this._fn = fn;
  this.context = captureContext(nil, 'a Future created with attempt/try', Attempt);
}

Attempt.prototype = Object.create(Future.prototype);

Attempt.prototype._interpret = function Attempt$interpret(rec, rej, res){
  var r;
  try{ r = this._fn() }catch(e){ rej(e); return noop }
  res(r);
  return noop;
};

Attempt.prototype.toString = function Attempt$toString(){
  return 'attempt(' + showf(this._fn) + ')';
};

export function attempt(f){
  if(!isFunction(f)) throwInvalidArgument('attempt', 0, 'be a Function', f);
  return new Attempt(f);
}
