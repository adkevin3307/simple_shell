%{
    #include <string.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <pwd.h>

    #include "constant.h"

    char* remove_quote(char *s)
    {
        for (int i = 1; i < strlen(s); i++) {
            if (i == strlen(s) - 1) s[i] = '\0';
            s[i - 1] = s[i];
        }

        return s;
    }

    char* get_home_path()
    {
        struct passwd *pw = getpwuid(getuid());

        return pw->pw_dir;
    }
%}

%%
"|" {
    return PIPE;
}
"<" {
    return REDIRECT_IN;
}
"<<" {
    return REDIRECT_IN_END;
}
">" {
    return REDIRECT_OUT;
}
">>" {
    return REDIRECT_OUT_APPEND;
}
^[a-zA-Z0-9]+[-+a-zA-Z0-9]* {
    return COMMAND;
}
[-+]*[\\/a-zA-Z0-9.,*_]+ {
    return ARGUMENT;
}
"~" {
    yytext = get_home_path();
    return ARGUMENT;
}
["].+["] {
    yytext = remove_quote(yytext);
    return ARGUMENT;
}
['].+['] {
    yytext = remove_quote(yytext);
    return ARGUMENT;
}
[ \t\n]+ {
    
}
%%

int yywrap()
{
    return 1;
}
