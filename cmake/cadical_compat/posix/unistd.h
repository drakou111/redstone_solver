#pragma once
// MSVC stand-in for the few POSIX calls CaDiCaL makes.
#include <io.h>
#include <process.h>
#include <direct.h>
#define isatty _isatty
#define fileno _fileno
#define getpid _getpid
#define access _access
#define unlink _unlink
#define close _close
#define dup _dup
#define dup2 _dup2
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define R_OK 4
#define W_OK 2
#define F_OK 0
