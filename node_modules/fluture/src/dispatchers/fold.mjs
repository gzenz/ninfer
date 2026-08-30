import {isFuture} from '../future';
import {partial1, partial2} from '../internal/utils';
import {isFunction} from '../internal/predicates';
import {throwInvalidArgument, throwInvalidFuture} from '../internal/throw';

function fold$f$g(f, g, m){
  if(!isFuture(m)) throwInvalidFuture('fold', 2, m);
  return m.fold(f, g);
}

function fold$f(f, g, m){
  if(!isFunction(g)) throwInvalidArgument('fold', 1, 'be a Function', g);
  if(arguments.length === 2) return partial2(fold$f$g, f, g);
  return fold$f$g(f, g, m);
}

export function fold(f, g, m){
  if(!isFunction(f)) throwInvalidArgument('fold', 0, 'be a Function', f);
  if(arguments.length === 1) return partial1(fold$f, f);
  if(arguments.length === 2) return fold$f(f, g);
  return fold$f(f, g, m);
}
