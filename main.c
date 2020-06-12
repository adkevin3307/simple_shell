#define _GNU_SOURCE

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

static char **g_envp;

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

char* get_envp(char* token);

void prompt_message()
{
    char *pwd;
    char buffer[1024];

    pwd = getcwd(buffer, 1024);
    printf("%s $ ", pwd);
}

char* readline()
{
    size_t index = 0;
    char *buffer = NULL;

    prompt_message();

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

    tokens[0] = (char**)malloc(buffer_size * sizeof(char*));
    for (int i = 0; i < buffer_size; i++) {
        tokens[0][i] = NULL;
    }

    // set scan string
    YY_BUFFER_STATE buffer_state = yy_scan_string(buffer);
    
    int token_type;

    while (token_type = yylex()) {
        token = strdup(yyget_text());

        if (token_type == PIPE) {
            process_amount += 1;

            tokens[process_amount - 1] = (char**)malloc(buffer_size * sizeof(char*));
            for (int i = 0; i < buffer_size; i++) {
                tokens[process_amount - 1][i] = NULL;
            }

            continue;
        }

        char *temp = get_envp(token);
        if (temp != NULL) token = temp;

        tokens[process_amount - 1][index[process_amount - 1]++] = token;
    }

    yy_delete_buffer(buffer_state);

    return tokens;
}

char** erase_command(int index, char **command)
{
    for (int i = index; command[i] != NULL; i++) {
        command[i] = command[i + 1];
    }

    return command;
}

char** redirect_handler(int *in, int *out, char **command)
{
    for (int i = 0; command[i] != NULL; i++) {
        if (strcmp(command[i], "<") == 0 || strcmp(command[i], ">") == 0 || strcmp(command[i], ">>") == 0) {

            if (strcmp(command[i], "<") == 0) *in = open(command[i + 1], O_RDONLY);
            else if (strcmp(command[i], ">") == 0) *out = open(command[i + 1], O_TRUNC | O_CREAT | O_WRONLY, 0644);
            else if (strcmp(command[i], ">>")) *out = open(command[i + 1], O_APPEND, 0644);

            command = erase_command(i, command);
            command = erase_command(i, command);

            i -= 1;
        }
    }

    return command;
}

void process(int in, int out, char **command)
{
    pid_t pid, wpid;

    pid = fork();

    if (pid == -1) {
        fprintf(stderr, "Failed to create child\n");
    }

    if (pid == 0) {
        command = redirect_handler(&in, &out, command);

        if (in != STDIN_FILENO) {
            dup2(in, STDIN_FILENO);
            close(in);
        }
        if (out != STDOUT_FILENO) {
            dup2(out, STDOUT_FILENO);
            close(out);
        }

        if (execvpe(command[0], command, g_envp) == -1) {
            fprintf(stderr, "Failed to execute command\n");
            exit(EXIT_FAILURE);
        }
    }
    else {
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

    if (last_signo != 0 || commands[0][0] == NULL) return;

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

        for (i = 0; i < process_amount - 1; i++) {
            // 0 -> read end, 1 -> write end
            if (pipe(fd) < 0) {
                fprintf(stderr, "Pipe cannot be initialized\n");

                exit(EXIT_FAILURE);
            }

            process(in, fd[1], commands[i]);

            close(fd[1]);
            in = fd[0];
        }

        int out = STDOUT_FILENO;
        commands[i] = redirect_handler(&in, &out, commands[i]);

        if (in != STDIN_FILENO) {
            dup2(in, STDIN_FILENO);
        }

        if (out != STDOUT_FILENO) {
            dup2(out, STDOUT_FILENO);
        }

        if (execvpe(commands[i][0], commands[i], g_envp) == -1) {
            fprintf(stderr, "Failed to execute command\n");
            exit(EXIT_FAILURE);
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

    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_sigaction = signal_handler;
    act.sa_flags = SA_SIGINFO | SA_RESTART;

    if (sigaction(signo, &act, NULL)) return errno;

    return 0;
}

char* get_envp(char *token)
{
    int i;
    int count = strlen(token);
    char *temp = (char*)malloc((count + 1) * sizeof(char));

    for (i = 0; i < count; i++) {
        temp[i] = token[i];
    }
    temp[i] = '=';
    temp[i + 1] = '\0';

    for (i = 0; g_envp[i]; i++) {
        int flag = 0;
        for (int j = 0; j < strlen(temp); j++) {
            flag += (g_envp[i][j] == temp[j]);
        }
        if (flag == strlen(temp)) return (g_envp[i] + strlen(temp));
    }

    return NULL;
}

void init_envp(char **envp)
{
    int count = 0;

    while (envp[count]) {
        count += 1;
    }

    g_envp = (char**)malloc((count + 1) * sizeof(char*));

    for (int i = 0; i < count + 1; i++) {
        g_envp[i] = envp[i];
    }
}

int main(int argc, char **argv, char **envp)
{
    init_envp(envp);

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
        char *buffer = readline();
        char ***commands = split(buffer);
        execute(commands);

        free(buffer);
        free(commands);
        last_signo = 0;
    }

    return 0;
}
