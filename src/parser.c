/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#include <attentive/parser.h>

#include <stdio.h>
#include <string.h>
#include "debug.h"

/* Defines -------------------------------------------------------------------*/
DBG_SET_LEVEL(DBG_LEVEL_V);

enum at_parser_state {
    STATE_IDLE,
    STATE_READLINE,
    STATE_DATAPROMPT,
    STATE_RAWDATA,
    STATE_HEXDATA,
};

struct at_parser {
    const struct at_parser_callbacks *cbs;
    at_character_handler_t character_handler;
    void *priv;

    enum at_parser_state state;
    const char* dataprompt;
    size_t data_left;
    int nibble;

    char *buf;
    size_t buf_used;
    size_t buf_size;
    size_t buf_current;
};

static const char *const final_ok_responses[] = {
    "OK",
    NULL
};

static const char *const final_responses[] = {
    "OK",
    "ERROR",
    "NO CARRIER",
    "+CME ERROR:",
    "+CMS ERROR:",
    NULL
};

static const char *const urc_responses[] = {
    "RING",
    NULL
};

struct at_parser *at_parser_alloc(const struct at_parser_callbacks *cbs, void *priv)
{
  static char at_buf[AT_BUF_SIZE];
  static struct at_parser at_parser;
  struct at_parser *parser = &at_parser;

    /* Allocate response buffer. */
    parser->buf = at_buf;
    parser->buf_size = sizeof(at_buf);
    parser->cbs = cbs;
    parser->priv = priv;

    /* Prepare instance. */
    at_parser_reset(parser);

    return parser;
}

void at_parser_reset(struct at_parser *parser)
{
    parser->state = STATE_IDLE;
    parser->dataprompt = NULL;
    parser->buf_used = 0;
    parser->buf_current = 0;
    parser->data_left = 0;
    parser->character_handler = NULL;
}

void at_parser_set_character_handler(struct at_parser *parser, at_character_handler_t handler)
{
    parser->character_handler = handler;
}

void at_parser_expect_dataprompt(struct at_parser *parser, const char *prompt)
{
    parser->dataprompt = prompt;
}

void at_parser_await_response(struct at_parser *parser)
{
    parser->state = (parser->dataprompt ? STATE_DATAPROMPT : STATE_READLINE);
}

bool at_prefix_in_table(const char *line, const char *const table[])
{
    for (int i = 0; table[i] != NULL; i++)
        if (!strncmp(line, table[i], strlen(table[i])))
            return true;

    return false;
}

static enum at_response_type generic_line_scanner(const char *line, size_t len, struct at_parser *parser)
{
    if (parser->state == STATE_DATAPROMPT && parser->dataprompt != NULL)
        if (len == strlen(parser->dataprompt) && !memcmp(line, parser->dataprompt, len))
            return AT_RESPONSE_FINAL_OK;

    if (at_prefix_in_table(line, urc_responses))
        return AT_RESPONSE_URC;
    else if (at_prefix_in_table(line, final_ok_responses))
        return AT_RESPONSE_FINAL_OK;
    else if (at_prefix_in_table(line, final_responses))
        return AT_RESPONSE_FINAL;
    else
        return AT_RESPONSE_INTERMEDIATE;
}

static void parser_append(struct at_parser *parser, char ch)
{
    if (parser->buf_used < parser->buf_size - 1)
        parser->buf[parser->buf_used++] = ch;
}

static void parser_include_line(struct at_parser *parser)
{
    /* Append a newline. */
    parser_append(parser, '\n');

    /* Advance the current command pointer to the new position. */
    parser->buf_current = parser->buf_used;
}

static void parser_discard_line(struct at_parser *parser)
{
    /* Rewind the end pointer back to the previous position. */
    parser->buf_used = parser->buf_current;
}

static void parser_finalize(struct at_parser *parser)
{
    /* Remove the last newline, if any. */
    if (parser->buf_used > 0)
        parser->buf_used--;

    /* NULL-terminate the response. */
    parser->buf[parser->buf_used] = '\0';
}

/**
 * Helper, called whenever a full response line is collected.
 */
static void parser_handle_line(struct at_parser *parser)
{
    /* Skip empty lines. */
    if (parser->buf_used == parser->buf_current)
        return;

    /* NULL-terminate the response .*/
    parser->buf[parser->buf_used] = '\0';

    /* Extract line address & length for later use. */
    const char *line = parser->buf + parser->buf_current;
    size_t len = parser->buf_used - parser->buf_current;

    /* Log the received line. */
    DBG_V(">> %s\r\n", line);

    /* Determine response type. */
    enum at_response_type type = AT_RESPONSE_UNKNOWN;
    if (parser->cbs->scan_line)
        type = parser->cbs->scan_line(line, len, parser->priv);
    if (!type)
        type = generic_line_scanner(line, len, parser);

    /* Expected URCs and all unexpected lines are sent to URC handler. */
    if (type == AT_RESPONSE_URC || parser->state == STATE_IDLE)
    {
        /* Fire the callback on the URC line. */
        parser->cbs->handle_urc(line, len, parser->priv);

        /* Discard the URC line from the buffer. */
        parser_discard_line(parser);

        return;
    }

    /* Accumulate everything that's not a final OK. */
    if (type != AT_RESPONSE_FINAL_OK) {
        /* Include the line in the buffer. */
        parser_include_line(parser);
    } else {
        /* Discard the line from the buffer. */
        parser_discard_line(parser);
    }

    /* Act on the response type. */
    switch (type & _AT_RESPONSE_TYPE_MASK) {
        case AT_RESPONSE_FINAL_OK:
        case AT_RESPONSE_FINAL:
        {
            /* Fire the response callback. */
            parser_finalize(parser);
            parser->cbs->handle_response(parser->buf, parser->buf_used, parser->priv);

            /* Go back to idle state. */
            at_parser_reset(parser);
        }
        break;

        case _AT_RESPONSE_RAWDATA_FOLLOWS:
        {
            /* Switch parser state to rawdata mode. */
            parser->data_left = (int)type >> 8;
            parser->state = STATE_RAWDATA;
        }
        break;

        case _AT_RESPONSE_HEXDATA_FOLLOWS:
        {
            /* Switch parser state to hexdata mode. */
            parser->data_left = (int)type >> 8;
            parser->nibble = -1;
            parser->state = STATE_HEXDATA;
        }
        break;

        default:
        {
            /* Keep calm and carry on. */
        }
        break;
    }
}

static int hex2int(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

void at_parser_feed(struct at_parser *parser, const void *data, size_t len)
{
    const uint8_t *buf = data;

    while (len > 0)
    {
        /* Fetch next character. */
        uint8_t ch = *buf++; len--;

        switch (parser->state)
        {
            case STATE_IDLE:
            case STATE_READLINE:
            case STATE_DATAPROMPT:
            {
                if ((ch != '\r') && (ch != '\n')) {
                    /* Append the character if it's not a newline. */
                    parser_append(parser, ch);
                }

                /* Handle a single character. */
                if (parser->character_handler) {
                    ch = parser->character_handler(ch, parser->buf + parser->buf_current,
                                                    parser->buf_used - parser->buf_current,
                                                    parser->priv);
                }

                /* Handle full lines. */
                if ((ch == '\n') ||
                    (parser->state == STATE_DATAPROMPT &&
                     parser->buf_used == strlen(parser->dataprompt) &&
                     !memcmp(parser->buf, parser->dataprompt, parser->buf_used)))
                {
                    parser_handle_line(parser);
                }
            }
            break;

            case STATE_RAWDATA: {
                if (parser->data_left > 0) {
                    parser_append(parser, ch);
                    parser->data_left--;
                }

                if (parser->data_left == 0) {
                    parser_include_line(parser);
                    parser->state = STATE_READLINE;
                }
            } break;

            case STATE_HEXDATA: {
                if (parser->data_left > 0) {
                    int value = hex2int(ch);
                    if (value != -1) {
                        if (parser->nibble == -1) {
                            parser->nibble = value;
                        } else {
                            value |= (parser->nibble << 4);
                            parser->nibble = -1;
                            parser_append(parser, value);
                            parser->data_left--;
                        }
                    }
                }

                if (parser->data_left == 0) {
                    parser_include_line(parser);
                    parser->state = STATE_READLINE;
                }
            } break;
        }
    }
}

void at_parser_free(struct at_parser *parser)
{

}

/* vim: set ts=4 sw=4 et: */
