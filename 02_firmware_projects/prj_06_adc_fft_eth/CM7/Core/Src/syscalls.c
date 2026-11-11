/**
  ******************************************************************************
  * @file    syscalls.c
  * @brief   Minimal newlib retargeting for CPU1.
  ******************************************************************************
  */

#include "uart_hal.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern char end;
extern char _estack;
extern char _Min_Stack_Size;

int _write(int file, char *ptr, int len)
{
    (void)file;

    if ((ptr == NULL) || (len <= 0))
    {
        return 0;
    }

    UART_Console_Write((const uint8_t *)ptr, (uint16_t)len);
    return len;
}

int _read(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;
    return 0;
}

caddr_t _sbrk(int incr)
{
    static char *heap_end;
    char        *prev_heap_end;
    const char  *heap_limit = &_estack - (unsigned int)&_Min_Stack_Size;

    if (heap_end == NULL)
    {
        heap_end = &end;
    }

    if ((heap_end + incr) > heap_limit)
    {
        errno = ENOMEM;
        return (caddr_t)-1;
    }

    prev_heap_end = heap_end;
    heap_end     += incr;

    return (caddr_t)prev_heap_end;
}

int _close(int file)
{
    (void)file;
    return -1;
}

int _fstat(int file, struct stat *st)
{
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

int _lseek(int file, int ptr, int dir)
{
    (void)file;
    (void)ptr;
    (void)dir;
    return 0;
}

int _getpid(void)
{
    return 1;
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

void _exit(int status)
{
    (void)status;
    for (;;)
    {
    }
}
