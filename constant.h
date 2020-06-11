#pragma once

#define MAX_PROCESS 64

enum TOKENTABLE {
    PIPE = 1,
    REDIRECT_IN = 2,
    REDIRECT_OUT = 3,
    COMMAND = 4,
    ARGUMENT = 5,
    REDIRECT_IN_END = 6,
    REDIRECT_OUT_APPEND = 7
};