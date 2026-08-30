import {Future} from './future';
import {show, showf, partial1, noop} from './internal/utils';
import {isFunction} from './internal/predicates';
import {throwInvalidArgument} from './internal/throw';
import {makeError} from './internal/error';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

export function EncaseN(fn, a){
  this._fn = fn;
  this._a = a;
  this.context = captureContext(nil, 'a Future created with encaseN', EncaseN);
}

EncaseN.prototype = Object.create(Future.prototype);

EncaseN.prototype._interpret = function EncaseN$interpret(rec, rej, res){
  var open = false, cont = function(){ open = true };
  var context = captureContext(this.context, 'consuming an encased Future', EncaseN$interpret);
  try{
    this._fn(this._a, function EncaseN$done(err, val){
      cont = err ? function EncaseN3$rej(){
        open = false;
        rej(err);
      } : function EncaseN3$res(){
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
  return function EncaseN$cancel(){ open = false };
};

EncaseN.prototype.toString = function EncaseN$toString(){
  return 'encaseN(' + showf(this._fn) + ', ' + show(this._a) + ')';
};

export function encaseN(f, x){
  if(!isFunction(f)) throwInvalidArgument('encaseN', 0, 'be a Function', f);
  if(arguments.length === 1) return partial1(encaseN, f);
  return new EncaseN(f, x);
}
