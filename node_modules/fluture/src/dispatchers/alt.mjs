import {isAlt} from '../internal/predicates';
import {FL} from '../internal/const';
import {partial1} from '../internal/utils';
import {throwInvalidArgument} from '../internal/throw';

function alt$left(left, right){
  if(!isAlt(right)) throwInvalidArgument('alt', 1, 'be an Alt', right);
  return left[FL.alt](right);
}

export function alt(left, right){
  if(!isAlt(left)) throwInvalidArgument('alt', 0, 'be an Alt', left);
  if(arguments.length === 1) return partial1(alt$left, left);
  return alt$left(left, right);
}
