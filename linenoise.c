/* linenoise.c -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 *
 *   http://github.com/antirez/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ------------------------------------------------------------------------
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars
 *
 * CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars
 *
 * The following is used to get the terminal width if getting
 * the width with the TIOCGWINSZ ioctl fails
 *
 * DSR (Device Status Report)
 *    Sequence: ESC [ 6 n
 *    Effect: reports the current cusor position as ESC [ n ; m R
 *            where n is the row and m is the column
 *
 * When multi line mode is enabled, we also use an additional escape
 * sequence. However multi line editing is disabled by default.
 *
 * CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * When linenoiseClearScreen() is called, two additional escape sequences
 * are used in order to clear the screen and position the cursor at home
 * position.
 *
 * CUP (Cursor position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED (Erase display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 *
 */

#include "linenoise.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//#include <termios.h>
#include <unistd.h>

#include <sys/stat.h>
//#include <sys/ioctl.h>
#include <sys/types.h>

#include "periph/mux_uart.h"
#include "util/timeout.h"

extern mux_uart_index_t stdio_uart;

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096

// static char *unsupported_term[] = {"dumb","cons25","emacs",NULL};
static linenoiseCompletionCallback *completionCallback = NULL;
static linenoiseHintsCallback *hintsCallback = NULL;
static linenoiseFreeHintsCallback *freeHintsCallback = NULL;

static bool mlmode = 0;  /* Multi line mode. Default is single line. */
static size_t history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static size_t history_len = 0;
static char **history = NULL;

/* The linenoiseState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct linenoiseState {
    /* Current mode of line editor state machine */
    enum {
        ln_init = 0,
        ln_read_regular,
        ln_read_esc,
        ln_completion,
        ln_getColumns,
        ln_getColumns_1,
        ln_getColumns_2
    } mode;

    /* State for esc sequence handling */
    char seq[3];        /* Esc sequence buffer. */
    size_t seq_idx;     /* Esc sequence read index. */

    /* State for completion handling */
    size_t completion_idx; /* Auto-completion selected entry index. */
    linenoiseCompletions lc;

    /* State for cursor pos. / column retrieval */
    char cur_pos_buf[32];
    ssize_t cur_pos_idx;
    ssize_t cur_pos_initial;
    timeout_t cur_pos_timeout;

    bool smart_term_connected;

    char *buf;          /* Edited line buffer. */
    size_t buflen;      /* Edited line buffer size. */
    const char *prompt; /* Prompt to display. */
    size_t plen;        /* Prompt length. */
    size_t pos;         /* Current cursor position. */
    size_t oldpos;      /* Previous refresh cursor position. */
    size_t len;         /* Current edited line length. */
    size_t cols;        /* Number of columns in terminal. */
    size_t maxrows;     /* Maximum num of rows used so far (multiline mode) */
    ssize_t history_index;  /* The history index we are currently editing. */
};

static struct linenoiseState l_state = {
    .mode = ln_getColumns
};

enum KEY_ACTION {
    KEY_NULL = 0,	    /* NULL */
    CTRL_A = 1,         /* Ctrl+a */
    CTRL_B = 2,         /* Ctrl-b */
    CTRL_C = 3,         /* Ctrl-c */
    CTRL_D = 4,         /* Ctrl-d */
    CTRL_E = 5,         /* Ctrl-e */
    CTRL_F = 6,         /* Ctrl-f */
    CTRL_H = 8,         /* Ctrl-h */
    TAB = 9,            /* Tab */
    CTRL_K = 11,        /* Ctrl+k */
    CTRL_L = 12,        /* Ctrl+l */
    ENTER = 13,         /* Enter */
    CTRL_N = 14,        /* Ctrl-n */
    CTRL_P = 16,        /* Ctrl-p */
    CTRL_T = 20,        /* Ctrl-t */
    CTRL_U = 21,        /* Ctrl+u */
    CTRL_W = 23,        /* Ctrl+w */
    ESC = 27,           /* Escape */
    BACKSPACE =  127    /* Backspace */
};

int linenoiseHistoryAdd(const char *line);
static void refreshLine(struct linenoiseState *l);

/* Debugging macro. */
#if 0
FILE *lndebug_fp = NULL;
#define lndebug(...) \
    do { \
        if (lndebug_fp == NULL) { \
            lndebug_fp = fopen("/tmp/lndebug.txt","a"); \
            fprintf(lndebug_fp, \
            "[%zu %zu %zu] p: %zu, rows: %zu, rpos: %zu, max: %zu, oldmax: %zu\n", \
            l->len,l->pos,l->oldpos,plen,rows,rpos,l->maxrows,old_rows); \
        } \
        fprintf(lndebug_fp, ", " __VA_ARGS__); \
        fflush(lndebug_fp); \
    } while (0)
#else
#define lndebug(...)
#endif

/* ======================= Low level terminal handling ====================== */

static int console_getch()
{
    if (!uart_has_input(stdio_uart)) {
        return -1;
    }
    return uart_getch(stdio_uart);
}

static inline void console_write(const char *buf, size_t n)
{
    uart_write_bytes(stdio_uart, buf, n);
}

static inline void console_write_string(const char *str)
{
    uart_write_string(stdio_uart, str);
}

/* Set if to use or not the multi line mode. */
void linenoiseSetMultiLine(bool ml)
{
    mlmode = ml;
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it.
 * Return values:
 *  -2: error
 *  -1: unfinished / call again.
 * >=0: cursor position.
 */
static ssize_t getCursorPosition(struct linenoiseState *ls)
{
    if (ls->cur_pos_idx < 0) {
        ls->cur_pos_idx = 0;
        /* Query cursor location */
        console_write_string("\x1b[6n");
        timeout_set(&ls->cur_pos_timeout, 100);
        return -1;
    }
    
    // Read one character
    int c = console_getch();
    if (c < 0) {
        // Return error, if timeout elapsed.
        return timeout_elapsed(&ls->cur_pos_timeout) ? -2 : -1;
    }

    // Store until 'R' or buffer full
    ls->cur_pos_buf[ls->cur_pos_idx++] = c;
    if (c != 'R' && ls->cur_pos_idx < (ssize_t)sizeof(ls->cur_pos_buf) - 1) {
        return -1;
    }

    ls->cur_pos_buf[ls->cur_pos_idx] = '\0';

    ssize_t rows, cols;
    if (sscanf(ls->cur_pos_buf, "\x1b[%zu;%zu", &rows, &cols) != 2) return -2;
    return (ssize_t)cols;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int getColumns(struct linenoiseState *ls)
{
    ssize_t result;
    switch (ls->mode) {
    case ln_getColumns:
    default:
        /* Get the initial position so we can restore it later. */
        ls->cur_pos_idx = -1;
        ls->mode = ln_getColumns_1;
        // fall through
    case ln_getColumns_1:
        result = getCursorPosition(ls);
        if (result == -1) {
            return -1;
        } else if (result == -2) {
            goto failed;
        }
        ls->smart_term_connected = true;
        ls->cur_pos_initial = result;

        /* Go to right margin and get position. */
        console_write_string("\x1b[999C");

        ls->cur_pos_idx = -1;
        ls->mode = ln_getColumns_2;
        // fall through
    case ln_getColumns_2:
        result = getCursorPosition(ls);
        if (result == -1) {
            return -1;
        } else if (result == -2) {
            goto failed;
        }
        ls->cols = result;
        /* Restore position. */
        if ((ssize_t)ls->cols > ls->cur_pos_initial) {
            char seq[16];
            snprintf(seq, sizeof(seq), "\x1b[%zuD", ls->cols - ls->cur_pos_initial);
            console_write_string(seq);
        }
        break;
    }
finished:
    ls->mode = ln_init;
    return 0;

failed:
    ls->smart_term_connected = false;
    ls->cols = result;
    goto finished;
}

/* Clear the screen. Used to handle ctrl+l */
void linenoiseClearScreen(void)
{
    console_write_string("\x1b[H\x1b[2J");
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void linenoiseBeep(void)
{
    fprintf(stderr, "\x7");
    fflush(stderr);
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by linenoiseAddCompletion(). */
static void freeCompletions(linenoiseCompletions *lc)
{
    size_t i;

    for (i = 0; i < lc->len; i++)
        free(lc->cvec[i]);

    if (lc->cvec != NULL)
        free(lc->cvec);
    
    lc->len = 0;
}

static void lnShowCompletion(struct linenoiseState *ls)
{
    /* Show completion or original buffer */
    if (ls->completion_idx < ls->lc.len) {
        struct linenoiseState saved = *ls;

        ls->len = ls->pos = strlen(ls->lc.cvec[ls->completion_idx]);
        ls->buf = ls->lc.cvec[ls->completion_idx];
        refreshLine(ls);
        ls->len = saved.len;
        ls->pos = saved.pos;
        ls->buf = saved.buf;
    } else {
        refreshLine(ls);
    }
}

/* This is an helper function for linenoiseEdit() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 *
 * The state of the editing is encapsulated into the pointed linenoiseState
 * structure as described in the structure definition. */
static void completeLine(struct linenoiseState *ls)
{
    ls->lc = (linenoiseCompletions){ 0, NULL };
    completionCallback(ls->buf, &ls->lc);

    if (ls->lc.len == 0) {
        linenoiseBeep();
        freeCompletions(&ls->lc);
    } else {
        ls->completion_idx = 0;
        ls->mode = ln_completion;
        lnShowCompletion(ls);
    }
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn)
{
    completionCallback = fn;
}

/* Register a hits function to be called to show hits to the user at the
 * right of the prompt. */
void linenoiseSetHintsCallback(linenoiseHintsCallback *fn)
{
    hintsCallback = fn;
}

/* Register a function to free the hints returned by the hints callback
 * registered with linenoiseSetHintsCallback(). */
void linenoiseSetFreeHintsCallback(linenoiseFreeHintsCallback *fn)
{
    freeHintsCallback = fn;
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str)
{
    size_t len = strlen(str);
    char *copy, **cvec;

    copy = malloc(len + 1);
    if (copy == NULL) return;
    memcpy(copy, str, len + 1);
    cvec = realloc(lc->cvec, sizeof(char*) * (lc->len + 1));

    if (cvec == NULL) {
        free(copy);
        return;
    }

    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    size_t len;
};

static void abInit(struct abuf *ab)
{
    ab->b = NULL;
    ab->len = 0;
}

static void abAppend(struct abuf *ab, const char *s, size_t len)
{
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;

    memcpy(new + ab->len, s, len);
    ab->b = new;
    ab->len += len;
}

static void abFree(struct abuf *ab)
{
    free(ab->b);
}

/* Helper of refreshSingleLine() and refreshMultiLine() to show hints
 * to the right of the prompt. */
static void refreshShowHints(struct abuf *ab, struct linenoiseState *l, size_t plen)
{
    char seq[64] = " ";

    if (hintsCallback && plen + l->len < l->cols) {
        int color = -1;
        bool bold = false;
        const char *hint = hintsCallback(l->buf, &color, &bold);
        if (hint) {
            size_t hintlen = strlen(hint);
            size_t hintmaxlen = l->cols - (plen + l->len);
            if (hintlen > hintmaxlen) hintlen = hintmaxlen;
            if (bold && color == -1) color = 37;

            if (color != -1 || bold)
                snprintf(seq, sizeof(seq), " \033[%d;%d;49m", bold, color);

            abAppend(ab, seq, strlen(seq));
            abAppend(ab, hint, hintlen);

            if (color != -1 || bold != 0)
                abAppend(ab, "\033[0m", 4);

            /* Call the function to free the hint returned. */
            if (freeHintsCallback) freeHintsCallback(hint);
        }
    }
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshSingleLine(struct linenoiseState *l)
{
    char seq[64];
    size_t plen = strlen(l->prompt);

    char *buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;
    struct abuf ab;

    while((plen + pos) >= l->cols) {
        buf++;
        len--;
        pos--;
    }

    while (plen + len > l->cols) {
        len--;
    }

    abInit(&ab);
    /* Cursor to left edge */
    snprintf(seq, sizeof(seq), "\r");
    abAppend(&ab, seq, strlen(seq));
    /* Write the prompt and the current buffer content */
    abAppend(&ab, l->prompt, strlen(l->prompt));
    abAppend(&ab, buf, len);
    /* Show hits if any. */
    refreshShowHints(&ab, l, plen);
    /* Erase to right */
    snprintf(seq, sizeof(seq), "\x1b[0K");
    abAppend(&ab, seq, strlen(seq));
    /* Move cursor to original position. */
    snprintf(seq, sizeof(seq), "\r\x1b[%dC", (int)(pos + plen));
    abAppend(&ab, seq, strlen(seq));

    console_write(ab.b, ab.len);

    abFree(&ab);
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshMultiLine(struct linenoiseState *l)
{
    char seq[64];
    size_t plen = strlen(l->prompt);
    size_t rows = (plen + l->len + l->cols - 1) / l->cols; /* rows used by current buf. */
    size_t rpos = (plen + l->oldpos + l->cols) / l->cols; /* cursor relative row. */
    size_t rpos2; /* rpos after refresh. */
    size_t col; /* colum position, zero-based. */
    size_t old_rows = l->maxrows;
    size_t j;
    struct abuf ab;

    /* Update maxrows if needed. */
    if (rows > l->maxrows) l->maxrows = rows;

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    abInit(&ab);
    if (old_rows - rpos > 0) {
        lndebug("go down %zd", old_rows - rpos);
        snprintf(seq, sizeof(seq), "\x1b[%zdB", old_rows - rpos);
        abAppend(&ab, seq, strlen(seq));
    }

    /* Now for every row clear it, go up. */
    for (j = 0; j < old_rows - 1; j++) {
        lndebug("clear+up");
        snprintf(seq, sizeof(seq), "\r\x1b[0K\x1b[1A");
        abAppend(&ab, seq, strlen(seq));
    }

    /* Clean the top line. */
    lndebug("clear");
    snprintf(seq, sizeof(seq), "\r\x1b[0K");
    abAppend(&ab, seq, strlen(seq));

    /* Write the prompt and the current buffer content */
    abAppend(&ab, l->prompt, strlen(l->prompt));
    abAppend(&ab, l->buf, l->len);

    /* Show hits if any. */
    refreshShowHints(&ab, l, plen);

    /* If we are at the very end of the screen with our prompt, we need to
     * emit a newline and move the prompt to the first column. */
    if (l->pos &&
            l->pos == l->len &&
            (l->pos + plen) % l->cols == 0) {
        lndebug("<newline>");
        abAppend(&ab, "\n", 1);
        snprintf(seq, sizeof(seq), "\r");
        abAppend(&ab, seq, strlen(seq));
        rows++;
        if (rows > l->maxrows) l->maxrows = rows;
    }

    /* Move cursor to right position. */
    rpos2 = (plen + l->pos + l->cols) / l->cols; /* current cursor relative row. */
    lndebug("rpos2 %zd", rpos2);

    /* Go up till we reach the expected positon. */
    if (rows - rpos2 > 0) {
        lndebug("go-up %zd", rows - rpos2);
        snprintf(seq, sizeof(seq), "\x1b[%zdA", rows - rpos2);
        abAppend(&ab, seq, strlen(seq));
    }

    /* Set column. */
    col = (plen + l->pos) % l->cols;
    lndebug("set col %zd", 1 + col);
    if (col)
        snprintf(seq, sizeof(seq), "\r\x1b[%zdC", col);
    else
        snprintf(seq, sizeof(seq), "\r");
    abAppend(&ab, seq, strlen(seq));

    lndebug("\n");
    l->oldpos = l->pos;

    console_write(ab.b, ab.len);
    abFree(&ab);
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void refreshLine(struct linenoiseState *l)
{
    if (mlmode)
        refreshMultiLine(l);
    else
        refreshSingleLine(l);
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
static int linenoiseEditInsert(struct linenoiseState *l, char c)
{
    if (l->len < l->buflen) {
        if (l->len == l->pos) {
            l->buf[l->pos] = c;
            l->pos++;
            l->len++;
            l->buf[l->len] = '\0';
            if ((!mlmode && l->plen + l->len < l->cols && !hintsCallback)) {
                /* Avoid a full update of the line in the
                 * trivial case. */
                console_write(&c, 1);
            } else {
                refreshLine(l);
            }
        } else {
            memmove(l->buf + l->pos + 1, l->buf + l->pos, l->len - l->pos);
            l->buf[l->pos] = c;
            l->len++;
            l->pos++;
            l->buf[l->len] = '\0';
            refreshLine(l);
        }
    }
    return 0;
}

/* Move cursor on the left. */
static void linenoiseEditMoveLeft(struct linenoiseState *l)
{
    if (l->pos > 0) {
        l->pos--;
        refreshLine(l);
    }
}

/* Move cursor on the right. */
static void linenoiseEditMoveRight(struct linenoiseState *l)
{
    if (l->pos != l->len) {
        l->pos++;
        refreshLine(l);
    }
}

/* Move cursor to the start of the line. */
static void linenoiseEditMoveHome(struct linenoiseState *l)
{
    if (l->pos != 0) {
        l->pos = 0;
        refreshLine(l);
    }
}

/* Move cursor to the end of the line. */
static void linenoiseEditMoveEnd(struct linenoiseState *l)
{
    if (l->pos != l->len) {
        l->pos = l->len;
        refreshLine(l);
    }
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1
static void linenoiseEditHistoryNext(struct linenoiseState *l, int dir)
{
    if (history_len > 1) {
        /* Update the current history entry before to
         * overwrite it with the next one. */
        free(history[history_len - 1 - (size_t)l->history_index]);
        history[history_len - 1 - (size_t)l->history_index] = strdup(l->buf);
        /* Show the new entry */
        l->history_index += (dir == LINENOISE_HISTORY_PREV) ? 1 : -1;
        if (l->history_index < 0) {
            l->history_index = 0;
            return;
        } else if ((size_t)l->history_index >= history_len) {
            l->history_index = (ssize_t)history_len - 1;
            return;
        }
        strncpy(l->buf, history[history_len - 1 - (size_t)l->history_index], l->buflen);
        l->buf[l->buflen - 1] = '\0';
        l->len = l->pos = strlen(l->buf);
        refreshLine(l);
    }
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
static void linenoiseEditDelete(struct linenoiseState *l)
{
    if (l->len > 0 && l->pos < l->len) {
        memmove(l->buf + l->pos, l->buf + l->pos + 1, l->len - l->pos - 1);
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Backspace implementation. */
static void linenoiseEditBackspace(struct linenoiseState *l)
{
    if (l->pos > 0 && l->len > 0) {
        memmove(l->buf + l->pos - 1, l->buf + l->pos, l->len - l->pos);
        l->pos--;
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Delete the previous word, maintaining the cursor at the start of the
 * current word. */
static void linenoiseEditDeletePrevWord(struct linenoiseState *l)
{
    size_t old_pos = l->pos;
    size_t diff;

    while (l->pos > 0 && l->buf[l->pos - 1] == ' ')
        l->pos--;

    while (l->pos > 0 && l->buf[l->pos - 1] != ' ')
        l->pos--;

    diff = old_pos - l->pos;
    memmove(l->buf + l->pos, l->buf + old_pos, l->len - old_pos + 1);
    l->len -= diff;
    refreshLine(l);
}

static void lnInitState(struct linenoiseState *l, char *buf, size_t buflen, const char *prompt)
{
    /* Populate the linenoise state that we pass to functions implementing
    * specific editing functionalities. */
    l->buf = buf;
    l->buflen = buflen;
    l->prompt = prompt;
    l->plen = strlen(prompt);
    l->oldpos = l->pos = 0;
    l->len = 0;
    l->maxrows = 0;
    l->history_index = 0;

    /* Buffer starts empty. */
    l->buf[0] = '\0';
    l->buflen--; /* Make sure there is always space for the nulterm */

    /* The latest history entry is always our current buffer, that
    * initially is just an empty string. */
    linenoiseHistoryAdd("");

    console_write_string(prompt);

    l->mode = ln_read_regular;
}

static int lnReadEscSequence(struct linenoiseState *l)
{
    /* Read at least two bytes representing the escape sequence */
    int c = console_getch();
    if (c < 0) {
        return -1;
    }
    if (l->seq_idx >= sizeof(l->seq)) {
        // This should never happen ...
        return -1;
    }
    l->seq[l->seq_idx++] = (char)c;
    if (l->seq_idx < 2) {
        // We need at least 2 characters
        return -1;
    }

    /* ESC [ sequences. */
    if (l->seq[0] == '[') {
        if (l->seq[1] >= '0' && l->seq[1] <= '9') {
            /* Extended escape, make sure we have read the additional byte. */
            if (l->seq_idx < 3) {
                return -1;
            }
            if (l->seq[2] == '~') {
                switch(l->seq[1]) {
                case '3': /* Delete key. */
                    linenoiseEditDelete(l);
                    break;
                default:
                    break;
                }
            }
        } else {
            switch(l->seq[1]) {
            case 'A': /* Up */
                linenoiseEditHistoryNext(l, LINENOISE_HISTORY_PREV);
                break;
            case 'B': /* Down */
                linenoiseEditHistoryNext(l, LINENOISE_HISTORY_NEXT);
                break;
            case 'C': /* Right */
                linenoiseEditMoveRight(l);
                break;
            case 'D': /* Left */
                linenoiseEditMoveLeft(l);
                break;
            case 'H': /* Home */
                linenoiseEditMoveHome(l);
                break;
            case 'F': /* End*/
                linenoiseEditMoveEnd(l);
                break;
            default:
                break;
            }
        }
    }

    /* ESC O sequences. */
    else if (l->seq[0] == 'O') {
        switch(l->seq[1]) {
        case 'H': /* Home */
            linenoiseEditMoveHome(l);
            break;
        case 'F': /* End*/
            linenoiseEditMoveEnd(l);
            break;
        default:
            break;
        }
    }

    l->mode = ln_read_regular;
    return -1;
}

static int lnHandleCharacter(struct linenoiseState *l, char c)
{
    /* Only autocomplete when the callback is set. It returns < 0 when
        * there was an error reading from fd. Otherwise it will return the
        * character that should be handled next. */
    if (c == '\t' && completionCallback != NULL) {
        completeLine(l);
        return -1;
    }

    switch(c) {
    case ENTER:    /* enter */
        history_len--;
        free(history[history_len]);
        if (mlmode) linenoiseEditMoveEnd(l);
        if (hintsCallback) {
            /* Force a refresh without hints to leave the previous
                * line as the user typed it after a newline. */
            linenoiseHintsCallback *hc = hintsCallback;
            hintsCallback = NULL;
            refreshLine(l);
            hintsCallback = hc;
        }
        l->mode = ln_getColumns;
        return (int)l->len;
    case CTRL_C:     /* ctrl-c */
        errno = EAGAIN;
        return -1;
    case BACKSPACE:   /* backspace */
    case 8:     /* ctrl-h */
        linenoiseEditBackspace(l);
        break;
    case CTRL_D:     /* ctrl-d, remove char at right of cursor, or if the
                        line is empty, act as end-of-file. */
        if (l->len > 0) {
            linenoiseEditDelete(l);
        } else {
            history_len--;
            free(history[history_len]);
            return -1;
        }
        break;
    case CTRL_T:    /* ctrl-t, swaps current character with previous. */
        if (l->pos > 0 && l->pos < l->len) {
            char aux = l->buf[l->pos - 1];
            l->buf[l->pos - 1] = l->buf[l->pos];
            l->buf[l->pos] = aux;
            if (l->pos != l->len - 1) l->pos++;
            refreshLine(l);
        }
        break;
    case CTRL_B:     /* ctrl-b */
        linenoiseEditMoveLeft(l);
        break;
    case CTRL_F:     /* ctrl-f */
        linenoiseEditMoveRight(l);
        break;
    case CTRL_P:    /* ctrl-p */
        linenoiseEditHistoryNext(l, LINENOISE_HISTORY_PREV);
        break;
    case CTRL_N:    /* ctrl-n */
        linenoiseEditHistoryNext(l, LINENOISE_HISTORY_NEXT);
        break;
    case ESC:    /* escape sequence */
        l->seq_idx = 0;
        l->mode = ln_read_esc;
        break;
    default:
        if (linenoiseEditInsert(l, (char)c)) return -1;
        break;
    case CTRL_U: /* Ctrl+u, delete the whole line. */
        l->buf[0] = '\0';
        l->pos = l->len = 0;
        refreshLine(l);
        break;
    case CTRL_K: /* Ctrl+k, delete from current to end of line. */
        l->buf[l->pos] = '\0';
        l->len = l->pos;
        refreshLine(l);
        break;
    case CTRL_A: /* Ctrl+a, go to the start of the line */
        linenoiseEditMoveHome(l);
        break;
    case CTRL_E: /* ctrl+e, go to the end of the line */
        linenoiseEditMoveEnd(l);
        break;
    case CTRL_L: /* ctrl+l, clear screen */
        linenoiseClearScreen();
        refreshLine(l);
        break;
    case CTRL_W: /* ctrl+w, delete previous word */
        linenoiseEditDeletePrevWord(l);
        break;
    }
    return -1;
}

static int lnCompletion(struct linenoiseState *ls)
{
    int c = console_getch();
    if (c < 0) {
        return -1;
    }

    switch(c) {
    case 9: /* tab */
        ls->completion_idx = (ls->completion_idx + 1) % (ls->lc.len + 1);
        if (ls->completion_idx == ls->lc.len) linenoiseBeep();
        lnShowCompletion(ls);
        return -1;
    case 27: /* escape */
        /* Re-show original buffer */
        if (ls->completion_idx < ls->lc.len) refreshLine(ls);
        break;
    default:
        /* Update buffer and return */
        if (ls->completion_idx < ls->lc.len) {
            ls->len = ls->pos = (size_t)snprintf(ls->buf, ls->buflen, "%s", ls->lc.cvec[ls->completion_idx]);
        }
        break;
    }

    ls->mode = ln_read_regular;
    freeCompletions(&ls->lc);
    return lnHandleCharacter(ls, (char)c);
}

static int lnReadUserInput(struct linenoiseState *l)
{
    int c = console_getch();

    if (c < 0) {
        return -1;
    }

    return lnHandleCharacter(l, (char)c);
}

/* This function is the core of the line editing capability of linenoise.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 *
 * The resulting string is put into 'buf' when the user type enter, or
 * when ctrl+d is typed.
 *
 * The function returns the length of the current buffer. */
int linenoiseEdit(char *buf, size_t buflen, const char *prompt)
{
    switch (l_state.mode) {
    default:
    case ln_getColumns:
    case ln_getColumns_1:
    case ln_getColumns_2:
        if (getColumns(&l_state) < 0) {
            return -1;
        }
    // fall through
    case ln_init:
        lnInitState(&l_state, buf, buflen, prompt);
    // fall through
    case ln_read_regular:
        return lnReadUserInput(&l_state);
    case ln_read_esc:
        return lnReadEscSequence(&l_state);
    case ln_completion:
        return lnCompletion(&l_state);
    }
    return -1;
}

void linenoiseRefreshEditor()
{
    switch (l_state.mode) {
    case ln_init:
    case ln_getColumns:
    case ln_getColumns_1:
    case ln_getColumns_2:
        // Don't refresh if line editor not active (yet).
        break;
    case ln_completion:
        lnShowCompletion(&l_state);
        break;
    default:
        refreshLine(&l_state);
        break;
    }
}

bool smartTerminalConnected()
{
    return l_state.smart_term_connected;
}

/* This special mode is used by linenoise in order to print scan codes
 * on screen for debugging / development purposes. It is implemented
 * by the linenoise_example program using the --keycodes option. */
void linenoisePrintKeyCodes(void)
{
    char quit[4];

    printf("Linenoise key codes debugging mode.\n"
           "Press keys to see scan codes. Type 'quit' at any time to exit.\n");

    memset(quit, ' ', sizeof(quit));
    while(1) {
        int c;

        do {
            c = console_getch();
        } while (c < 0);

        memmove(quit, quit + 1, sizeof(quit) - 1); /* shift string to left. */
        quit[sizeof(quit) - 1] = (char)c; /* Insert current char on the right. */
        if (memcmp(quit, "quit", sizeof(quit)) == 0) break;

        printf("'%c' %02x (%d) (type quit to exit)\n",
               isprint(c) ? c : '?', (int)c, (int)c);
        printf("\r"); /* Go left edge manually, we are in raw mode. */
        fflush(stdout);
    }
}

/* ================================ History ================================= */

/* This is the API call to add a new entry in the linenoise history.
 * It uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(const char *line)
{
    char *linecopy;

    if (history_max_len == 0) return 0;

    /* Initialization on first call. */
    if (history == NULL) {
        history = calloc(history_max_len, sizeof(char*));
        if (history == NULL) return 0;
    }

    /* Don't add duplicated lines. */
    if (history_len && !strcmp(history[history_len - 1], line)) return 0;

    /* Add an heap allocated copy of the line in the history.
     * If we reached the max length, remove the older line. */
    linecopy = strdup(line);

    if (!linecopy) return 0;
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char*) * (history_max_len - 1));
        history_len--;
    }

    history[history_len] = linecopy;
    history_len++;
    return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int linenoiseHistorySetMaxLen(size_t len)
{
    char **new;

    if (len < 1) return 0;
    if (history) {
        size_t tocopy = history_len;

        new = malloc(sizeof(char*)*len);
        if (new == NULL) return 0;

        /* If we can't copy everything, free the elements we'll not use. */
        if (len < tocopy) {
            size_t j;

            for (j = 0; j < tocopy - len; j++) free(history[j]);
            tocopy = len;
        }

        memset(new, 0, sizeof(char*)*len);
        memcpy(new, history + (history_len - tocopy), sizeof(char*)*tocopy);
        free(history);
        history = new;
    }

    history_max_len = len;

    if (history_len > history_max_len)
        history_len = history_max_len;

    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(const char *filename)
{
    FILE *fp;
    size_t j;

    fp = fopen(filename, "w");
    if (fp == NULL) return -1;

    for (j = 0; j < history_len; j++)
        fprintf(fp, "%s\n", history[j]);

    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    char buf[LINENOISE_MAX_LINE];

    if (fp == NULL) return -1;

    while (fgets(buf, LINENOISE_MAX_LINE, fp) != NULL) {
        char *p;

        p = strchr(buf, '\r');
        if (!p) p = strchr(buf, '\n');
        if (p) *p = '\0';

        linenoiseHistoryAdd(buf);
    }

    fclose(fp);
    return 0;
}
