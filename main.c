#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#include "constant.h"

typedef struct yy_buffer_state* YY_BUFFER_STATE;
extern int yylex();
extern YY_BUFFER_STATE yy_scan_string(const char *base);
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern char* yyget_text();

static pid_t internal_child_pid = 0;

static inline void set_child_pid(pid_t pid)
{
    __atomic_store_n(&internal_child_pid, pid, __ATOMIC_SEQ_CST);
}

static inline pid_t get_child_pid()
{
    return __atomic_load_n(&internal_child_pid, __ATOMIC_SEQ_CST);
}

int last_signo = 0;
int process_amount = 1;
int command_concat_type[MAX_PROCESS] = { 0 };

char* readline()
{
    size_t index = 0;
    char *buffer = NULL;

    if (getline(&buffer, &index, stdin) == -1) {
        if (feof(stdin)) {
            printf("exit\n");
            exit(EXIT_SUCCESS);
        }

        fprintf(stderr, "Getline error\n");

    }

    if (strcmp(buffer, "exit\n") == 0) {
        exit(EXIT_SUCCESS);
    }

    return buffer;
}

char*** split(char *buffer)
{
    int buffer_size = sizeof(buffer) / sizeof(buffer[0]);
    int index[MAX_PROCESS] = { 0 };
    char *token;
    char ***tokens = (char***)malloc(MAX_PROCESS * sizeof(char**));

    // initialize
    process_amount = 1;

    for (int i = 0; i < MAX_PROCESS; i++) {
        command_concat_type[i] = 0;
    }

    tokens[0] = (char**)malloc(buffer_size * sizeof(char*));
    for (int i = 0; i < buffer_size; i++) {
        tokens[0][i] = NULL;
    }

    // set scan string
    YY_BUFFER_STATE buffer_state = yy_scan_string(buffer);
    
    int token_type;

    while (token_type = yylex()) {
        token = strdup(yyget_text());

        if (
            token_type == PIPE ||
            token_type == REDIRECT_IN ||
            token_type == REDIRECT_OUT ||
            token_type == REDIRECT_OUT_APPEND
        ) {
            process_amount += 1;

            command_concat_type[process_amount - 1] = token_type;

            tokens[process_amount - 1] = (char**)malloc(buffer_size * sizeof(char*));
            for (int i = 0; i < buffer_size; i++) {
                tokens[process_amount - 1][i] = NULL;
            }

            continue;
        }

        tokens[process_amount - 1][index[process_amount - 1]++] = token;
    }

    yy_delete_buffer(buffer_state);

    return tokens;
}

void process(int in, int out, char **command)
{
    pid_t pid, wpid;

    pid = fork();

    if (pid == -1) {
        fprintf(stderr, "Failed to create child\n");
    }

    if (pid == 0) {
        if (in != STDIN_FILENO) {
            dup2(in, STDIN_FILENO);
            close(in);
        }
        if (out != STDOUT_FILENO) {
            dup2(out, STDOUT_FILENO);
            close(out);
        }

        if (execvp(command[0], command) == -1) {
            fprintf(stderr, "Failed to execute command\n");
        }
    }
    else {
        set_child_pid(pid);
    
        int status;
        while (1) {
            wpid = waitpid(pid, &status, WNOHANG);

            if (wpid == pid) break;
        }
    }
}

void execute(char ***commands)
{
    pid_t pid, wpid;

    if (last_signo != 0) return;

    if (strcmp(commands[0][0], "cd") == 0) {
        chdir(commands[0][1]);

        return;
    }

    pid = fork();

    if (pid == -1) {
        fprintf(stderr, "Failed to create child\n");
    }

    if (pid == 0) {
        int i, in, fd[2];

        in = STDIN_FILENO;

        // change in
        for (int j = 1; j < process_amount; j++) {
            if (command_concat_type[j] == REDIRECT_IN) {
                in = open(commands[j][0], O_RDONLY);

                for (int k = j; k < process_amount; k++) {
                    if (k == process_amount - 1) {
                        commands[k] = NULL;
                        command_concat_type[k] = 0;
                    }
                    else {
                        commands[k] = commands[k + 1];
                        command_concat_type[k] = command_concat_type[k + 1];
                    }
                }

                process_amount -= 1;
            }
        }

        for (i = 0; i < process_amount - 1; i++) {
            // 0 -> read end, 1 -> write end
            if (pipe(fd) < 0) {
                fprintf(stderr, "Pipe cannot be initialized\n");

                exit(EXIT_FAILURE);
            }

            if (command_concat_type[i + 1] == REDIRECT_OUT) {
                fd[1] = open(commands[i + 1][0], O_TRUNC | O_CREAT | O_WRONLY, 0644);
            }

            if (command_concat_type[i + 1] == REDIRECT_OUT_APPEND) {
                fd[1] = open(commands[i + 1][0], O_WRONLY | O_APPEND, 0644);
            }

            process(in, fd[1], commands[i]);

            i += (command_concat_type[i + 1] == REDIRECT_OUT || command_concat_type[i + 1] == REDIRECT_OUT_APPEND);

            close(fd[1]);
            in = fd[0];
        }

        if (
            command_concat_type[i + 1] != REDIRECT_IN &&
            command_concat_type[i + 1] != REDIRECT_OUT &&
            command_concat_type[i + 1] != REDIRECT_OUT_APPEND
        ) {
            if (in != STDIN_FILENO) {
                dup2(in, STDIN_FILENO);
            }

            if (execvp(commands[i][0], commands[i]) == -1) {
                fprintf(stderr, "Failed to execute command\n");
            }
        }
    }
    else {
        set_child_pid(pid);

        int status;
        while (1) {
            wpid = waitpid(pid, &status, WNOHANG);

            if (wpid == pid) break;
        }
    }
}

void signal_handler(int signo, siginfo_t *info, void *context)
{
    pid_t target = get_child_pid();

    if (target != 0 && info->si_pid != target) {
        last_signo = signo;
        kill(target, signo);
    }
}

int forward_signal(int signo)
{
    struct sigaction act;

    memset(&act, 0, sizeof act);
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = signal_handler;
    act.sa_flags = SA_SIGINFO | SA_RESTART;

    if (sigaction(signo, &act, NULL)) return errno;

    return 0;
}

int main(int argc, char **argv)
{
    // signal forward
    if (
        forward_signal(SIGINT) ||
        forward_signal(SIGHUP) ||
        forward_signal(SIGTERM) ||
        forward_signal(SIGQUIT) ||
        forward_signal(SIGCHLD) ||
        forward_signal(SIGUSR1) ||
        forward_signal(SIGUSR2)
    ) {
        fprintf(stderr, "Cannot install signal handlers: %s\n", strerror(errno));

        return EXIT_FAILURE;
    }

    while (1) {
        char *buffer;
        char ***commands;

        printf("$ ");

        buffer = readline();
        commands = split(buffer);
        execute(commands);

        free(buffer);
        free(commands);
        last_signo = 0;
    }

    return 0;
}
