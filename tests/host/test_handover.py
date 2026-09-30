#!/usr/bin/env python3
"""Compile production parser/sink functions with controlled I/O and allocation.

Requires gcc. This tests the 16-bit output build used by this device; it does
not emulate FreeRTOS scheduling or run a complete firmware build.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    end, depth = begin + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


util = (ROOT / 'components/raop/util.c').read_text()
parser = function(util, 'bool http_parse(')
parser_prelude = r'''
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG_ERROR(...) ((void)0)
#define LOG_SDEBUG(...) ((void)0)
typedef struct { char *key, *data; } key_data_t;
static const char *lines[8];
static int line_index, remaining, fail_alloc;
static size_t allocation_requested;
static int read_line(int sock, char *out, int size, int timeout) {
    (void)sock; (void)timeout;
    const char *s = lines[line_index++];
    snprintf(out, size, "%s", s);
    return strlen(s);
}
static int recv(int sock, char *out, int size, int flags) {
    (void)sock; (void)flags;
    int n = remaining < size ? remaining : size;
    memset(out, 'x', n);
    remaining -= n;
    return n;
}
static void *body_malloc(size_t n) {
    allocation_requested = n;
    return fail_alloc ? NULL : malloc(n);
}
static char *ltrim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    return s;
}
static void kd_free(key_data_t *kd) {
    for (int i = 0; kd[i].key; ++i) { free(kd[i].key); free(kd[i].data); }
    kd[0].key = NULL;
}
#define malloc body_malloc
'''
parser_tests = r'''
#undef malloc
static void check(const char *length, int available, int oom, bool expected) {
    char header[128], method[16], *body = NULL;
    key_data_t headers[16] = {{0}};
    snprintf(header, sizeof(header), "Content-Length: %s", length);
    lines[0] = "SET_PARAMETER / RTSP/1.0";
    lines[1] = header;
    lines[2] = "";
    remaining = available;
    line_index = 0;
    fail_alloc = oom;
    allocation_requested = 0;
    int len = 0;
    assert(http_parse(0, method, headers, &body, &len) == expected);
    if (oom) {
        assert(body == NULL);
        assert(allocation_requested == 180225);
    }
    if (expected && len) {
        assert(body[len] == 0);
        for (int i = 0; i < len; ++i) assert(body[i] == 'x');
    }
    free(body);
    kd_free(headers);
}
int main(void) {
    check("180224", 0, 1, false); // Actual fault: NULL + 0x2c000
    check("180224", 180224, 0, true);
    check("12", 3, 0, false); // Truncated body must never reach RTSP handler
    check("0", 0, 0, true);
    check("-1", 0, 0, false);
    check("2147483647", 0, 0, false);
    check("999999999999999999999999", 0, 0, false);
    check("abc", 0, 0, false);
    check("12junk", 0, 0, false);
    puts("PASS: 9 RTSP body/allocation cases");
}
'''

sink = (ROOT / 'components/squeezelite/decode_external.c').read_text()
buffer = (ROOT / 'components/squeezelite/buffer.c').read_text()
helpers = ''.join(function(buffer, signature) for signature in [
    'unsigned _buf_used(', 'unsigned _buf_space(', 'unsigned _buf_cont_write(',
    'void _buf_inc_writep('])
sink_function = function(sink, 'static uint32_t sink_data_handler(')
wrappers = ''.join(function(sink, signature) for signature in [
    'static void bt_sink_data_handler(', 'static void raop_sink_data_handler(',
    'static uint32_t cspot_sink_data_handler('])
sink_prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32_t;
enum { DECODE_BT = 1, DECODE_RAOP, DECODE_CSPOT };
enum { SINK_RUNNING, SINK_ABORT, SINK_DISCARD };
static int sink_state, locked, switch_to, switch_after;
static struct { int external; } output;
static struct { u32_t playtime, len; } raop_sync;
struct buffer { uint8_t *buf, *readp, *writep, *wrap; unsigned size; };
static uint8_t storage[32];
static struct buffer fifo;
static struct buffer *outputbuf = &fifo;
#define BYTES_PER_FRAME 4
#define min(a,b) ((a)<(b)?(a):(b))
#define LOCK_O do { assert(!locked); locked = 1; } while (0)
#define UNLOCK_O do { assert(locked); locked = 0; } while (0)
#define LOG_WARN(...) ((void)0)
#define LOG_SDEBUG(...) ((void)0)
static void usleep(unsigned delay) {
    (void)delay;
    assert(!locked);
    assert(switch_to);
    if (--switch_after > 0) return;
    output.external = switch_to;
    memset(storage, 0xa5, sizeof(storage));
    fifo.readp = fifo.writep = storage;
}
static void reset(int owner) {
    output.external = owner;
    sink_state = SINK_RUNNING;
    switch_to = 0;
    switch_after = 1;
    memset(storage, 0xa5, sizeof(storage));
    fifo = (struct buffer){ storage, storage, storage, storage + 32, 32 };
}
'''
sink_tests = r'''
int main(void) {
    uint8_t pcm[64];
    memset(pcm, 0x33, sizeof(pcm));
    for (int owner = 0; owner <= 3; ++owner) {
        for (int source = 1; source <= 3; ++source) {
            reset(owner);
            if (source == DECODE_BT) bt_sink_data_handler(pcm, 8);
            if (source == DECODE_RAOP) raop_sink_data_handler(pcm, 8, 123);
            if (source == DECODE_CSPOT)
                assert(cspot_sink_data_handler(pcm, 8) == (owner == source ? 8 : 0));
            assert(!locked);
            assert(fifo.writep == storage + (owner == source ? 8 : 0));
            assert(storage[0] == (owner == source ? 0x33 : 0xa5));
        }
    }
    // Source changes while producer releases the mutex for backpressure.
    reset(DECODE_CSPOT);
    switch_to = DECODE_RAOP;
    assert(cspot_sink_data_handler(pcm, sizeof(pcm)) == 31);
    assert(fifo.writep == storage && storage[0] == 0xa5 && !locked);
    // Even on last retry, an old source must not realign a new owner's FIFO.
    reset(DECODE_BT);
    switch_to = DECODE_CSPOT;
    switch_after = 2;
    assert(sink_data_handler(pcm, sizeof(pcm), DECODE_BT, 1, 1, true) == 31);
    assert(fifo.writep == storage && storage[0] == 0xa5 && !locked);
    puts("PASS: 14 source-ownership cases (including handover during backpressure)");
}
'''

with tempfile.TemporaryDirectory(prefix='cspot-handover-test-') as tmp:
    for name, code in [
        ('rtsp', parser_prelude + parser + parser_tests),
        ('sink', sink_prelude + helpers + sink_function + wrappers + sink_tests),
    ]:
        src = Path(tmp) / (name + '.c')
        exe = Path(tmp) / name
        src.write_text(code)
        subprocess.run(['gcc', '-std=gnu99', '-g', '-O1', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined',
                        str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
