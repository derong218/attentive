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

/*
 * SIM800 probably holds the highly esteemed position of the world's worst
 * behaving GSM modem, ever. The following quirks have been spotted so far:
 * - response continues after OK (AT+CIPSTATUS)
 * - response without a final OK (AT+CIFSR)
 * - freeform URCs coming at random moments like "DST: 1" (AT+CLTS=1)
 * - undocumented URCs like "+CIEV: ..." (AT+CLTS=1)
 * - text-only URCs like "NORMAL POWER DOWN"
 * - suffix-based URCs like "1, CONNECT OK" (AT+CIPSTART)
 * - bizarre OK responses like "SHUT OK" (AT+CIPSHUT)
 * - responses without a final OK (sic!) (AT+CIFSR)
 * - no response at all (AT&K0)
 * We work it all around, but it makes the code unnecessarily complex.
 */

#define SIM800_AUTOBAUD_ATTEMPTS    10
#define SIM800_WAITACK_TIMEOUT      40
#define SIM800_CIICR_TIMEOUT        45  // TODO: fix timing
#define SIM800_TCP_CONNECT_TIMEOUT  40
#define SIM800_SPP_CONNECT_TIMEOUT  60
#define SIM800_CIPCFG_RETRIES       10
#define SIM800_NSOCKETS             6
#define NTP_BUF_SIZE                4

enum sim800_socket_status {
    SIM800_SOCKET_STATUS_ERROR = -1,
    SIM800_SOCKET_STATUS_UNKNOWN = 0,
    SIM800_SOCKET_STATUS_CONNECTED = 1,
};



static const char *const sim800_urc_responses[] = {
    "+CIPRXGET: 1,",    /* Incoming socket data notification */
    "+BTSPPMAN: ",      /* Incoming BT SPP data notification */
    "+BTPAIRING: ",     /* BT pairing request notification */
    "+BTPAIR: ",        /* BT paired */
    "+BTCONNECTING: ",  /* BT connecting request notification */
    "+BTCONNECT: ",     /* BT connected */
    "+BTDISCONN: ",     /* BT disconnected */
    "+PDP: DEACT",      /* PDP disconnected */
    "+SAPBR 1: DEACT",  /* PDP disconnected (for SAPBR apps) */
    "*PSNWID: ",        /* AT+CLTS network name */
    "*PSUTTZ: ",        /* AT+CLTS time */
    "+CTZV: ",          /* AT+CLTS timezone */
    "DST: ",            /* AT+CLTS dst information */
    "+CIEV: ",          /* AT+CLTS undocumented indicator */
    "RDY",              /* Assorted crap on newer firmware releases. */
    "+CPIN: READY",
    "Call Ready",
    "SMS Ready",
    "NORMAL POWER DOWN",
    "UNDER-VOLTAGE POWER DOWN",
    "UNDER-VOLTAGE WARNNING",
    "OVER-VOLTAGE POWER DOWN",
    "OVER-VOLTAGE WARNNING",
    NULL
};

struct cellular_sim800 {
    struct cellular dev;

    enum sim800_socket_status socket_status[SIM800_NSOCKETS];
    enum sim800_socket_status spp_status;
    int spp_connid;
};

static enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    (void) len;
    struct cellular_sim800 *priv = arg;

    if (at_prefix_in_table(line, sim800_urc_responses))
        return AT_RESPONSE_URC;

    /* Socket status notifications in form of "%d, <status>". */
    if (line[0] >= '0' && line[0] <= '0'+SIM800_NSOCKETS &&
        !strncmp(line+1, ", ", 2))
    {
        int socket = line[0] - '0';

        if (!strcmp(line+3, "CONNECT OK"))
        {
            priv->socket_status[socket] = SIM800_SOCKET_STATUS_CONNECTED;
            return AT_RESPONSE_URC;
        }

        if (!strcmp(line+3, "CONNECT FAIL") ||
            !strcmp(line+3, "ALREADY CONNECT") ||
            !strcmp(line+3, "CLOSED"))
        {
            priv->socket_status[socket] = SIM800_SOCKET_STATUS_ERROR;
            return AT_RESPONSE_URC;
        }
    }

    return AT_RESPONSE_UNKNOWN;
}

static void handle_urc(const char *line, size_t len, void *arg)
{
    struct cellular_sim800 *priv = arg;

    DBG_V("U> %s\r\n", line);

    if (!strncmp(line, "+BTPAIRING: \"Druid_Tech\"", strlen("+BTPAIRING: \"Druid_Tech\""))) {
      at_send(priv->dev.at, "AT+BTPAIR=1,1");
    } else if(!strncmp(line, "+BTCONNECTING: ", strlen("+BTCONNECTING: "))) {
      if(strstr(line, "\"SPP\"")) {
        at_send(priv->dev.at, "AT+BTACPT=1");
      }
    } else if(sscanf(line, "+BTCONNECT: %d,\"Druid_Tech\",%*s,\"SPP\"", &priv->spp_connid) == 1) {
      priv->spp_status = SIM800_SOCKET_STATUS_CONNECTED;
    } else if(!strncmp(line, "+BTDISCONN: \"Druid_Tech\"", strlen("+BTDISCONN: \"Druid_Tech\""))) {
      priv->spp_status = SIM800_SOCKET_STATUS_UNKNOWN;
    }
    return;
}

static const struct at_callbacks sim800_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};


/**
 * SIM800 IP configuration commands fail if the IP application is running,
 * even though the configuration settings are already right. The following
 * monkey dance is therefore needed.
 */
static int sim800_config(struct cellular *modem, const char *option, const char *value, int attempts)
{
    for (int i=0; i<attempts; i++) {
        /* Blindly try to set the configuration option. */
        at_command(modem->at, "AT+%s=%s", option, value);

        /* Query the setting status. */
        const char *response = at_command(modem->at, "AT+%s?", option);
        /* Bail out on timeouts. */
        if (response == NULL)
            return -1;

        /* Check if the setting has the correct value. */
        char expected[16];
        if (snprintf(expected, sizeof(expected), "+%s: %s", option, value) >= (int) sizeof(expected)) {
            return -1;
        }
        if (!strcmp(response, expected))
            return 0;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return -1;
}


static int sim800_attach(struct cellular *modem)
{
    at_set_callbacks(modem->at, &sim800_callbacks, (void *) modem);

    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);

    /* Perform autobauding. */
    for (int i=0; i<SIM800_AUTOBAUD_ATTEMPTS; i++) {
        const char *response = at_command(modem->at, "AT");
        if (response != NULL)
            /* Modem replied. Good. */
            break;
    }

    /* Disable local echo. */
    at_command(modem->at, "ATE0");

    /* Disable local echo again; make sure it was disabled successfully. */
    at_command_simple(modem->at, "ATE0");

    /* Initialize modem. */
    static const char *const init_strings[] = {
//        "AT+IPR=0",                     /* Enable autobauding if not already enabled. */
//        "AT+IFC=0,0",                   /* Disable hardware flow control. */
        "AT+CFUN=1",                    /* Enable full functionality. */
        "AT+CMEE=2",                    /* Enable extended error reporting. */
        "AT+CLTS=0",                    /* Don't sync RTC with network time, it's broken. */
        "AT+CIURC=0",                   /* Disable "Call Ready" URC. */
        "AT&W0",                        /* Save configuration. */
        NULL
    };
    for (const char *const *command=init_strings; *command; command++)
        at_command_simple(modem->at, "%s", *command);

    /* Configure IP application. */

    /* Switch to multiple connections mode; it's less buggy. */
    if (sim800_config(modem, "CIPMUX", "1", SIM800_CIPCFG_RETRIES) != 0)
        return -1;
    /* Receive data manually. */
    if (sim800_config(modem, "CIPRXGET", "1", SIM800_CIPCFG_RETRIES) != 0)
        return -1;
    /* Enable quick send mode. */
    if (sim800_config(modem, "CIPQSEND", "1", SIM800_CIPCFG_RETRIES) != 0)
        return -1;

    return 0;
}

static int sim800_detach(struct cellular *modem)
{
    at_set_callbacks(modem->at, NULL, NULL);
    return 0;
}

//static int sim800_clock_gettime(struct cellular *modem, struct timespec *ts)
//{
//    /* TODO: See CYC-1255. */
//    return -1;
//}

//static int sim800_clock_settime(struct cellular *modem, const struct timespec *ts)
//{
//    /* TODO: See CYC-1255. */
//    return -1;
//}

//static int sim800_clock_ntptime(struct cellular *modem, struct timespec *ts)
//{
//    /* network stuff. */
//    int socket = 2;

//    if (modem->ops->socket_connect(modem, socket, "time-nw.nist.gov", 37) == 0) {
//        printf("sim800: connect successful\n");
//    } else {
//        perror("sim800: connect failed");
//        goto close_conn;
//    }

//    int len = 0;
//    char buf[NTP_BUF_SIZE];
//    while ((len = modem->ops->socket_recv(modem, socket, buf, NTP_BUF_SIZE, 0)) >= 0)
//    {
//        if (len > 0)
//        {
//            printf("Received: >\x1b[1m");
//            for (int i = 0; i<len; i++)
//            {
//                printf("%02x", buf[i]);
//            }
//            printf("\x1b[0m<\n");

//            if (len == 4)
//            {
//                ts->tv_sec = 0;
//                for (int i = 0; i<4; i++)
//                {
//                    ts->tv_sec = (long int)buf[i] + ts->tv_sec*256;
//                }
//                printf("sim800: catched UTC timestamp -> %d\n", ts->tv_sec);
//                ts->tv_sec -= 2208988800L;        //UTC to UNIX time conversion
//                printf("sim800: final UNIX timestamp -> %d\n", ts->tv_sec);
//                goto close_conn;
//            }

//            len = 0;
//        }
//        else
//            vTaskDelay(pdMS_TO_TICKS(1000));
//    }

//#if 0
//    // FIXME: It's wrong. It should be removed
//    while (modem->ops->socket_recv(modem, socket, NULL, 1, 0) != 0)
//    {} //flush
//#endif

//close_conn:
//    if (modem->ops->socket_close(modem, socket) == 0)
//    {
//        printf("sim800: close successful\n");
//    } else {
//        perror("sim800: close");
//    }

//    return 0;
//}

static enum at_response_type scanner_cipstatus(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    /* There are response lines after OK. Keep reading. */
    if (!strcmp(line, "OK"))
        return AT_RESPONSE_INTERMEDIATE;
    /* Collect the entire post-OK response until the last C: line. */
    if (!strncmp(line, "C: 5", strlen("C: 5")))
        return AT_RESPONSE_FINAL;
    return AT_RESPONSE_UNKNOWN;
}

/**
 * Retrieve AT+CIPSTATUS state.
 *
 * @returns Zero if context is open, -1 and sets errno otherwise.
 */
static int sim800_ipstatus(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_set_command_scanner(modem->at, scanner_cipstatus);
    const char *response = at_command(modem->at, "AT+CIPSTATUS");

    if (response == NULL)
        return -1;

    const char *state = strstr(response, "STATE: ");
    if (!state) {
        return -1;
    }
    state += strlen("STATE: ");
    if (!strncmp(state, "IP STATUS", strlen("IP STATUS")))
        return 0;
    if (!strncmp(state, "IP PROCESSING", strlen("IP PROCESSING")))
        return 0;

    return -1;
}

static enum at_response_type scanner_cifsr(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    /* Accept an IP address as an OK response. */
    int ip[4];
    if (sscanf(line, "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]) == 4)
        return AT_RESPONSE_FINAL_OK;
    return AT_RESPONSE_UNKNOWN;
}

static int sim800_pdp_open(struct cellular *modem, const char *apn)
{
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);

    /* Configure and open context for FTP/HTTP applications. */
//    at_command_simple(modem->at, "AT+SAPBR=3,1,APN,\"%s\"", apn);
//    at_command(modem->at, "AT+SAPBR=1,1");

    /* Skip the configuration if context is already open. */
    if (sim800_ipstatus(modem) == 0)
        return 0;

    /* Commands below don't check the response. This is intentional; instead
     * of trying to stay in sync with the GPRS state machine we blindly issue
     * the command sequence needed to transition through all the states and
     * reach IP STATUS. See SIM800 Series_TCPIP_Application Note_V1.01.pdf for
     * the GPRS states documentation. */

    /* Configure context for TCP/IP applications. */
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command(modem->at, "AT+CSTT=\"%s\"", apn);
    /* Establish context. */
    at_set_timeout(modem->at, SIM800_CIICR_TIMEOUT);
    at_command(modem->at, "AT+CIICR");
    /* Read local IP address. Switches modem to IP STATUS state. */
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    at_set_command_scanner(modem->at, scanner_cifsr);
    at_command(modem->at, "AT+CIFSR");

    return sim800_ipstatus(modem);
}


static enum at_response_type scanner_cipshut(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;
    if (!strcmp(line, "SHUT OK"))
        return AT_RESPONSE_FINAL_OK;
    return AT_RESPONSE_UNKNOWN;
}

static int sim800_pdp_close(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    at_set_command_scanner(modem->at, scanner_cipshut);
    at_command_simple(modem->at, "AT+CIPSHUT");

    return 0;
}


static int sim800_socket_connect(struct cellular *modem, int connid, const char *host, uint16_t port)
{
    struct cellular_sim800 *priv = (struct cellular_sim800 *) modem;

    if(connid == SIM800_NSOCKETS) {
      if(cellular_sim800_bt_enable(modem) != 0) {
        return -1;
      }
      for (int i=0; i<SIM800_SPP_CONNECT_TIMEOUT; i++) {
        if(SIM800_SOCKET_STATUS_CONNECTED == priv->spp_status) {
          return 0;
        } else if(SIM800_SOCKET_STATUS_ERROR == priv->spp_status) {
          return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    } else if(connid < SIM800_NSOCKETS) {
      /* Send connection request. */
      at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
      priv->socket_status[connid] = SIM800_SOCKET_STATUS_UNKNOWN;
      cellular_command_simple_pdp(modem, "AT+CIPSTART=%d,TCP,\"%s\",%d", connid, host, port);

      /* Wait for socket status URC. */
      for (int i=0; i<SIM800_TCP_CONNECT_TIMEOUT; i++) {
          if (priv->socket_status[connid] == SIM800_SOCKET_STATUS_CONNECTED) {
              return 0;
          } else if (priv->socket_status[connid] == SIM800_SOCKET_STATUS_ERROR) {
              return -1;
          }
          vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    return -1;
}

static enum at_response_type scanner_cipsend(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int connid, amount;
    char last;
    if (sscanf(line, "DATA ACCEPT:%d,%d", &connid, &amount) == 2)
        return AT_RESPONSE_FINAL_OK;
    if (sscanf(line, "%d, SEND O%c", &connid, &last) == 2 && last == 'K')
        return AT_RESPONSE_FINAL_OK;
    if (sscanf(line, "%d, SEND FAI%c", &connid, &last) == 2 && last == 'L')
        return AT_RESPONSE_FINAL;
    if (!strcmp(line, "SEND OK"))
        return AT_RESPONSE_FINAL_OK;
    if (!strcmp(line, "SEND FAIL"))
        return AT_RESPONSE_FINAL;
    return AT_RESPONSE_UNKNOWN;
}

static ssize_t sim800_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    struct cellular_sim800 *priv = (struct cellular_sim800 *) modem;
    (void) flags;
    if(connid == SIM800_NSOCKETS) {
      if(priv->spp_status != SIM800_SOCKET_STATUS_CONNECTED) {
        return -1;
      }
      amount = amount > 1024 ? 1024 : amount;
      /* Request transmission. */
      at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
      at_expect_dataprompt(modem->at);
      at_command_simple(modem->at, "AT+BTSPPSEND=%d,%zu", priv->spp_connid, amount);

      /* Send raw data. */
      at_set_command_scanner(modem->at, scanner_cipsend);
      at_command_raw_simple(modem->at, buffer, amount);
    } else if(connid < SIM800_NSOCKETS) {
      if(priv->socket_status[connid] != SIM800_SOCKET_STATUS_CONNECTED) {
        return -1;
      }
      amount = amount > 1460 ? 1460 : amount;
      /* Request transmission. */
      at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
      at_expect_dataprompt(modem->at);
      at_command_simple(modem->at, "AT+CIPSEND=%d,%zu", connid, amount);

      /* Send raw data. */
      at_set_command_scanner(modem->at, scanner_cipsend);
      at_command_raw_simple(modem->at, buffer, amount);
    } else {
      return 0;
    }

    return amount;
}

static enum at_response_type scanner_ciprxget(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int read, left;
    if (sscanf(line, "+CIPRXGET: 2,%*d,%d,%d", &read, &left) == 2)
        if (read > 0)
            return AT_RESPONSE_RAWDATA_FOLLOWS(read);

    return AT_RESPONSE_UNKNOWN;
}

static enum at_response_type scanner_btsppget(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int read;
    if (sscanf(line, "+BTSPPGET: %*d,%d", &read) == 1)
        if (read > 0)
            return AT_RESPONSE_RAWDATA_FOLLOWS(read);

    return AT_RESPONSE_UNKNOWN;
}

static char character_handler_btsppget(char ch, char *line, size_t len, void *arg) {
    struct at *priv = (struct at *) arg;
    int read;

    if(ch == ',') {
      line[len] = '\0';
      if (sscanf(line, "+BTSPPGET: %*d,%d,", &read) == 1) {
        at_set_character_handler(priv, NULL);
        ch = '\n';
      }
    }

    return ch;
}

static ssize_t sim800_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags)
{
    struct cellular_sim800 *priv = (struct cellular_sim800 *) modem;
    (void) flags;

    int cnt = 0;
    // TODO its dumb and exceptions should be handled in other right way
    // FIXME: It has to be changed. Leave for now
    if(connid == SIM800_NSOCKETS) {
      if(priv->spp_status != SIM800_SOCKET_STATUS_CONNECTED) {
        return -1;
      }
      char tries = 4;
      while ( (cnt < (int) length) && tries-- ) {
          int chunk = (int) length - cnt;
          /* Limit read size to avoid overflowing AT response buffer. */
          chunk = chunk > 480 ? 480 : chunk;

          /* Perform the read. */
          at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
          at_set_command_scanner(modem->at, scanner_btsppget);
          at_set_character_handler(modem->at, character_handler_btsppget);
          const char *response = at_command(modem->at, "AT+BTSPPGET=3,%d,%d", priv->spp_connid, chunk);
          if (response == NULL)
              return -1;

          /* Find the header line. */
          int read;
          // TODO:
          // 1. connid is not checked
          // 2. there is possible a bug here. if not all data are ready (confirmed < requested)
          // then wierd things can happen. see memcpy
          // requested should be equal to chunk
          // confirmed is that what can be read
          at_simple_scanf(response, "+BTSPPGET: %*d,%d", &read);

          /* Bail out if we're out of data. */
          /* FIXME: We should maybe block until we receive something? */
          if (read == 0)
              break;

          /* Locate the payload. */
          /* TODO: what if no \n is in input stream?
           * should use strnchr at least */
          const char *data = strchr(response, '\n');
          if (data++ == NULL) {
              return -1;
          }

          /* Copy payload to result buffer. */
          memcpy((char *)buffer + cnt, data, read);
          cnt += read;
      }
    }
    else if(connid < SIM800_NSOCKETS) {
      if(priv->socket_status[connid] != SIM800_SOCKET_STATUS_CONNECTED) {
        return -1;
      }
      char tries = 4;
      while ( (cnt < (int) length) && tries-- ){
          int chunk = (int) length - cnt;
          /* Limit read size to avoid overflowing AT response buffer. */
          chunk = chunk > 480 ? 480 : chunk;

          /* Perform the read. */
          at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
          at_set_command_scanner(modem->at, scanner_ciprxget);
          const char *response = at_command(modem->at, "AT+CIPRXGET=2,%d,%d", connid, chunk);
          if (response == NULL)
              return -1;

          /* Find the header line. */
          int read, left;
          // TODO:
          // 1. connid is not checked
          // 2. there is possible a bug here. if not all data are ready (confirmed < requested)
          // then wierd things can happen. see memcpy
          // requested should be equal to chunk
          // confirmed is that what can be read
          at_simple_scanf(response, "+CIPRXGET: 2,%*d,%d,%d", &read, &left);

          /* Bail out if we're out of data. */
          /* FIXME: We should maybe block until we receive something? */
          if (read == 0)
              break;

          /* Locate the payload. */
          /* TODO: what if no \n is in input stream?
           * should use strnchr at least */
          const char *data = strchr(response, '\n');
          if (data++ == NULL) {
              return -1;
          }

          /* Copy payload to result buffer. */
          memcpy((char *)buffer + cnt, data, read);
          cnt += read;
      }
    }

    return cnt;
}

static int sim800_socket_waitack(struct cellular *modem, int connid)
{
    const char *response;
    if(connid == SIM800_NSOCKETS) {
      return 0;
    } else if(connid < SIM800_NSOCKETS) {
      at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
      for (int i=0; i<SIM800_WAITACK_TIMEOUT; i++) {
          /* Read number of bytes waiting. */
          int nacklen;
          response = at_command(modem->at, "AT+CIPACK=%d", connid);
          at_simple_scanf(response, "+CIPACK: %*d,%*d,%d", &nacklen);

          /* Return if all bytes were acknowledged. */
          if (nacklen == 0)
              return 0;

          vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }
    return -1;
}

static enum at_response_type scanner_cipclose(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int connid;
    char last;
    if (sscanf(line, "%d, CLOSE O%c", &connid, &last) == 2 && last == 'K')
        return AT_RESPONSE_FINAL_OK;
    return AT_RESPONSE_UNKNOWN;
}

int sim800_socket_close(struct cellular *modem, int connid)
{
    struct cellular_sim800 *priv = (struct cellular_sim800 *) modem;

    if(connid == SIM800_NSOCKETS) {
      at_command_simple(modem->at, "AT+BTDISCONN=%d", priv->spp_connid);
    } else if(connid < SIM800_NSOCKETS) {
      at_set_timeout(modem->at, AT_TIMEOUT_LONG);
      at_set_command_scanner(modem->at, scanner_cipclose);
      at_command_simple(modem->at, "AT+CIPCLOSE=%d", connid);
    }
    return 0;
}

static const struct cellular_ops sim800_ops = {
    .attach = sim800_attach,
    .detach = sim800_detach,

    .pdp_open = sim800_pdp_open,
    .pdp_close = sim800_pdp_close,

    .imei = cellular_op_imei,
    .iccid = cellular_op_iccid,
    .imsi = cellular_op_imsi,
    .creg = cellular_op_creg,
    .rssi = cellular_op_rssi,
    .cops = cellular_op_cops,
    .test = cellular_op_test,
    .ats0 = cellular_op_ats0,
    .sms = cellular_op_sms,
//    .clock_gettime = sim800_clock_gettime,
//    .clock_settime = sim800_clock_settime,
//    .clock_ntptime = sim800_clock_ntptime,
    .socket_connect = sim800_socket_connect,
    .socket_send = sim800_socket_send,
    .socket_recv = sim800_socket_recv,
    .socket_waitack = sim800_socket_waitack,
    .socket_close = sim800_socket_close,
};

static struct cellular_sim800 cellular;

struct cellular *cellular_sim800_alloc(void)
{
    struct cellular_sim800 *modem = &cellular;
    memset(modem, 0, sizeof(*modem));

    modem->dev.ops = &sim800_ops;

    return (struct cellular *) modem;
}

void cellular_sim800_free(struct cellular *modem)
{
}

int cellular_sim800_bt_mac(struct cellular *modem, char* buf, int len)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    const char *response = at_command(modem->at, "AT+BTHOST?");
    at_simple_scanf(response, "+BTHOST: SIM800C,%s", buf);
    buf[len-1] = '\0';

    return 0;
}

int cellular_sim800_bt_enable(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command_simple(modem->at, "AT+BTSPPCFG=\"MC\",1");
    at_command_simple(modem->at, "AT+BTPAIRCFG=0");
    at_command_simple(modem->at, "AT+BTSPPGET=1");
    at_set_timeout(modem->at, AT_TIMEOUT_LONG);
    at_command_simple(modem->at, "AT+CFUN=4");
    at_command(modem->at, "AT+BTPOWER=1");

    return 0;
}

int cellular_sim800_bt_disable(struct cellular *modem)
{
    at_set_timeout(modem->at, AT_TIMEOUT_SHORT);
    at_command(modem->at, "AT+BTPOWER=0");

    return 0;
}

/* vim: set ts=4 sw=4 et: */
