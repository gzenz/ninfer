import {isApply} from '../internal/predicates';
import {FL} from '../internal/const';
import {partial1} from '../internal/utils';
import {throwInvalidArgument} from '../internal/throw';

function ap$mval(mval, mfunc){
  if(!isApply(mfunc)) throwInvalidArgument('ap', 1, 'be an Apply', mfunc);
  return mfunc[FL.ap](mval);
}

export function ap(mval, mfunc){
  if(!isApply(mval)) throwInvalidArgument('ap', 0, 'be an Apply', mval);
  if(arguments.length === 1) return partial1(ap$mval, mval);
  return ap$mval(mval, mfunc);
}
