// #include <stdio.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "nvs_flash.h"
// #include "driver/gpio.h"
// // Bluetooth Includes
// #include "esp_bt.h"
// #include "esp_bt_main.h"
// #include "esp_bt_device.h"
// #include "esp_gap_bt_api.h"
// #include "esp_hidh_api.h" // HID API
// #include "esp_log.h"
// // BLE
// #include "esp_gap_ble_api.h"
// #include "esp_gattc_api.h"
// #include "esp_gatts_api.h"
// // Bluetooth Includes END
// #include "string.h"
// #include "esp_crc.h"


// #define LED_PIN 5
// #define TAG "Log"

// #define MAX_DEVICES 10
// #define INVALID_RSSI -127
// #define INVALID_DEVICE_INDEX -1
// #define DUALSENSE_REPORT_LEN 78
// #define ADV_DATA_LEN 31
// #define RSSI_QUERY_RANGE -60
// #define BT_ADDR_STR_LEN 18

// int REPEATING_DEVICE_FLAG = 0;
// int REPEATING_DEVICE_ADDR = INVALID_DEVICE_INDEX;
// esp_bd_addr_t CONNECTED_DEVICE_BT_ADDR[ESP_BD_ADDR_LEN];

// TaskHandle_t HID_set_protocol_task_handle = NULL;
// TaskHandle_t HID_report_mode_task_handle = NULL;

// // Report Initialization for Dualsense
// uint8_t DUALSENSE_REPORT[DUALSENSE_REPORT_LEN] = {0};
// void dualsense_report_init(){
//     DUALSENSE_REPORT[0] = 0x31;   // Report ID
//     DUALSENSE_REPORT[1] = 0x02;   // Flags: enable motors + LEDs
//     DUALSENSE_REPORT[4] = 0x80;   // Right motor (low freq)
//     DUALSENSE_REPORT[5] = 0x40;   // Left motor (high freq)
//     DUALSENSE_REPORT[45] = 0xFF;  // Lightbar Red
//     DUALSENSE_REPORT[46] = 0x00;  // Lightbar Green
//     DUALSENSE_REPORT[47] = 0x00;  // Lightbar Blue
// }
// // Report Initialization

// typedef struct {
//     esp_bd_addr_t addr;
//     int rssi;
//     uint8_t name[249];
//     uint8_t adv_data[ADV_DATA_LEN];
//     uint8_t adv_data_len;
// } ble_device_t;

// ble_device_t BLE_device_list[MAX_DEVICES];
// int BLE_device_count = 0;

// typedef struct {
//     esp_bd_addr_t addr;
//     int rssi;
//     uint8_t name[249];
// } bt_device_t;

// bt_device_t BT_device_list[MAX_DEVICES];
// int BT_device_count = 0;

// void wait_ms(uint16_t ms){
//     vTaskDelay(pdMS_TO_TICKS(ms));
// }

// void printer(const char* word){
//     printf(word);
//     fflush(stdout);
// }

// void printer_esp(const char* word) {
//     ESP_LOGI("Log", "%s", word);
// }

// void blink_LED(uint8_t amount){
//     for (uint8_t i = 0; i < amount; ++i){
//         vTaskDelay(pdMS_TO_TICKS(1000));
//         gpio_set_level(LED_PIN, 1);
//         vTaskDelay(pdMS_TO_TICKS(1000));
//         gpio_set_level(LED_PIN, 0);
//     }
// }

// void stringify_BT_address(char* dest, esp_bd_addr_t addr){
//     sprintf(dest, ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(addr));
//     dest[BT_ADDR_STR_LEN] = '\0';
// }

// /* Checks if a BT address is in device list
//    Returns index else -1                 */
// int _check_bleaddr_exists(esp_bd_addr_t addr){
//     for (int i=0; i < MAX_DEVICES; ++i){
//         if (BLE_device_list[i].rssi == INVALID_RSSI) {
//             continue; // skip unused slots
//         }
//         if (memcmp(BLE_device_list[i].addr, addr, ESP_BD_ADDR_LEN) == 0) {
//             return i; // found match
//         }
//     }
//     return INVALID_DEVICE_INDEX;
// }

// int _check_btaddr_exists(esp_bd_addr_t addr){
//     for (int i=0; i < MAX_DEVICES; ++i){
//         if (BT_device_list[i].rssi == INVALID_RSSI) {
//             continue; // skip unused slots
//         }
//         if (memcmp(BT_device_list[i].addr, addr, ESP_BD_ADDR_LEN) == 0) {
//             return i; // found match
//         }
//     }
//     return INVALID_DEVICE_INDEX;
// }

// void _callback_display_scan_device(esp_ble_gap_cb_param_t *param){

//     // Ensure event is device
//     if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT || param->scan_rst.rssi < -60) return;

//     // Make string BT address
//     char bt_str[30]; stringify_BT_address(bt_str, param->scan_rst.bda);

//     // Check If Device exists already in list
//     int check_index = _check_bleaddr_exists(param->scan_rst.bda);
//         //printf("Index is [%d] For addr: %s\n", check_index, bt_str);

//     // Get Device Name
//     uint8_t name_len = 0;
//     char name[64] = "\0";
//     uint8_t* eir = param->scan_rst.ble_adv;
//     uint8_t* scan_ptr = eir + param->scan_rst.adv_data_len;
//     // Get Complete Name
//     uint8_t *adv_name = esp_ble_resolve_adv_data_by_type(eir, param->scan_rst.adv_data_len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
//     // Else Try Short Name
//     if (!adv_name) adv_name = esp_ble_resolve_adv_data_by_type(eir, param->scan_rst.adv_data_len, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
//     // Try Scan Packet
//     if (!adv_name && param->scan_rst.scan_rsp_len > 0) 
//         esp_ble_resolve_adv_data_by_type(scan_ptr, param->scan_rst.adv_data_len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
//     // Try Short Name
//     if (!adv_name)
//         esp_ble_resolve_adv_data_by_type(scan_ptr, param->scan_rst.adv_data_len, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    
//     if (adv_name) {
//         strcat((char*)adv_name, "\0");
//         printf("Got Name: %s\n", adv_name);
//     }
//     // If name exists give it to name var
//     if (adv_name && name_len > 0) memcpy(name, adv_name, name_len);

//     // If New Device
//     if (check_index == INVALID_DEVICE_INDEX){
//         printf("ADV raw: ");
//         for (int i = 0; i < param->scan_rst.adv_data_len; i++) {
//             printf("%02X ", param->scan_rst.ble_adv[i]);
//         }
//         printf("\n"); fflush(stdout);

//         // Check Max Devices
//         if (BLE_device_count >= MAX_DEVICES || param->scan_rst.rssi < -60) return;
//         // Index
//         int index = BLE_device_count;
//         // Add Device to device_list
//         // Copy Name
//         strcpy((char*)BLE_device_list[index].name, name);
//         // Copy BT Addr
//         memcpy(BLE_device_list[index].addr, param->scan_rst.bda, ESP_BD_ADDR_LEN);
//         // Copy advanced data
//         mempcpy(BLE_device_list[index].adv_data, param->scan_rst.ble_adv, param->scan_rst.adv_data_len);
//         BLE_device_list[index].adv_data_len = param->scan_rst.adv_data_len;
//         // Copy RSSI
//         BLE_device_list[index].rssi = param->scan_rst.rssi;
//         // Add to Count
//         BLE_device_count++;
//         // Print Device
//         char* dis_name = (name[0] == '\0') ? "Unknown": name;
//         printf("%s - %s - RSSI: %d\n", bt_str, dis_name, param->scan_rst.rssi); fflush(stdout);
//     }
//     // If Existing Device & name different
//     else if (strcmp((char*)BLE_device_list[check_index].name, name) != 0){
//         strcpy((char*)BLE_device_list[check_index].name, name);
//         printf("%s - %s - RSSI: %d - UPDATED\n", bt_str, name, param->scan_rst.rssi);
//     }
// }

// void BLE_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param){
//     switch (event){

//         // When scan parameters set complete
//         case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
//             if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS)
//                 printer_esp("Scanning parameters Successfuly SET");
//             else
//                 ESP_LOGI(TAG, "Setting Scan Parameter failed code: %d", param->scan_param_cmpl.status);
//         break;

//         // When ONE scan result ready
//         case ESP_GAP_BLE_SCAN_RESULT_EVT:
//             _callback_display_scan_device(param);
//         break;

//         // When advertising data set complete
//         case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
//             // TODO
//         break;
        
//         // When scan started
//         case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
//         printf("--------------SCANNING FOR DEVICES--------------\n"); fflush(stdout);
//         break;

//         // When stop scan complete
//         case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
//         printf("----------SCANNING STOPPED FOR DEVICES----------\n");
//         printf("FOUND [%d] Devices\n", BLE_device_count); fflush(stdout);
//         break;

//         // BLE security request - Pairing requested essentially
//         case ESP_GAP_BLE_SEC_REQ_EVT:
//             // TODO
//         break;

//         // Pairing has completed and device in flash
//         case ESP_GAP_BLE_AUTH_CMPL_EVT:
//             // TODO
//         break;

//         default:
//         ESP_LOGI(TAG, "In default for event not made\n");
//     }
// }



// void BLE_start_scanning(uint32_t secs){

//     esp_ble_scan_params_t scan_params = {
//         .scan_type = BLE_SCAN_TYPE_ACTIVE,
//         .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
//         .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
//         .scan_interval = 0x50,
//         .scan_window = 0x50,
//         .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
//     };
//     esp_ble_gap_set_scan_params(&scan_params);
//     esp_ble_gap_start_scanning(0);
//     wait_ms(secs * 1000);
//     esp_ble_gap_stop_scanning();
// }

// void callback_handle_discovery_changed(esp_bt_gap_cb_param_t* params){
//     if (params->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED)
//         printf("----------BLUETOOTH Discovery Started...----------\n");
//     else
//         printf("----------BLUETOOTH Discovery Stopped...----------\n");
//     fflush(stdout);
// }

// /*True if addr invalid(All Zeros), false if not*/
// bool _query_addr_invalid(esp_bd_addr_t addr){
//     for (uint8_t i=0; i < ESP_BD_ADDR_LEN; ++i){
//         if (addr[i] != 0) return false;
//     }
//     return true;
// }

// void callback_handle_discovery_result(esp_bt_gap_cb_param_t* params){

//     char bt_str[BT_ADDR_STR_LEN];
//     stringify_BT_address(bt_str, params->disc_res.bda);

//     // Check If device exists
//     int existing_index = _check_btaddr_exists(params->disc_res.bda);

//     if (existing_index != INVALID_DEVICE_INDEX){
//         if (BT_device_list[existing_index].name[0] == '\0'){
//             /* printf("%02X:%02X:%02X:%02X:%02X:%02X - ",
//                 params->disc_res.bda[0], params->disc_res.bda[1], params->disc_res.bda[2],
//                 params->disc_res.bda[3], params->disc_res.bda[4], params->disc_res.bda[5]);
//             printf("Already Exists.... at INDEX: %d - DevCount: %d\n", existing_index, BT_device_count); fflush(stdout); */
//             esp_bt_gap_read_remote_name(BT_device_list[existing_index].addr);
//         }
//         return;
//     }
//     // Check If Max Devices Reached
//     if (BT_device_count >= MAX_DEVICES) return;

//     // Continue getting values for new device
//     // Get rssi
//     int rssi_val = -128;
//     for (uint8_t i=0; i < params->disc_res.num_prop; ++i){
//         if (params->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_RSSI)
//             rssi_val = *(int8_t*)params->disc_res.prop[i].val;
//     }
//     BT_device_list[BT_device_count].rssi = rssi_val;

//     // Copy Bluetooth Address
//     for (int i=0; i < ESP_BD_ADDR_LEN; ++i)
//         BT_device_list[BT_device_count].addr[i] = params->disc_res.bda[i];

//     // Get name & print in ESP_BT_GAP_READ_REMOTE_NAME_EVT event
//     if (BT_device_count < MAX_DEVICES) BT_device_count++;
//     esp_bt_gap_read_remote_name(BT_device_list[BT_device_count].addr);
// }

// void callback_handle_read_remote_name(esp_bt_gap_cb_param_t* params){

//     // Get index, will always exist if this function is called.
//     int device_index = _check_btaddr_exists(params->read_rmt_name.bda);
//     if (device_index == INVALID_DEVICE_INDEX) return;

//     // Display contents
//     strcpy((char*)BT_device_list[device_index].name, (char*)params->read_rmt_name.rmt_name);
//     char* display_name = (BT_device_list[device_index].name[0] == '\0') ? "Unknown": (char*)BT_device_list[device_index].name;

//     printf("%02X:%02X:%02X:%02X:%02X:%02X - ",
//            BT_device_list[device_index].addr[0], BT_device_list[device_index].addr[1], BT_device_list[device_index].addr[2],
//            BT_device_list[device_index].addr[3], BT_device_list[device_index].addr[4], BT_device_list[device_index].addr[5]);
//     printf("%s - RSSI: %d - Device Index: %d\n", display_name, BT_device_list[device_index].rssi, device_index);
// }

// void callback_BTclassic(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* params){
//     switch (event){
//         // Device Discovery Result Event - DEVICES LISTED HERE
//         case ESP_BT_GAP_DISC_RES_EVT:
//         callback_handle_discovery_result(params);
//         break;

//         // Discovery State Changed Event - WHEN DISCOVERING OR NOT
//         case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
//         callback_handle_discovery_changed(params);
//         break;

//         // Get Remote Services Event
//         case ESP_BT_GAP_RMT_SRVCS_EVT:
//             // TODO
//         break;

//         // Get remote service record event
//         case ESP_BT_GAP_RMT_SRVC_REC_EVT:
//             // TODO
//         break;

//         // Authentication complete event
//         case ESP_BT_GAP_AUTH_CMPL_EVT:
//         printer_esp("ESP_BT_GAP_AUTH_CMPL_EVT Fired!");
//         printf("----------BLUETOOTH Pairing Started...----------\n"); fflush(stdout);
//         break;

//         // Legacy Pairing Pin code request
//         case ESP_BT_GAP_PIN_REQ_EVT:
//         printer_esp("ESP_BT_GAP_PIN_REQ_EVT Fired!");
//             // TODO
//         break;

//         // Security Simple Pairing User Confirmation request
//         case ESP_BT_GAP_CFM_REQ_EVT:
//         printer_esp("ESP_BT_GAP_CFM_REQ_EVT Fired!");
//             // TODO
//         break;

//         // Security Simple Pairing Passkey Notification
//         case ESP_BT_GAP_KEY_NOTIF_EVT:
//         printer_esp("ESP_BT_GAP_KEY_NOTIF_EVT Fired!");
//             // TODO
//         break;

//         // Security Simple Pairing Passkey Request
//         case ESP_BT_GAP_KEY_REQ_EVT:
//             // TODO
//         break;

//         // Read rssi event - Reads SIGNAL STRENGTH - 30 dBm good - 90 dBm weak
//         case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
//             // TODO
//         break;

//         // Config EIR data event (Extended Inquiry Response) - Gives extra information
//         case ESP_BT_GAP_CONFIG_EIR_DATA_EVT:
//             // TODO
//         break;

//         // Set AFH channels event - DO NOT TOUCH, handled by controller - dynamically avoids bad channels (noisy)
//         case ESP_BT_GAP_SET_AFH_CHANNELS_EVT:
//             // TODO - NOT DO
//         break;
        
//         // Reads the name of the device - IMPORTANT                                                 ---------------------------------------
//         case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
//             callback_handle_read_remote_name(params);
//         break;

//         // Tells when baseband link mode (active/sniff/hold/park) with a remote device has changed
//         case ESP_BT_GAP_MODE_CHG_EVT:
//             // TODO
//         break;

//         // Remove bond device complete event - Confirmation when device unpaired
//         case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
//             // TODO
//         break;

//         // Fires when QOS (Quality of Service) change, how baseband schedules traffic, E.g Audio for guaranteeing timely packet delivery
//         case ESP_BT_GAP_QOS_CMPL_EVT:
//             // TODO
//         break;

//         // ACL connection complete status event - Fires when paired successfully
//         case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
//             // TODO
//         break;

//         // ACL disconnection complete status event - Fires when disconnected successfully
//         case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
//             // TODO
//         break;

//         // Set page timeout event - Request to configure timeout acknowledgement - print statement
//         case ESP_BT_GAP_SET_PAGE_TO_EVT:
//             // TODO
//         break;

//         // Get page timeout event value 
//         case ESP_BT_GAP_GET_PAGE_TO_EVT:
//             // TODO
//         break;

//         // Set ACL packet types event
//         case ESP_BT_GAP_ACL_PKT_TYPE_CHANGED_EVT:
//             // TODO
//         break;

//         // Encryption change event
//         case ESP_BT_GAP_ENC_CHG_EVT:
//             // TODO
//         break;

//         // Set minimum encryption key size
//         case ESP_BT_GAP_SET_MIN_ENC_KEY_SIZE_EVT:
//             // TODO
//         break;

//         // Get device name complete event
//         case ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT:
//             // TODO
//         break;

//         //
//         case ESP_BT_GAP_EVT_MAX:
//             // TODO
//         break;
//     };
// }

// esp_err_t BT_control_discovery(bool enable){
//     esp_err_t discovery;
//     if (enable)
//         discovery = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, ESP_BT_GAP_MAX_INQ_LEN, 0);
//     else
//         discovery = esp_bt_gap_cancel_discovery();
//     return discovery;
// }

// /*
//   Wrapper for BT_control_discovery
//   Displays all devices and can set time to search
//   Shows count of all devices found.
// */
// void BT_discover_devices(int ms_wait){
//     BT_control_discovery(true);
//     wait_ms(ms_wait);
//     BT_control_discovery(false);
//     printf("Found Devices Count: %d\n", BT_device_count);
//     fflush(stdout);
// }

// /* Finds Device by Name, returns INVALID_DEVICE_INDEX (-1) if cannot find index*/
// int find_device_by_name(char* name){
//     for (int i=0; i < MAX_DEVICES; ++i){
//         if (strcmp(name, (char*)BT_device_list[i].name) == 0)
//             return i;
//     }
//     return INVALID_DEVICE_INDEX;
// }

// bool BT_pair_device(char* name){
//     int index = find_device_by_name(name);
//     if (index == INVALID_DEVICE_INDEX) return false;
//     esp_err_t res = esp_bt_hid_host_connect(BT_device_list[index].addr);
//     if (res == ESP_FAIL) return false;
//     return true;
// }

// void BT_list_bonded_devices(){
//     int dev_num = esp_bt_gap_get_bond_device_num();
//     esp_bd_addr_t paired_list[dev_num];
//     esp_bt_gap_get_bond_device_list(&dev_num, paired_list);

//     printf("----------------Printing Bonded Devices---------------\n");
//     for (int i=0; i < dev_num; ++i){
//         printf("Bonded Device at index [%d] - %02X:%02X:%02X:%02X:%02X:%02X\n", i,
//            paired_list[i][0], paired_list[i][1], paired_list[i][2],
//            paired_list[i][3], paired_list[i][4], paired_list[i][5]);
//         fflush(stdout);
//     }
//     printf("----------------Printing Bonded Finished---------------\n"); fflush(stdout);
// }

// void HID_set_protocol_task(void *arg) {
//     while (1) {
//         ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // wait for signal
//         printf("Sending the Protocol\n");
//         esp_bt_hid_host_set_protocol((uint8_t*)CONNECTED_DEVICE_BT_ADDR, ESP_HIDH_REPORT_MODE);
//     }
// }

// void create_init_report(uint8_t* init_report){

//     init_report[0] = 0x02;  // Report ID
//     init_report[1] = 0x80;  // Subcommand: enable input streaming
//     init_report[2] = 0x01;  // Flags (optional LED/rumble enable)

//     // Fill rumble / trigger bytes with zeros
//     init_report[3] = 0x00;  // Weak motor
//     init_report[4] = 0x00;  // Strong motor

//     // Compute CRC over first 74 bytes
//     uint32_t crc = esp_crc32_le(0, init_report, 74);
//     init_report[74] = (crc >> 0) & 0xFF;
//     init_report[75] = (crc >> 8) & 0xFF;
//     init_report[76] = (crc >> 16) & 0xFF;
//     init_report[77] = (crc >> 24) & 0xFF;
// }

// void report_task(void *arg) {
//     while (1) {
//         // Wait for notification from OPEN_EVT
//         ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

//         //wait_ms(20);

//         printer_esp("Sending Report...");


//         uint8_t init_report[78] = {0};
//         create_init_report(init_report);
//         esp_err_t err = esp_bt_hid_host_set_report((uint8_t*)CONNECTED_DEVICE_BT_ADDR, ESP_HIDH_REPORT_TYPE_OUTPUT, init_report, sizeof(init_report));
//         if (err == ESP_OK) printer_esp("Sent Reports OK");
//         else ESP_LOGI(TAG, "Report Did NOT send OK: %d", err);

//         uint8_t report[78] = {0};
//         report[0] = 0x02;
//         report[1] = 0xC0;
//         report[2] = 0x07;   // More flags (LED/rumble/mute enable)
//         // Rumble
//         report[3] = 0x00; // weak
//         report[4] = 0x0F; // strong
//         // Lightbar RGB
//         report[44] = 0x00;   // Red
//         report[45] = 0xFF;   // Green
//         report[46] = 0x00;   // Blue
//         // Compute CRC
//         uint32_t crc = esp_crc32_le(0, report, 74);
//         report[74] = (crc >> 0) & 0xFF;
//         report[75] = (crc >> 8) & 0xFF;
//         report[76] = (crc >> 16) & 0xFF;
//         report[77] = (crc >> 24) & 0xFF;

//         err = esp_bt_hid_host_set_report((uint8_t*)CONNECTED_DEVICE_BT_ADDR, ESP_HIDH_REPORT_TYPE_OUTPUT, report, sizeof(report));
//         if (err == ESP_OK) printer_esp("Sent Reports OK");
//         else ESP_LOGI(TAG, "Report Did NOT send OK: %d", err);
        
//     }
// }

// bool IS_REPORT_SENT = false;
// bool REPORT_QUEUED = false;

// void callback_BT_HIDcontroller(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param){
//     switch(event){
//         // When HID host is initialized, the event comes
//         case ESP_HIDH_INIT_EVT:
//             // TODO
//         break;

//         // When HID host is deinitialized, the event comes
//         case ESP_HIDH_DEINIT_EVT:
//             // TODO
//         break;

//         // When HID host connection opened, the event comes
//         case ESP_HIDH_OPEN_EVT:
//         printer_esp("Device Connection Opened");
//         if (param->open.status == ESP_HIDH_OK) printf("STATUS OK\n");
//         else printf("STATUS NOT OK\n");
//         fflush(stdout);
//         memcpy(CONNECTED_DEVICE_BT_ADDR, param->open.bd_addr, ESP_BD_ADDR_LEN);

//         // Send Task for Report Sending
//         xTaskNotifyGive(HID_report_mode_task_handle);
//         REPORT_QUEUED = true;
//         break;

//         // When HID host connection closed, the event comes
//         case ESP_HIDH_CLOSE_EVT:
//         printer_esp("Device Connection Closed");
//         break;

//         // When Get_Report command is called, the event comes
//         case ESP_HIDH_GET_RPT_EVT:
//             // TODO
//         break;

//         // When Set_Report command is called, the event comes
//         case ESP_HIDH_SET_RPT_EVT:
//             printer_esp("Got HERE to set Report");
//             IS_REPORT_SENT = true;
//             // TODO
//         break;

//         // When Get_Protocol command is called, the event comes
//         case ESP_HIDH_GET_PROTO_EVT:
//             char* type;
//             if (param->get_proto.proto_mode == ESP_HIDH_BOOT_MODE)
//                 type = "BOOT MODE";
//             else if (param->get_proto.proto_mode == ESP_HIDH_REPORT_MODE)
//                 type = "REPORT MODE";
//             else
//                 type = "UNSUPPORTED";

//             ESP_LOGI(TAG, "Protocol currently is: %s", type);
//         break;

//         // When Set_Protocol command is called, the event comes
//         case ESP_HIDH_SET_PROTO_EVT:
//             ESP_LOGI(TAG, "Protocol was SET");
//             if (param->set_proto.status == ESP_HIDH_HS_TRANS_NOT_SPT){
//                 ESP_LOGI(TAG, "Protocol NOT SUPPORTED");
//             }
//             else if (param->set_proto.status != ESP_HIDH_OK) ESP_LOGI(TAG, "Protocol NOT SET...???");
//             ESP_LOGI(TAG, "STATUS #: %d", param->set_proto.status);
//         break;

//         // When Get_Idle command is called, the event comes
//         case ESP_HIDH_GET_IDLE_EVT:
//             // TODO
//         break;

//         // When Set_Idle command is called, the event comes
//         case ESP_HIDH_SET_IDLE_EVT:
//             // TODO
//         break;

//         // When HIDH is initialized, the event comes
//         case ESP_HIDH_GET_DSCP_EVT:
//             // TODO
//         break;

//         // When a device is added, the event comes
//         case ESP_HIDH_ADD_DEV_EVT:
//             printer_esp("Device Added...");
//         break;

//         // When a device is removed, the event comes
//         case ESP_HIDH_RMV_DEV_EVT:
//             printer_esp("Device was Removed...");
//             // TODO
//         break;

//         // When virtually unplugged, the event comes
//         case ESP_HIDH_VC_UNPLUG_EVT:
//             // TODO
//         break;

//         // When send data on interrupt channel, the event comes
//         case ESP_HIDH_DATA_EVT:
//             // TODO
//         break;

//         // When receive data on interrupt channel, the event comes
//         case ESP_HIDH_DATA_IND_EVT:
        
//             if (!IS_REPORT_SENT && !REPORT_QUEUED){
//                 /* // Send Task for Report Sending
//                 xTaskNotifyGive(HID_report_mode_task_handle);
//                 REPORT_QUEUED = true; */
//             }
//             if (IS_REPORT_SENT){
//                 char buffer[100];
//                 snprintf(buffer, 100, "RECIEVED DATA FROM: [%d] - ", param->data_ind.handle);
//                 for (int i=0; i < param->data_ind.len; ++i){
//                     char temp[50];
//                     snprintf(temp, sizeof(temp), "%u - ", param->data_ind.data[i]);
//                     strcat(buffer, temp);
//                 }
//                 ESP_LOGI(TAG, "%s\n", buffer);
//             }
//         break;

//         // When set the HID device descriptor, the event comes
//         case ESP_HIDH_SET_INFO_EVT:
//             // TODO
//         break;
//     }
   
// }

// void app_main() {
//     // Initialize Devices
//     for (int i=0; i < MAX_DEVICES; ++i){
//         ble_device_t placeholder = {.addr = {0, 0, 0, 0, 0, 0}, .name = "\0", .rssi = INVALID_RSSI, .adv_data = {0}, .adv_data_len = 0};
//         BLE_device_list[i] = placeholder;
//     }
//     // Initialize Devices
//     for (int i=0; i < MAX_DEVICES; ++i){
//         bt_device_t placeholder = {.addr = {0, 0, 0, 0, 0, 0}, .name = "\0", .rssi = INVALID_RSSI};
//         BT_device_list[i] = placeholder;
//     }

//     xTaskCreate(report_task, "report_task", 4096, NULL, 5, &HID_report_mode_task_handle);
//     xTaskCreate(HID_set_protocol_task, "set_protocol_task", 4096, NULL, 5, &HID_set_protocol_task_handle);

//     // LED GPIO Pin init
//     gpio_reset_pin(LED_PIN);
//     gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

//     // Bluetooth startup for BLE
//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         nvs_flash_erase();
//         ret = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(ret);
//     esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
//     ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
//     ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));

//     // Initialize Bluedroid
//     ESP_ERROR_CHECK(esp_bluedroid_init());
//     ESP_ERROR_CHECK(esp_bluedroid_enable());

//     // Register BLE GAP callback
//     //esp_ble_gap_register_callback(BLE_gap_callback);

//     // Register GAP callback
//     esp_bt_gap_register_callback(callback_BTclassic);

//     // Register HID callback && Initialize
//     esp_bt_hid_host_init();
//     esp_bt_hid_host_register_callback(callback_BT_HIDcontroller);
    
//     // Initial Settings - Be connectable
//     esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

//     printer_esp("Successfully Booted for BT & BLE!!!...\n");

//     // Set discovery mode ON
//     BT_discover_devices(10000);

//     // Print Bonded Devices
//     BT_list_bonded_devices();

//     // Start Scanning BLE
//     //BLE_start_scanning(15);
    

    

//     while (1){
//          blink_LED(1);
//     }

// }