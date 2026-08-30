import {Future, Resolved, isFuture} from './future';
import {throwInvalidFuture, throwInvalidArgument} from './internal/throw';
import {nil} from './internal/list';
import {noop, show, partial1} from './internal/utils';
import {isUnsigned, isArray} from './internal/predicates';
import {makeError} from './internal/error';
import {captureContext} from './internal/debug';

export function Parallel(max, futures){
  this._futures = futures;
  this._length = futures.length;
  this._max = Math.min(this._length, max);
  this.context = captureContext(nil, 'a Future created with parallel', Parallel);
}

Parallel.prototype = Object.create(Future.prototype);

Parallel.prototype._interpret = function Parallel$interpret(rec, rej, res){

  var _futures = this._futures, _length = this._length, _max = this._max;
  var cancels = new Array(_length), out = new Array(_length);
  var cursor = 0, running = 0, blocked = false, cont = noop;
  var context = captureContext(this.context, 'consuming a parallel Future', Parallel$interpret);

  function Parallel$cancel(){
    rec = noop;
    rej = noop;
    res = noop;
    cursor = _length;
    for(var n = 0; n < _length; n++) cancels[n] && cancels[n]();
  }

  function Parallel$run(idx){
    running++;
    cancels[idx] = _futures[idx]._interpret(function Parallel$rec(e){
      cont = rec;
      cancels[idx] = noop;
      Parallel$cancel();
      cont(makeError(e, _futures[idx], context));
    }, function Parallel$rej(reason){
      cont = rej;
      cancels[idx] = noop;
      Parallel$cancel();
      cont(reason);
    }, function Parallel$res(value){
      cancels[idx] = noop;
      out[idx] = value;
      running--;
      if(cursor === _length && running === 0) res(out);
      else if(blocked) Parallel$drain();
    });
  }

  function Parallel$drain(){
    blocked = false;
    while(cursor < _length && running < _max) Parallel$run(cursor++);
    blocked = true;
  }

  Parallel$drain();

  return Parallel$cancel;

};

Parallel.prototype.toString = function Parallel$toString(){
  return 'parallel(' + this._max + ', ' + show(this._futures) + ')';
};

var emptyArray = new Resolved([]);

function validateNthFuture(m, xs){
  if(!isFuture(m)) throwInvalidFuture(
    'parallel',
    'its second argument to be an Array of valid Futures',
    xs
  );
}

function parallel$max(max, xs){
  if(!isArray(xs)) throwInvalidArgument('parallel', 1, 'be an Array of valid Futures', xs);
  for(var idx = 0; idx < xs.length; idx++) validateNthFuture(xs[idx], xs);
  return xs.length === 0 ? emptyArray : new Parallel(max, xs);
}

export function parallel(max, xs){
  if(!isUnsigned(max)) throwInvalidArgument('parallel', 0, 'be a positive Integer', max);
  if(arguments.length === 1) return partial1(parallel$max, max);
  return parallel$max(max, xs);
}
