#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_attr.h" // For Packed Structs
// Bluetooth Includes
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hidh_api.h" // HID API
#include "esp_log.h"
#include "esp_err.h"
// BLE
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatts_api.h"
// Bluetooth Includes END
#include "string.h"
#include "esp_crc.h"
#include "endian.h"


#define LED_PIN 5
#define TAG "Log"

#define MAX_DEVICES 10
#define INVALID_RSSI -127
#define INVALID_DEVICE_INDEX -1
#define DUALSENSE_REPORT_LEN 78
#define ADV_DATA_LEN 31
#define RSSI_QUERY_RANGE -60
#define BT_ADDR_STR_LEN 18
#define DUALSENSE_MAX_PACKET_LEN 64
#define DUALSENSE_OUTPUT_TAG 0x10
#define DUALSENSE_OUTPUT_CRC_SEED 0xA2
#define BT_REPORT_ID 0x31

// Holders for Bluetooth Searching
/* int REPEATING_DEVICE_FLAG = 0;
int REPEATING_DEVICE_ADDR = INVALID_DEVICE_INDEX; */
esp_bd_addr_t CONNECTED_DEVICE_BT_ADDR;

// Set Task Handles (Names to point to tasks (threads))
TaskHandle_t HID_set_protocol_task_handle = NULL;
TaskHandle_t HID_report_mode_task_handle = NULL;
TaskHandle_t HID_print_packet_task_handle = NULL;
TaskHandle_t HID_process_packet_task_handle = NULL;

// Set Queue Handles (Queues to store data for Tasks)
QueueHandle_t HID_process_packet_queue;
QueueHandle_t print_packet_queue;

// Seq Num Holder for calculations
static uint8_t SEQ_NUM = 0;

/* Dummy function to do nothing */
static void do_nothing() {}

/* Holder for BLE Devices */
typedef struct {
    esp_bd_addr_t addr;
    int rssi;
    uint8_t name[249];
    uint8_t adv_data[ADV_DATA_LEN];
    uint8_t adv_data_len;
} ble_device_t;

ble_device_t BLE_device_list[MAX_DEVICES];
int BLE_device_count = 0;

/* Bluetooth Classic Device Information Holder */
typedef struct {
    esp_bd_addr_t addr;
    int rssi;
    uint8_t name[249];
} bt_device_t;

/* Dualsense Trigger Def -- 11 BYTES */
typedef struct PACKED_ATTR{
    uint8_t cmd; // 0xFF to KEEP UPDATING EVERYTHING, Refer to NOTES for each bit
    uint8_t effect_mode; // 0x00=Off, 0x01=Continuous, 0x02=Section, 0x06=Vibrate
    uint8_t start_pos; // 0-255
    uint8_t end_pos; // 0-255
    // Force Gradients
    uint8_t begin_force; // Starting Tension
    uint8_t middle_force; // Halfway Tension
    uint8_t end_force; // Ending Tension
    // Vibrate Freq
    uint8_t freq; // Frequency
    uint8_t PADDING[3];
} dualsense_trigger_report_t;

/* Dualsense Report Struct Def */
typedef struct PACKED_ATTR {
    uint8_t report_id; // 0x31 for BT
    uint8_t seq_tag; // 4 Bit Incrementing Counter, 0-15 & WRAPS
    uint8_t tag; // DS_OUTPUT_TAG -- Defined as 0x10

    uint8_t valid_flg0; // 0x01 Read Rumble, 0x02 Read Mic Mute, 0x04 Right Trigger, 0x08 Left Trigger, 0x10 Headphone Vol, 0x20 Speaker Vol
    uint8_t valid_flg1; // 0x01 Read Mic Volume, 0x02 Read Audio Control, 0x04 Read lightbar RGB

    /* Dualshock Compatibility ... ?*/
    uint8_t motor_right; // 0-255 RUMBLE
    uint8_t motor_left; // 0-255 RUMBLE

    /* Audio controls */
	uint8_t headphone_volume;	/* 0x0 - 0x7f */
	uint8_t speaker_volume;	/* 0x0 - 0xff */
	uint8_t mic_volume;		/* 0x0 - 0x40 */
	uint8_t audio_control;
	uint8_t mute_button_led;

    uint8_t power_save_control; // Allows disabling certain features to save battery -- Refer to NOTES
    /* ADVANCED TRIGGER CONTROL HERE */
	//uint8_t RESERVED[27]; // Reserved
    dualsense_trigger_report_t right_trigger;
    dualsense_trigger_report_t left_trigger;
    uint8_t RESERVED_T[5];

	uint8_t audio_control2;

    /* LEDs and lightbar */
	uint8_t valid_flg2; // 0x02 Lightbar Setup (Fading etc), 0x04 LED Brightness, 0x10 Player LED Lights
	uint8_t RESERVED2[2]; // Reserved
	uint8_t lightbar_setup; // 0x00 NO Action, 0x01 FADE IN, 0x02 FADE OUT
	uint8_t led_brightness; // 0x00 NONE, 0x01 Med?, 0x02 High?
	uint8_t player_leds; // Leds under touchpad, left->right, 0x00 -> 0x1F
	uint8_t lightbar_red; // r
	uint8_t lightbar_green; // g
	uint8_t lightbar_blue; // b

    uint8_t RESERVED3[24]; // Reserved
    uint32_t crc;

} dualsense_report_t;

/* Returns the sequence tag based on the CURRENT number
   Will also CHANGE the "current" parameter to what it will be next
   Note* -- Sequence tag is little endian so result will be shifted << 4 */
uint8_t calculate_sequence_tag(uint8_t* current){
    // Hold Seq Num
    uint8_t holder = *current;
    // Set Next Num
    *current = (*current + 1) & 0x0F;
    // Return Shifted Val
    return (holder << 4);
}

/* Essentially a WRAPPER Function
   Takes in the desired function to call to DO something with the report e.g set fields
   THEN initializes all the constants, seq num, CRC automamtically and 
   RETURNS the pointer to the HOLDER report ready to send.
        Note* -- This will change the holder params data via CRC                  */
dualsense_report_t* dualsense_report_create(void (*func)(dualsense_report_t*), dualsense_report_t* holder){
    // Zero everything first (incase some fields not used)
    memset(holder, 0, sizeof(*holder));

    // Init Constants first 
    holder->report_id = BT_REPORT_ID;
    holder->tag = DUALSENSE_OUTPUT_TAG;

    // Calculate Sequence Tag
    holder->seq_tag = calculate_sequence_tag(&SEQ_NUM);

    // Run Function to edit any fields
    func(holder);
    ESP_LOGI("TEST", "SEQ TAG: %d, MOTOR LEFT: %d, SIZE CALC: %d", holder->seq_tag, holder->motor_left, sizeof(*holder));

    // CRC the report
    uint32_t crc32;

    uint8_t crc_seed = DUALSENSE_OUTPUT_CRC_SEED;
    /* crc32 = esp_crc32_le(0xFFFFFFFF, &crc_seed, 1); // CRC BT Header first (so decoding knows output)
    crc32 = ~esp_crc32_le(crc32, (uint8_t*)holder, (sizeof(*holder)-4)); // CRC the rest of data */

    crc32 = esp_crc32_le(0, &crc_seed, 1); // CRC BT Header first (so decoding knows output)
    crc32 = esp_crc32_le(crc32, (uint8_t*)holder, (sizeof(*holder)-4)); // CRC the rest of data

    holder->crc = htole32(crc32); // REDUNDANT -- ESP32 should be LITTLE ENDIAN already -- Here for security

    return holder;
}

/* Initializes a NOTHING report */
dualsense_report_t* dualsense_report_void(dualsense_report_t* holder){
    // Create the Proper report -- Do nothing report since report will not do anything
    dualsense_report_create(&do_nothing, holder);
    return holder;
}

/* Param Func to set Rumble Flags */
void dualsense_report_param_rumble_lightbar(dualsense_report_t* holder){
    holder->valid_flg0 = 0x01 | 0x02; // Enable Rumble AND Haptics
    holder->motor_left = 127; // Rumble
    holder->valid_flg1 = 0x04; // Set Lightbar
    holder->valid_flg2 = 0x02 | 0x04; // Enable Lightbar Setup & LED Brightness
    holder->led_brightness = 0x02; // Set Brightness to bright
    holder->lightbar_setup = 0x02; // Continuous Light
    // Set Lightbar to Green
    holder->lightbar_red = 0;
    holder->lightbar_green = 255;
    holder->lightbar_blue = 0;
}

/* 
   Stores button states for a Dualsense Controller.
   Simple since does not store ALL information. (Gyroscope info, etc)
*/
typedef struct {
    bool button_X, button_Circle, button_Triangle, button_Square,
         button_L1, button_R1, button_R3, button_L3, button_Options,
         button_Share, button_Bar;
    bool dpad_Up, dpad_Down, dpad_Left, dpad_Right;
    uint8_t stick_Left_X, stick_Left_Y, stick_Right_X, stick_Right_Y;
    uint8_t trigger_L2, trigger_R2;
} dualsense_simple_holder_t;

/* Initializes a default Simple Report. */
dualsense_simple_holder_t* dualsense_simple_holder_default(dualsense_simple_holder_t* out){
    // Zero all Digital Buttons & Analogs where 0 is unpressed
    memset(out, 0, sizeof(dualsense_simple_holder_t));
    // Set sticks to middle
    out->stick_Left_X = out->stick_Left_Y = out->stick_Right_X = out->stick_Right_Y = 127;

    return out;
}

/* Prints all fields in a simple Dualsense Holder Struct */
void dualsense_simple_holder_print(dualsense_simple_holder_t* data){
    char* ptg = "PRINT_HODLDER";
    char str[1024];
    snprintf(str, sizeof(str),
                 "\n  X: %d\n\
                    O: %d\n\
                   Tr: %d\n\
                   Sq: %d\n\
                   L1: %d\n\
                   R1: %d\n\
                   R3: %d\n\
                   L3: %d\n\
                   Op: %d\n\
                   Sh: %d\n\
                   Br: %d\n\
                   dU: %d\n\
                   dD: %d\n\
                   dL: %d\n\
                   dR: %d\n\
                   lS: %d-%d\n\
                   rS: %d-%d\n\
                   R2: %d\n\
                   L2: %d\n", data->button_X, data->button_Circle, data->button_Triangle,
                              data->button_Square, data->button_L1, data->button_R1,
                              data->button_R3, data->button_L3, data->button_Options,
                              data->button_Share, data->button_Bar, data->dpad_Up,
                              data->dpad_Down, data->dpad_Left, data->dpad_Right,
                              data->stick_Left_X, data->stick_Left_Y, data->stick_Right_X,
                              data->stick_Right_Y, data->trigger_R2, data->trigger_L2);
    ESP_LOGI(ptg, "%s", str);
}

/* Device List of ALL Bluetooth devices */
bt_device_t BT_device_list[MAX_DEVICES];
/* Count for Number of Devices FOUND */
int BT_device_count = 0;

// Holder to know if power saving mode
volatile bool IS_POWER_SAVING = false;
// Counter for Packet Numbers Recieved
volatile int PACKET_COUNTER = 0;

void wait_ms(uint16_t ms){
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* Prints and Flushes */
void printer(const char* word){
    printf(word);
    fflush(stdout);
}

/* Prints under ESP LOGI */
void printer_esp(const char* word) {
    ESP_LOGI("Log", "%s", word);
}

/* Blinks an LED with 1 Second Delay x times */
void blink_LED(uint8_t amount){
    for (uint8_t i = 0; i < amount; ++i){
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(LED_PIN, 0);
    }
}

/* Makes type esp_bd_addr_t into a string */
void stringify_BT_address(char* dest, esp_bd_addr_t addr){
    sprintf(dest, ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(addr));
    dest[BT_ADDR_STR_LEN] = '\0';
}

/* Checks if a BT address is in device list
   Returns index else -1                 */
int _check_bleaddr_exists(esp_bd_addr_t addr){
    for (int i=0; i < MAX_DEVICES; ++i){
        if (BLE_device_list[i].rssi == INVALID_RSSI) {
            continue; // skip unused slots
        }
        if (memcmp(BLE_device_list[i].addr, addr, ESP_BD_ADDR_LEN) == 0) {
            return i; // found match
        }
    }
    return INVALID_DEVICE_INDEX;
}

/* Checks if a BT address is in device list
   Returns index else -1                 */
int _check_btaddr_exists(esp_bd_addr_t addr){
    for (int i=0; i < MAX_DEVICES; ++i){
        if (BT_device_list[i].rssi == INVALID_RSSI) {
            continue; // skip unused slots
        }
        if (memcmp(BT_device_list[i].addr, addr, ESP_BD_ADDR_LEN) == 0) {
            return i; // found match
        }
    }
    return INVALID_DEVICE_INDEX;
}

/* Displays the scanned device */
void _callback_display_scan_device(esp_ble_gap_cb_param_t *param){

    // Ensure event is device
    if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT || param->scan_rst.rssi < -60) return;

    // Make string BT address
    char bt_str[30]; stringify_BT_address(bt_str, param->scan_rst.bda);

    // Check If Device exists already in list
    int check_index = _check_bleaddr_exists(param->scan_rst.bda);
        //printf("Index is [%d] For addr: %s\n", check_index, bt_str);

    // Get Device Name
    uint8_t name_len = 0;
    char name[64] = "\0";
    uint8_t* eir = param->scan_rst.ble_adv;
    uint8_t* scan_ptr = eir + param->scan_rst.adv_data_len;
    // Get Complete Name
    uint8_t *adv_name = esp_ble_resolve_adv_data_by_type(eir, param->scan_rst.adv_data_len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    // Else Try Short Name
    if (!adv_name) adv_name = esp_ble_resolve_adv_data_by_type(eir, param->scan_rst.adv_data_len, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    // Try Scan Packet
    if (!adv_name && param->scan_rst.scan_rsp_len > 0) 
        esp_ble_resolve_adv_data_by_type(scan_ptr, param->scan_rst.adv_data_len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    // Try Short Name
    if (!adv_name)
        esp_ble_resolve_adv_data_by_type(scan_ptr, param->scan_rst.adv_data_len, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    
    if (adv_name) {
        strcat((char*)adv_name, "\0");
        printf("Got Name: %s\n", adv_name);
    }
    // If name exists give it to name var
    if (adv_name && name_len > 0) memcpy(name, adv_name, name_len);

    // If New Device
    if (check_index == INVALID_DEVICE_INDEX){
        printf("ADV raw: ");
        for (int i = 0; i < param->scan_rst.adv_data_len; i++) {
            printf("%02X ", param->scan_rst.ble_adv[i]);
        }
        printf("\n"); fflush(stdout);

        // Check Max Devices
        if (BLE_device_count >= MAX_DEVICES || param->scan_rst.rssi < -60) return;
        // Index
        int index = BLE_device_count;
        // Add Device to device_list
        // Copy Name
        strcpy((char*)BLE_device_list[index].name, name);
        // Copy BT Addr
        memcpy(BLE_device_list[index].addr, param->scan_rst.bda, ESP_BD_ADDR_LEN);
        // Copy advanced data
        memcpy(BLE_device_list[index].adv_data, param->scan_rst.ble_adv, param->scan_rst.adv_data_len);
        BLE_device_list[index].adv_data_len = param->scan_rst.adv_data_len;
        // Copy RSSI
        BLE_device_list[index].rssi = param->scan_rst.rssi;
        // Add to Count
        BLE_device_count++;
        // Print Device
        char* dis_name = (name[0] == '\0') ? "Unknown": name;
        printf("%s - %s - RSSI: %d\n", bt_str, dis_name, param->scan_rst.rssi); fflush(stdout);
    }
    // If Existing Device & name different
    else if (strcmp((char*)BLE_device_list[check_index].name, name) != 0){
        strcpy((char*)BLE_device_list[check_index].name, name);
        printf("%s - %s - RSSI: %d - UPDATED\n", bt_str, name, param->scan_rst.rssi);
    }
}

void BLE_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param){
    switch (event){

        // When scan parameters set complete
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS)
                printer_esp("Scanning parameters Successfuly SET");
            else
                ESP_LOGI(TAG, "Setting Scan Parameter failed code: %d", param->scan_param_cmpl.status);
        break;

        // When ONE scan result ready
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            _callback_display_scan_device(param);
        break;

        // When advertising data set complete
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            // TODO
        break;
        
        // When scan started
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        printf("--------------SCANNING FOR DEVICES--------------\n"); fflush(stdout);
        break;

        // When stop scan complete
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        printf("----------SCANNING STOPPED FOR DEVICES----------\n");
        printf("FOUND [%d] Devices\n", BLE_device_count); fflush(stdout);
        break;

        // BLE security request - Pairing requested essentially
        case ESP_GAP_BLE_SEC_REQ_EVT:
            // TODO
        break;

        // Pairing has completed and device in flash
        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            // TODO
        break;

        default:
        ESP_LOGI(TAG, "In default for event not made\n");
    }
}

void BLE_start_scanning(uint32_t secs){

    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x50,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };
    esp_ble_gap_set_scan_params(&scan_params);
    esp_ble_gap_start_scanning(0);
    wait_ms(secs * 1000);
    esp_ble_gap_stop_scanning();
}

/* Handles discovery changed event */
void callback_handle_discovery_changed(esp_bt_gap_cb_param_t* params){
    if (params->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED)
        printf("----------BLUETOOTH Discovery Started...----------\n");
    else
        printf("----------BLUETOOTH Discovery Stopped...----------\n");
    fflush(stdout);
}

/*True if addr invalid(All Zeros), false if not*/
bool _query_addr_invalid(esp_bd_addr_t addr){
    for (uint8_t i=0; i < ESP_BD_ADDR_LEN; ++i){
        if (addr[i] != 0) return false;
    }
    return true;
}

/* Runs when a device is discovered */
void callback_handle_discovery_result(esp_bt_gap_cb_param_t* params){

    char bt_str[BT_ADDR_STR_LEN];
    stringify_BT_address(bt_str, params->disc_res.bda);

    // Check If device already exists
    int existing_index = _check_btaddr_exists(params->disc_res.bda);
    if (existing_index != INVALID_DEVICE_INDEX){
        if (BT_device_list[existing_index].name[0] == '\0'){
            /* printf("%02X:%02X:%02X:%02X:%02X:%02X - ",
                params->disc_res.bda[0], params->disc_res.bda[1], params->disc_res.bda[2],
                params->disc_res.bda[3], params->disc_res.bda[4], params->disc_res.bda[5]);
            printf("Already Exists.... at INDEX: %d - DevCount: %d\n", existing_index, BT_device_count); fflush(stdout); */
            esp_bt_gap_read_remote_name(BT_device_list[existing_index].addr);
        }
        return;
    }
    // ELSE NEW device below
    // Check If Max Devices Reached
    if (BT_device_count >= MAX_DEVICES) return;

    // Continue getting values for new device
    // Get rssi
    int rssi_val = -128;
    for (uint8_t i=0; i < params->disc_res.num_prop; ++i){
        if (params->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_RSSI)
            rssi_val = *(int8_t*)params->disc_res.prop[i].val;
    }
    BT_device_list[BT_device_count].rssi = rssi_val;

    // Copy Bluetooth Address to store
    for (int i=0; i < ESP_BD_ADDR_LEN; ++i)
        BT_device_list[BT_device_count].addr[i] = params->disc_res.bda[i];

    // Get name & print in ESP_BT_GAP_READ_REMOTE_NAME_EVT event
    if (BT_device_count < MAX_DEVICES) BT_device_count++;
    esp_bt_gap_read_remote_name(BT_device_list[BT_device_count].addr);
}

/* Handles the DISPLAY of contents*/
void callback_handle_read_remote_name(esp_bt_gap_cb_param_t* params){

    // Get index, will always exist if this function is called.
    int device_index = _check_btaddr_exists(params->read_rmt_name.bda);
    if (device_index == INVALID_DEVICE_INDEX) return;

    // Display contents
    strcpy((char*)BT_device_list[device_index].name, (char*)params->read_rmt_name.rmt_name);
    char* display_name = (BT_device_list[device_index].name[0] == '\0') ? "Unknown": (char*)BT_device_list[device_index].name;

    printf("%02X:%02X:%02X:%02X:%02X:%02X - ",
           BT_device_list[device_index].addr[0], BT_device_list[device_index].addr[1], BT_device_list[device_index].addr[2],
           BT_device_list[device_index].addr[3], BT_device_list[device_index].addr[4], BT_device_list[device_index].addr[5]);
    printf("%s - RSSI: %d - Device Index: %d\n", display_name, BT_device_list[device_index].rssi, device_index);
}

/* BT Classic GAP Callback Handler function */
void callback_BTclassic(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* params){
    switch (event){
        // Device Discovery Result Event - DEVICES LISTED HERE
        case ESP_BT_GAP_DISC_RES_EVT:
            callback_handle_discovery_result(params);
        break;

        // Discovery State Changed Event - WHEN DISCOVERING OR NOT
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        callback_handle_discovery_changed(params);
        break;

        // Get Remote Services Event
        case ESP_BT_GAP_RMT_SRVCS_EVT:
            // TODO
        break;

        // Get remote service record event
        case ESP_BT_GAP_RMT_SRVC_REC_EVT:
            // TODO
        break;

        // Authentication complete event
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            printer_esp("ESP_BT_GAP_AUTH_CMPL_EVT Fired!");
        //printf("----------BLUETOOTH Pairing Started...----------\n"); fflush(stdout);
        break;

        // Legacy Pairing Pin code request
        case ESP_BT_GAP_PIN_REQ_EVT:
        printer_esp("ESP_BT_GAP_PIN_REQ_EVT Fired!");
            // TODO
        break;

        // Security Simple Pairing User Confirmation request
        case ESP_BT_GAP_CFM_REQ_EVT:
            printer_esp("ESP_BT_GAP_CFM_REQ_EVT Fired!");
            // TODO
        break;

        // Security Simple Pairing Passkey Notification
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            printer_esp("ESP_BT_GAP_KEY_NOTIF_EVT Fired!");
            // TODO
        break;

        // Security Simple Pairing Passkey Request
        case ESP_BT_GAP_KEY_REQ_EVT:
            // TODO
        break;

        // Read rssi event - Reads SIGNAL STRENGTH - 30 dBm good - 90 dBm weak
        case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
            // TODO
        break;

        // Config EIR data event (Extended Inquiry Response) - Gives extra information
        case ESP_BT_GAP_CONFIG_EIR_DATA_EVT:
            // TODO
        break;

        // Set AFH channels event - DO NOT TOUCH, handled by controller - dynamically avoids bad channels (noisy)
        case ESP_BT_GAP_SET_AFH_CHANNELS_EVT:
            // TODO - NOT DO
        break;
        
        // Reads the name of the device - IMPORTANT                                                 ---------------------------------------
        case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
            callback_handle_read_remote_name(params);
        break;

        // Tells when baseband link mode (active/sniff/hold/park) with a remote device has changed
        case ESP_BT_GAP_MODE_CHG_EVT:
            ESP_LOGI(TAG, "MODE HAS CHANGED TO: %d", params->mode_chg.mode);
            IS_POWER_SAVING = (params->mode_chg.mode == 2) ? true: false;
        break;

        // Remove bond device complete event - Confirmation when device unpaired
        case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
            // TODO
        break;

        // Fires when QOS (Quality of Service) change, how baseband schedules traffic, E.g Audio for guaranteeing timely packet delivery
        case ESP_BT_GAP_QOS_CMPL_EVT:
            // TODO
        break;

        // ACL connection complete status event - Fires when paired successfully
        case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
            // TODO
        break;

        // ACL disconnection complete status event - Fires when disconnected successfully
        case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
            // TODO
        break;

        // Set page timeout event - Request to configure timeout acknowledgement - print statement
        case ESP_BT_GAP_SET_PAGE_TO_EVT:
            // TODO
        break;

        // Get page timeout event value 
        case ESP_BT_GAP_GET_PAGE_TO_EVT:
            // TODO
        break;

        // Set ACL packet types event
        case ESP_BT_GAP_ACL_PKT_TYPE_CHANGED_EVT:
            // TODO
        break;

        // Encryption change event
        case ESP_BT_GAP_ENC_CHG_EVT:
            // TODO
        break;

        // Set minimum encryption key size
        case ESP_BT_GAP_SET_MIN_ENC_KEY_SIZE_EVT:
            // TODO
        break;

        // Get device name complete event
        case ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT:
            // TODO
        break;

        //
        case ESP_BT_GAP_EVT_MAX:
            // TODO
        break;
    };
}

esp_err_t BT_control_discovery(bool enable){
    esp_err_t discovery;
    if (enable)
        discovery = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, ESP_BT_GAP_MAX_INQ_LEN, 0);
    else
        discovery = esp_bt_gap_cancel_discovery();
    return discovery;
}

/*
  Wrapper for BT_control_discovery
  Displays all devices and can set time to search
  Shows count of all devices found.
*/
void BT_discover_devices(int ms_wait){
    BT_control_discovery(true);
    wait_ms(ms_wait);
    BT_control_discovery(false);
    printf("Found Devices Count: %d\n", BT_device_count);
    fflush(stdout);
}

/* Finds Device by Name in BT_device_list
   returns INVALID_DEVICE_INDEX (-1) if cannot find index */
int find_device_by_name(char* name){
    for (int i=0; i < MAX_DEVICES; ++i){
        if (strcmp(name, (char*)BT_device_list[i].name) == 0)
            return i;
    }
    return INVALID_DEVICE_INDEX;
}

/* Pairs a BT device in device list given the device NAME listed */
bool BT_pair_device(char* name){
    int index = find_device_by_name(name);
    if (index == INVALID_DEVICE_INDEX) return false;
    esp_err_t res = esp_bt_hid_host_connect(BT_device_list[index].addr);
    if (res == ESP_FAIL) return false;
    return true;
}

void BT_list_bonded_devices(){
    int dev_num = esp_bt_gap_get_bond_device_num();
    esp_bd_addr_t paired_list[dev_num];
    esp_bt_gap_get_bond_device_list(&dev_num, paired_list);

    printf("----------------Printing Bonded Devices---------------\n");
    for (int i=0; i < dev_num; ++i){
        printf("Bonded Device at index [%d] - %02X:%02X:%02X:%02X:%02X:%02X\n", i,
           paired_list[i][0], paired_list[i][1], paired_list[i][2],
           paired_list[i][3], paired_list[i][4], paired_list[i][5]);
        fflush(stdout);
    }
    printf("----------------Printing Bonded Finished---------------\n"); fflush(stdout);
}

void HID_set_protocol_task(void *arg) {
    /* while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // wait for signal
        printf("Sending the Protocol\n");
        esp_bt_hid_host_set_protocol((uint8_t*)CONNECTED_DEVICE_BT_ADDR, ESP_HIDH_REPORT_MODE);
    } */
}

/*  DEPREACATED /////////////////////////////////////////////////////////////
    void DualSense_change_lightbar(uint8_t r, uint8_t g, uint8_t b){
    uint8_t light_report[DUALSENSE_REPORT_LEN];
    _lightbar_report(light_report, r, g, b);
    esp_bt_hid_host_set_report((uint8_t*)CONNECTED_DEVICE_BT_ADDR, ESP_HIDH_REPORT_TYPE_OUTPUT, light_report, sizeof(light_report));
}
 */
// FOR testing sending reports, not active as of NOW
/* void report_task(void *arg) {

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Waits until ready

    uint8_t report[78] = {0};
    wait_ms(50);

    while (1) {
        // Wait for notification from OPEN_EVT
        //printer_esp("Sending Report...");

        create_init_report(report);
            //ESP_LOGI(TAG, "%d:%d:%d:%d\n", report[73], report[74], report[75], report[76], report[77]);

        esp_err_t err = esp_bt_hid_host_set_report((uint8_t*)CONNECTED_DEVICE_BT_ADDR, ESP_HIDH_REPORT_TYPE_OUTPUT, report, sizeof(report));
       // if (err == ESP_OK) printer_esp("Sent Reports OK");
        //else ESP_LOGI(TAG, "Report Did NOT send OK: %d", err);
        wait_ms(200);
        
    }
} */

bool IS_REPORT_SENT = false;
bool REPORT_QUEUED = false;

typedef struct {
    uint8_t data[DUALSENSE_MAX_PACKET_LEN];
    size_t len;
    int number;
} packet_data_t;


void HID_print_packet(packet_data_t* packet){
    /* DEPRACATED PRINT PART COMMENT
    ESP_LOGI("PACKET", "#%05d: %u - %u - %u - %u - %u - %u - %u - %u - %u - %u", packet.number,
            packet.data[0], packet.data[1], packet.data[2], packet.data[3], packet.data[4], packet.data[5],
            packet.data[6], packet.data[7], packet.data[8], packet.data[9]);*/
    int len = 400;
    char str[len];
    sniprintf(str, 15, "#%05d: ", packet->number); 
    len -= strlen(str);
    for (uint8_t i=0; i < packet->len; ++i){
        char holder[10];
        if (i == packet->len-1) sniprintf(holder, 10, "%03u", packet->data[i]);
        else sniprintf(holder, 10, "%03u - ", packet->data[i]);
        len -= strlen(holder);
        len = (len < 0) ? 0: len;
        strncat(str, holder, len);
    }
    ESP_LOGI("PACKET", "%s", str);
    /* uint32_t size = esp_get_free_heap_size();
    ESP_LOGI("HEAP", "%u", size); */
}
void HID_print_packet_task(void *arg){
    packet_data_t packet;
    while (1){
        //ESP_LOGI(TAG, "THIS MANY IN QUEUE: %d", uxQueueMessagesWaiting(print_packet_queue));
        if (xQueueReceive(print_packet_queue, &packet, portMAX_DELAY) == pdTRUE){
            HID_print_packet(&packet);
        }
    }
}

/* Translates simple dualsense packet data to buttons, triggers, & sticks
   REPORT: OUT report of translated pkt
   PKT:  IN pkt to translate                                           */
void process_simple_dualsense_packet(packet_data_t *pkt, dualsense_simple_holder_t* report){
    /*
    Packet Part 2: X AXIS LEFT STICK - 0-255, 0 LEFT
    Packet Part 3: Y AXIS LEFT STICK - 0-255, 0 UP
    Packet Part 4: X AXIS RIGHT STICK - 0-255, 0 LEFT
    Packet Part 5: Y AXIS RIGHT STICK - 0-255, 0 UP
    Packet Part 6:
        8 - No pressed
        40 - X
            104 - X & O
            168 - X & Triangle
            56 - X & Square
        72 - O
            200 - O & Triangle
            88 - O & Square
        136 - Triangle
            152 - Triangle & Square
        24 - Square
        DPAD:
            0 - UP
                1 - UP & RIGHT
                7 - UP & LEFT
            2 - RIGHT
                3 - RIGHT & DOWN
            4 - DOWN
                5 - DOWN & LEFT
            6 - LEFT
        DPAD & BUTTONS:


    Packet Part 7: L1 L2 etc
        1 - L1
            3 - L1 & R1
            7 - L1 & R1 & R2
            15 - L1 & L2 & R1 & R2
            5 - L1 & L2
            13 - L1 & L2 & R2
            9 - L1 & R2
        2 - R1
            10 - R1 & R2
            14 - R1 & R2 & L2
            6 - R1 & L2
        4 - L2
        8 - R2
        64 - L3
            68 - L3 & L2
            72 - L3 & R2
            76 - L3 & L2 & R2
            66 - L3 & R1
            74 - L3 & R2 & R1
            78 - L3 & R2 & R1 & L2
            65 - L3 & L1
            69 - L3 & L2 & L1
            77 - L3 & L2 & L1 & R2
            79 - L3 & L2 & L1 & R2 & R1
            67 - L3 & L1 & R1
            207 - L3 & R3 & L1 & R1 & L2 & R2
        128 - R3
            132 - R3 & L2
            136 - R3 & R2
            129 - R3 & L1
            133 - R3 & L2 & L1
            141 - R3 & L2 & L1 & R2
            130 - R3 & R1
            138 - R3 & R2 & R1
            142 - R3 & R2 & L2 & R1
            131 - R3 & R1 & L1
            143 - R3 & L2 & L1 & R2 & R1


        192 - R3 & L3
        32 - Options
        16 - Share
    Packet Part 9: 0-255 L2
    Packet Part 10: 0-255 R2
    Packet Part 8: Intermal timer up to 64???
    */

    // Extract data to better names
    uint8_t *data = pkt->data;
    uint8_t buttons = data[5];
    uint8_t bumpers = data[6];
    uint8_t L2_amount = data[8];
    uint8_t R2_amount = data[9];

    // Default the report (Everything False, Sticks Middle)
    dualsense_simple_holder_default(report);

    // Set Stick Values
    report->stick_Left_X = data[1];
    report->stick_Left_Y = data[2];
    report->stick_Right_X = data[3];
    report->stick_Right_Y = data[4];

    // Set Trigger Values
    report->trigger_R2 = R2_amount;
    report->trigger_L2 = L2_amount;

    // Set buttons
    switch (buttons){
        // X
        case 40:
        report->button_X = true;
        break;
        // X & Circle
        case 104:
        report->button_X = report->button_Circle = true;
        break;
        // X & Triangle
        case 168:
        report->button_X = report->button_Triangle = true;
        break;
        // X & Square
        case 56:
        report->button_X = report->button_Square = true;
        break;
        // O
        case 72:
        report->button_Circle = true;
        break;
        // O & Triangle
        case 200:
        report->button_Circle = report->button_Triangle = true;
        break;
        // O & Square
        case 88:
        report->button_Circle = report->button_Square = true;
        break;
        // Triangle
        case 136:
        report->button_Triangle = true;
        break;
        // Triangle & Square
        case 152:
        report->button_Triangle = report->button_Square = true;
        break;
        // Square
        case 24:
        report->button_Square = true;
        break;
        // DPAD CONTROLS BELOW
        // UP
        case 0:
        report->dpad_Up = true;
        break;
        // UP & RIGHT
        case 1:
        report->dpad_Up = report->dpad_Right = true;
        break;
        // UP & LEFT
        case 7:
        report->dpad_Up = report->dpad_Left = true;
        break;
        // RIGHT
        case 2:
        report->dpad_Right = true;
        break;
        // RIGHT & DOWN
        case 3:
        report->dpad_Right = report->dpad_Down = true;
        break;
        // DOWN
        case 4:
        report->dpad_Down = true;
        break;
        // DOWN & LEFT
        case 5:
        report->dpad_Down = report->dpad_Left = true;
        break;
        // LEFT
        case 6:
        report->dpad_Left = true;
        break;
        default: break;
    }
    // Set Triggers / Other Buttons -- NOTE* trigger val for L2 & R2 set above, need to still handle button combo for other buttons
    switch (bumpers){
        // L1
        case 1:
        report->button_L1 = true;
        break;
        // L1 & R1
        case 3:
        // L1 & R1 & R2
        case 7:
        // L1 & L2 & R1 & R2
        case 15:
        report->button_L1 = report->button_R1 = true;
        break;
        // L1 & L2
        case 5:
        // L1 & L2 & R2
        case 13:
        // L1 & R2
        case 9:
        report->button_L1 = true;
        break;
        // R1
        case 2:
        // R1 & R2
        case 10:
        // R1 & R2 & L2
        case 14:
        // R1 & L2
        case 6:
        report->button_R1 = true;
        break;

        // L2
        // R2

        // L3
        case 64:
        // L3 & L2
        case 68:
        // L3 & R2
        case 72:
        // L3 & L2 & R2
        case 76:
        report->button_L3 = true;
        break;
        // L3 & R1
        case 66:
        // L3 & R2 & R1
        case 74:
        // L3 & R2 & R1 & L2
        case 78:
        report->button_L3 = report->button_R1 = true;
        break;
        // L3 & L1
        case 65:
        // L3 & L2 & L1
        case 69:
        // L3 & L2 & L1 & R2
        case 77:
        report->button_L3 = report->button_L1 = true;
        break;
        // L3 & L2 & L1 & R2 & R1
        case 79:
        // L3 & L1 & R1
        case 67:
        report->button_L3 = report->button_L1 = report->button_R1 = true;
        break;
        // L3 & R3 & L1 & R1 & L2 & R2
        case 207:
        report->button_L3 = report->button_R3 = report->button_L1 = report->button_R1 = true;
        break;
        // R3
        case 128:
        // R3 & L2
        case 132:
        // R3 & R2
        case 136:
        report->button_R3 = true;
        break;
        // R3 & L1
        case 129:
        // R3 & L2 & L1
        case 133:
        // R3 & L2 & L1 & R2
        case 141:
        report->button_R3 = report->button_L1 = true;
        break;
        // R3 & R1
        case 130:
        // R3 & R2 & R1
        case 138:
        // R3 & R2 & L2 & R1
        case 142:
        report->button_R3 = report->button_R1 = true;
        break;
        // R3 & R1 & L1
        case 131:
        // R3 & L2 & L1 & R2 & R1
        case 143:
        report->button_R3 = report->button_R1 = report->button_L1 = true;
        break;
        // R3 & L3
        case 192:
        report->button_L3 = report->button_R3 = true;
        break;
        // Options
        case 32:
        report->button_Options = true;
        break;
        // Share
        case 16:
        report->button_Share = true;
        break;

        default: break;
    }

}

/* Return True if Difference between rpt 1 & rpt 2*/
bool dualsense_detect_simple_holder_difference(const dualsense_simple_holder_t* rpt1, const dualsense_simple_holder_t* rpt2){

    // Compare all fields. Boolean Logic -- If any false then entire false
    bool buttons = (rpt1->button_Bar == rpt2->button_Bar) && (rpt1->button_Circle == rpt2->button_Circle) &&
                   (rpt1->button_L1 == rpt2->button_L1) && (rpt1->button_L3 == rpt2->button_L3) &&
                   (rpt1->button_Options == rpt2->button_Options) && (rpt1->button_R1 == rpt2->button_R1) &&
                   (rpt1->button_R3 == rpt2->button_R3) && (rpt1->button_Share == rpt2->button_Share) &&
                   (rpt1->button_Square == rpt2->button_Square) && (rpt1->button_Triangle == rpt2->button_Triangle) &&
                   (rpt1->button_X == rpt2->button_X);
    
    bool dpad = (rpt1->dpad_Down == rpt2->dpad_Down) && (rpt1->dpad_Left == rpt2->dpad_Left) &&
                (rpt1->dpad_Right == rpt2->dpad_Right) && (rpt1->dpad_Up == rpt2->dpad_Up);

    bool sticks = (rpt1->stick_Left_X == rpt2->stick_Left_X) && (rpt1->stick_Left_Y == rpt2->stick_Left_Y) &&
                  (rpt1->stick_Right_X == rpt2->stick_Right_X) && (rpt1->stick_Right_Y == rpt2->stick_Right_Y);
    
    bool triggers = (rpt1->trigger_L2 == rpt2->trigger_L2) && (rpt1->trigger_R2 == rpt2->trigger_R2);

    bool result = buttons && dpad && triggers; // && sticks

    return !result;
}

void HID_send_dummy_report(){
    dualsense_report_t dummy_report;
    dualsense_report_void(&dummy_report);
    esp_bt_hid_host_set_report(CONNECTED_DEVICE_BT_ADDR, ESP_HIDH_REPORT_TYPE_OUTPUT,
                               (uint8_t*)&dummy_report, sizeof(dummy_report));
}

void dualsense_send_report_bt(dualsense_report_t* holder){
    // Send
    esp_bt_hid_host_send_data(CONNECTED_DEVICE_BT_ADDR, (uint8_t*)holder, sizeof(*holder));
}

void HID_process_packet(packet_data_t* pkt, dualsense_simple_holder_t* prev_pkt, dualsense_simple_holder_t* out){
    // Turn the data into a report
    process_simple_dualsense_packet(pkt, out);
    // Compare reports if in Power Saving Mode
    if (IS_POWER_SAVING){
        bool is_pkt_diff = dualsense_detect_simple_holder_difference(prev_pkt, out);
        ESP_LOGI("POWER SAVING", "PACKET DIFF IS: %d", is_pkt_diff);
        // If inputs different, send a report to stop
        if (is_pkt_diff) {
            HID_send_dummy_report();
            //IS_POWER_SAVING = false;
        }
    }
}
void HID_process_packet_task(){
    dualsense_simple_holder_t prev_pkt; dualsense_simple_holder_default(&prev_pkt);
    dualsense_simple_holder_t processed_pkt;
    packet_data_t packet;
    while (true){
        // Recieve the Packet, this is COPIED
        if (xQueueReceive(HID_process_packet_queue, &packet, portMAX_DELAY) == pdTRUE){
            // Process pkt into report
            //HID_process_packet(&packet, &prev_pkt, &processed_pkt);
            HID_print_packet(&packet);
            // Set Prev Pkt to current Pkt
            //prev_pkt = processed_pkt;
            // Do something with the report
            //dualsense_simple_holder_print(&prev_pkt);
                // Make the Report
        }
    }
}

void callback_BT_HIDcontroller(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param){
    switch(event){
        // When HID host is initialized, the event comes
        case ESP_HIDH_INIT_EVT: break;

        // When HID host is deinitialized, the event comes
        case ESP_HIDH_DEINIT_EVT: break;

        // When HID host connection opened, the event comes
        case ESP_HIDH_OPEN_EVT:
            printer_esp("Device Connection Opened");
            if (param->open.status == ESP_HIDH_OK) printf("STATUS OK\n");
            else printf("STATUS NOT OK\n");
            fflush(stdout);
            memcpy(CONNECTED_DEVICE_BT_ADDR, param->open.bd_addr, ESP_BD_ADDR_LEN);

            dualsense_report_t rpt;
            dualsense_report_create(&dualsense_report_param_rumble_lightbar, &rpt);
            dualsense_send_report_bt(&rpt);

            // Send Task for Report Sending
            //xTaskNotifyGive(HID_report_mode_task_handle);
            //REPORT_QUEUED = true;
        break;

        // When HID host connection closed, the event comes
        case ESP_HIDH_CLOSE_EVT:
            printer_esp("Device Connection Closed");
            //vTaskSuspend(HID_report_mode_task_handle)
        break;

        // When Get_Report command is called, the event comes
        case ESP_HIDH_GET_RPT_EVT:
            printf("GOT RPT DATA: ");
            /* for (int i=0; i < param->get_rpt.len; ++i){
                printf("%u - ", param->get_rpt.data[i]);
            }
            printf("\n"); */

            // TODO
        break;

        // When Set_Report command is called, the event comes
        case ESP_HIDH_SET_RPT_EVT:
            printer_esp("Report Event Fired, Event Recieved");
            //IS_REPORT_SENT = true;
            // TODO
        break;

        // When Get_Protocol command is called, the event comes
        case ESP_HIDH_GET_PROTO_EVT:
            char* type;
            if (param->get_proto.proto_mode == ESP_HIDH_BOOT_MODE)
                type = "BOOT MODE";
            else if (param->get_proto.proto_mode == ESP_HIDH_REPORT_MODE)
                type = "REPORT MODE";
            else
                type = "UNSUPPORTED";

            ESP_LOGI(TAG, "Protocol currently is: %s", type);
        break;

        // When Set_Protocol command is called, the event comes
        case ESP_HIDH_SET_PROTO_EVT:
            ESP_LOGI(TAG, "Protocol was SET");
            if (param->set_proto.status == ESP_HIDH_HS_TRANS_NOT_SPT){
                ESP_LOGI(TAG, "Protocol NOT SUPPORTED");
            }
            else if (param->set_proto.status != ESP_HIDH_OK) ESP_LOGI(TAG, "Protocol NOT SET...???");
            ESP_LOGI(TAG, "STATUS #: %d", param->set_proto.status);
        break;

        // When Get_Idle command is called, the event comes
        case ESP_HIDH_GET_IDLE_EVT:
            // TODO
        break;

        // When Set_Idle command is called, the event comes
        case ESP_HIDH_SET_IDLE_EVT:
            // TODO
        break;

        // When HIDH is initialized, the event comes
        case ESP_HIDH_GET_DSCP_EVT:
            // TODO
        break;

        // When a device is added, the event comes
        case ESP_HIDH_ADD_DEV_EVT:
            printer_esp("Device Added...");
        break;

        // When a device is removed, the event comes
        case ESP_HIDH_RMV_DEV_EVT:
            printer_esp("Device was Removed...");
            // TODO
        break;

        // When virtually unplugged, the event comes
        case ESP_HIDH_VC_UNPLUG_EVT:
            // TODO
        break;

        // When send data on interrupt channel, the event comes
        case ESP_HIDH_DATA_EVT:
            ESP_LOGI(TAG, "DATA WAS RECEIVED IN ESP_HIDH_DATA_EVT");
            // TODO
        break;

        // When receive data on interrupt channel, the event comes
        case ESP_HIDH_DATA_IND_EVT:

            if (PACKET_COUNTER > 60000) PACKET_COUNTER = 0;
            if (PACKET_COUNTER % 500 == 0) {
                packet_data_t pkt;

                // Get proper length of packet - Checks if data_ind len is the actual packet size, if not use actual packet size
                size_t valid_length = (param->data_ind.len > sizeof(pkt.data)) ? sizeof(pkt.data): param->data_ind.len;

                // Copy packet over
                memcpy(pkt.data, param->data_ind.data, valid_length);
                pkt.len = valid_length;
                pkt.number = PACKET_COUNTER;
                // Send the packet to be handled
                xQueueSend(HID_process_packet_queue, &pkt, 0);
            }
            // Increment Packet Counter
            ++PACKET_COUNTER;
        break;

        // When set the HID device descriptor, the event comes
        case ESP_HIDH_SET_INFO_EVT:
            // TODO
        break;
    }
   
}

void BT_classic_init(){

    // Initialize Devices
    for (int i=0; i < MAX_DEVICES; ++i){
        bt_device_t placeholder = {.addr = {0, 0, 0, 0, 0, 0}, .name = "\0", .rssi = INVALID_RSSI};
        BT_device_list[i] = placeholder;
    }

    // Create Tasks & Queues (HID)
    //xTaskCreate(report_task, "report_task", 4096, NULL, 5, &HID_report_mode_task_handle);
    //xTaskCreate(HID_set_protocol_task, "set_protocol_task", 4096, NULL, 5, &HID_set_protocol_task_handle);

    // Print Packet Queue & Task (Thread)
    print_packet_queue = xQueueCreate(100, sizeof(packet_data_t));
    //xTaskCreate(HID_print_packet_task, "print_packet", 4096, NULL, 5, &HID_print_packet_task_handle);
    xTaskCreatePinnedToCore(HID_print_packet_task, "print_packet", 4096, NULL, 5, &HID_print_packet_task_handle, 1);

    // Processing Data Queue & Task Init
    HID_process_packet_queue = xQueueCreate(100, sizeof(packet_data_t));
    //xTaskCreate(HID_process_packet_task, "process_packet_task", 4096, NULL, 5, &HID_process_packet_task_handle);
    xTaskCreatePinnedToCore(HID_process_packet_task, "process_packet_task", 8192, NULL, 5, &HID_process_packet_task_handle, 1);

    // Initialize Flash -- Check Updated or No free pages (free then redo)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Controller 
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));

    // Initialize Bluedroid
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // Register GAP callback
    esp_bt_gap_register_callback(callback_BTclassic);

    // Register HID callback && Initialize
    esp_bt_hid_host_init();
    esp_bt_hid_host_register_callback(callback_BT_HIDcontroller);
    
    // Initial Settings - Be connectable
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

}

void app_main() {

    // LED GPIO Pin init
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    // BT init
    BT_classic_init();

    printer_esp("Successfully Booted for BT & BLE!!!...\n");

    // Set discovery mode ON
    BT_discover_devices(10000);

    // Pair to Dualsense HARDCODED FOR NOW
    BT_pair_device("DualSense Wireless Controller");

    // Print Bonded Devices
    BT_list_bonded_devices();

    esp_log_level_set("BT_HCI", ESP_LOG_DEBUG);

    // Start Scanning BLE
    //BLE_start_scanning(15);

    
    while (1){
         blink_LED(1);
         //ESP_LOGI(TAG, "THIS MANY PACKETS: %d", PACKET_COUNTER);
    }

}


/*
EXTRA TEST CODE:

DIFFERENCE TEST CODE:
    dualsense_simple_holder_t t1, t2;
    dualsense_simple_holder_default(&t1);
    dualsense_simple_holder_default(&t2);
    t2.button_Circle = false;
    bool val = dualsense_detect_simple_holder_difference(&t1, &t2);
    char* s = (val) ? "TRUE": "FALSE";
    dualsense_simple_holder_print(&t1);
    dualsense_simple_holder_print(&t2);

    ESP_LOGI("TEST", "IS DIFF: %s(%d)", s, val);

*/