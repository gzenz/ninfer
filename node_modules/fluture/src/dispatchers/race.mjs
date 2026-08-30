import {isFuture} from '../future';
import {partial1} from '../internal/utils';
import {throwInvalidFuture} from '../internal/throw';

function race$right(right, left){
  if(!isFuture(left)) throwInvalidFuture('race', 1, left);
  return left.race(right);
}

export function race(right, left){
  if(!isFuture(right)) throwInvalidFuture('race', 0, right);
  if(arguments.length === 1) return partial1(race$right, right);
  return race$right(right, left);
}
