import {isFuture} from '../future';
import {partial1} from '../internal/utils';
import {isFunction} from '../internal/predicates';
import {throwInvalidArgument, throwInvalidFuture} from '../internal/throw';

function chainRej$chainer(chainer, m){
  if(!isFuture(m)) throwInvalidFuture('chainRej', 1, m);
  return m.chainRej(chainer);
}

export function chainRej(chainer, m){
  if(!isFunction(chainer)) throwInvalidArgument('chainRej', 0, 'be a Function', chainer);
  if(arguments.length === 1) return partial1(chainRej$chainer, chainer);
  return chainRej$chainer(chainer, m);
}
