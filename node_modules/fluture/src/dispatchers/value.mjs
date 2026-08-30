import {isFuture} from '../future';
import {partial1} from '../internal/utils';
import {isFunction} from '../internal/predicates';
import {throwInvalidArgument, throwInvalidFuture} from '../internal/throw';

function value$cont(cont, m){
  if(!isFuture(m)) throwInvalidFuture('value', 1, m);
  return m.value(cont);
}

export function value(cont, m){
  if(!isFunction(cont)) throwInvalidArgument('value', 0, 'be a Function', cont);
  if(arguments.length === 1) return partial1(value$cont, cont);
  return value$cont(cont, m);
}
