import {Future} from './future';
import {noop, show, showf, partial1} from './internal/utils';
import {isFunction} from './internal/predicates';
import {throwInvalidArgument} from './internal/throw';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

export function Encase(fn, a){
  this._fn = fn;
  this._a = a;
  this.context = captureContext(nil, 'a Future created with encase', Encase);
}

Encase.prototype = Object.create(Future.prototype);

Encase.prototype._interpret = function Encase$interpret(rec, rej, res){
  var r;
  try{ r = this._fn(this._a) }catch(e){ rej(e); return noop }
  res(r);
  return noop;
};

Encase.prototype.toString = function Encase$toString(){
  return 'encase(' + showf(this._fn) + ', ' + show(this._a) + ')';
};

export function encase(f, x){
  if(!isFunction(f)) throwInvalidArgument('encase', 0, 'be a Function', f);
  if(arguments.length === 1) return partial1(encase, f);
  return new Encase(f, x);
}
