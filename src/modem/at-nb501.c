/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#include <attentive/cellular.h>

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "at-common.h"
#define printf(...)
#include "debug.h"

/* Defines -------------------------------------------------------------------*/
DBG_SET_LEVEL(DBG_LEVEL_V);

#define AUTOBAUD_ATTEMPTS         10
#define WAITACK_TIMEOUT           24        // Retransmission mechanism: 1.5 + 3 + 6 + 12 = 22.5
#define UPSDA_TIMEOUT             40        // Should be 150 seconds, According to the AT_Command_Manual
#define TCP_CONNECT_TIMEOUT       (20 + 3)  // According to the AT_Command_Manual

static const char *const nb501_urc_responses[] = {
    NULL
};

struct cellular_nb501 {
    struct cellular dev;
};

static enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    if (at_prefix_in_table(line, nb501_urc_responses)) {
        return AT_RESPONSE_URC;
    }

    return AT_RESPONSE_UNKNOWN;
}

static void handle_urc(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    DBG_V("U> %s\r\n", line);
}

static const struct at_callbacks nb501_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};




static int nb501_attach(struct cellular *modem)
{
    at_set_callbacks(modem->at, &nb501_callbacks, (void *) modem);

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);

    /* Perform autobauding. */
    for (int i=0; i<AUTOBAUD_ATTEMPTS; i++) {
        const char *response = at_command(modem->at, "AT");
        if (response != NULL) {
            break;
        }
    }

    /* Initialize modem. */
    static const char *const init_strings[] = {
        NULL
    };
    for (const char *const *command=init_strings; *command; command++)
        at_command_simple(modem->at, "%s", *command);

    return 0;
}

static int nb501_detach(struct cellular *modem)
{
    at_set_callbacks(modem->at, NULL, NULL);
    return 0;
}

static int nb501_pdp_open(struct cellular *modem, const char *apn)
{
    // int active = 0;
    // [> Skip the configuration if context is already open. <]
    // at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    // const char *response = at_command(modem->at, "AT+UPSND=0,8");
    // at_simple_scanf(response, "+UPSND: 0,8,%d", &active);
    // if(active) {
    //     return 0;
    // }
    // [> Configure and open internal pdp context. <]
    // at_command_simple(modem->at, "AT+UPSD=0,1,\"%s\"", apn); // TODO: add usr & pwd [Dale]
    // [> at_command_simple(modem->at, "AT+UPSD=0,2,\"%s\"", usr); <]
    // [> at_command_simple(modem->at, "AT+UPSD=0,3,\"%s\"", pwd); <]
    // [> at_command_simple(modem->at, "AT+UPSD=0,6,\"%s\"", auth); <]
    // at_command_simple(modem->at, "AT+UPSD=0,7,\"0.0.0.0\"");
    // at_set_timeout(modem->at, UPSDA_TIMEOUT);
    // at_command_simple(modem->at, "AT+UPSDA=0,3");
    // [> Read local IP address. <]
    // at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    // const char *resp = at_command(modem->at, "AT+UPSND=0,0");
    // if(resp == NULL) {
    //   return -2;
    // }

    return 0;
}

static int nb501_pdp_close(struct cellular *modem)
{
    // at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    // at_command_simple(modem->at, "AT+UPSDA=0,4");

    return 0;
}

static int nb501_shutdown(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT+CFUN=0");

    return 0;
}

static int nb501_socket_connect(struct cellular *modem, const char *host, uint16_t port)
{
    // struct cellular_nb501 *priv = (struct cellular_nb501 *) modem;
    // [> Open pdp context <]
    // if(cellular_pdp_request(modem) != 0) {
    //   return -1;
    // }
    // [> Create a tcp socket. <]
    // int connid = -1;
    // at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    // const char *response = at_command(modem->at, "AT+USOCR=6");
    // at_simple_scanf(response, "+USOCR: %d", &connid);
    // if(connid >= SARA_NSOCKETS) {
    //     return -1;
    // }
    // priv->socket_status[connid] = SOCKET_STATUS_UNKNOWN;
    // [> Send connection request. <]
    // at_set_timeout(modem->at, TCP_CONNECT_TIMEOUT);
    // at_command_simple(modem->at, "AT+USOCO=%d,\"%s\",%d", connid, host, port);
    // priv->socket_status[connid] = SOCKET_STATUS_CONNECTED;

    // return connid;
    return 0;
}

static ssize_t nb501_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    (void) flags;
    struct cellular_nb501 *priv = (struct cellular_nb501 *) modem;

    amount = amount > 512 ? 512 : amount;

    /* Request transmission. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_send(modem->at, "AT+NMGS=%d,", amount);
    at_send_hex(modem->at, buffer, amount);
    at_command_simple(modem->at, "");

    return amount;
}

static int scanner_nmgr(const char *line, size_t len, void *arg)
{
    (void) arg;

    if (sscanf(line, "%d", &len) == 1)
        if (len > 0) {
            return AT_RESPONSE_HEXDATA_FOLLOWS(len);
        }

    return AT_RESPONSE_UNKNOWN;
}

static char character_handler_nmgr(char ch, char *line, size_t len, void *arg) {
    struct at *priv = (struct at *) arg;

    if(ch == ',') {
        line[len] = '\0';
        if (sscanf(line, "%d,", &len) == 1) {
            at_set_character_handler(priv, NULL);
            ch = '\n';
        }
    }

    return ch;
}

static ssize_t nb501_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags)
{
    (void) flags;

    struct cellular_nb501 *priv = (struct cellular_nb501 *) modem;

    /* Perform the read. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_set_character_handler(modem->at, character_handler_nmgr);
    at_set_command_scanner(modem->at, scanner_nmgr);
    const char *response = at_command(modem->at, "AT+NMGR");
    if (response == NULL) {
        DBG_W(">>>>NO RESPONSE\r\n");
        return -2;
    }
    /* Find the header line. */
    int read;
    if(sscanf(response, "%d,", &read) != 1) {
        DBG_I(">>>>BAD RESPONSE\r\n");
        return -1;
    }

    /* Locate the payload. */
    const char *data = strchr(response, '\n');
    if (data++ == NULL) {
        DBG_I(">>>>NO DATA\r\n");
        return -1;
    }

    /* Copy payload to result buffer. */
    memcpy((char *)buffer, data, read);

    return read;
}

static int nb501_socket_waitack(struct cellular *modem, int connid)
{
    /* struct cellular_nb501 *priv = (struct cellular_nb501 *) modem; */

    /* if(priv->socket_status[connid] == SOCKET_STATUS_CONNECTED) { */
    /*     at_set_timeout(modem->at, AT_TIMEOUT_SHORT); */
    /*     for (int i=0; i<WAITACK_TIMEOUT*2; i++) { */

    /*         int nack; */
    /*         const char *response = at_command(modem->at, "AT+USOCTL=%d,11", connid); */
    /*         at_simple_scanf(response, "+USOCTL: %*d,11,%d", &nack); */

    /*         [> Return if all bytes were acknowledged. <] */
    /*         if (nack == 0) { */
    /*             return 0; */
    /*         } */

    /*         vTaskDelay(pdMS_TO_TICKS(500)); */
    /*     } */
    /* } */
    /* return -1; */
    return 0;
}

static int nb501_socket_close(struct cellular *modem, int connid)
{
    /* struct cellular_nb501 *priv = (struct cellular_nb501 *) modem; */

    /* if(priv->socket_status[connid] == SOCKET_STATUS_CONNECTED) { */
    /*     priv->socket_status[connid] = SOCKET_STATUS_UNKNOWN; */
    /*     at_set_timeout(modem->at, AT_TIMEOUT_LONG); */
    /*     at_command_simple(modem->at, "AT+USOCL=%d", connid); */
    /* } */

    return 0;
}

static int nb501_op_creg(struct cellular *modem)
{
    int creg;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CEREG?");
    at_simple_scanf(response, "+CEREG: %*d,%d", &creg);

    return creg;
}

static int nb501_op_cops(struct cellular *modem)
{
    int ops = -1;
    int rat = -1;

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+COPS?");
    if(response == NULL) {
        return -2;
    }
    int ret = sscanf(response, "+COPS: %*d,%*d,\"%d\",%d", &ops, &rat);
    if(ret == 2) {
        ops |= rat << 24;
    }

    return ops;
}

static int nb501_op_imei(struct cellular *modem, char *buf, size_t len)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+CGSN=1");
    if(response == NULL) {
        return -2;
    }
    if(len > CELLULAR_IMEI_LENGTH && sscanf(response, "+CGSN:%16s", buf) == 1) {

    } else {
        return -1;
    }

    return 0;
}

static int nb501_op_nccid(struct cellular *modem, char *buf, size_t len)
{
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    const char *response = at_command(modem->at, "AT+NCCID");
    if(response == NULL) {
        return -2;
    }
    if(len > CELLULAR_ICCID_LENGTH && sscanf(response, "+NCCID:%21s", buf) == 1) {
        strncpy(buf, response, len);
    } else {
        return -1;
    }

    return 0;
}

static char character_handler_nrb(char ch, char *line, size_t len, void *arg) {
    struct at *priv = (struct at *) arg;

    if(ch > 0x1F && ch < 0x7F) {

    } else if(ch == '\r' || ch == '\n') {

    } else {
        ch = ' ';
        line[len - 1] = ch;
    }

    return ch;
}

static int nb501_op_reset(struct cellular *modem)
{
    // Set CDP
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT+CFUN=0");
    at_command_simple(modem->at, "AT+NCDP=180.101.147.115");

    // Reboot
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    at_set_character_handler(modem->at, character_handler_nrb);
    if(at_command(modem->at, "AT+NRB") == NULL) {
        return -2;
    } else {
        at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
        at_command_simple(modem->at, "AT+CMEE=1");
    }

    return 0;
}

static const struct cellular_ops nb501_ops = {
    .reset = nb501_op_reset,
    .attach = nb501_attach,
    .detach = nb501_detach,

    .pdp_open = nb501_pdp_open,
    .pdp_close = nb501_pdp_close,
    .shutdown = nb501_shutdown,

    .imei = nb501_op_imei,
    .iccid = nb501_op_nccid,
    .imsi = cellular_op_imsi,
    .creg = nb501_op_creg,
    .cgatt = cellular_op_cgatt,
    .rssi = cellular_op_rssi,
    .cops = nb501_op_cops,
    .test = cellular_op_test,
    .command = cellular_op_command,
    .ats0 = cellular_op_ats0,
    .sms = cellular_op_sms,
    .cnum = cellular_op_cnum,
    .onum = cellular_op_onum,
    .socket_connect = nb501_socket_connect,
    .socket_send = nb501_socket_send,
    .socket_recv = nb501_socket_recv,
    .socket_waitack = nb501_socket_waitack,
    .socket_close = nb501_socket_close,
};

static struct cellular_nb501 cellular;

struct cellular *cellular_nb501_alloc(void)
{
    struct cellular_nb501 *modem = &cellular;

    memset(modem, 0, sizeof(*modem));
    modem->dev.ops = &nb501_ops;

    return (struct cellular *) modem;
}

void cellular_nb501_free(struct cellular *modem)
{
}

/* vim: set ts=4 sw=4 et: */
