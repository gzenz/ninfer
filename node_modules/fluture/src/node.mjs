import {Future} from './future';
import {showf, noop} from './internal/utils';
import {isFunction} from './internal/predicates';
import {throwInvalidArgument} from './internal/throw';
import {makeError} from './internal/error';
import {nil} from './internal/list';
import {captureContext} from './internal/debug';

export function Node(fn){
  this._fn = fn;
  this.context = captureContext(nil, 'a Future created with node');
}

Node.prototype = Object.create(Future.prototype);

Node.prototype._interpret = function Node$interpret(rec, rej, res){
  var open = false, cont = function(){ open = true };
  try{
    this._fn(function Node$done(err, val){
      cont = err ? function Node$rej(){
        open = false;
        rej(err);
      } : function Node$res(){
        open = false;
        res(val);
      };
      if(open){
        cont();
      }
    });
  }catch(e){
    rec(makeError(e, this, this.context));
    open = false;
    return noop;
  }
  cont();
  return function Node$cancel(){ open = false };
};

Node.prototype.toString = function Node$toString(){
  return 'node(' + showf(this._fn) + ')';
};

export function node(f){
  if(!isFunction(f)) throwInvalidArgument('node', 0, 'be a Function', f);
  return new Node(f);
}
