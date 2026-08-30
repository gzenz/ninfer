import {isFuture} from '../future';
import {partial1} from '../internal/utils';
import {throwInvalidFuture} from '../internal/throw';

function lastly$right(right, left){
  if(!isFuture(left)) throwInvalidFuture('lastly', 1, left);
  return left.lastly(right);
}

export function lastly(right, left){
  if(!isFuture(right)) throwInvalidFuture('lastly', 0, right);
  if(arguments.length === 1) return partial1(lastly$right, right);
  return lastly$right(right, left);
}
