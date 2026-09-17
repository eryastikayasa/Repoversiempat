#include "websocket_event.h"
#include "websocket_transport.h"
#include "websocket.h"
#include "gemini_protocol.h"
#include "gemini_message.h"
#include "gemini_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
static const char *TAG="WS_EVENT";
static volatile bool s_gemini_ready=false;
static constexpr size_t RX_MAX_PAYLOAD=64*1024;
static constexpr size_t RX_BUFFER_COUNT=3;
static constexpr uint32_t RX_WORKER_STACK=8192;
static constexpr UBaseType_t RX_WORKER_PRIORITY=5;
static QueueHandle_t s_rx_free_queue=nullptr,s_rx_ready_queue=nullptr;
static StaticQueue_t s_rx_free_queue_storage,s_rx_ready_queue_storage;
static char *s_rx_free_storage[RX_BUFFER_COUNT],*s_rx_ready_storage[RX_BUFFER_COUNT],*s_rx_buffers[RX_BUFFER_COUNT]={};
static TaskHandle_t s_rx_worker_task=nullptr; static bool s_rx_worker_ready=false;
static char *s_rx_assembling_buffer=nullptr; static size_t s_rx_expected=0,s_rx_received=0; static bool s_rx_assembling=false;
static bool ensure_rx_worker(void){if(s_rx_worker_ready)return true;s_rx_free_queue=xQueueCreateStatic(RX_BUFFER_COUNT,sizeof(char*),(uint8_t*)s_rx_free_storage,&s_rx_free_queue_storage);s_rx_ready_queue=xQueueCreateStatic(RX_BUFFER_COUNT,sizeof(char*),(uint8_t*)s_rx_ready_storage,&s_rx_ready_queue_storage);if(!s_rx_free_queue||!s_rx_ready_queue)return false;for(size_t i=0;i<RX_BUFFER_COUNT;i++){s_rx_buffers[i]=(char*)heap_caps_malloc(RX_MAX_PAYLOAD+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!s_rx_buffers[i])return false;if(xQueueSend(s_rx_free_queue,&s_rx_buffers[i],0)!=pdPASS)return false;}if(xTaskCreate([](void*){for(;;){char*json=nullptr;if(xQueueReceive(s_rx_ready_queue,&json,portMAX_DELAY)!=pdPASS)continue;if(json){size_t len=strlen(json);gemini_message_type_t type=gemini_message_classify(json,len);bool handled=gemini_protocol_process_message(json,len)||gemini_audio_process_server_message(json,len);if(type==GEMINI_MESSAGE_SETUP){s_gemini_ready=true;ESP_LOGI(TAG,"Gemini setupComplete - audio uplink READY");}(void)handled;xQueueSend(s_rx_free_queue,&json,portMAX_DELAY);}}},"ws_rx",RX_WORKER_STACK,nullptr,RX_WORKER_PRIORITY,&s_rx_worker_task)!=pdPASS)return false;s_rx_worker_ready=true;return true;}
static void reset_rx(void){if(s_rx_assembling_buffer&&s_rx_free_queue)xQueueSend(s_rx_free_queue,&s_rx_assembling_buffer,0);s_rx_assembling_buffer=nullptr;s_rx_expected=s_rx_received=0;s_rx_assembling=false;}
static void handle_data_event(esp_websocket_event_data_t*d){if(!d||!d->data_ptr||d->data_len<=0||d->payload_len<=0)return;size_t total=d->payload_len,off=d->payload_offset,n=d->data_len;if(total>RX_MAX_PAYLOAD||off>total||n>total-off){reset_rx();return;}if(!ensure_rx_worker()){reset_rx();return;}if(off==0){reset_rx();if(xQueueReceive(s_rx_free_queue,&s_rx_assembling_buffer,0)!=pdPASS)return;s_rx_expected=total;s_rx_assembling=true;}else if(!s_rx_assembling||s_rx_expected!=total||off!=s_rx_received){reset_rx();return;}memcpy(s_rx_assembling_buffer+off,d->data_ptr,n);s_rx_received+=n;if(s_rx_received!=s_rx_expected)return;s_rx_assembling_buffer[s_rx_expected]='\0';char*ready=s_rx_assembling_buffer;s_rx_assembling_buffer=nullptr;s_rx_expected=s_rx_received=0;s_rx_assembling=false;if(xQueueSend(s_rx_ready_queue,&ready,0)!=pdPASS)xQueueSend(s_rx_free_queue,&ready,0);}
void websocket_event_handler(void*args,esp_event_base_t base,int32_t event_id,void*event_data){(void)args;(void)base;websocket_transport_handle_event(event_id,event_data);websocket_tx_handle_event(event_id);switch(event_id){case WEBSOCKET_EVENT_CONNECTED:s_gemini_ready=false;reset_rx();ensure_rx_worker();break;case WEBSOCKET_EVENT_DISCONNECTED:case WEBSOCKET_EVENT_ERROR:s_gemini_ready=false;reset_rx();break;case WEBSOCKET_EVENT_DATA:handle_data_event((esp_websocket_event_data_t*)event_data);break;default:break;}}
bool websocket_event_gemini_ready(void){return s_gemini_ready;}
