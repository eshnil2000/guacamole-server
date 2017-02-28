/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "config.h"

#include "terminal/char_mappings.h"
#include "terminal/terminal.h"
#include "terminal/terminal_handlers.h"
#include "terminal/types.h"

#include <guacamole/client.h>
#include <guacamole/protocol.h>
#include <guacamole/socket.h>

#include <stdbool.h>
#include <stdlib.h>
#include <wchar.h>

/**
 * Response string sent when identification is requested.
 */
#define GUAC_TERMINAL_VT102_ID    "\x1B[?6c"

/**
 * Arbitrary response to ENQ control character.
 */
#define GUAC_TERMINAL_ANSWERBACK  "GUACAMOLE"

/**
 * Response which indicates the terminal is alive.
 */
#define GUAC_TERMINAL_OK          "\x1B[0n"

/**
 * Advances the cursor to the next row, scrolling if the cursor would otherwise
 * leave the scrolling region. If the cursor is already outside the scrolling
 * region, the cursor is prevented from leaving the terminal bounds.
 *
 * @param term
 *     The guac_terminal whose cursor should be advanced to the next row.
 */
static void guac_terminal_linefeed(guac_terminal* term) {

    /* Scroll up if necessary */
    if (term->cursor_row == term->scroll_end)
        guac_terminal_scroll_up(term, term->scroll_start,
                term->scroll_end, 1);

    /* Otherwise, just advance to next row if space remains */
    else if (term->cursor_row < term->term_height - 1)
        term->cursor_row++;

}

/**
 * Moves the cursor backward to the previous row, scrolling if the cursor would
 * otherwise leave the scrolling region. If the cursor is already outside the
 * scrolling region, the cursor is prevented from leaving the terminal bounds.
 *
 * @param term
 *     The guac_terminal whose cursor should be moved backward by one row.
 */
static void guac_terminal_reverse_linefeed(guac_terminal* term) {

    /* Scroll down if necessary */
    if (term->cursor_row == term->scroll_start)
        guac_terminal_scroll_down(term, term->scroll_start,
                term->scroll_end, 1);

    /* Otherwise, move back one row if space remains */
    else if (term->cursor_row > 0)
        term->cursor_row--;

}

/**
 * Sets the position of the cursor without exceeding terminal bounds. Values
 * which are out of bounds will be shifted to the nearest legal boundary.
 *
 * @param term
 *     The guac_terminal whose cursor position is being set.
 *
 * @param row
 *     The desired new row position.
 *
 * @param col
 *     The desired new column position.
 */
static void guac_terminal_move_cursor(guac_terminal* term, int row, int col) {

    /* Ensure cursor row is within terminal bounds */
    if (row >= term->term_height)
        row = term->term_height - 1;
    else if (row < 0)
        row = 0;

    /* Ensure cursor column is within terminal bounds */
    if (col >= term->term_width)
        col = term->term_width - 1;
    else if (col < 0)
        col = 0;

    /* Update cursor position */
    term->cursor_row = row;
    term->cursor_col = col;

}

int guac_terminal_echo(guac_terminal* term, unsigned char c) {

    int width;

    static int bytes_remaining = 0;
    static int codepoint = 0;

    const int* char_mapping = term->char_mapping[term->active_char_set];

    /* Echo to pipe stream if open and not starting an ESC sequence */
    if (term->pipe_stream != NULL && c != 0x1B) {
        guac_terminal_pipe_stream_write(term, c);
        return 0;
    }

    /* If using non-Unicode mapping, just map straight bytes */
    if (char_mapping != NULL) {
        codepoint = c;
        bytes_remaining = 0;
    }

    /* 1-byte UTF-8 codepoint */
    else if ((c & 0x80) == 0x00) {    /* 0xxxxxxx */
        codepoint = c & 0x7F;
        bytes_remaining = 0;
    }

    /* 2-byte UTF-8 codepoint */
    else if ((c & 0xE0) == 0xC0) { /* 110xxxxx */
        codepoint = c & 0x1F;
        bytes_remaining = 1;
    }

    /* 3-byte UTF-8 codepoint */
    else if ((c & 0xF0) == 0xE0) { /* 1110xxxx */
        codepoint = c & 0x0F;
        bytes_remaining = 2;
    }

    /* 4-byte UTF-8 codepoint */
    else if ((c & 0xF8) == 0xF0) { /* 11110xxx */
        codepoint = c & 0x07;
        bytes_remaining = 3;
    }

    /* Continuation of UTF-8 codepoint */
    else if ((c & 0xC0) == 0x80) { /* 10xxxxxx */
        codepoint = (codepoint << 6) | (c & 0x3F);
        bytes_remaining--;
    }

    /* Unrecognized prefix */
    else {
        codepoint = '?';
        bytes_remaining = 0;
    }

    /* If we need more bytes, wait for more bytes */
    if (bytes_remaining != 0)
        return 0;

    switch (codepoint) {

        /* Enquiry */
        case 0x05:
            guac_terminal_send_string(term, GUAC_TERMINAL_ANSWERBACK);
            break;

        /* Bell */
        case 0x07:
            break;

        /* Backspace */
        case 0x08:
            guac_terminal_move_cursor(term, term->cursor_row,
                    term->cursor_col - 1);
            break;

        /* Tab */
        case 0x09:
            guac_terminal_move_cursor(term, term->cursor_row,
                    guac_terminal_next_tab(term, term->cursor_col));
            break;

        /* Line feed / VT / FF */
        case '\n':
        case 0x0B: /* VT */
        case 0x0C: /* FF */

            /* Advance to next row */
            guac_terminal_linefeed(term);

            /* If automatic carriage return, fall through to CR handler */
            if (!term->automatic_carriage_return)
                break;

        /* Carriage return */
        case '\r':
            guac_terminal_move_cursor(term, term->cursor_row, 0);
            break;

        /* SO (activates character set G1) */
        case 0x0E:
            term->active_char_set = 1;
            break;

        /* SI (activates character set G0) */
        case 0x0F:
            term->active_char_set = 0;
            break;

        /* ESC */
        case 0x1B:
            term->char_handler = guac_terminal_escape; 
            break;

        /* CSI */
        case 0x9B:
            term->char_handler = guac_terminal_csi; 
            break;

        /* DEL (ignored) */
        case 0x7F:
            break;

        /* Displayable chars */
        default:

            /* Don't bother handling control chars if unknown */
            if (codepoint < 0x20)
                break;

            /* Translate mappable codepoints to whatever codepoint is mapped */
            if (codepoint >= 0x20 && codepoint <= 0xFF && char_mapping != NULL)
                codepoint = char_mapping[codepoint - 0x20];

            /* Wrap if necessary */
            if (term->cursor_col >= term->term_width) {
                term->cursor_col = 0;
                guac_terminal_linefeed(term);
            }

            /* If insert mode, shift other characters right by 1 */
            if (term->insert_mode)
                guac_terminal_copy_columns(term, term->cursor_row,
                        term->cursor_col, term->term_width-2, 1);

            /* Write character */
            guac_terminal_set(term,
                    term->cursor_row,
                    term->cursor_col,
                    codepoint);

            width = wcwidth(codepoint);
            if (width < 0)
                width = 1;

            /* Advance cursor */
            term->cursor_col += width;

    }

    return 0;

}

int guac_terminal_escape(guac_terminal* term, unsigned char c) {

    switch (c) {

        case '(':
            term->char_handler = guac_terminal_g0_charset; 
            break;

        case ')':
            term->char_handler = guac_terminal_g1_charset; 
            break;

        case ']':
            term->char_handler = guac_terminal_osc; 
            break;

        case '[':
            term->char_handler = guac_terminal_csi; 
            break;

        case '#':
            term->char_handler = guac_terminal_ctrl_func; 
            break;

        /* Save Cursor (DECSC) */
        case '7':
            term->saved_cursor_row = term->cursor_row;
            term->saved_cursor_col = term->cursor_col;

            term->char_handler = guac_terminal_echo; 
            break;

        /* Restore Cursor (DECRC) */
        case '8':
            guac_terminal_move_cursor(term,
                    term->saved_cursor_row,
                    term->saved_cursor_col);

            term->char_handler = guac_terminal_echo; 
            break;

        /* Index (IND) */
        case 'D':
            guac_terminal_linefeed(term);
            term->char_handler = guac_terminal_echo; 
            break;

        /* Next Line (NEL) */
        case 'E':
            guac_terminal_move_cursor(term, term->cursor_row, 0);
            guac_terminal_linefeed(term);
            term->char_handler = guac_terminal_echo; 
            break;

        /* Set Tab (HTS) */
        case 'H':
            guac_terminal_set_tab(term, term->cursor_col);
            term->char_handler = guac_terminal_echo; 
            break;

        /* Reverse Linefeed */
        case 'M':
            guac_terminal_reverse_linefeed(term);
            term->char_handler = guac_terminal_echo; 
            break;

        /* DEC Identify */
        case 'Z':
            guac_terminal_send_string(term, GUAC_TERMINAL_VT102_ID);
            term->char_handler = guac_terminal_echo; 
            break;

        /* Reset */
        case 'c':
            guac_terminal_reset(term);
            break;

        default:
            guac_client_log(term->client, GUAC_LOG_INFO, "Unhandled ESC sequence: %c", c);
            term->char_handler = guac_terminal_echo; 

    }

    return 0;

}

/**
 * Given a character mapping specifier (such as B, 0, U, or K),
 * returns the corresponding character mapping.
 */
static const int* __guac_terminal_get_char_mapping(char c) {

    /* Translate character specifier to actual mapping */
    switch (c) {
        case 'B': return NULL;
        case '0': return vt100_map;
        case 'U': return null_map;
        case 'K': return user_map;
    }

    /* Default to Unicode */
    return NULL;

}

int guac_terminal_g0_charset(guac_terminal* term, unsigned char c) {

    term->char_mapping[0] = __guac_terminal_get_char_mapping(c);
    term->char_handler = guac_terminal_echo; 
    return 0;

}

int guac_terminal_g1_charset(guac_terminal* term, unsigned char c) {

    term->char_mapping[1] = __guac_terminal_get_char_mapping(c);
    term->char_handler = guac_terminal_echo; 
    return 0;

}

/**
 * Looks up the flag specified by the given number and mode. Used by the Set/Reset Mode
 * functions of the terminal.
 */
static bool* __guac_terminal_get_flag(guac_terminal* term, int num, char private_mode) {

    if (private_mode == '?') {
        switch (num) {
            case 1:  return &(term->application_cursor_keys); /* DECCKM */
        }
    }

    else if (private_mode == 0) {
        switch (num) {
            case 4:  return &(term->insert_mode); /* DECIM */
            case 20: return &(term->automatic_carriage_return); /* LF/NL */
        }
    }

    /* Unknown flag */
    return NULL;

}

int guac_terminal_csi(guac_terminal* term, unsigned char c) {

    /* CSI function arguments */
    static int argc = 0;
    static int argv[16] = {0};

    /* Sequence prefix, if any */
    static char private_mode_character = 0;

    /* Argument building counter and buffer */
    static int argv_length = 0;
    static char argv_buffer[256];

    /* Digits get concatenated into argv */
    if (c >= '0' && c <= '9') {

        /* Concatenate digit if there is space in buffer */
        if (argv_length < sizeof(argv_buffer)-1)
            argv_buffer[argv_length++] = c;

    }

    /* Specific non-digits stop the parameter, and possibly the sequence */
    else if ((c >= 0x40 && c <= 0x7E) || c == ';') {

        int i, row, col, amount;
        bool* flag;

        /* At most 16 parameters */
        if (argc < 16) {

            /* Finish parameter */
            argv_buffer[argv_length] = 0;
            argv[argc++] = atoi(argv_buffer);

            /* Prepare for next parameter */
            argv_length = 0;

        }

        /* Handle CSI functions */ 
        switch (c) {

            /* @: Insert characters (scroll right) */
            case '@':

                amount = argv[0];
                if (amount == 0) amount = 1;

                /* Scroll right by amount */
                if (term->cursor_col + amount < term->term_width)
                    guac_terminal_copy_columns(term, term->cursor_row,
                            term->cursor_col, term->term_width - amount - 1,
                            amount);

                /* Clear left */
                guac_terminal_clear_columns(term, term->cursor_row,
                        term->cursor_col, term->cursor_col + amount - 1);

                break;

            /* A: Move up */
            case 'A':

                /* Get move amount */
                amount = argv[0];
                if (amount == 0) amount = 1;

                /* Move cursor */
                guac_terminal_move_cursor(term,
                        term->cursor_row - amount,
                        term->cursor_col);

                break;

            /* B: Move down */
            case 'e':
            case 'B':

                /* Get move amount */
                amount = argv[0];
                if (amount == 0) amount = 1;

                /* Move cursor */
                guac_terminal_move_cursor(term,
                        term->cursor_row + amount,
                        term->cursor_col);

                break;

            /* C: Move right */
            case 'a':
            case 'C':

                /* Get move amount */
                amount = argv[0];
                if (amount == 0) amount = 1;

                /* Move cursor */
                guac_terminal_move_cursor(term,
                        term->cursor_row,
                        term->cursor_col + amount);

                break;

            /* D: Move left */
            case 'D':

                /* Get move amount */
                amount = argv[0];
                if (amount == 0) amount = 1;

                /* Move cursor */
                guac_terminal_move_cursor(term,
                        term->cursor_row,
                        term->cursor_col - amount);

                break;

            /* E: Move cursor down given number rows, column 1 */
            case 'E':

                /* Get move amount */
                amount = argv[0];
                if (amount == 0) amount = 1;

                /* Move cursor down, reset to column 1 */
                guac_terminal_move_cursor(term,
                        term->cursor_row + amount,
                        0);

                break;

            /* F: Move cursor up given number rows, column 1 */
            case 'F':

                /* Get move amount */
                amount = argv[0];
                if (amount == 0) amount = 1;

                /* Move cursor up , reset to column 1 */
                guac_terminal_move_cursor(term,
                        term->cursor_row - amount,
                        0);

                break;

            /* G: Move cursor, current row */
            case '`':
            case 'G':
                col = argv[0]; if (col != 0) col--;
                guac_terminal_move_cursor(term, term->cursor_row, col);
                break;

            /* H: Move cursor */
            case 'f':
            case 'H':

                row = argv[0]; if (row != 0) row--;
                col = argv[1]; if (col != 0) col--;

                guac_terminal_move_cursor(term, row, col);
                break;

            /* J: Erase display */
            case 'J':
 
                /* Erase from cursor to end of display */
                if (argv[0] == 0)
                    guac_terminal_clear_range(term,
                            term->cursor_row, term->cursor_col,
                            term->term_height-1, term->term_width-1);
                
                /* Erase from start to cursor */
                else if (argv[0] == 1)
                    guac_terminal_clear_range(term,
                            0, 0,
                            term->cursor_row, term->cursor_col);

                /* Entire screen */
                else if (argv[0] == 2 || argv[0] == 3)
                    guac_terminal_clear_range(term,
                            0, 0, term->term_height - 1, term->term_width - 1);

                break;

            /* K: Erase line */
            case 'K':

                /* Erase from cursor to end of line */
                if (argv[0] == 0)
                    guac_terminal_clear_columns(term, term->cursor_row,
                            term->cursor_col, term->term_width - 1);

                /* Erase from start to cursor */
                else if (argv[0] == 1)
                    guac_terminal_clear_columns(term, term->cursor_row,
                            0, term->cursor_col);

                /* Erase line */
                else if (argv[0] == 2)
                    guac_terminal_clear_columns(term, term->cursor_row,
                            0, term->term_width - 1);

                break;

            /* L: Insert blank lines (scroll down) */
            case 'L':

                amount = argv[0];
                if (amount == 0) amount = 1;

                guac_terminal_scroll_down(term,
                        term->cursor_row, term->scroll_end, amount);

                break;

            /* M: Delete lines (scroll up) */
            case 'M':

                amount = argv[0];
                if (amount == 0) amount = 1;

                guac_terminal_scroll_up(term,
                        term->cursor_row, term->scroll_end, amount);

                break;

            /* P: Delete characters (scroll left) */
            case 'P':

                amount = argv[0];
                if (amount == 0) amount = 1;

                /* Scroll left by amount */
                if (term->cursor_col + amount < term->term_width)
                    guac_terminal_copy_columns(term, term->cursor_row,
                            term->cursor_col + amount, term->term_width - 1,
                            -amount);

                /* Clear right */
                guac_terminal_clear_columns(term, term->cursor_row,
                        term->term_width - amount, term->term_width - 1);

                break;

            /* X: Erase characters (no scroll) */
            case 'X':

                amount = argv[0];
                if (amount == 0) amount = 1;

                /* Clear characters */
                guac_terminal_clear_columns(term, term->cursor_row,
                        term->cursor_col, term->cursor_col + amount - 1);

                break;

            /* ]: Linux Private CSI */
            case ']':
                /* Explicitly ignored */
                break;

            /* c: Identify */
            case 'c':
                if (argv[0] == 0 && private_mode_character == 0)
                    guac_terminal_send_string(term, GUAC_TERMINAL_VT102_ID);
                break;

            /* d: Move cursor, current col */
            case 'd':
                row = argv[0]; if (row != 0) row--;
                guac_terminal_move_cursor(term, row, term->cursor_col);
                break;

            /* g: Clear tab */
            case 'g':

                /* Clear tab at current location */
                if (argv[0] == 0)
                    guac_terminal_unset_tab(term, term->cursor_col);

                /* Clear all tabs */
                else if (argv[0] == 3)
                    guac_terminal_clear_tabs(term);

                break;

            /* h: Set Mode */
            case 'h':
             
                /* Look up flag and set */ 
                flag = __guac_terminal_get_flag(term, argv[0], private_mode_character);
                if (flag != NULL)
                    *flag = true;

                break;

            /* l: Reset Mode */
            case 'l':
              
                /* Look up flag and clear */ 
                flag = __guac_terminal_get_flag(term, argv[0], private_mode_character);
                if (flag != NULL)
                    *flag = false;

                break;

            /* m: Set graphics rendition */
            case 'm':

                for (i=0; i<argc; i++) {

                    int value = argv[i];

                    /* Reset attributes */
                    if (value == 0)
                        term->current_attributes = term->default_char.attributes;

                    /* Bold */
                    else if (value == 1)
                        term->current_attributes.bold = true;

                    /* Underscore on */
                    else if (value == 4)
                        term->current_attributes.underscore = true;

                    /* Reverse video */
                    else if (value == 7)
                        term->current_attributes.reverse = true;

                    /* Normal intensity (not bold) */
                    else if (value == 21 || value == 22)
                        term->current_attributes.bold = false;

                    /* Reset underscore */
                    else if (value == 24)
                        term->current_attributes.underscore = false;

                    /* Reset reverse video */
                    else if (value == 27)
                        term->current_attributes.reverse = false;

                    /* Foreground */
                    else if (value >= 30 && value <= 37)
                        term->current_attributes.foreground = value - 30;

                    /* Underscore on, default foreground */
                    else if (value == 38) {
                        term->current_attributes.underscore = true;
                        term->current_attributes.foreground =
                            term->default_char.attributes.foreground;
                    }

                    /* Underscore off, default foreground */
                    else if (value == 39) {
                        term->current_attributes.underscore = false;
                        term->current_attributes.foreground =
                            term->default_char.attributes.foreground;
                    }

                    /* Background */
                    else if (value >= 40 && value <= 47)
                        term->current_attributes.background = value - 40;

                    /* Reset background */
                    else if (value == 49)
                        term->current_attributes.background =
                            term->default_char.attributes.background;

                }

                break;

            /* n: Status report */
            case 'n':

                /* Device status report */
                if (argv[0] == 5 && private_mode_character == 0)
                    guac_terminal_send_string(term, GUAC_TERMINAL_OK);

                /* Cursor position report */
                else if (argv[0] == 6 && private_mode_character == 0)
                    guac_terminal_sendf(term, "\x1B[%i;%iR", term->cursor_row+1, term->cursor_col+1);

                break;

            /* q: Set keyboard LEDs */
            case 'q':
                /* Explicitly ignored */
                break;

            /* r: Set scrolling region */
            case 'r':

                /* If parameters given, set region */
                if (argc == 2) {
                    term->scroll_start = argv[0]-1;
                    term->scroll_end   = argv[1]-1;
                }

                /* Otherwise, reset scrolling region */
                else {
                    term->scroll_start = 0;
                    term->scroll_end = term->term_height - 1;
                }

                break;

            /* Save Cursor */
            case 's':
                term->saved_cursor_row = term->cursor_row;
                term->saved_cursor_col = term->cursor_col;
                break;

            /* Restore Cursor */
            case 'u':
                guac_terminal_move_cursor(term,
                        term->saved_cursor_row,
                        term->saved_cursor_col);
                break;

            /* Warn of unhandled codes */
            default:
                if (c != ';') {

                    guac_client_log(term->client, GUAC_LOG_INFO,
                            "Unhandled CSI sequence: %c", c);

                    for (i=0; i<argc; i++)
                        guac_client_log(term->client, GUAC_LOG_INFO,
                                " -> argv[%i] = %i", i, argv[i]);

                }

        }

        /* If not a semicolon, end of CSI sequence */
        if (c != ';') {
            term->char_handler = guac_terminal_echo;

            /* Reset parameters */
            for (i=0; i<argc; i++)
                argv[i] = 0;

            /* Reset private mode character */
            private_mode_character = 0;

            /* Reset argument counters */
            argc = 0;
            argv_length = 0;
        }

    }

    /* Set private mode character if given and unset */
    else if (c >= 0x3A && c <= 0x3F && private_mode_character == 0)
        private_mode_character = c;

    return 0;

}

int guac_terminal_set_directory(guac_terminal* term, unsigned char c) {

    static char filename[2048];
    static int length = 0;

    /* Stop on ECMA-48 ST (String Terminator */
    if (c == 0x9C || c == 0x5C || c == 0x07) {
        filename[length++] = '\0';
        term->char_handler = guac_terminal_echo;
        if (term->upload_path_handler)
            term->upload_path_handler(term->client, filename);
        else
            guac_client_log(term->client, GUAC_LOG_DEBUG,
                    "Cannot set upload path. File transfer is not enabled.");
        length = 0;
    }

    /* Otherwise, store character */
    else if (length < sizeof(filename)-1)
        filename[length++] = c;

    return 0;

}

int guac_terminal_download(guac_terminal* term, unsigned char c) {

    static char filename[2048];
    static int length = 0;

    /* Stop on ECMA-48 ST (String Terminator */
    if (c == 0x9C || c == 0x5C || c == 0x07) {
        filename[length++] = '\0';
        term->char_handler = guac_terminal_echo;
        if (term->file_download_handler)
            term->file_download_handler(term->client, filename);
        else
            guac_client_log(term->client, GUAC_LOG_DEBUG,
                    "Cannot send file. File transfer is not enabled.");
        length = 0;
    }

    /* Otherwise, store character */
    else if (length < sizeof(filename)-1)
        filename[length++] = c;

    return 0;

}

int guac_terminal_open_pipe_stream(guac_terminal* term, unsigned char c) {

    static char stream_name[2048];
    static int length = 0;

    /* Open pipe on ECMA-48 ST (String Terminator) */
    if (c == 0x9C || c == 0x5C || c == 0x07) {

        /* End stream name string */
        stream_name[length++] = '\0';
        length = 0;

        /* Open new pipe stream */
        guac_terminal_pipe_stream_open(term, stream_name);

        /* Return to echo mode */
        term->char_handler = guac_terminal_echo;

    }

    /* Otherwise, store character within stream name */
    else if (length < sizeof(stream_name)-1)
        stream_name[length++] = c;

    return 0;

}

int guac_terminal_close_pipe_stream(guac_terminal* term, unsigned char c) {

    /* Handle closure on ECMA-48 ST (String Terminator) */
    if (c == 0x9C || c == 0x5C || c == 0x07) {

        /* Close any existing pipe */
        guac_terminal_pipe_stream_close(term);

        /* Return to echo mode */
        term->char_handler = guac_terminal_echo;

    }

    /* Ignore all other characters */
    return 0;

}

int guac_terminal_osc(guac_terminal* term, unsigned char c) {

    static int operation = 0;

    /* If digit, append to operation */
    if (c >= '0' && c <= '9')
        operation = operation * 10 + c - '0';

    /* If end of parameter, check value */
    else if (c == ';') {

        /* Download OSC */
        if (operation == 482200)
            term->char_handler = guac_terminal_download;

        /* Set upload directory OSC */
        else if (operation == 482201)
            term->char_handler = guac_terminal_set_directory;

        /* Open and redirect output to pipe stream OSC */
        else if (operation == 482202)
            term->char_handler = guac_terminal_open_pipe_stream;

        /* Close pipe stream OSC */
        else if (operation == 482203)
            term->char_handler = guac_terminal_close_pipe_stream;

        /* Reset parameter for next OSC */
        operation = 0;

    }

    /* Stop on ECMA-48 ST (String Terminator */
    else if (c == 0x9C || c == 0x5C || c == 0x07)
        term->char_handler = guac_terminal_echo;

    /* Stop on unrecognized character */
    else {
        guac_client_log(term->client, GUAC_LOG_INFO, "Unexpected character in OSC: 0x%X", c);
        term->char_handler = guac_terminal_echo;
    }

    return 0;
}

int guac_terminal_ctrl_func(guac_terminal* term, unsigned char c) {

    int row;

    /* Build character with current attributes */
    guac_terminal_char guac_char;
    guac_char.value = 'E';
    guac_char.attributes = term->current_attributes;

    switch (c) {

        /* Alignment test (fill screen with E's) */
        case '8':

            for (row=0; row<term->term_height; row++)
                guac_terminal_set_columns(term, row, 0, term->term_width-1, &guac_char);

            break;

    }

    term->char_handler = guac_terminal_echo; 

    return 0;

}

