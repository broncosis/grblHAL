/*

  usb_serial.cpp - USB serial port wrapper for Arduino MKRZERO

  Part of GrblHAL

  Copyright (c) 2018-2020 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <string.h>

#include "Arduino.h"

#include "driver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLOCK_RX_BUFFER_SIZE 20

static stream_block_tx_buffer_t txbuf = {0};
static char rxbuf[BLOCK_RX_BUFFER_SIZE];

static stream_rx_buffer_t usb_rxbuffer, usb_rxbackup;

void usb_serialInit(void)
{
    SerialUSB.begin(BAUD_RATE);

#if USB_SERIAL_WAIT
//    while(!SerialUSB); // Hangs forever...
#endif

    txbuf.s = txbuf.data;
    txbuf.max_length = SerialUSB.availableForWrite(); // 63 bytes
    txbuf.max_length = (txbuf.max_length > BLOCK_TX_BUFFER_SIZE ? BLOCK_TX_BUFFER_SIZE : txbuf.max_length) - 20;
}

//
// Returns number of characters in serial input buffer
//
uint16_t usb_serialRxCount (void)
{
    uint_fast16_t tail = usb_rxbuffer.tail, head = usb_rxbuffer.head;
    return (uint16_t)BUFCOUNT(head, tail, RX_BUFFER_SIZE);
}

//
// Returns number of free characters in serial input buffer
//
uint16_t usb_serialRxFree (void)
{
    uint_fast16_t tail = usb_rxbuffer.tail, head = usb_rxbuffer.head;
    return (uint16_t)((RX_BUFFER_SIZE - 1) - BUFCOUNT(head, tail, RX_BUFFER_SIZE));
}

//
// Flushes the serial input buffer (including the USB buffer)
//
void usb_serialRxFlush (void)
{
    SerialUSB.flush();
    usb_rxbuffer.overflow = Off;
    usb_rxbuffer.head = usb_rxbuffer.tail = 0;
}

//
// Flushes and adds a CAN character to the serial input buffer
//
void usb_serialRxCancel (void)
{
    usb_rxbuffer.data[usb_rxbuffer.head] = CMD_RESET;
    usb_rxbuffer.tail = usb_rxbuffer.head;
    usb_rxbuffer.head = (usb_rxbuffer.tail + 1) & (RX_BUFFER_SIZE - 1);
}

//
// Writes a character to the serial output stream
//
bool usb_serialPutC (const char c)
{
    SerialUSB.write(c);

    return true;
}

//
// Writes a null terminated string to the serial output stream, blocks if buffer full.
// Buffers locally up to 40 characters or until the string is terminated with a ASCII_LF character.
// NOTE: grbl always sends ASCII_LF terminated strings!
//
void usb_serialWriteS (const char *s)
{
    size_t length = strlen(s);

    if((length + txbuf.length) < BLOCK_TX_BUFFER_SIZE) {

        memcpy(txbuf.s, s, length);
        txbuf.length += length;
        txbuf.s += length;

        if(s[length - 1] == ASCII_LF || txbuf.length > txbuf.max_length) {

            size_t avail;
            txbuf.s = txbuf.data;

            while(txbuf.length) {

                if((avail = SerialUSB.availableForWrite()) > 10) {

                    length = avail < txbuf.length ? avail : txbuf.length;

                    SerialUSB.write((uint8_t *)txbuf.s, length); // doc is wrong - does not return bytes sent!

                    txbuf.length -= length;
                    txbuf.s += length;
                }

                if(txbuf.length && !hal.stream_blocking_callback())
                    return;
            }
            txbuf.length = 0;
            txbuf.s = txbuf.data;
        }
    }
}

//
// Writes a null terminated string to the serial output stream followed by EOL, blocks if buffer full
//
void usb_serialWriteLn (const char *s)
{
    usb_serialWriteS(s);
    usb_serialWriteS(ASCII_EOL);
}

//
// Writes a number of characters from string to the serial output stream, blocks if buffer full
//
void usb_serialWrite (const char *s, uint16_t length)
{
    char *ptr = (char *)s;

    while(length--)
        usb_serialPutC(*ptr++);
}

//
// serialGetC - returns -1 if no data available
//
int16_t usb_serialGetC (void)
{
    uint16_t bptr = usb_rxbuffer.tail;

    if(bptr == usb_rxbuffer.head)
        return -1; // no data available else EOF

    char data = usb_rxbuffer.data[bptr++];              // Get next character, increment tmp pointer
    usb_rxbuffer.tail = bptr & (RX_BUFFER_SIZE - 1);    // and update pointer

    return (int16_t)data;
}

// "dummy" version of serialGetC
static int16_t serialGetNull (void)
{
    return -1;
}

bool usb_serialSuspendInput (bool suspend)
{
    if(suspend)
        hal.stream.read = serialGetNull;
    else if(usb_rxbuffer.backup)
        memcpy(&usb_rxbuffer, &usb_rxbackup, sizeof(stream_rx_buffer_t));

    return usb_rxbuffer.tail != usb_rxbuffer.head;
}

//
// This function get called from the protocol_execute_realtime function,
// used here to get characters off the USB serial input stream and buffer
// them for processing by grbl. Real time command characters are stripped out
// and submitted for realtime processing.
//
void usb_execute_realtime (uint_fast16_t state)
{
    char c, *dp;
    int avail, free;

    if((avail = SerialUSB.available())) {

        dp = rxbuf;
        free = usb_serialRxFree();
        free = free > BLOCK_RX_BUFFER_SIZE ? BLOCK_RX_BUFFER_SIZE : free;
        avail = avail > free ? free : avail;

        SerialUSB.readBytes(rxbuf, avail);

        while(avail--) {
            c = *dp++;
            if(c == CMD_TOOL_ACK && !usb_rxbuffer.backup) {
                memcpy(&usb_rxbackup, &usb_rxbuffer, sizeof(stream_rx_buffer_t));
                usb_rxbuffer.backup = true;
                usb_rxbuffer.tail = usb_rxbuffer.head;
                hal.stream.read = usb_serialGetC; // restore normal input
            } else if(!hal.stream.enqueue_realtime_command(c)) {;
                uint32_t bptr = (usb_rxbuffer.head + 1) & (RX_BUFFER_SIZE - 1); // Get next head pointer
                if(bptr == usb_rxbuffer.tail)                                   // If buffer full
                    usb_rxbuffer.overflow = On;                                 // flag overflow,
                else {
                    usb_rxbuffer.data[usb_rxbuffer.head] = c;                   // else add character data to buffer
                    usb_rxbuffer.head = bptr;                                   // and update pointer
                }
            }
        }
    }
}

#ifdef __cplusplus
}
#endif
