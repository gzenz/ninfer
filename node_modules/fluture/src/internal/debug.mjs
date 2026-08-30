import {noop} from './utils';
import {cons, cat, nil} from './list';

/* istanbul ignore next: non v8 compatibility */
var captureStackTrace = Error.captureStackTrace || captureStackTraceFallback;
var _debug = noop;

export {captureStackTrace};

export function debugMode(debug){
  _debug = debug ? debugHandleAll : noop;
}

export function debugHandleAll(fn){
  return fn();
}

export function debug(fn){
  return _debug(fn);
}

export function captureContext(previous, tag, fn){
  return debug(function debugCaptureContext(){
    var context = {
      tag: tag,
      name: ' from ' + tag + ':',
    };
    captureStackTrace(context, fn);
    return cat(previous, cons(context, nil));
  }) || previous;
}

export function captureStackTraceFallback(x){
  var e = new Error;
  /* istanbul ignore else: non v8 compatibility */
  if(typeof e.stack === 'string'){
    x.stack = x.name + '\n' + e.stack.split('\n').slice(1).join('\n');
  }else{
    x.stack = x.name;
  }
}
