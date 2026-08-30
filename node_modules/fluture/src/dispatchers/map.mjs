import {isFunctor} from '../internal/predicates';
import {FL} from '../internal/const';
import {partial1} from '../internal/utils';
import {isFunction} from '../internal/predicates';
import {throwInvalidArgument} from '../internal/throw';

function map$mapper(mapper, m){
  if(!isFunctor(m)) throwInvalidArgument('map', 1, 'be a Functor', m);
  return m[FL.map](mapper);
}

export function map(mapper, m){
  if(!isFunction(mapper)) throwInvalidArgument('map', 0, 'be a Function', mapper);
  if(arguments.length === 1) return partial1(map$mapper, mapper);
  return map$mapper(mapper, m);
}
