/* node9: colors used as string ANSI codes by assertion_error; no-op (no TTY colors). */
module.exports = {
  blue:'', green:'', white:'', red:'', gray:'', clear:'', reset:'', hasColors:false,
  shouldColorize:function(){ return false; },
  refresh:function(){ this.hasColors=false; return this; },
};
