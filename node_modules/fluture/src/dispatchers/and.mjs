import {isFuture} from '../future';
import {partial1} from '../internal/utils';
import {throwInvalidFuture} from '../internal/throw';

function and$left(left, right){
  if(!isFuture(right)) throwInvalidFuture('and', 1, right);
  return left.and(right);
}

export function and(left, right){
  if(!isFuture(left)) throwInvalidFuture('and', 0, left);
  if(arguments.length === 1) return partial1(and$left, left);
  return and$left(left, right);
}
