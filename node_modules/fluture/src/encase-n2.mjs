import {Future} from './future';
import {show, showf, partial1, partial2, noop} from './internal/utils';
import {isFunction} from './internal/predicates';
import {throwInvalidArgument} from './internal/throw';
import {makeError} from './internal/error';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

export function EncaseN2(fn, a, b){
  this._fn = fn;
  this._a = a;
  this._b = b;
  this.context = captureContext(nil, 'a Future created with encaseN2', EncaseN2);
}

EncaseN2.prototype = Object.create(Future.prototype);

EncaseN2.prototype._interpret = function EncaseN2$interpret(rec, rej, res){
  var open = false, cont = function(){ open = true };
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
  return function EncaseN2$cancel(){ open = false };
};

EncaseN2.prototype.toString = function EncaseN2$toString(){
  return 'encaseN2(' + showf(this._fn) + ', ' + show(this._a) + ', ' + show(this._b) + ')';
};

export function encaseN2(f, x, y){
  if(!isFunction(f)) throwInvalidArgument('encaseN2', 0, 'be a Function', f);

  switch(arguments.length){
    case 1: return partial1(encaseN2, f);
    case 2: return partial2(encaseN2, f, x);
    default: return new EncaseN2(f, x, y);
  }
}
