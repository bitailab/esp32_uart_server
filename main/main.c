/* UART Echo Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <sys/param.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_wifi.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "wifi_manager.h"

#define PORT                        CONFIG_EXAMPLE_PORT
#define KEEPALIVE_IDLE              CONFIG_EXAMPLE_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL          CONFIG_EXAMPLE_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT             CONFIG_EXAMPLE_KEEPALIVE_COUNT
/**
 * This is an example which echos any data it receives on configured UART back to the sender,
 * with hardware flow control turned off. It does not use UART driver event queue.
 *
 * - Port: configured UART
 * - Receive (Rx) buffer: on
 * - Transmit (Tx) buffer: off
 * - Flow control: off
 * - Event queue: off
 * - Pin assignment: see defines below (See Kconfig)
 */

#define ECHO_TEST_TXD (CONFIG_EXAMPLE_UART_TXD)
#define ECHO_TEST_RXD (CONFIG_EXAMPLE_UART_RXD)
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      (CONFIG_EXAMPLE_UART_PORT_NUM)
#define ECHO_UART_BAUD_RATE     (CONFIG_EXAMPLE_UART_BAUD_RATE)
#define ECHO_TASK_STACK_SIZE    (CONFIG_EXAMPLE_TASK_STACK_SIZE)

static QueueHandle_t s_tcp2uart_queue = NULL;
static QueueHandle_t s_uart2tcp_queue = NULL;

static const char *TAG = "UART SERVER";

#define BUF_SIZE (1024 * 2)
#define UART2TCP_QUEUE_LEN (100)
#define TCP2UART_QUEUE_LEN (100)


#define TCP_SERVER_TASK_PRIORITY (10)
#define UART_WRITE_TASK_PRIORITY (5)
#define UART_READ_TASK_PRIORITY (5)
#define TCP_CLIENT_SEND_TASK_PRIORITY (4)
#define TCP_CLIENT_RECV_TASK_PRIORITY (4)

typedef struct {
    char* data;
    int len;
} tcp_uart_data_t;

enum UART_SERVER_SOCK_STAT {
    UART_SERVER_SOCK_STAT_OK = 0,
    UART_SERVER_SOCK_STAT_ERROR = -1
};

static int connected_flag = 0;
static SemaphoreHandle_t xMutex = NULL;
static int set_connected_flag() {
    if (xSemaphoreTake(xMutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    connected_flag = 1;
    xSemaphoreGive(xMutex);
    return 0;
}

static int clear_connected_flag() {
    if (xSemaphoreTake(xMutex, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    connected_flag = 0;
    xSemaphoreGive(xMutex);
    return 0;
}

static int get_connected_flag(int *res) {
    if (xSemaphoreTake(xMutex, 10/portTICK_PERIOD_MS) != pdTRUE) {
        return -1;
    }
    *res = connected_flag;
    xSemaphoreGive(xMutex);
    return 0;
}

static esp_err_t send_uart2tcp_queue(tcp_uart_data_t *d);
static esp_err_t send_tcp2uart_queue(tcp_uart_data_t *d);

static esp_err_t uart_init(void)
{
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = ECHO_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS));

    return ESP_OK;
}

static void uart_write_task(void)
{
    tcp_uart_data_t d;

    while (1) {
        if(xQueueReceive(s_tcp2uart_queue, &d, 10/portTICK_PERIOD_MS) == pdTRUE) {
            uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) d.data, d.len);
            if (d.len) {
                ESP_LOGI(TAG, "Writed %d bytes to UART", d.len);
                //ESP_LOG_BUFFER_HEX(TAG, d.data, d.len);
            }
            free(d.data);
        }
    }
    vTaskDelete(NULL);//删除整个task，不然会触发看门狗
}

static void uart_read_task(void)
{

    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);
    while(1) {
        // Read data from the UART
        int len = uart_read_bytes(ECHO_UART_PORT_NUM, data, (BUF_SIZE - 1), 10 / portTICK_PERIOD_MS);
        if (len) {
            ESP_LOGI(TAG, "Received data from uart, len = %d", len);
            tcp_uart_data_t d;
            // Configure a temporary buffer for the incoming data
            d.data = (char *) malloc(len+1);
            d.len = len;
            memcpy(d.data, data, len);
            d.data[len] = 0;
            send_uart2tcp_queue(&d);
        }
    }

    free(data);
    vTaskDelete(NULL);//删除整个task，不然会触发看门狗
}

static esp_err_t send_tcp2uart_queue(tcp_uart_data_t *d) {
    if(xQueueSend(s_tcp2uart_queue, d, 0) != pdTRUE) {
        ESP_LOGE(TAG, "failed to send tcp data to uart queue");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static int try_receive(const char *tag, const int sock, char * data, size_t max_len)
{
    int len = recv(sock, data, max_len, 0);
    if (len < 0) {
        if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;   // Not an error
        }
        if (errno == ENOTCONN) {
            ESP_LOGW(tag, "[sock=%d]: Connection closed", sock);
            return -2;  // Socket has been disconnected
        }
        ESP_LOGE(tag, "Error occurred during receiving, socket = %d, errno = %d", sock, errno);
        return -1;
    }

    return len;
}

static int readable_timeo(int fd, int sec) {
    fd_set rset;
    struct timeval tv;
    FD_ZERO(&rset);
    FD_SET(fd, &rset);

    tv.tv_sec = sec;
    tv.tv_usec = 0;
    return (select(fd+1, &rset, NULL, NULL, &tv));
}

static int get_socket_stat(int sock) {
    int error;
    socklen_t error_len = sizeof(error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0) {
        if (error != 0) {
            return UART_SERVER_SOCK_STAT_ERROR;
        }
        return UART_SERVER_SOCK_STAT_OK;
    } 
    return UART_SERVER_SOCK_STAT_ERROR;
}

static void handle_client_send_task(void *pt){
    int len;
    char rx_buffer[BUF_SIZE];//接收TCP的数据
    int sock = (int)pt;//创建任务传过来的参数（sock句柄）
    int connecttag = 0;//断开连接结束整个任务的标记
    tcp_uart_data_t uart2tcp_data;
    ESP_LOGI(TAG, "tcp sending task loop start");
    while (1)
    {
        if (get_socket_stat(sock) != UART_SERVER_SOCK_STAT_OK) {
            goto CLEAN_UP;
        }
        // 读UART QUEUE
        if(xQueueReceive(s_uart2tcp_queue, &uart2tcp_data, 10/portTICK_PERIOD_MS)) {
            int to_write = uart2tcp_data.len;
            ESP_LOGI(TAG, "Sending data from uart to tcp, bytes: %d", to_write);
            while(to_write > 0) {
                int written = send(sock, uart2tcp_data.data + (uart2tcp_data.len - to_write), to_write, 0);
                if (written < 0) {
                    ESP_LOGE(TAG, "Error occurred during sending: error %d", errno);
                    connecttag = 1;
                    break;
                }
                to_write -= written;
            }
            // 释放内存
            free(uart2tcp_data.data);
            if(connecttag) {
                goto CLEAN_UP;
            }
        } else {
            taskYIELD();
        }
    }

CLEAN_UP:
    shutdown(sock, 0);
    close(sock);
    ESP_LOGI(TAG, "sock: %d closed, exit sending task", sock);
    vTaskDelete(NULL);//删除整个task，不然会触发看门狗
}

static void handle_client_recv_task(void *pt){
    int len;
    char rx_buffer[BUF_SIZE];//接收TCP的数据
    int sock = (int)pt;//创建任务传过来的参数（sock句柄）
    int connecttag = 0;//断开连接结束整个任务的标记

    tcp_uart_data_t uart2tcp_data;
    ESP_LOGI(TAG, "tcp receiving task loop start");
    while (1)
    {
        int ret = readable_timeo(sock, 1);
        if (ret <= 0) {
            continue;
        }
        //  读TCP
        len = try_receive(TAG, sock, rx_buffer, sizeof(rx_buffer));
        if (len < 0) {
            goto CLEAN_UP;
        } else if (len > 0) {
            ESP_LOGI(TAG, "Received %d bytes", len);
            tcp_uart_data_t tcp2uart_data;
            tcp2uart_data.data = (char*) malloc(len);
            if(tcp2uart_data.data == NULL) {
                ESP_LOGE(TAG, "malloc error");
                continue;
            }
            memset(tcp2uart_data.data, 0, len);
            tcp2uart_data.len = len;
            memcpy(tcp2uart_data.data, rx_buffer, len);
            send_tcp2uart_queue(&tcp2uart_data);
        } else {
            taskYIELD();
        }
    }

CLEAN_UP:
    shutdown(sock, 0);
    close(sock);
    if(clear_connected_flag()<0) {
        ESP_LOGE(TAG, "sock: %d, clear connected flag failed", sock);
    }
    ESP_LOGI(TAG, "sock: %d closed, exit receiving task", sock);
    vTaskDelete(NULL);//删除整个task，不然会触发看门狗
}

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    int is_connected = -1;
    struct sockaddr_storage dest_addr;

    if (addr_family == AF_INET) {
        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(PORT);
        ip_protocol = IPPROTO_IP;
    }

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGI(TAG, "Socket created");

    // Marking the socket as non-blocking
    // int flags = fcntl(listen_sock, F_GETFL);
    // if (fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
    //     ESP_LOGE(TAG, "Unable to set socket non blocking, errno = %d", errno);
    //     goto CLEAN_UP;
    // }
    // ESP_LOGI(TAG, "Socket marked as non blocking");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    ESP_LOGI(TAG, "Socket listening");
    while (1) {

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            if (errno == EWOULDBLOCK) { // The listener socket did not accepts any connection
                                            // continue to serve open connections and try to accept again upon the next iteration
                ESP_LOGV(TAG, "No pending connections...");
                continue;
            } else {
                ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
                break;
            }
        }
        if(get_connected_flag(&is_connected) < 0 || 
                (get_connected_flag(&is_connected) == 0 && is_connected)) {
            ESP_LOGE(TAG, "Max client connected. Rejecting new client.");
            close(sock);
            continue;
        }

        // Set connected flag
        set_connected_flag();

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        // // ...and set the client's socket non-blocking
        // flags = fcntl(sock, F_GETFL);
        // if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        //     ESP_LOGE(TAG, "Unable to set socket non blocking, sock = %d, errno = %d", sock, errno);
        // } else {
        //     ESP_LOGI(TAG, "Socket marked as non blocking, sock = %d", sock);
        // }
        // Convert ip address to string
        if (source_addr.ss_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        }
        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

        //do_retransmit(sock);

        //shutdown(sock, 0);
        //close(sock);
        xTaskCreate(handle_client_recv_task, "handle_client_recv_task", 4096, 
                        (void *)sock, TCP_CLIENT_RECV_TASK_PRIORITY, NULL);
        xTaskCreate(handle_client_send_task, "handle_client_send_task", 4096, 
                        (void *)sock, TCP_CLIENT_SEND_TASK_PRIORITY, NULL);
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}


static esp_err_t send_uart2tcp_queue(tcp_uart_data_t *d)
{
    if(xQueueSend(s_uart2tcp_queue, d, 0) != pdTRUE) {
        ESP_LOGE(TAG, "failed to send to uart2tcp queue");
        return ESP_FAIL;
    }
    return ESP_OK;
}


/**
 * @brief this is an exemple of a callback that you can setup in your own app to get notified of wifi manager event.
 */
void cb_connection_ok(void *pvParameter){
	ip_event_got_ip_t* param = (ip_event_got_ip_t*)pvParameter;

	/* transform IP to human readable string */
	char str_ip[16];
	esp_ip4addr_ntoa(&param->ip_info.ip, str_ip, IP4ADDR_STRLEN_MAX);

	ESP_LOGI(TAG, "I have a connection and my IP is %s!", str_ip);
    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*)AF_INET, 
                    TCP_SERVER_TASK_PRIORITY, NULL);
    xTaskCreate(uart_write_task, "uart_write_task", ECHO_TASK_STACK_SIZE, NULL, 
                    UART_WRITE_TASK_PRIORITY, NULL);
    xTaskCreate(uart_read_task, "uart_read_task", ECHO_TASK_STACK_SIZE, NULL,
                    UART_READ_TASK_PRIORITY, NULL);
}
void app_main(void)
{
    //ESP_ERROR_CHECK(nvs_flash_init());
    //ESP_ERROR_CHECK(esp_netif_init());
    //ESP_ERROR_CHECK(esp_event_loop_create_default());

    ///* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     //* Read "Establishing Wi-Fi or Ethernet Connection" section in
     //* examples/protocols/README.md for more information about this function.
     //*/
    //ESP_ERROR_CHECK(example_connect());

    ESP_ERROR_CHECK(uart_init());

    /* start the wifi manager */
	wifi_manager_start();
    /* register a callback as an example to how you can integrate your code with the wifi manager */
	wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);

    // 创建互斥量
    xMutex = xSemaphoreCreateMutex();
    if (xMutex == NULL) {
        // 互斥量创建失败
        ESP_LOGE(TAG, "Create mutex failed");
        abort();
    }


    s_uart2tcp_queue = xQueueCreate(UART2TCP_QUEUE_LEN, sizeof(tcp_uart_data_t));
    if (!s_uart2tcp_queue) {
        ESP_LOGE(TAG, "create uart to tcp queue failed");
        abort();
    }

    s_tcp2uart_queue = xQueueCreate(TCP2UART_QUEUE_LEN, sizeof(tcp_uart_data_t));
    if (!s_tcp2uart_queue) {
        ESP_LOGE(TAG, "create tcp to uart queue failed");
        abort();
    }

}
