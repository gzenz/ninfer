import {Future, never} from './future';
import {show, partial1} from './internal/utils';
import {isUnsigned} from './internal/predicates';
import {throwInvalidArgument} from './internal/throw';
import {captureContext} from './internal/debug';
import {nil} from './internal/list';

export function After(time, value){
  this._time = time;
  this._value = value;
  this.context = captureContext(nil, 'a Future created with after', After);
}

After.prototype = Object.create(Future.prototype);

After.prototype._interpret = function After$interpret(rec, rej, res){
  var id = setTimeout(res, this._time, this._value);
  return function After$cancel(){ clearTimeout(id) };
};

After.prototype.extractRight = function After$extractRight(){
  return [this._value];
};

After.prototype.toString = function After$toString(){
  return 'after(' + show(this._time) + ', ' + show(this._value) + ')';
};

export function RejectAfter(time, value){
  this._time = time;
  this._value = value;
  this.context = captureContext(nil, 'a Future created with rejectAfter', After);
}

RejectAfter.prototype = Object.create(Future.prototype);

RejectAfter.prototype._interpret = function RejectAfter$interpret(rec, rej){
  var id = setTimeout(rej, this._time, this._value);
  return function RejectAfter$cancel(){ clearTimeout(id) };
};

RejectAfter.prototype.extractLeft = function RejectAfter$extractLeft(){
  return [this._value];
};

RejectAfter.prototype.toString = function RejectAfter$toString(){
  return 'rejectAfter(' + show(this._time) + ', ' + show(this._value) + ')';
};

function after$time(time, value){
  return time === Infinity ? never : new After(time, value);
}

export function after(time, value){
  if(!isUnsigned(time)) throwInvalidArgument('after', 0, 'be a positive Integer', time);
  if(arguments.length === 1) return partial1(after$time, time);
  return after$time(time, value);
}

function rejectAfter$time(time, reason){
  return time === Infinity ? never : new RejectAfter(time, reason);
}

export function rejectAfter(time, reason){
  if(!isUnsigned(time)){
    throwInvalidArgument('rejectAfter', 0, 'be a positive Integer', time);
  }
  if(arguments.length === 1) return partial1(rejectAfter$time, time);
  return rejectAfter$time(time, reason);
}
