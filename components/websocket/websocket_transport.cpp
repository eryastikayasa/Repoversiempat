#include "websocket_transport.h"
#include "websocket_event.h"
#include "web_config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
static const char *TAG="WS_TRANSPORT";
static esp_websocket_client_handle_t s_client=nullptr;
static volatile bool s_connected=false;
static bool s_initialized=false;
static constexpr size_t API_KEY_MAX=128;
static constexpr size_t URL_MAX=512;
static bool build_server_url(char *url,size_t url_size){if(!url||!url_size)return false;char api_key[API_KEY_MAX]={0};if(!web_config_load_api_key(api_key,sizeof(api_key))||!web_config_api_key_is_valid(api_key))return false;int written=snprintf(url,url_size,"wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=%s",api_key);memset(api_key,0,sizeof(api_key));return written>0&&(size_t)written<url_size;}
esp_err_t websocket_transport_init(void){if(s_initialized)return ESP_OK;s_client=nullptr;s_connected=false;s_initialized=true;ESP_LOGI(TAG,"Transport WebSocket siap");return ESP_OK;}
esp_err_t websocket_transport_connect(void){if(!s_initialized){esp_err_t e=websocket_transport_init();if(e!=ESP_OK)return e;}if(s_client&&esp_websocket_client_is_connected(s_client)){s_connected=true;return ESP_OK;}if(s_client){esp_websocket_client_destroy(s_client);s_client=nullptr;s_connected=false;}static char url[URL_MAX];if(!build_server_url(url,sizeof(url)))return ESP_ERR_INVALID_ARG;esp_websocket_client_config_t cfg={};cfg.uri=url;cfg.crt_bundle_attach=esp_crt_bundle_attach;cfg.skip_cert_common_name_check=false;cfg.cert_common_name="generativelanguage.googleapis.com";cfg.network_timeout_ms=15000;cfg.disable_auto_reconnect=true;cfg.keep_alive_enable=true;cfg.keep_alive_idle=30;cfg.keep_alive_interval=10;cfg.keep_alive_count=3;cfg.buffer_size=8192;s_client=esp_websocket_client_init(&cfg);if(!s_client)return ESP_FAIL;esp_err_t err=esp_websocket_register_events(s_client,WEBSOCKET_EVENT_ANY,websocket_event_handler,nullptr);if(err!=ESP_OK){esp_websocket_client_destroy(s_client);s_client=nullptr;return err;}err=esp_websocket_client_start(s_client);if(err!=ESP_OK){esp_websocket_client_destroy(s_client);s_client=nullptr;return err;}return ESP_OK;}
esp_err_t websocket_transport_disconnect(void){s_connected=false;if(!s_client)return ESP_OK;return esp_websocket_client_close(s_client,pdMS_TO_TICKS(1000));}
bool websocket_transport_is_connected(void){return s_client&&s_connected&&esp_websocket_client_is_connected(s_client);}
esp_websocket_client_handle_t websocket_transport_get_client(void){return s_client;}
esp_err_t websocket_transport_send_text(const char *text,size_t len){if(!text||!len||len>8192)return ESP_ERR_INVALID_ARG;if(!websocket_transport_is_connected())return ESP_ERR_INVALID_STATE;return esp_websocket_client_send_text(s_client,text,(int)len,pdMS_TO_TICKS(5000))==(int)len?ESP_OK:ESP_FAIL;}
esp_err_t websocket_transport_send_binary(const uint8_t *data,size_t len){if(!data||!len||len>4096)return ESP_ERR_INVALID_ARG;if(!websocket_transport_is_connected())return ESP_ERR_INVALID_STATE;return esp_websocket_client_send_bin(s_client,(const char*)data,(int)len,pdMS_TO_TICKS(5000))==(int)len?ESP_OK:ESP_FAIL;}
void websocket_transport_handle_event(int32_t event_id,void *event_data){(void)event_data;if(event_id==WEBSOCKET_EVENT_CONNECTED){s_connected=true;ESP_LOGI(TAG,"WebSocket CONNECTED");}else if(event_id==WEBSOCKET_EVENT_DISCONNECTED){s_connected=false;ESP_LOGW(TAG,"WebSocket DISCONNECTED");}else if(event_id==WEBSOCKET_EVENT_ERROR){s_connected=false;ESP_LOGE(TAG,"WebSocket ERROR");}}
