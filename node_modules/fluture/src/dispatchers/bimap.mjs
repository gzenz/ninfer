import {isBifunctor} from '../internal/predicates';
import {FL} from '../internal/const';
import {partial1, partial2} from '../internal/utils';
import {isFunction} from '../internal/predicates';
import {throwInvalidArgument} from '../internal/throw';

function bimap$lmapper$rmapper(lmapper, rmapper, m){
  if(!isBifunctor(m)) throwInvalidArgument('bimap', 2, 'be a Bifunctor', m);
  return m[FL.bimap](lmapper, rmapper);
}

function bimap$lmapper(lmapper, rmapper, m){
  if(!isFunction(rmapper)) throwInvalidArgument('bimap', 1, 'be a Function', rmapper);
  if(arguments.length === 2) return partial2(bimap$lmapper$rmapper, lmapper, rmapper);
  return bimap$lmapper$rmapper(lmapper, rmapper, m);
}

export function bimap(lmapper, rmapper, m){
  if(!isFunction(lmapper)) throwInvalidArgument('bimap', 0, 'be a Function', lmapper);
  if(arguments.length === 1) return partial1(bimap$lmapper, lmapper);
  if(arguments.length === 2) return bimap$lmapper(lmapper, rmapper);
  return bimap$lmapper(lmapper, rmapper, m);
}
