import {isChain} from '../internal/predicates';
import {FL} from '../internal/const';
import {partial1} from '../internal/utils';
import {isFunction} from '../internal/predicates';
import {throwInvalidArgument} from '../internal/throw';

function chain$chainer(chainer, m){
  if(!isChain(m)) throwInvalidArgument('chain', 1, 'be a Chain', m);
  return m[FL.chain](chainer);
}

export function chain(chainer, m){
  if(!isFunction(chainer)) throwInvalidArgument('chain', 0, 'be a Function', chainer);
  if(arguments.length === 1) return partial1(chain$chainer, chainer);
  return chain$chainer(chainer, m);
}
