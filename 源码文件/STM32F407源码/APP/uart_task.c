#include "uart_task.h"
#include "soft_uart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <string.h>

#define UART_LINE_MAX   128
#define UART_Q_DEPTH    4

static QueueHandle_t s_q = NULL;

/* \u4efb\u52a1\u4f53\uff1a\u4e13\u95e8\u6536\u5b57\u8282\u3001\u62fc\u884c\uff0c\u6ca1\u6709\u4efb\u4f55 LVGL \u8c03\u7528 */
static void uart_task_entry(void *pv)
{
    static char     line[UART_LINE_MAX];
    static uint16_t len;
    uint8_t b;

    (void)pv;

    for (;;)
    {
        if (!suart_read_byte(&b))
        {
            vTaskDelay(pdMS_TO_TICKS(5));      /* \u6ca1\u6570\u636e\u5c31\u8ba9\u51fa CPU */
            continue;
        }

        if (b == '\n' || b == '\r')
        {
            if (len > 0)
            {
                char item[UART_LINE_MAX];

                line[len] = 0;
                strncpy(item, line, UART_LINE_MAX - 1);
                item[UART_LINE_MAX - 1] = 0;

                xQueueSend(s_q, item, pdMS_TO_TICKS(5));   /* \u6ee1\u4e86\u5c31\u4e22 */

                len = 0;
            }
        }
        else if (len < UART_LINE_MAX - 1)
        {
            line[len++] = (char)b;
        }
        else
        {
            len = 0;                            /* \u6ea2\u51fa\uff1a\u4e22\u5f03\u6574\u884c */
        }
    }
}

void uart_task_start(void)
{
    if (s_q) return;

    s_q = xQueueCreate(UART_Q_DEPTH, UART_LINE_MAX);

    if (s_q)
        xTaskCreate(uart_task_entry, "ESP", 512, NULL, 1, NULL);
}

int uart_task_pop(char *out, int max_len)
{
    char item[UART_LINE_MAX];

    if (s_q == NULL) return 0;
    if (xQueueReceive(s_q, item, 0) != pdTRUE) return 0;

    item[UART_LINE_MAX - 1] = 0;
    strncpy(out, item, (size_t)max_len - 1);
    out[max_len - 1] = 0;

    return 1;
}
