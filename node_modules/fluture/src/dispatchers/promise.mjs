import {isFuture} from '../future';
import {throwInvalidFuture} from '../internal/throw';

export function promise(m){
  if(!isFuture(m)) throwInvalidFuture('promise', 0, m);
  return m.promise();
}
