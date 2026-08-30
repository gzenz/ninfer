import {resolve} from './future';
import {Next, Done} from './internal/iteration';

export function chainRec(step, init){
  return resolve(Next(init))._chain(function chainRec$recur(o){
    return o.done ? resolve(o.value) : step(Next, Done, o.value)._chain(chainRec$recur);
  });
}
