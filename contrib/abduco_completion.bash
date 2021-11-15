
function _abduco() {
        case $2 in
        -*) # Option
                COMPREPLY=($(compgen -W ' \
                        -n -nf \
                        -c -cf -cr \
                        -f -fn -fc -fcr \
                        -A -Ar \
                        -a -ar \
                        -l -lr \
                        -r -rc -rcf -rA -ra -rl \
                        -e -e^ \
                        -v \
                        ' -- $2))
        ;;
        *) # Session
                local sessions=$(abduco | tail -n+2 | cut -f 3)
                COMPREPLY=($(compgen -W $sessions -- $2))
                [ -n "$2" ] && compopt -o plusdirs
        ;;
        esac
}

command -F _abduco abduco

