import {Future} from './future';
import {noop, show, showf, partial1, partial2} from './internal/utils';
import {isFunction} from './internal/predicates';
import {throwInvalidArgument} from './internal/throw';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

export function Encase2(fn, a, b){
  this._fn = fn;
  this._a = a;
  this._b = b;
  this.context = captureContext(nil, 'a Future created with encase2', Encase2);
}

Encase2.prototype = Object.create(Future.prototype);

Encase2.prototype._interpret = function Encase2$interpret(rec, rej, res){
  var r;
  try{ r = this._fn(this._a, this._b) }catch(e){ rej(e); return noop }
  res(r);
  return noop;
};

Encase2.prototype.toString = function Encase2$toString(){
  return 'encase2(' + showf(this._fn) + ', ' + show(this._a) + ', ' + show(this._b) + ')';
};

export function encase2(f, x, y){
  if(!isFunction(f)) throwInvalidArgument('encase2', 0, 'be a Function', f);

  switch(arguments.length){
    case 1: return partial1(encase2, f);
    case 2: return partial2(encase2, f, x);
    default: return new Encase2(f, x, y);
  }
}
