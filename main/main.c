/*

MIT No Attribution

Copyright (c) 2020 Mika Tuupola

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

-cut-

SPDX-License-Identifier: MIT-0

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_task_wdt.h>

#include <unistd.h>

#include "sdkconfig.h"

#include <hagl_hal.h>
#include <hagl.h>
#include <bitmap.h>
#include <font6x9.h>
#include <fps.h>
#include <bps.h>
#include <sdcard.h>
#include <tjpgd.h>
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include <sys/dirent.h>
#include <sys/stat.h>
#include "fnt9x18B.h"
#include <ctype.h>
#include <esp_err.h>

static const char *TAG = "main";

static float sd_fps;
static float sd_bps;
static bitmap_t *bb;

static EventGroupHandle_t event;

static const uint8_t FRAME_LOADED = (1 << 0);
static const uint8_t DEV_EOF = (1 << 1);


struct dirent *pDirent;
DIR *pDir;
struct stat _stat;
char cPath[1024];

#define MAX_FILES 30    // maximum number of files to list
// maintain list of up to 10 files

static void *list[MAX_FILES];
int nList = 0;      // 0 -> MAX_FILES
int nFirst = 0;     // first list item displayed
int curList = 0; // currently selected list item

// hard coded for now
#define MIPI_DISPLAY_PIN_BUTTON_A 37
#define MIPI_DISPLAY_PIN_BUTTON_B 38
#define MIPI_DISPLAY_PIN_BUTTON_C 39
#define GPIO_INPUT_IO_0     MIPI_DISPLAY_PIN_BUTTON_A
#define GPIO_INPUT_IO_1     MIPI_DISPLAY_PIN_BUTTON_B
#define GPIO_INPUT_IO_2     MIPI_DISPLAY_PIN_BUTTON_C
#define GPIO_INPUT_PIN_SEL  ((1ULL<<GPIO_INPUT_IO_0) | (1ULL<<GPIO_INPUT_IO_1)| (1ULL<<GPIO_INPUT_IO_2))
#define ESP_INTR_FLAG_DEFAULT 0

static xQueueHandle gpio_evt_queue = NULL;

static TaskHandle_t video_task_handle = NULL;
static TaskHandle_t display_list_handle = NULL;
enum IMGTYPE
{
    NONE,
    MJPG,
    RAW,
    JPG
};
int img_type = NONE;

bool bTerminate = false;

static void display_list();
static void raw_video_task(char *params);
static void mjpg_video_task(char *params);
static void photo_task(char *params);

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void startDisplay()
{
    hagl_clear_screen();
    sprintf(cPath,"/sdcard/%s",(char*)list[curList]);
    if(memcmp((char*)list[curList] + strlen((char*)list[curList])-3, "RAW",3)==0)
        xTaskCreatePinnedToCore(raw_video_task, "Video", 8192, cPath, 1, &video_task_handle, 0);
    else if(memcmp((char*)list[curList] + strlen((char*)list[curList])-3, "MJP",3)==0)
        xTaskCreatePinnedToCore(mjpg_video_task, "Video", 8192, cPath, 1, &video_task_handle, 0);
    else
        xTaskCreatePinnedToCore(photo_task, "Photo", 8192, cPath, 2, &video_task_handle, 0);
}

static void err_display(char * msg)
{
    char16_t message[120];
    hagl_set_clip_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT -1);
    hagl_fill_rectangle(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT -1, hagl_color(250,0,0));
    swprintf(message, sizeof(message), u"%s", msg);
    hagl_put_text(message, (strlen(msg)/2)*9, 120, hagl_color(255,255,255), fnt9x18B);
    /* Notify flush task that frame has been loaded. */
    xEventGroupSetBits(event, FRAME_LOADED);
    vTaskDelete(NULL);
}

static void gpio_task(void* arg)
{
    uint32_t io_num;

    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
            if(io_num == MIPI_DISPLAY_PIN_BUTTON_A && video_task_handle > 0)
            {
                while(video_task_handle)
                {
                    bTerminate = true;
                    vTaskDelay(100/ portTICK_RATE_MS);
                }

                vTaskDelay(100 / portTICK_RATE_MS);
                xTaskCreatePinnedToCore(display_list, "List", 2048, NULL, 1, &display_list_handle, 0);
            }
            else if(io_num == MIPI_DISPLAY_PIN_BUTTON_A && video_task_handle == NULL)
            {
                ESP_LOGI(TAG, "Resume");

                ESP_LOGI(TAG, "select #%d. %s", curList, (char *)list[curList]);


                bTerminate = false;
                startDisplay();
                if(display_list_handle)
                {
                    vTaskDelete(display_list_handle);
                    display_list_handle = NULL;
                }
            }
            if(io_num == MIPI_DISPLAY_PIN_BUTTON_B && display_list_handle)
            {
                // up
                ESP_LOGI(TAG, "nList=%d, nFirst=%d, curList=%d", nList, nFirst, curList);

                if(curList < (nList-1))
                {
                    if((curList - nFirst) >= 9)
                        nFirst++;
                    curList++;
                }
                else
                {
                    curList = 0; // wrap around to beginning
                    nFirst = 0;
                }
            }
            if(io_num == MIPI_DISPLAY_PIN_BUTTON_C && display_list_handle)
            {
                ESP_LOGI(TAG, "nList=%d, nFirst=%d, curList=%d", nList, nFirst, curList);

                // down
                if(curList > 0)
                {
                    if((curList - nFirst) == 0)
                    {
                        nFirst--;
                    }
                    curList--;
                }
                else
                {
                    curList = nList - 1;
                    nFirst = (nList - 10)<0?0:nList-10;
                }
            }
        }
    }
}

static void setup_Gpio()
{
    gpio_config_t io_conf;
    //interrupt of rising edge
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    //change gpio intrrupt type for one pin
    //gpio_set_intr_type(GPIO_INPUT_IO_0, GPIO_INTR_ANYEDGE);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(gpio_task, "gpio_task_example", 2048, NULL, 10, NULL);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);

    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*) GPIO_INPUT_IO_0);
    gpio_isr_handler_add(GPIO_INPUT_IO_1, gpio_isr_handler, (void*) GPIO_INPUT_IO_1);
    gpio_isr_handler_add(GPIO_INPUT_IO_2, gpio_isr_handler, (void*) GPIO_INPUT_IO_2);
}

/*
 * Flush backbuffer to display always when new frame is loaded.
 */
void flush_task(void *params)
{
    ESP_LOGI(TAG, "flush task started");
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            event,
            FRAME_LOADED | DEV_EOF,
            pdTRUE,
            pdFALSE,
            40 / portTICK_PERIOD_MS
        );

        /* Flush only when FRAME_LOADED is set. */
        if ((bits & FRAME_LOADED) != 0 ) {
            hagl_flush();
        }

        if(bits & DEV_EOF)
        {
            while(video_task_handle)
            {
                bTerminate = true;
                vTaskDelay(100/ portTICK_RATE_MS);
            }
            vTaskDelay(100 / portTICK_RATE_MS);
            xTaskCreatePinnedToCore(display_list, "List", 2048, NULL, 1, &display_list_handle, 0);
        }
    }

    vTaskDelete(NULL);
}

/*
 * Read video data from . This should be capped to video
 * framerate. However currently the sd card is the bottleneck and
 * data can be read at only about 15 fps. Adding vsync causes
 * fps to drop to 14.
 */
void raw_video_task(char *params)
{
    FILE *fp;
    ssize_t bytes_read = 0;
    img_type = RAW;
    ESP_LOGI(TAG, "Loading: %s", params);

    //fp = fopen("/sdcard/bbb12.raw", "rb");
    fp = fopen(params, "rb");


    if (!fp) {
        ESP_LOGE(TAG, "Unable to open file!");
    } else {
        ESP_LOGI(TAG, "Successfully opened file.");
    }

    while (!bTerminate) {
        /* https://linux.die.net/man/3/read */
        bytes_read = read(
            fileno(fp),
            /* Center the video on 320 * 240 display */
            bb->buffer + bb->pitch * 30,
            320 * 180 * 2
        );
        if(!bytes_read)
        {
            /* Notify flush task that no more data is present. */
            xEventGroupSetBits(event, DEV_EOF);
            break;
        }

        /* Update counters. */
        sd_bps = bps(bytes_read);
        sd_fps = fps();

        /* Notify flush task that frame has been loaded. */
        xEventGroupSetBits(event, FRAME_LOADED);

        /* Add some leeway for flush so SD card does catch up. */
        ets_delay_us(5000);
    }
    if(fp)
        fclose(fp);
    video_task_handle = NULL;
    vTaskDelete(NULL);
}

/*
 * TJPGD input function
 * http://www.elm-chan.org/fsw/tjpgd/en/input.html
 */

static uint16_t tjpgd_data_reader(JDEC *decoder, uint8_t *buffer, uint16_t size)
{
    FILE *fp = (FILE *)decoder->device;
    uint16_t bytes_read = 0;
    uint16_t bytes_skip = 0;
    const uint16_t EOI = 0xffd9;
    uint8_t *eoi_ptr;

    if (buffer) {
        /* Read bytes from input stream. */
        bytes_read = read(fileno(fp), buffer, size);

        sd_bps = bps(bytes_read);

        /* Search for EOI. */
        eoi_ptr = memmem(buffer, size, &EOI, 2);
        int16_t offset = eoi_ptr - buffer;
        int16_t rewind = offset - size + 1;

        if (eoi_ptr) {
            ESP_LOGD(TAG, "EOI found at offset: %d", offset);
            ESP_LOGD(TAG, "Rewind %d bytes", rewind);

            lseek(fileno(fp), rewind, SEEK_CUR);
            bytes_read += rewind;
        }

        ESP_LOGD(TAG, "Read %d bytes", bytes_read);
        return bytes_read;
    } else {
        /* Skip bytes from input stream. */
        bytes_skip = 0;
        if (lseek(fileno(fp), size, SEEK_CUR) > 0) {
            bytes_skip = size;
        }

        ESP_LOGD(TAG, "Skipped %d bytes", bytes_read);
        return bytes_skip;
    }
}

/*
 * TJPGD output function
 * http://www.elm-chan.org/fsw/tjpgd/en/output.html
 */
static uint16_t tjpgd_data_writer(JDEC* decoder, void* bitmap, JRECT* rectangle)
{
    uint8_t width = (rectangle->right - rectangle->left) + 1;
    uint8_t height = (rectangle->bottom - rectangle->top) + 1;

    /* Create a HAGL bitmap from uncompressed block. */
    bitmap_t block = {
        .width = width,
        .height = height,
        .depth = DISPLAY_DEPTH,
    };

    bitmap_init(&block, (uint8_t *)bitmap);

    /* Blit the block to the display. */
    hagl_blit(rectangle->left, rectangle->top + 30, &block);

    return 1;
}

/*
 * Read video data from sdcard. This should be capped to video
 * framerate. However currently the ESP32 is the bottleneck and
 * data can be uncompressed at about 8 fps.
 */
void mjpg_video_task(char *params)
{
    FILE *fp;
    uint8_t work[3100];
    JDEC decoder;
    JRESULT result;
    img_type = MJPG;
    ESP_LOGI(TAG, "Loading: %s", params);

    //fp = fopen("/sdcard/bbb08.mjp", "rb");
    fp = fopen(params, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Unable to open file! %s", params);
    } else {
        ESP_LOGI(TAG, "Successfully opened file.");
    }

    while (!bTerminate) {
        result = jd_prepare(&decoder, tjpgd_data_reader, work, 3100, fp);
        if (result == JDR_OK) {
            result = jd_decomp(&decoder, tjpgd_data_writer, 0);
            if (JDR_OK != result) {
                ESP_LOGE(TAG, "TJPGD decompress failed. err %d", result);
            }
        } else {
            if(result == JDR_INP)
            {
                /* Notify flush task that no more data is present. */
                xEventGroupSetBits(event, DEV_EOF);
                break;
            }
            ESP_LOGE(TAG, "TJPGD prepare failed. err %d", result);
        }

        /* Update counters. */
        sd_fps = fps();

        /* Notify flush task that frame has been loaded. */
        xEventGroupSetBits(event, FRAME_LOADED);

        /* Add some leeway for flush so SD card does catch up. */
        ets_delay_us(5000);
    }
    if(fp)
        close(fp);
    video_task_handle = NULL;
    vTaskDelete(NULL);
}

/*
 * Displays the info bar on top of the screen.
 */
void infobar_task(void *params)
{
    uint16_t color = hagl_color(0, 255, 0);
    char16_t message[64];

#ifdef HAGL_HAL_USE_BUFFERING
    while (1) {
        hagl_fill_rectangle(0, 0, DISPLAY_WIDTH - 1, 8, hagl_color(0,0,0));
        swprintf(message, sizeof(message), u"SD %.*f kBPS  ",  1, sd_bps / 1000);
        hagl_put_text(message, 8, 8, color, font6x9);

        switch(img_type)
        {
        case MJPG:
            swprintf(message, sizeof(message), u"MJPG %.*f FPS  ", 1, sd_fps);
            hagl_put_text(message, DISPLAY_WIDTH - 90, 8, color, font6x9);
            break;
        case RAW:
            swprintf(message, sizeof(message), u"RAW RGB565 %.*f FPS  ", 1, sd_fps);
            hagl_put_text(message, DISPLAY_WIDTH - 124, 8, color, font6x9);
            break;
        case JPG:
            swprintf(message, sizeof(message), u"JPG %.*f FPS  ", 1, sd_fps);
            hagl_put_text(message, DISPLAY_WIDTH - 124, 8, color, font6x9);
            break;
        default:
            swprintf(message, sizeof(message), u"None %.*f FPS  ", 1, sd_fps);
            hagl_put_text(message, DISPLAY_WIDTH - 124, 8, color, font6x9);
            break;
        }

        vTaskDelay(1000 / portTICK_RATE_MS);
    }
#endif
    vTaskDelete(NULL);
}

void photo_task(char *params)
{
    img_type = JPG;
    char16_t message[64];
    while (!bTerminate) {
        ESP_LOGI(TAG, "Loading: %s", params);
        uint32_t status = hagl_load_image(0, 30, params);
        if(status)
            ESP_LOGE(TAG, "error %d reading %s", status, params);
        swprintf(message, sizeof(message), u"testing text over img");
        hagl_put_text(message, 10, 120, hagl_color(0, 0, 255), fnt9x18B);
        xEventGroupSetBits(event, FRAME_LOADED);

        vTaskDelay(1000 / portTICK_RATE_MS);
        //break;
#if 0
        ESP_LOGI(TAG, "Loading: %s", "/sdcard/002.jpg");
        hagl_load_image(0, 30, "/sdcard/002.jpg");
        xEventGroupSetBits(event, FRAME_LOADED);
        vTaskDelay(1000 / portTICK_RATE_MS);

        ESP_LOGI(TAG, "Loading: %s", "/sdcard/003.jpg");
        hagl_load_image(0, 30, "/sdcard/003.jpg");
        xEventGroupSetBits(event, FRAME_LOADED);
        vTaskDelay(1000 / portTICK_RATE_MS);

        ESP_LOGI(TAG, "Loading: %s", "/sdcard/004.jpg");
        hagl_load_image(0, 30, "/sdcard/004.jpg");
        vTaskDelay(1000 / portTICK_RATE_MS);
#endif
    }
    video_task_handle = NULL;
    vTaskDelete(NULL);
}

void display_list()
{
    char16_t message[64];
    hagl_clear_screen();
    bool bFirst=true;

    ESP_LOGI(TAG, "Begin display_list");

    while(1)
    {
        int y = 20;
        int max = (nList - nFirst)< 10?nList-nList:(nFirst+10); //displaying only 10 lines
        if(bFirst)
        {
            ESP_LOGI(TAG, "nList=%d, nFirst=%d, max=%d", nList, nFirst, max);
            bFirst = false;
        }
        for(int i=nFirst; i < max; i++)
        {
            swprintf(message, sizeof(message), u"%s", (char*)list[i]);
            if(curList == i)
                hagl_put_text(message, 5, y, hagl_color(255, 0, 255), fnt9x18B);
            else
                hagl_put_text(message, 5, y, hagl_color(0, 255, 0), fnt9x18B);
            y += 20;
        }
        /* Notify flush task that frame has been loaded. */
        xEventGroupSetBits(event, FRAME_LOADED);

        vTaskDelay(1000 / portTICK_RATE_MS);
    }
    vTaskDelete(NULL);
}

void app_main()
{
    ESP_LOGI(TAG, "SDK version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Heap when starting: %d", esp_get_free_heap_size());

    setup_Gpio();

    event = xEventGroupCreate();

    /* Save the backbuffer pointer so we can later read() directly into it. */
    bb = hagl_init();
    if (bb) {
        ESP_LOGI(TAG, "Back buffer: %dx%dx%d", bb->width, bb->height, bb->depth);
    }

    esp_err_t status = sdcard_init();
    if(status != ESP_OK)
    {
        xTaskCreatePinnedToCore(flush_task, "Flush", 8192, NULL, 1, NULL, 0);
        xTaskCreatePinnedToCore(err_display, "error", 8192, "Error opening SD card", 1, NULL, 0);
        return;
    }

    pDir = opendir("/sdcard");
    if (pDir == NULL) {
        printf ("Cannot open directory '/sdcard'\n");
        return;
    }
    while ((pDirent = readdir(pDir)) != NULL) {
        sprintf(cPath,"/sdcard/%s",pDirent->d_name);
        stat(cPath,&_stat);
        if(S_ISDIR(_stat.st_mode)) {
            printf ("[%s] DIR %ld\n", pDirent->d_name,_stat.st_size);
        } else {
            printf ("[%s] FILE %ld\n", pDirent->d_name,_stat.st_size);
            int len = strlen(pDirent->d_name);
            if(strncmp(pDirent->d_name+len-3, "MJP",3) ==0 || strncmp(pDirent->d_name+len-3, "RAW",3) ==0
                    || strncmp(pDirent->d_name+len-3, "JPG",3) ==0)
            {
                if(nList < MAX_FILES)
                {
                    list[nList] = (char*)malloc(len +1);
                    strcpy(list[nList], pDirent->d_name);
//                    char* p = (char*)list[nList];
//                    for ( ; *p; ++p) *p = tolower(*p);
                    nList++;
                }
            }
        }
    }
    closedir (pDir);
    for(int i=0; i < nList; i++)
    {
        printf("%d. %s\n", i, (char*)list[i]);
    }

    ESP_LOGI(TAG, "Heap after init: %d", esp_get_free_heap_size());

#ifdef HAGL_HAL_USE_BUFFERING
    xTaskCreatePinnedToCore(flush_task, "Flush", 8192, NULL, 1, NULL, 0);
    startDisplay();
#endif /* HAGL_HAL_USE_BUFFERING */
    xTaskCreatePinnedToCore(infobar_task, "info", 8192, NULL, 2, NULL, 1);
}
