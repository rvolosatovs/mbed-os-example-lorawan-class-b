#include "mbed.h"

#include "mbed_trace.h"
#include "mbed_events.h"
#include "lora_radio_helper.h"
#include "dev_eui_helper.h"
#include "LoRaWANInterface.h"
#include "platform/Callback.h"

bool send_queued = false;
bool class_b_on = false;
bool ping_slot_synched = false;
bool device_time_synched = false;
bool beacon_found = false;

#define APP_DUTY_CYCLE         (device_time_synched && ping_slot_synched) ? 60000 : 10000
#define PING_SLOT_PERIODICITY  MBED_CONF_LORA_PING_SLOT_PERIODICITY
#define PRINT_GPS_TIME_INTERVAL 60000

MBED_STATIC_ASSERT(PING_SLOT_PERIODICITY <= 7, "Valid Ping Slot Periodicity values are 0 to 7");

typedef struct {
    uint16_t rx;
    uint16_t beacon_lock;
    uint16_t beacon_miss;
} app_data_frame_t;

app_data_frame_t app_data;

// Device credentials, register device as OTAA in The Things Network and copy credentials here
static uint8_t DEV_EUI[] = MBED_CONF_LORA_DEVICE_EUI;
static uint8_t APP_EUI[] = MBED_CONF_LORA_APPLICATION_EUI;
static uint8_t APP_KEY[] = MBED_CONF_LORA_APPLICATION_KEY;

// The port we're sending and receiving on
#define MBED_CONF_LORA_APP_PORT     15

static void queue_next_send_message();
static void print_received_beacon();

// EventQueue is required to dispatch events around
static EventQueue ev_queue;

// Constructing Mbed LoRaWANInterface and passing it down the radio object.
static LoRaWANInterface lorawan(radio);

// Application specific callbacks
static lorawan_app_callbacks_t callbacks;

// LoRaWAN stack event handler
static void lora_event_handler(lorawan_event_t event);


// Send a message over LoRaWAN
static void send_message()
{
    send_queued = false;

    uint8_t tx_buffer[6];
    tx_buffer[0] = (app_data.beacon_lock >> 8) & 0xff;
    tx_buffer[1] = app_data.beacon_lock & 0xff;
    tx_buffer[2] = (app_data.beacon_miss >> 8) & 0xff;
    tx_buffer[3] = app_data.beacon_miss & 0xff;
    tx_buffer[4] = (app_data.rx >> 8) & 0xff;
    tx_buffer[5] = app_data.rx & 0xff;

    int packet_len = sizeof(tx_buffer);
    printf("Sending %d bytes\n", packet_len);

    int16_t retcode = lorawan.send(MBED_CONF_LORA_APP_PORT, tx_buffer, packet_len, MSG_UNCONFIRMED_FLAG);

    if (retcode < 0) {
        retcode == LORAWAN_STATUS_WOULD_BLOCK ? printf("send - duty cycle violation\n")
        : printf("send() - Error code %d\n", retcode);

        queue_next_send_message();
        return;
    }

    printf("%d bytes scheduled for transmission\n", retcode);
}

static void queue_next_send_message()
{
    int backoff;

    if (send_queued) {
        return;
    }

    lorawan.get_backoff_metadata(backoff);
    if (backoff < APP_DUTY_CYCLE) {
        backoff = APP_DUTY_CYCLE;
    }

    printf("Next send in %d seconds\r\n", backoff / 1000);
    send_queued = true;
    ev_queue.call_in(backoff, &send_message);
}


void print_gps_time(){
    printf("Current GPS Time = %llu\n",lorawan.get_current_gps_time());
}

int main()
{
    memset(&app_data, 0, sizeof(app_data));

    // Enable trace output for this demo, so we can see what the LoRaWAN stack does
    mbed_trace_init();

    if (lorawan.initialize(&ev_queue) != LORAWAN_STATUS_OK) {
        printf("LoRa initialization failed!\n");
        return -1;
    }

    // prepare application callbacks
    callbacks.events = mbed::callback(lora_event_handler);
    lorawan.add_app_callbacks(&callbacks);

    lorawan_connect_t connect_params;
    connect_params.connect_type = LORAWAN_CONNECTION_OTAA;
    connect_params.connection_u.otaa.dev_eui = DEV_EUI;
    connect_params.connection_u.otaa.app_eui = APP_EUI;
    connect_params.connection_u.otaa.app_key = APP_KEY;
    connect_params.connection_u.otaa.nwk_key = APP_KEY;
    connect_params.connection_u.otaa.nb_trials = MBED_CONF_LORA_NB_TRIALS;
    lorawan_status_t retcode = lorawan.connect(connect_params);

    if (retcode == LORAWAN_STATUS_OK ||
            retcode == LORAWAN_STATUS_CONNECT_IN_PROGRESS) {
    } else {
        printf("Connection error, code = %d\n", retcode);
        return -1;
    }

    printf("Connection - In Progress ...\r\n");

    ev_queue.call_every(PRINT_GPS_TIME_INTERVAL, &print_gps_time);

    // make your event queue dispatching events forever
    ev_queue.dispatch_forever();

    return 0;
}

// This is called from RX_DONE, so whenever a message came in
static void receive_message()
{
    uint8_t rx_buffer[255] = { 0 };
    uint8_t port;
    int flags;
    int16_t retcode = lorawan.receive(rx_buffer, sizeof(rx_buffer), port, flags);

    if (retcode < 0) {
        printf("receive() - Error code %d\n", retcode);
        return;
    }
    app_data.rx++;

    printf("Received %d bytes on port %u\n", retcode, port);

    printf("Data received on port %d (length %d): ", port, retcode);

    for (uint8_t i = 0; i < retcode; i++) {
        printf("%02x ", rx_buffer[i]);
    }
    printf("\n");
}

lorawan_status_t enable_beacon_acquisition()
{
    lorawan_status_t status;
    beacon_found = false;

    status = lorawan.enable_beacon_acquisition();
    if (status != LORAWAN_STATUS_OK) {
        printf("Beacon Acquisition Error - EventCode = %d\n", status);
    } 

    // Send device time request. Beqcon acquisition is optimized when device time is synched
    device_time_synched = false;
    status = lorawan.add_device_time_request();
    if (status != LORAWAN_STATUS_OK) {
        printf("Add device time request Error - EventCode = %d", status);
    }

    return status;
}

void switch_to_class_b(void)
{
    lorawan_status_t status;

    if(!class_b_on && beacon_found && ping_slot_synched){
        status = lorawan.set_device_class(CLASS_B);
        if (status == LORAWAN_STATUS_OK) {
            class_b_on = true;
            // Send uplink now to notify server device is class B
            uint8_t dummy_value;
            lorawan.send(MBED_CONF_LORA_APP_PORT, &dummy_value, 1, MSG_UNCONFIRMED_FLAG);

        } else {
            printf("Switch Device Class -> B Error - EventCode = %d\n", status);
        }
    } 
}

// Event handler
static void lora_event_handler(lorawan_event_t event)
{
    lorawan_status_t status;

    switch (event) {
        case CONNECTED:
            printf("Connection - Successful\n");
            // Send ping slot configuration to the server
            status = lorawan.add_ping_slot_info_request(PING_SLOT_PERIODICITY);
            if (status != LORAWAN_STATUS_OK) {
                printf("Add ping slot info request Error - EventCode = %d", status);
            }
            // Enable beacon acquisition.
            enable_beacon_acquisition();
            send_message();
            break;
        case DISCONNECTED:
            ev_queue.break_dispatch();
            printf("Disconnected Successfully\n");
            break;
        case TX_DONE:
            printf("Message sent to Network Server\n");
            queue_next_send_message();
            break;
        case TX_TIMEOUT:
        case TX_ERROR:
        case TX_CRYPTO_ERROR:
        case TX_SCHEDULING_ERROR:
            printf("Transmission Error - EventCode = %d\n", event);
            queue_next_send_message();
            break;
        case RX_DONE:
            printf("Received Message from Network Server\n");
            receive_message();
            break;
        case RX_TIMEOUT:
        case RX_ERROR:
            printf("Error in reception - Code = %d\n", event);
            break;
        case JOIN_FAILURE:
            printf("OTAA Failed - Check Keys\n");
            break;
        case DEVICE_TIME_SYNCHED:
            printf("Device Time received from Network Server\n");
            device_time_synched = true;
            break;
        case PING_SLOT_INFO_SYNCHED:
            printf("Ping Slots = %u Synchronized with Network Server\n", 1 << (7 - PING_SLOT_PERIODICITY));
            ping_slot_synched = true;
            switch_to_class_b();
            break;
        case BEACON_NOT_FOUND:
            app_data.beacon_miss++; // This is not accurate since acquisition can span multiple beacon periods
            printf("Beacon Acquisition Failed\n");
            // Restart beacon acquisition
            enable_beacon_acquisition();
            break;
        case BEACON_FOUND:
            beacon_found = true;
            app_data.beacon_lock++;
            printf("Beacon Acquisiton Success\n");
            print_received_beacon();
            switch_to_class_b();
            break;
        case BEACON_LOCK:
            app_data.beacon_lock++;
            print_received_beacon();
            printf("Beacon Lock Count=%u\r\n", app_data.beacon_lock);
            break;
        case BEACON_MISS:
            app_data.beacon_miss++;
            printf("Beacon Miss Count=%u\r\n", app_data.beacon_miss);
            break;
        case SWITCH_CLASS_B_TO_A:
            printf("Reverted Class B -> A\n");
            class_b_on = false;
            enable_beacon_acquisition();
            break;
        default:
            MBED_ASSERT("Unknown Event");
    }
}

void print_received_beacon()
{
    loramac_beacon_t beacon;
    lorawan_status_t status;

    status = lorawan.get_last_rx_beacon(beacon);
    if (status != LORAWAN_STATUS_OK) {
        printf("Get Received Beacon Error - EventCode = %d\n", status);
    }

    printf("\nReceived Beacon Time=%lu, GwSpecific=", beacon.time);
    for (uint8_t i = 0; i < sizeof(beacon.gw_specific); i++) {
        printf("%02X", beacon.gw_specific[i]);
    }
    printf("\n");

}

