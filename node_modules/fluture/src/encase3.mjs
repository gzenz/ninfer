import {Future} from './future';
import {noop, show, showf, partial1, partial2, partial3} from './internal/utils';
import {isFunction} from './internal/predicates';
import {throwInvalidArgument} from './internal/throw';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

export function Encase3(fn, a, b, c){
  this._fn = fn;
  this._a = a;
  this._b = b;
  this._c = c;
  this.context = captureContext(nil, 'a Future created with encase3', Encase3);
}

Encase3.prototype = Object.create(Future.prototype);

Encase3.prototype._interpret = function Encase3$interpret(rec, rej, res){
  var r;
  try{ r = this._fn(this._a, this._b, this._c) }catch(e){ rej(e); return noop }
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

export function encase3(f, x, y, z){
  if(!isFunction(f)) throwInvalidArgument('encase3', 0, 'be a Function', f);

  switch(arguments.length){
    case 1: return partial1(encase3, f);
    case 2: return partial2(encase3, f, x);
    case 3: return partial3(encase3, f, x, y);
    default: return new Encase3(f, x, y, z);
  }
}
