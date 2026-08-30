import {invalidArgument, invalidContext, invalidFuture} from './error';

export function throwInvalidArgument(it, at, expected, actual){
  throw invalidArgument(it, at, expected, actual);
}

export function throwInvalidContext(it, actual){
  throw invalidContext(it, actual);
}

export function throwInvalidFuture(it, at, m, s){
  throw invalidFuture(it, at, m, s);
}
