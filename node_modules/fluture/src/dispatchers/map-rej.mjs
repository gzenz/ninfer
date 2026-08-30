import {isFuture} from '../future';
import {partial1} from '../internal/utils';
import {isFunction} from '../internal/predicates';
import {throwInvalidArgument, throwInvalidFuture} from '../internal/throw';

function mapRej$mapper(mapper, m){
  if(!isFuture(m)) throwInvalidFuture('mapRej', 1, m);
  return m.mapRej(mapper);
}

export function mapRej(mapper, m){
  if(!isFunction(mapper)) throwInvalidArgument('mapRej', 0, 'be a Function', mapper);
  if(arguments.length === 1) return partial1(mapRej$mapper, mapper);
  return mapRej$mapper(mapper, m);
}
