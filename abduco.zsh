#compdef abduco

typeset -A opt_args

_abduco_sessions() {
  declare -a sessions
  sessions=( $(abduco | sed '1d;s/.*\t[0-9][0-9]*\t//') )
  _describe -t session 'session' sessions
}

_abduco_firstarg() {
  if (( $+opt_args[-a] || $+opt_args[-A] )); then
    _abduco_sessions
  elif (( $+opt_args[-c] || $+opt_args[-n] )); then
    _guard "^-*" 'session name'
  elif [[ -z $words[CURRENT] ]]; then
    compadd "$@" -S '' -- -
  fi
}

_arguments -s \
  '(-a -A -c -n -f)-a[attach to an existing session]' \
  '(-a -A -c -n)-A[attach to a session, create if does not exist]' \
  '(-a -A -c -n -l)-c[create a new session and attach to it]' \
  '(-a -A -c -n -l)-n[create a new session but do not attach to it]' \
  '-e[set the detachkey (default: ^\\)]:detachkey' \
  '(-a)-f[force create the session]' \
  '(-q)-p[pass-through mode]' \
  '-q[be quiet]' \
  '-r[read-only session, ignore user input]' \
  '(-c -n)-l[attach with the lowest priority]' \
  '(-)-v[show version information and exit]' \
  '1: :_abduco_firstarg' \
  '2:command:_path_commands' \
  '*:: :{ shift $((CURRENT-3)) words; _precommand; }'
