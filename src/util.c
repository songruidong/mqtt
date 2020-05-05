/* BSD 2-Clause License
 *
 * Copyright (c) 2019, Andrea Giacomo Baldan All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <time.h>
#include <crypt.h>
#include <ctype.h>
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <stdatomic.h>
#include "util.h"
#include "mqtt.h"
#include "config.h"

static atomic_size_t memory = ATOMIC_VAR_INIT(0);

static FILE *fh = NULL;

void sol_log_init(const char *file) {
    if (!file) return;
    fh = fopen(file, "a+");
    if (!fh)
        printf("%lu * WARNING: Unable to open file %s\n",
               (unsigned long) time(NULL), file);
}

void sol_log_close(void) {
    if (fh) {
        fflush(fh);
        fclose(fh);
    }
}

void sol_log(int level, const char *fmt, ...) {

    if (level < conf->loglevel)
        return;

    assert(fmt);

    va_list ap;
    char msg[MAX_LOG_SIZE + 4];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Truncate message too long and copy 3 bytes to make space for 3 dots */
    memcpy(msg + MAX_LOG_SIZE, "...", 3);
    msg[MAX_LOG_SIZE + 3] = '\0';

    // Open two handler, one for standard output and a second for the
    // persistent log file
    FILE *fp = stdout;

    if (!fp)
        return;

    fprintf(fp, "%lu %s\n", (unsigned long) time(NULL), msg);
    if (fh)
        fprintf(fh, "%lu %s\n", (unsigned long) time(NULL), msg);
}

/* Auxiliary function to check wether a string is an integer */
bool is_integer(const char *string) {
    for (; *string; ++string)
        if (!isdigit(*string))
            return false;
    return true;
}

/* Parse the integer part of a string, by effectively iterate through it and
   converting the numbers found */
int parse_int(const char *string) {
    int n = 0;

    while (*string && isdigit(*string)) {
        n = (n * 10) + (*string - '0');
        string++;
    }
    return n;
}

char *remove_occur(char *str, char c) {
    char *p = str;
    char *pp = str;

    while (*p) {
        *pp = *p++;
        pp += (*pp != c);
    }

    *pp = '\0';

    return str;
}

char *update_integer_string(char *str, int num) {

    int n = parse_int(str);
    n += num;
    /*
     * Check for realloc if the new value is "larger" then
     * previous
     */
    char tmp[number_len(n) + 1];  // max size in bytes
    sprintf(tmp, "%d", n);  // XXX Unsafe
    size_t len = strlen(tmp);
    str = xrealloc(str, len + 1);
    memcpy(str, tmp, len + 1);

    return str;
}

/*
 * Append a string to another for the destination part that will be appended
 * the function require the length, the resulting string will be heap alloced
 * and nul-terminated.
 */
char *append_string(const char *src, char *dst, size_t chunklen) {
    size_t srclen = strlen(src);
    char *ret = xmalloc(srclen + chunklen + 1);
    memcpy(ret, src, srclen);
    memcpy(ret + srclen, dst, chunklen);
    ret[srclen + chunklen] = '\0';
    return ret;
}

/*
 * Return the 'length' of a positive number, as the number of chars it would
 * take in a string
 */
int number_len(size_t number) {
    int len = 1;
    while (number) {
        len++;
        number /= 10;
    }
    return len;
}

unsigned long unix_time_ns(void) {
	struct timeval time;
	gettimeofday(&time, NULL);
	return time.tv_sec * (int) 1e6 + time.tv_usec;
}

void generate_random_id(char *dest) {
    unsigned long utime_ns = unix_time_ns();
    snprintf(dest, MQTT_CLIENT_ID_LEN - 1, "%s-%lu", SOL_PREFIX, utime_ns);
}

bool check_passwd(const char *passwd, const char *salt) {
    return STREQ(crypt(passwd, salt), salt, strlen(salt));
}

/*
 * Custom malloc function, allocate a defined size of bytes plus 8, the size
 * of an unsigned long long, and append the length choosen at the beginning of
 * the memory chunk as an unsigned long long, returning the memory chunk
 * allocated just 8 bytes after the start; this way it is possible to track
 * the memory usage at every allocation
 */
void *xmalloc(size_t size) {

    assert(size > 0);

    void *ptr = malloc(size + sizeof(size_t));

    if (!ptr)
        return NULL;

    memory += size + sizeof(size_t);

    *((size_t *) ptr) = size;

    return (char *) ptr + sizeof(size_t);
}

/*
 * Same as xmalloc, but with calloc, creating chunk o zero'ed memory.
 * TODO: still a suboptimal solution
 */
void *xcalloc(size_t len, size_t size) {

    assert(len > 0 && size > 0);

    void *ptr = xmalloc(len * size);
    if (!ptr)
        return NULL;

    memset(ptr, 0x00, len * size);

    return ptr;
}

/*
 * Same of xmalloc but with realloc, resize a chunk of memory pointed by a
 * given pointer, again appends the new size in front of the byte array
 */
void *xrealloc(void *ptr, size_t size) {

    assert(size > 0);

    if (!ptr)
        return xmalloc(size);

    void *realptr = (char *)ptr-sizeof(size_t);

    size_t curr_size = *((size_t *) realptr);

    if (size == curr_size)
        return ptr;

    void *newptr = realloc(realptr, size + sizeof(size_t));

    if (!newptr)
        return NULL;

    *((size_t *) newptr) = size;

    memory += (-curr_size) + size + sizeof(size_t);

    return (char *) newptr + sizeof(size_t);

}

/*
 * Custom free function, must be used on memory chunks allocated with t*
 * functions, it move the pointer 8 position backward by the starting address
 * of memory pointed by `ptr`, this way it knows how many bytes will be
 * free'ed by the call
 */
void xfree(void *ptr) {

    if (!ptr)
        return;

    void *realptr = (char *) ptr - sizeof(size_t);

    if (!realptr)
        return;

    size_t ptr_size = *((size_t *) realptr);

    memory -= ptr_size + sizeof(size_t);

    free(realptr);
}

/*
 * Retrieve the bytes allocated by t* functions by backwarding the pointer of
 * 8 positions, the size of an unsigned long long in order to read the number
 * of allcated bytes
 */
size_t xmalloc_size(void *ptr) {

    if (!ptr)
        return 0L;

    void *realptr = (char *) ptr - sizeof(size_t);

    if (!realptr)
        return 0L;

    size_t ptr_size = *((size_t *) realptr);

    return ptr_size;
}

/*
 * As strdup but using xmalloc instead of malloc, to track the number of bytes
 * allocated and to enable use of xfree on duplicated strings without having
 * to care when to use a normal free or a xfree
 */
char *xstrdup(const char *s) {

    size_t len = strlen(s);
    char *ds = xmalloc(len + 1);

    if (!ds)
        return NULL;

    snprintf(ds, len + 1, "%s", s);

    return ds;
}

size_t memory_used(void) {
    return memory;
}

long get_fh_soft_limit(void) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit)) {
        perror("Failed to get limit");
        return -1;
    }
    return limit.rlim_cur;
}
