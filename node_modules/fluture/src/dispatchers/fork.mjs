import {isFuture} from '../future';
import {raise, partial1, partial2} from '../internal/utils';
import {isFunction} from '../internal/predicates';
import {throwInvalidArgument, throwInvalidFuture} from '../internal/throw';

function fork$f$g(f, g, m){
  if(!isFuture(m)) throwInvalidFuture('fork', 2, m);
  return m._interpret(raise, f, g);
}

function fork$f(f, g, m){
  if(!isFunction(g)) throwInvalidArgument('fork', 1, 'be a Function', g);
  if(arguments.length === 2) return partial2(fork$f$g, f, g);
  return fork$f$g(f, g, m);
}

export function fork(f, g, m){
  if(!isFunction(f)) throwInvalidArgument('fork', 0, 'be a Function', f);
  if(arguments.length === 1) return partial1(fork$f, f);
  if(arguments.length === 2) return fork$f(f, g);
  return fork$f(f, g, m);
}
