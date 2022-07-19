/*----------------------------------------------------------------------------
 * Tencent is pleased to support the open source community by making TencentOS
 * available.
 *
 * Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
 * If you have downloaded a copy of the TencentOS binary from Tencent, please
 * note that the TencentOS binary is licensed under the BSD 3-Clause License.
 *
 * If you have downloaded a copy of the TencentOS source code from Tencent,
 * please note that TencentOS source code is licensed under the BSD 3-Clause
 * License, except for the third-party components listed below which are
 * subject to different license terms. Your integration of TencentOS into your
 * own projects may require compliance with the BSD 3-Clause License, as well
 * as the other licenses applicable to the third-party components included
 * within TencentOS.
 *---------------------------------------------------------------------------*/

#include "ec600s.h"
#include "tos_at.h"
#include "tos_hal.h"
#include "sal_module_wrapper.h"

#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

typedef struct ip_addr_st {
    uint8_t seg1;
    uint8_t seg2;
    uint8_t seg3;
    uint8_t seg4;
}ip_addr_t;

at_agent_t ec600s_agent;
static ip_addr_t domain_parser_addr = {0};
static k_sem_t domain_parser_sem;
static k_stack_t ec600s_at_parse_task_stk[AT_PARSER_TASK_STACK_SIZE];

#define AT_AGENT    ((at_agent_t *)&ec600s_agent)

static int ec600s_check_ready(void)
{
    at_echo_t echo;
    int try = 0;

    while (try++ < 10) {
        tos_at_echo_create(&echo, NULL, 0, NULL);
        tos_at_cmd_exec(AT_AGENT, &echo, 1000, "AT\r\n");
        if (echo.status == AT_ECHO_STATUS_OK) {
            return 0;
        }
    }

    return -1;
}

static int ec600s_echo_close(void)
{
    at_echo_t echo;
    int try = 0;

    tos_at_echo_create(&echo, NULL, 0, NULL);

    while (try++ < 10) {
        tos_at_cmd_exec(AT_AGENT, &echo, 1000, "ATE0\r\n");
        if (echo.status == AT_ECHO_STATUS_OK) {
            return 0;
        }
    }

    return -1;
}
static int ec600s_sim_card_check(void)
{
    at_echo_t echo;
    int try = 0;
    char echo_buffer[32];

    tos_at_echo_create(&echo, echo_buffer, sizeof(echo_buffer), NULL);
    while (try++ < 10) {
        tos_at_cmd_exec(AT_AGENT, &echo, 1000, "AT+CPIN?\r\n");
        if (strstr(echo_buffer, "READY")) {
            return 0;
        }
        tos_sleep_ms(2000);
    }

    return -1;
}

static int ec600s_signal_quality_check(void)
{
    int rssi, ber;
    at_echo_t echo;
    char echo_buffer[32], *str;
    int try = 0;

    tos_at_echo_create(&echo, echo_buffer, sizeof(echo_buffer), NULL);
    while (try++ < 10) {
        tos_at_cmd_exec(AT_AGENT, &echo, 1000, "AT+CSQ\r\n");
        if (echo.status != AT_ECHO_STATUS_OK) {
            return -1;
        }

        str = strstr(echo.buffer, "+CSQ:");
        if (!str) {
            return -1;
        }

        sscanf(str, "+CSQ:%d,%d", &rssi, &ber);
        if (rssi != 99) {
            return 0;
        }
        tos_sleep_ms(2000);
    }

    return -1;
}
static int ec600s_gsm_network_check(void)
{
    int n, stat;
    at_echo_t echo;
    char echo_buffer[32], *str;
    int try = 0;

    tos_at_echo_create(&echo, echo_buffer, sizeof(echo_buffer), NULL);
    while (try++ < 10) {
        tos_at_cmd_exec(AT_AGENT, &echo, 1000, "AT+CREG?\r\n");
        if (echo.status != AT_ECHO_STATUS_OK) {
            return -1;
        }

        str = strstr(echo.buffer, "+CREG:");
        if (!str) {
            return -1;
        }
        sscanf(str, "+CREG:%d,%d", &n, &stat);
        if (stat == 1) {
            return 0;
        }
        tos_sleep_ms(2000);
    }

    return -1;
}

static int ec600s_gprs_network_check(void)
{
    int n, stat;
    at_echo_t echo;
    char echo_buffer[32], *str;
    int try = 0;

    tos_at_echo_create(&echo, echo_buffer, sizeof(echo_buffer), NULL);
    while (try++ < 10) {
        tos_at_cmd_exec(AT_AGENT, &echo, 1000, "AT+CGREG?\r\n");
        if (echo.status != AT_ECHO_STATUS_OK) {
            return -1;
        }

        str = strstr(echo.buffer, "+CGREG:");
        if (!str) {
            return -1;
        }
        sscanf(str, "+CGREG:%d,%d", &n, &stat);
        if (stat == 1) {
            return 0;
        }
        tos_sleep_ms(2000);
    }

    return -1;
}

static int ec600s_close_apn(void)
{
    at_echo_t echo;

    tos_at_echo_create(&echo, NULL, 0, NULL);
    tos_at_cmd_exec(AT_AGENT, &echo, 3000, "AT+QIDEACT=1\r\n");
    if (echo.status == AT_ECHO_STATUS_OK) {
        return 0;
    }

    return -1;
}

static int ec600s_set_apn(void)
{
    at_echo_t echo;

    tos_at_echo_create(&echo, NULL, 0, NULL);
    tos_at_cmd_exec(AT_AGENT, &echo, 1000, "AT+QICSGP=1,1,\"CMNET\"\r\n");
    if (echo.status != AT_ECHO_STATUS_OK) {
        return -1;
    }

    tos_at_cmd_exec(AT_AGENT, &echo, 3000, "AT+QIACT=1\r\n");
    if (echo.status != AT_ECHO_STATUS_OK) {
        return -1;
    }

    return 0;
}

static int ec600s_init(void)
{
    printf("Init ec600s ...\n" );

    if (ec600s_check_ready() != 0) {
        printf("wait module ready timeout, please check your module\n");
        return -1;
    }

    if (ec600s_echo_close() != 0) {
        printf("echo close failed,please check your module\n");
        return -1;
    }

    if(ec600s_sim_card_check() != 0) {
        printf("sim card check failed,please insert your card\n");
        return -1;
    }

    if (ec600s_signal_quality_check() != 0) {
        printf("signal quality check status failed\n");
        return -1;
    }

    if(ec600s_gsm_network_check() != 0) {
        printf("GSM network register status check fail\n");
        return -1;
    }

    if(ec600s_gprs_network_check() != 0) {
        printf("GPRS network register status check fail\n");
        return -1;
    }

    if(ec600s_close_apn() != 0) {
        printf("close apn failed\n");
        return -1;
    }

    if (ec600s_set_apn() != 0) {
        printf("apn set FAILED\n");
        return -1;
    }

    printf("Init ec600s ok\n" );
    return 0;
}

static int ec600s_connect_establish(int id, sal_proto_t proto)
{
    at_echo_t echo;
    char except_str[16];
    char echo_buffer[64];
    char *query_result_str = NULL;
    char *remote_ip = NULL;
    char *remote_port = NULL;

    tos_at_echo_create(&echo, echo_buffer, sizeof(echo_buffer), NULL);
    tos_at_cmd_exec(AT_AGENT, &echo, 1000, "AT+QISTATE=1,%d\r\n", id);
    if (echo.status != AT_ECHO_STATUS_OK) {
        printf("query socket %d state fail\r\n", id);
        return -1;
    }

    sprintf(except_str, "+QISTATE: %d", id);
    query_result_str = strstr(echo_buffer, except_str);
    if (query_result_str) {
        printf("socket %d established on module already\r\n", id);
        tos_at_echo_create(&echo, NULL, 0, NULL);
        tos_at_cmd_exec(AT_AGENT, &echo, 1000, "AT+QICLOSE=%d\r\n", id);
    }

    memset(except_str, 0, sizeof(except_str));
    sprintf(except_str, "+QIOPEN: %d,0", id);

    remote_ip = (char*)tos_at_channel_ip_get(AT_AGENT, id);
    remote_port = (char*)tos_at_channel_port_get(AT_AGENT, id);
    if (!remote_ip || !remote_port) {
        return -2;
    }

    tos_at_echo_create(&echo, NULL, 0, except_str);
    tos_at_cmd_exec_until(AT_AGENT, &echo, 4000, "AT+QIOPEN=1,%d,\"%s\",\"%s\",%d,0,1\r\n",
                        id, proto == TOS_SAL_PROTO_UDP ? "UDP" : "TCP", remote_ip, atoi(remote_port));
    if (echo.status != AT_ECHO_STATUS_EXPECT) {
        printf("establish socket %d on module fail\r\n", id);
        return -3;
    }

    return 0;
}

static int ec600s_connect(const char *ip, const char *port, sal_proto_t proto)
{
    int id;

    id = tos_at_channel_alloc(AT_AGENT, ip, port);
    if (id == -1) {
        printf("at channel alloc fail\r\n");
        return -1;
    }

    if (ec600s_connect_establish(id, proto) < 0) {
        tos_at_channel_free(AT_AGENT, id);
        return -2;
    }

    return id;
}

static int ec600s_connect_with_size(const char *ip, const char *port, sal_proto_t proto, size_t socket_buffer_size)
{
    int id;

    id = tos_at_channel_alloc_with_size(AT_AGENT, ip, port, socket_buffer_size);
    if (id == -1) {
        printf("at channel alloc fail\r\n");
        return -1;
    }

    if (ec600s_connect_establish(id, proto) < 0) {
        tos_at_channel_free(AT_AGENT, id);
        return -2;
    }

    return id;
}

static int ec600s_recv_timeout(int id, void *buf, size_t len, uint32_t timeout)
{
    return tos_at_channel_read_timed(AT_AGENT, id, buf, len, timeout);
}

static int ec600s_recv(int id, void *buf, size_t len)
{
    return ec600s_recv_timeout(id, buf, len, (uint32_t)4000);
}

int ec600s_send(int id, const void *buf, size_t len)
{
    at_echo_t echo;

    if (!tos_at_channel_is_working(AT_AGENT, id)) {
        return -1;
    }

    tos_at_echo_create(&echo, NULL, 0, ">");

    tos_at_cmd_exec_until(AT_AGENT, &echo, 1000, "AT+QISEND=%d,%d\r\n", id, len);

    if (echo.status != AT_ECHO_STATUS_EXPECT) {
        return -1;
    }

    tos_at_echo_create(&echo, NULL, 0, "SEND OK");
    tos_at_raw_data_send_until(AT_AGENT, &echo, 10000, (uint8_t *)buf, len);
    if (echo.status != AT_ECHO_STATUS_EXPECT) {
        return -1;
    }

    return len;
}

int ec600s_recvfrom_timeout(int id, void *buf, size_t len, uint32_t timeout)
{
    return tos_at_channel_read_timed(AT_AGENT, id, buf, len, timeout);
}

int ec600s_recvfrom(int id, void *buf, size_t len)
{
    return ec600s_recvfrom_timeout(id, buf, len, (uint32_t)4000);
}

int ec600s_sendto(int id, char *ip, char *port, const void *buf, size_t len)
{
    at_echo_t echo;

    tos_at_echo_create(&echo, NULL, 0, ">");
    tos_at_cmd_exec_until(AT_AGENT, &echo, 1000, "AT+QISEND=%d,%d\r\n", id, len);

    if (echo.status != AT_ECHO_STATUS_EXPECT) {
        return -1;
    }

    tos_at_echo_create(&echo, NULL, 0, "SEND OK");
    tos_at_raw_data_send(AT_AGENT, &echo, 1000, (uint8_t *)buf, len);
    if (echo.status != AT_ECHO_STATUS_EXPECT) {
        return -1;
    }

    return len;
}

static void ec600s_transparent_mode_exit(void)
{
    at_echo_t echo;

    tos_at_echo_create(&echo, NULL, 0, NULL);
    tos_at_cmd_exec(AT_AGENT, &echo, 500, "+++");
}

static int ec600s_close(int id)
{
    at_echo_t echo;

    ec600s_transparent_mode_exit();

    tos_at_echo_create(&echo, NULL, 0, NULL);
    tos_at_cmd_exec(AT_AGENT, &echo, 1000, "AT+QICLOSE=%d\r\n", id);

    tos_at_channel_free(AT_AGENT, id);

    return 0;
}

static int ec600s_parse_domain(const char *host_name, char *host_ip, size_t host_ip_len)
{
    at_echo_t echo;
    char echo_buffer[128];

    tos_sem_create_max(&domain_parser_sem, 0, 1);

    tos_at_echo_create(&echo, echo_buffer, sizeof(echo_buffer), NULL);
    tos_at_cmd_exec(AT_AGENT, &echo, 2000, "AT+QIDNSGIP=1,\"%s\"\r\n", host_name);

    if (echo.status != AT_ECHO_STATUS_OK) {
        return -1;
    }

    tos_sem_pend(&domain_parser_sem, TOS_TIME_FOREVER);
    snprintf(host_ip, host_ip_len, "%d.%d.%d.%d", domain_parser_addr.seg1, domain_parser_addr.seg2, domain_parser_addr.seg3, domain_parser_addr.seg4);
    host_ip[host_ip_len - 1] = '\0';

    printf("GOT IP: %s\n", host_ip);

    return 0;
}

__STATIC__ void ec600s_incoming_data_process(void)
{
    uint8_t data;
    int channel_id = 0, data_len = 0, read_len;
    uint8_t buffer[128];

    /*
		+QIURC: "recv",<sockid>,<datalen>
		<data content>
    */

    while (1) {
        if (tos_at_uart_read(AT_AGENT, &data, 1) != 1) {
            return;
        }
        if (data == ',') {
            break;
        }
        channel_id = channel_id * 10 + (data - '0');
    }

    while (1) {
        if (tos_at_uart_read(AT_AGENT, &data, 1) != 1) {
            return;
        }

        if (data == '\r') {
            break;
        }
        data_len = data_len * 10 + (data - '0');
    }

    if (tos_at_uart_read(AT_AGENT, &data, 1) != 1) {
        return;
    }

    do {
#if !defined(MIN)
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#endif
        read_len = MIN(data_len, sizeof(buffer));
        if (tos_at_uart_read(AT_AGENT, buffer, read_len) != read_len) {
            return;
        }

        if (tos_at_channel_write(AT_AGENT, channel_id, buffer, read_len) <= 0) {
            return;
        }

        data_len -= read_len;
    } while (data_len > 0);

    return;
}

__STATIC__ void ec600s_domain_data_process(void)
{
    uint8_t data;

    /*
        +QIURC: "dnsgip",0,1,600

		+QIURC: "dnsgip","xxx.xxx.xxx.xxx"
    */

    if (tos_at_uart_read(AT_AGENT, &data, 1) != 1) {
        return;
    }

    if (data == '0') {
        return;
    }

    if (data == '\"') {
        /* start parser domain */
        while (1) {
            if (tos_at_uart_read(AT_AGENT, &data, 1) != 1) {
                return;
            }
            if (data == '.') {
                break;
            }
            domain_parser_addr.seg1 = domain_parser_addr.seg1 *10 + (data-'0');
        }
        while (1) {
            if (tos_at_uart_read(AT_AGENT, &data, 1) != 1) {
                return;
            }
            if (data == '.') {
                break;
            }
            domain_parser_addr.seg2 = domain_parser_addr.seg2 *10 + (data-'0');
        }
        while (1) {
            if (tos_at_uart_read(AT_AGENT, &data, 1) != 1) {
                return;
            }
            if (data == '.') {
                break;
            }
            domain_parser_addr.seg3 = domain_parser_addr.seg3 *10 + (data-'0');
        }
        while (1) {
            if (tos_at_uart_read(AT_AGENT, &data, 1) != 1) {
                return;
            }
            if (data == '\"') {
                break;
            }
            domain_parser_addr.seg4 = domain_parser_addr.seg4 *10 + (data-'0');
        }
        tos_sem_post(&domain_parser_sem);
    }
    return;

}

at_event_t ec600s_at_event[] = {
	{ "+QIURC: \"recv\",",   ec600s_incoming_data_process},
    { "+QIURC: \"dnsgip\",", ec600s_domain_data_process},
};

sal_module_t sal_module_ec600s = {
    .init               = ec600s_init,
    .connect            = ec600s_connect,
    .connect_with_size  = ec600s_connect_with_size,
    .send               = ec600s_send,
    .recv_timeout       = ec600s_recv_timeout,
    .recv               = ec600s_recv,
    .sendto             = ec600s_sendto,
    .recvfrom           = ec600s_recvfrom,
    .recvfrom_timeout   = ec600s_recvfrom_timeout,
    .close              = ec600s_close,
    .parse_domain       = ec600s_parse_domain,
};

int ec600s_sal_init(hal_uart_port_t uart_port)
{

    if (tos_at_init(AT_AGENT, "ec600s_at", ec600s_at_parse_task_stk,
                    uart_port, ec600s_at_event,
                        sizeof(ec600s_at_event) / sizeof(ec600s_at_event[0])) != 0) {
        return -1;
    }

    if (tos_sal_module_register(&sal_module_ec600s) != 0) {
        return -1;
    }
    if (tos_sal_module_init() != 0) {
        return -1;
    }

    return 0;
}

int ec600s_sal_deinit()
{
    int id = 0;

    for (id = 0; id < AT_DATA_CHANNEL_NUM; ++id) {
        tos_sal_module_close(id);
    }

    tos_sal_module_register_default();

    tos_at_deinit(AT_AGENT);

    return 0;
}

