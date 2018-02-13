/* Copyright (c) 2014, Nordic Semiconductor ASA
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


/** @defgroup ble_uart_project_template ble_uart_project_template
@{
@ingroup projects
@brief Empty project that can be used as a template for new projects.

@details
This project is a firmware template for new projects.
The project will run correctly in its current state.
It can send data on the UART TX characteristic
It can receive data on the UART RX characterisitc.
With this project you have a starting point for adding your own application functionality.

The following instructions describe the steps to be made on the Windows PC:

 -# Install the Master Control Panel on your computer. Connect the Master Emulator
    (nRF2739) and make sure the hardware drivers are installed.

-# You can use the nRF UART app in the Apple iOS app store and Google Play for Android 4.3 for Samsung Galaxy S4
   with this UART template app

-# You can send data from the Arduino serial monitor, maximum length of a string is 19 bytes
   Set the line ending to "Newline" in the Serial monitor (The newline is also sent over the air

 *
 * Click on the "Serial Monitor" button on the Arduino IDE to reset the Arduino and start the application.
 * The setup() function is called first and is called only once for each reset of the Arduino.
 * The loop() function as the name implies is called in a loop.
 *
 * The setup() and loop() function are called in this way.
 * main()
 *  {
 *   setup();
 *   while(1)
 *   {
 *     loop();
 *   }
 * }
 *
 */
#include <EEPROM.h>
#include <SPI.h>
#include <Nordic_nRF8001.h>
#include <avr/pgmspace.h>
#include <lib_aci.h>
#include <aci_setup.h>
#include "uart_over_ble.h"
#include <avr/io.h>
#include <avr/wdt.h>

/**
Put the nRF8001 setup in the RAM of the nRF8001.
*/
#include "services_lock.h"
/**
Include the services_lock.h to put the setup in the OTP memory of the nRF8001.
This would mean that the setup cannot be changed once put in.
However this removes the need to do the setup of the nRF8001 on every reset.
*/

#ifdef SERVICES_PIPE_TYPE_MAPPING_CONTENT
    static services_pipe_type_mapping_t
        services_pipe_type_mapping[NUMBER_OF_PIPES] = SERVICES_PIPE_TYPE_MAPPING_CONTENT;
#else
    #define NUMBER_OF_PIPES 0
    static services_pipe_type_mapping_t * services_pipe_type_mapping = NULL;
#endif

/* Store the setup for the nRF8001 in the flash of the AVR to save on RAM */
static hal_aci_data_t setup_msgs[NB_SETUP_MESSAGES] PROGMEM =
  SETUP_MESSAGES_CONTENT;

static struct aci_state_t aci_state;

/* Temporary buffer for sending ACI commands */
static hal_aci_evt_t  aci_data;

/* Timing change state variable */
static bool timing_change_done          = false;

/* Used to test the UART TX characteristic notification */
static uart_over_ble_t uart_over_ble;
static uint8_t         uart_buffer[20];
static uint8_t         uart_buffer_len = 0;

String inputString     = "";     // a string to hold incoming data
bool stringComplete = false;  // whether the string is complete

#define BOOTLOADER_START_ADDR 0x7000
#define BOOTLOADER_KEY        0xDC42
uint16_t boot_key __attribute__ ((section (".noinit")));
void bootloader_jump_check (void) __attribute__ ((used, naked, section (".init3")));
void bootloader_jump_check (void)
{
  uint8_t wdt_flag = MCUSR & (1 << WDRF);

  MCUSR &= ~(1 << WDRF);
  wdt_disable();

  if (wdt_flag && (boot_key == BOOTLOADER_KEY)) {
    boot_key = 0;

    ((void (*)(void)) BOOTLOADER_START_ADDR) ();
  }
}

static bool bootloader_jump()
{
  Serial.println("Jumping to bootloader");
  delay(100);

  if ((aci_state.data_credit_available != aci_state.data_credit_total)) {
    return false;
  }

  if (!lib_aci_is_pipe_available(&aci_state,
        PIPE_DEVICE_FIRMWARE_UPDATE_BLE_SERVICE_DFU_PACKET_RX)) {
    return false;
  }

  if (!lib_aci_is_pipe_available(&aci_state,
        PIPE_DEVICE_FIRMWARE_UPDATE_BLE_SERVICE_DFU_CONTROL_POINT_TX)) {
    return false;
  }

  if (!lib_aci_is_pipe_available(&aci_state,
        PIPE_DEVICE_FIRMWARE_UPDATE_BLE_SERVICE_DFU_CONTROL_POINT_RX_ACK_AUTO)) {
    return false;
  }

  /* Wait until ready line goes low before jump */
  while (digitalRead(aci_state.aci_pins.rdyn_pin));

  /* Set the special bootloader key value */
  boot_key = BOOTLOADER_KEY;


  wdt_enable(WDTO_15MS);
  while(1);

  return true;
}

/* Define how assert should function in the BLE library */
void __ble_assert(const char *file, uint16_t line)
{
  Serial.print("ERROR ");
  Serial.print(file);
  Serial.print(": ");
  Serial.print(line);
  Serial.print("\n");
  while(1);
}

/* crc function to re-calulate the CRC after making changes to the setup data. */
uint16_t crc_16_ccitt(uint16_t crc, uint8_t * data_in, uint16_t data_len)
{
  uint16_t i;

  for(i = 0; i < data_len; i++)
  {
    crc  = (unsigned char)(crc >> 8) | (crc << 8);
    crc ^= pgm_read_byte(&data_in[i]);
    crc ^= (unsigned char)(crc & 0xff) >> 4;
    crc ^= (crc << 8) << 4;
    crc ^= ((crc & 0xff) << 4) << 1;
  }

  return crc;
}

/* This creates an array of the DFU data that should be stored in EEPROM,
 * computes a CRC value for the data, compares that CRC value with the one in
 * EEPROM and stores our data in EEPROM if there is a CRC mismatch
 */
bool store_dfu_info_in_eeprom (void)
{
  uint8_t crc; uint8_t addr; uint16_t
  crc_seed = 0xFFFF;

  const uint16_t len = 17;
  uint8_t data[len] = {
    digitalPinToPort (aci_state.aci_pins.reqn_pin),
    digitalPinToBitMask (aci_state.aci_pins.reqn_pin),
    digitalPinToPort (aci_state.aci_pins.rdyn_pin),
    digitalPinToBitMask (aci_state.aci_pins.rdyn_pin),
    digitalPinToPort (aci_state.aci_pins.mosi_pin),
    digitalPinToBitMask (aci_state.aci_pins.mosi_pin),
    digitalPinToPort (aci_state.aci_pins.miso_pin),
    digitalPinToBitMask (aci_state.aci_pins.miso_pin),
    digitalPinToPort (aci_state.aci_pins.sck_pin),
    digitalPinToBitMask (aci_state.aci_pins.sck_pin),
    digitalPinToPort (aci_state.aci_pins.reset_pin),
    digitalPinToBitMask (aci_state.aci_pins.reset_pin),
    aci_state.data_credit_total,
    aci_state.data_credit_available,
    PIPE_DEVICE_FIRMWARE_UPDATE_BLE_SERVICE_DFU_PACKET_RX,
    PIPE_DEVICE_FIRMWARE_UPDATE_BLE_SERVICE_DFU_CONTROL_POINT_TX,
    PIPE_DEVICE_FIRMWARE_UPDATE_BLE_SERVICE_DFU_CONTROL_POINT_RX_ACK_AUTO
  };

  /* Compute CRC16 */
  crc = crc_16_ccitt(crc_seed, data, len);

  addr = 0;
  if (EEPROM.read(addr) == crc)
  {
    Serial.println(F("CRC matches EEPROM"));
    return false;
  }
  Serial.println(F("CRC does not match EEPROM"));

  /* As the computed CRC value does not match the one in EEPROM,
   * we write the crc and data[] to EEPROM
   */
  EEPROM.write(addr++, crc);

  for (uint8_t i = 0; i++; i < len)
  {
    EEPROM.write(addr++, data[i]);
  }

  return true;
}

/*
Description:

In this template we are using the BTLE as a UART and can send and receive
packets.  The maximum size of a packet is 20 bytes.  When a command it received
a response(s) are transmitted back.  Since the response is done using a
Notification the peer must have opened it(subscribed to it) before any packet
is transmitted.  The pipe for the UART_TX becomes available once the peer opens
it.  See section 20.4.1 -> Opening a Transmit pipe In the master control panel,
clicking Enable Services will open all the pipes on the nRF8001.

The ACI Evt Data Credit provides the radio level ack of a transmitted packet.
*/
void setup(void)
{
  Serial.begin(115200);
  //Wait until the serial port is available (useful only for the leonardo)
  while(!Serial)
  {}
  Serial.println(F("Arduino setup, UART template"));
  Serial.println(F("Set line ending to newline to send data from the serial monitor"));

  /**
    Point ACI data structures to the the setup data that the nRFgo studio generated for the nRF8001
    */
  if (NULL != services_pipe_type_mapping)
  {
    aci_state.aci_setup_info.services_pipe_type_mapping = &services_pipe_type_mapping[0];
  }
  else
  {
    aci_state.aci_setup_info.services_pipe_type_mapping = NULL;
  }
  aci_state.aci_setup_info.number_of_pipes    = NUMBER_OF_PIPES;
  aci_state.aci_setup_info.setup_msgs         = setup_msgs;
  aci_state.aci_setup_info.num_setup_msgs     = NB_SETUP_MESSAGES;

  /* Tell the ACI library, the MCU to nRF8001 pin connections.
   * The Active pin is optional and can be marked UNUSED
   */
  aci_state.aci_pins.board_name = BOARD_DEFAULT; //See board.h for details REDBEARLAB_SHIELD_V1_1 or BOARD_DEFAULT
  aci_state.aci_pins.reqn_pin   = SS; //SS for Nordic board, 9 for REDBEARLAB_SHIELD_V1_1
  aci_state.aci_pins.rdyn_pin   = 3; //3 for Nordic board, 8 for REDBEARLAB_SHIELD_V1_1
  aci_state.aci_pins.mosi_pin   = MOSI;
  aci_state.aci_pins.miso_pin   = MISO;
  aci_state.aci_pins.sck_pin    = SCK;

  aci_state.aci_pins.spi_clock_divider          = SPI_CLOCK_DIV8;

  aci_state.aci_pins.reset_pin             = 4; //4 for Nordic board, UNUSED for REDBEARLAB_SHIELD_V1_1
  aci_state.aci_pins.active_pin            = UNUSED;
  aci_state.aci_pins.optional_chip_sel_pin = UNUSED;

  aci_state.aci_pins.interface_is_interrupt	  = false;
  aci_state.aci_pins.interrupt_number			  = 1;


  store_dfu_info_in_eeprom();

  /* We reset the nRF8001 here by toggling the RESET line connected to the nRF8001
   * If the RESET line is not available we call the ACI Radio Reset to soft reset the nRF8001
   * then we initialize the data structures required to setup the nRF8001.
   * We call lib_aci_init() with debug true to enable debug printing for ACI Commands and Events
   */
  lib_aci_init(&aci_state, true);
}

void uart_over_ble_init(void)
{
  uart_over_ble.uart_rts_local = true;
}

bool uart_tx(uint8_t *buffer, uint8_t buffer_len)
{
  bool status = false;

  if (lib_aci_is_pipe_available(&aci_state, PIPE_UART_OVER_BTLE_UART_TX_TX) &&
      (aci_state.data_credit_available >= 1))
  {
    status = lib_aci_send_data(PIPE_UART_OVER_BTLE_UART_TX_TX, buffer, buffer_len);
    if (status)
    {
      aci_state.data_credit_available--;
    }
  }

  return status;
}

bool uart_process_control_point_rx(uint8_t *byte, uint8_t length)
{
  bool status = false;
  aci_ll_conn_params_t *conn_params;

  if (lib_aci_is_pipe_available(&aci_state, PIPE_UART_OVER_BTLE_UART_CONTROL_POINT_TX) )
  {
    Serial.println(*byte, HEX);
    switch(*byte)
    {
      /* Queues a ACI Disconnect to the nRF8001 when this packet is received.
       * May cause some of the UART packets being sent to be dropped
       */
      case UART_OVER_BLE_DISCONNECT:
        /*
Parameters:
None
*/
        lib_aci_disconnect(&aci_state, ACI_REASON_TERMINATE);
        status = true;
        break;


      /* Queues an ACI Change Timing to the nRF8001 */
      case UART_OVER_BLE_LINK_TIMING_REQ:

      /* Parameters:
       * Connection interval min: 2 bytes
       * Connection interval max: 2 bytes
       * Slave latency:           2 bytes
       * Timeout:                 2 bytes
       *
       * Same format as Peripheral Preferred Connection Parameters (See nRFgo
       * studio -> nRF8001 Configuration -> GAP Settings Refer to the ACI
       * Change Timing Request in the nRF8001 Product Specifications
       */
        conn_params = (aci_ll_conn_params_t *)(byte+1);
        lib_aci_change_timing(conn_params->min_conn_interval,
            conn_params->max_conn_interval,
            conn_params->slave_latency, conn_params->timeout_mult);
        status = true;
        break;


      /* Clears the RTS of the UART over BLE */
      case UART_OVER_BLE_TRANSMIT_STOP:
        /* Parameters: None */
        uart_over_ble.uart_rts_local = false;
        status = true;
        break;

      /* Set the RTS of the UART over BLE */
      case UART_OVER_BLE_TRANSMIT_OK:
        /* Parameters: None */
        uart_over_ble.uart_rts_local = true;
        status = true;
        break;
    }
  }

  return status;
}

void aci_loop()
{
  static bool setup_required = false;
  static bool bootloader_jump_required = false;

  // We enter the if statement only when there is a ACI event available to be processed
  if (lib_aci_event_get(&aci_state, &aci_data))
  {
    aci_evt_t * aci_evt;

    aci_evt = &aci_data.evt;
    switch(aci_evt->evt_opcode)
    {
      /**
        As soon as you reset the nRF8001 you will get an ACI Device Started Event
        */
      case ACI_EVT_DEVICE_STARTED:
        {
          aci_state.data_credit_total = aci_evt->params.device_started.credit_available;
          switch(aci_evt->params.device_started.device_mode)
          {
            case ACI_DEVICE_SETUP:
              /**
                When the device is in the setup mode
                */
              Serial.println(F("Evt Device Started: Setup"));
              setup_required = true;
              break;


            case ACI_DEVICE_STANDBY:
              Serial.println(F("Evt Device Started: Standby"));
              //Looking for an iPhone by sending radio advertisements
              //When an iPhone connects to us we will get an ACI_EVT_CONNECTED event from the nRF8001
              if (aci_evt->params.device_started.hw_error)
              {
                delay(20); //Magic number used to make sure the HW error event is handled correctly.
              }
              else
              {
                lib_aci_connect(180/* in seconds */, 0x0050 /* advertising interval 50ms*/);
                Serial.println(F("Advertising started"));
              }

              break;
          }
        }
        break; //ACI Device Started Event

      case ACI_EVT_CMD_RSP:
        //If an ACI command response event comes with an error -> stop
        if (ACI_STATUS_SUCCESS != aci_evt->params.cmd_rsp.cmd_status)
        {
          //ACI ReadDynamicData and ACI WriteDynamicData will have status codes of
          //TRANSACTION_CONTINUE and TRANSACTION_COMPLETE
          //all other ACI commands will have status code of ACI_STATUS_SCUCCESS for a successful command
          Serial.print(F("ACI Command "));
          Serial.println(aci_evt->params.cmd_rsp.cmd_opcode, HEX);
          Serial.print(F("Evt Cmd respone: Status "));
          Serial.println(aci_evt->params.cmd_rsp.cmd_status, HEX);
        }
        if (ACI_CMD_GET_DEVICE_VERSION == aci_evt->params.cmd_rsp.cmd_opcode)
        {
          //Store the version and configuration information of the nRF8001 in the Hardware Revision String Characteristic
          lib_aci_set_local_data(&aci_state, PIPE_DEVICE_INFORMATION_HARDWARE_REVISION_STRING_SET,
              (uint8_t *)&(aci_evt->params.cmd_rsp.params.get_device_version), sizeof(aci_evt_cmd_rsp_params_get_device_version_t));
        }
        break;

      case ACI_EVT_CONNECTED:
        Serial.println(F("Evt Connected"));
        uart_over_ble_init();
        timing_change_done              = false;
        aci_state.data_credit_available = aci_state.data_credit_total;

        /*
           Get the device version of the nRF8001 and store it in the Hardware Revision String
           */
        lib_aci_device_version();
        break;

      case ACI_EVT_PIPE_STATUS:
        Serial.println(F("Evt Pipe Status"));
        if (lib_aci_is_pipe_available(&aci_state, PIPE_UART_OVER_BTLE_UART_TX_TX) && (false == timing_change_done))
        {
          lib_aci_change_timing_GAP_PPCP();
          
          // change the timing on the link as specified in the nRFgo studio -> nRF8001 conf. -> GAP.
          // Used to increase or decrease bandwidth
          
          timing_change_done = true;
        }
        break;

      case ACI_EVT_TIMING:
        Serial.println(F("Evt link connection interval changed"));
        lib_aci_set_local_data(&aci_state,
            PIPE_UART_OVER_BTLE_UART_LINK_TIMING_CURRENT_SET,
            (uint8_t *)&(aci_evt->params.timing.conn_rf_interval), /* Byte aligned */
            PIPE_UART_OVER_BTLE_UART_LINK_TIMING_CURRENT_SET_MAX_SIZE);
        break;

      case ACI_EVT_DISCONNECTED:
        Serial.println(F("Evt Disconnected/Advertising timed out"));
        lib_aci_connect(180/* in seconds */, 0x0100 /* advertising interval 100ms*/);
        Serial.println(F("Advertising started"));
        break;

      case ACI_EVT_DATA_RECEIVED:
        Serial.print(F("Pipe Number: "));
        Serial.println(aci_evt->params.data_received.rx_data.pipe_number, DEC);

        switch (aci_evt->params.data_received.rx_data.pipe_number)
        {
          case PIPE_UART_OVER_BTLE_UART_RX_RX:
            Serial.print(F(" Data(Hex) : "));
            for(int i=0; i<aci_evt->len - 2; i++)
            {
              Serial.print((char)aci_evt->params.data_received.rx_data.aci_data[i]);
              uart_buffer[i] = aci_evt->params.data_received.rx_data.aci_data[i];
              Serial.print(F(" "));
            }
            uart_buffer_len = aci_evt->len - 2;
            Serial.println(F(""));
            if (lib_aci_is_pipe_available(&aci_state, PIPE_UART_OVER_BTLE_UART_TX_TX))
            {

              /*Do this to test the loopback otherwise comment it out
              */
              /*
                 if (!uart_tx(&uart_buffer[0], aci_evt->len - 2))
                 {
                 Serial.println(F("UART loopback failed"));
                 }
                 else
                 {
                 Serial.println(F("UART loopback OK"));
                 }
                 */
            }
            break;

          case PIPE_UART_OVER_BTLE_UART_CONTROL_POINT_RX:
            uart_process_control_point_rx(&aci_evt->params.data_received.rx_data.aci_data[0],
                aci_evt->len - 2); //Subtract for Opcode and Pipe number
            break;

          case PIPE_DEVICE_FIRMWARE_UPDATE_BLE_SERVICE_DFU_CONTROL_POINT_RX_ACK_AUTO:
            if (1 == aci_evt->params.data_received.rx_data.aci_data[0] &&
                lib_aci_is_pipe_available(&aci_state, PIPE_DEVICE_FIRMWARE_UPDATE_BLE_SERVICE_DFU_CONTROL_POINT_TX))
            {
              bootloader_jump();
            }
            break;
        }
        break;

      case ACI_EVT_DATA_CREDIT:
        aci_state.data_credit_available = aci_state.data_credit_available + aci_evt->params.data_credit.credit;
        break;

      case ACI_EVT_PIPE_ERROR:
        //See the appendix in the nRF8001 Product Specication for details on the error codes
        Serial.print(F("ACI Evt Pipe Error: Pipe #:"));
        Serial.print(aci_evt->params.pipe_error.pipe_number, DEC);
        Serial.print(F("  Pipe Error Code: 0x"));
        Serial.println(aci_evt->params.pipe_error.error_code, HEX);

        //Increment the credit available as the data packet was not sent.
        //The pipe error also represents the Attribute protocol Error Response 
        //sent from the peer and that should not be counted for the credit.
        
        if (ACI_STATUS_ERROR_PEER_ATT_ERROR != aci_evt->params.pipe_error.error_code)
        {
          aci_state.data_credit_available++;
        }
        break;

      case ACI_EVT_HW_ERROR:
        Serial.print(F("HW error: "));
        Serial.println(aci_evt->params.hw_error.line_num, DEC);

        for(uint8_t counter = 0; counter <= (aci_evt->len - 3); counter++)
        {
          Serial.write(aci_evt->params.hw_error.file_name[counter]); //uint8_t file_name[20];
        }
        Serial.println();
        lib_aci_connect(180/* in seconds */, 0x0050 /* advertising interval 50ms*/);
        Serial.println(F("Advertising started"));
        break;

    }
  }
  else
  {
    //Serial.println(F("No ACI Events available"));
    // No event in the ACI Event queue and if there is no event in the ACI command queue the arduino can go to sleep
    // Arduino can go to sleep now
    // Wakeup from sleep from the RDYN line
  }

  /* setup_required is set to true when the device starts up and enters setup mode.
   * It indicates that do_aci_setup() should be called. The flag should be cleared if
   * do_aci_setup() returns ACI_STATUS_TRANSACTION_COMPLETE.
   */
  if(setup_required)
  {
    if (SETUP_SUCCESS == do_aci_setup(&aci_state))
    {
      setup_required = false;
    }
  }

  /* If the bootloader_jump_required flag has been set, we attempt to jump to bootloader.
   * The bootloader_jump() function will do a series of checks before jumping.
   */
  if (bootloader_jump_required)
  {
    bootloader_jump ();
  }
}

void loop()
{
  //Process any ACI commands or events
  aci_loop();

  // print the string when a newline arrives:
  if (stringComplete) {
    Serial.print(F("Sending: "));
    Serial.println(inputString);
    inputString.toCharArray((char*)uart_buffer,20);

    if (inputString.length() > 20)
    {
      uart_buffer_len = 20;
      uart_buffer[19] = '\n';
      Serial.println(F("Serial input truncted"));
    }
    else
    {
      uart_buffer_len = inputString.length();
    }

    if (!lib_aci_send_data(PIPE_UART_OVER_BTLE_UART_TX_TX, uart_buffer, uart_buffer_len))
    {
      Serial.println(F("Serial input dropped"));
    }
    // clear the string:
    inputString = "";
    stringComplete = false;
  }
}

/*
* SerialEvent occurs whenever a new data comes in the hardware serial RX.
* This routine is run between each time loop() runs, so using delay inside
* loop can delay response.  Multiple bytes of data may be available.  Serial
* Event is NOT compatible with Leonardo, Micro, Esplora
*/
void serialEvent()
{
  while (Serial.available()) {
    // get the new byte:
    char inChar = (char)Serial.read();
    // add it to the inputString:
    inputString += inChar;
    // if the incoming character is a newline, set a flag
    // so the main loop can do something about it:
    if (inChar == '\n') {
      stringComplete = true;
    }
  }
}