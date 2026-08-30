import {isFuture} from '../future';
import {partial1} from '../internal/utils';
import {throwInvalidFuture} from '../internal/throw';

function both$left(left, right){
  if(!isFuture(right)) throwInvalidFuture('both', 1, right);
  return left.both(right);
}

export function both(left, right){
  if(!isFuture(left)) throwInvalidFuture('both', 0, left);
  if(arguments.length === 1) return partial1(both$left, left);
  return both$left(left, right);
}
