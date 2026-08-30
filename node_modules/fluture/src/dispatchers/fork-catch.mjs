import {isFuture} from '../future';
import {partial1, partial2, partial3} from '../internal/utils';
import {isFunction} from '../internal/predicates';
import {throwInvalidArgument, throwInvalidFuture} from '../internal/throw';

export function forkCatch(f, g, h, m){
  if(!isFunction(f)) throwInvalidArgument('forkCatch', 0, 'be a Function', f);
  if(arguments.length === 1) return partial1(forkCatch, f);
  if(!isFunction(g)) throwInvalidArgument('forkCatch', 1, 'be a Function', g);
  if(arguments.length === 2) return partial2(forkCatch, f, g);
  if(!isFunction(h)) throwInvalidArgument('forkCatch', 2, 'be a Function', h);
  if(arguments.length === 3) return partial3(forkCatch, f, g, h);
  if(!isFuture(m)) throwInvalidFuture('forkCatch', 3, m);
  return m._interpret(f, g, h);
}
