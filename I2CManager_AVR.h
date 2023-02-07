/*
 *  © 2023, Neil McKechnie. All rights reserved.
 *
 *  This file is part of CommandStation-EX
 *
 *  This is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  It is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with CommandStation.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef I2CMANAGER_AVR_H
#define I2CMANAGER_AVR_H

#include <Arduino.h>
#include "I2CManager.h"
#include "I2CManager_NonBlocking.h"   // to satisfy intellisense

#include <avr/io.h>
#include <avr/interrupt.h>

/****************************************************************************
  TWI State codes
****************************************************************************/
// General TWI Master staus codes                      
#define TWI_START                  0x08  // START has been transmitted  
#define TWI_REP_START              0x10  // Repeated START has been transmitted
#define TWI_ARB_LOST               0x38  // Arbitration lost

// TWI Master Transmitter staus codes                      
#define TWI_MTX_ADR_ACK            0x18  // SLA+W has been tramsmitted and ACK received
#define TWI_MTX_ADR_NACK           0x20  // SLA+W has been tramsmitted and NACK received 
#define TWI_MTX_DATA_ACK           0x28  // Data byte has been tramsmitted and ACK received
#define TWI_MTX_DATA_NACK          0x30  // Data byte has been tramsmitted and NACK received 

// TWI Master Receiver staus codes  
#define TWI_MRX_ADR_ACK            0x40  // SLA+R has been tramsmitted and ACK received
#define TWI_MRX_ADR_NACK           0x48  // SLA+R has been tramsmitted and NACK received
#define TWI_MRX_DATA_ACK           0x50  // Data byte has been received and ACK tramsmitted
#define TWI_MRX_DATA_NACK          0x58  // Data byte has been received and NACK tramsmitted

// TWI Miscellaneous status codes
#define TWI_NO_STATE               0xF8  // No relevant state information available
#define TWI_BUS_ERROR              0x00  // Bus error due to an illegal START or STOP condition

#define TWI_TWBR  ((F_CPU / I2C_FREQ) - 16) / 2 // TWI Bit rate Register setting.

#if defined(I2C_USE_INTERRUPTS)
#define ENABLE_TWI_INTERRUPT (1<<TWIE)
#else
#define ENABLE_TWI_INTERRUPT 0
#endif

/***************************************************************************
 *  Set I2C clock speed register.
 ***************************************************************************/
void I2CManagerClass::I2C_setClock(unsigned long i2cClockSpeed) {
  unsigned long temp = ((F_CPU / i2cClockSpeed) - 16) / 2;
  for (uint8_t preScaler = 0; preScaler<=3; preScaler++) {
    if (temp <= 255) {
      TWBR = temp;
      TWSR = (TWSR & 0xfc) | preScaler;
      return;
    } else 
      temp /= 4;
  }
  // Set slowest speed ~= 500 bits/sec
  TWBR = 255;
  TWSR |= 0x03;
}

/***************************************************************************
 *  Initialise I2C registers.
 ***************************************************************************/
void I2CManagerClass::I2C_init()
{
  TWSR = 0;
  TWBR = TWI_TWBR;                                  // Set bit rate register (Baudrate). Defined in header file.
  TWDR = 0xFF;                                      // Default content = SDA released.
  TWCR = (1<<TWINT);                                // Clear interrupt flag
 
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
}

/***************************************************************************
 *  Initiate a start bit for transmission.
 ***************************************************************************/
void I2CManagerClass::I2C_sendStart() {
  bytesToSend = currentRequest->writeLen;
  bytesToReceive = currentRequest->readLen;
  rxCount = 0;
  txCount = 0;
#if defined(I2C_EXTENDED_ADDRESS) 
  if (currentRequest->i2cAddress.muxNumber() != I2CMux_None) {
    // Send request to multiplexer
    muxPhase = MuxPhase_PROLOG;  // When start bit interrupt comes in, send SLA+W to MUX
  } else
    muxPhase = 0;
#endif
  TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT)|(1<<TWEA)|(1<<TWSTA);  // Send Start

}

/***************************************************************************
 *  Initiate a stop bit for transmission (does not interrupt)
 ***************************************************************************/
void I2CManagerClass::I2C_sendStop() {
  TWDR = 0xff;  // Default condition = SDA released
  TWCR = (1<<TWEN)|(1<<TWINT)|(1<<TWSTO);  // Send Stop
}

/***************************************************************************
 *  Close I2C down
 ***************************************************************************/
void I2CManagerClass::I2C_close() {
  // disable TWI
  TWCR = (1<<TWINT);                 // clear any interrupt and stop twi.
  delayMicroseconds(10);  // Wait for things to stabilise (hopefully)
}

/***************************************************************************
 *  Main state machine for I2C, called from interrupt handler or,
 *  if I2C_USE_INTERRUPTS isn't defined, from the I2CManagerClass::loop() function
 *  (and therefore, indirectly, from I2CRB::wait() and I2CRB::isBusy()).
 ***************************************************************************/

void I2CManagerClass::I2C_handleInterrupt() {
  if (!(TWCR & (1<<TWINT))) return;  // Nothing to do.

  uint8_t twsr = TWSR & 0xF8;

#if defined(I2C_EXTENDED_ADDRESS)
  // First process the MUX state machine.
  if (muxPhase > MuxPhase_OFF) {
    switch (twsr) {
      case TWI_MTX_ADR_ACK:       // SLA+W has been transmitted and ACK received
        if (muxPhase == MuxPhase_PROLOG) {
          // Send MUX selecter mask to follow address
          I2CSubBus subBus = currentRequest->i2cAddress.subBus();
          TWDR =  (subBus==SubBus_All) ? 0xff :
                  (subBus==SubBus_None) ? 0x00 :
                  1 << subBus;
          TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT);
          return;
        } else if (muxPhase == MuxPhase_EPILOG) {
          TWDR = 0x00;  // Disable all subbuses
          TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT);
          return;
        }
        break;

      case TWI_MTX_DATA_ACK:      // Data byte has been transmitted and ACK received
        if (muxPhase == MuxPhase_PASSTHRU && !bytesToSend && !bytesToReceive) {
          if (_muxCount > 1) {
            // Device transaction complete, prepare to deselect MUX by sending start bit
            TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT)|(1<<TWSTO)|(1<<TWSTA);
            muxPhase = MuxPhase_EPILOG;
            return;
          } else {
            // Only one MUX so no need to deselect it.  Just finish off
            TWCR = (1<<TWEN)|(1<<TWINT)|(1<<TWSTO);
            state = I2C_STATE_COMPLETED;
            muxPhase = MuxPhase_OFF;
            return;
          }
        } else if (muxPhase == MuxPhase_PROLOG) {
          // If device address is zero, then finish here (i.e. send mux subBus mask only)
          if (currentRequest->i2cAddress.deviceAddress() == 0) {
            // Send stop and post rb.
            TWDR = 0xff;
            TWCR = (1<<TWEN)|(1<<TWINT)|(1<<TWSTO);
            state = I2C_STATE_COMPLETED;
            muxPhase = MuxPhase_OFF;
            return;
          } else {
            // Send stop followed by start, preparing to send device address
            TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT)|(1<<TWSTO)|(1<<TWSTA);
            muxPhase = MuxPhase_PASSTHRU;
            return;
          } 
        } else if (muxPhase == MuxPhase_EPILOG) {
          // Send stop and allow RB to be posted.
          TWDR = 0xff;
          TWCR = (1<<TWEN)|(1<<TWINT)|(1<<TWSTO);
          state = I2C_STATE_COMPLETED;
          muxPhase = MuxPhase_OFF;
          return;
        }
        break;

      case TWI_MRX_DATA_NACK:     // Last data byte has been received and NACK transmitted
        // We must read the data before processing the MUX, so do this here.
        if (bytesToReceive > 0) {
          currentRequest->readBuffer[rxCount++] = TWDR;
          bytesToReceive--;
        }
        if (muxPhase == MuxPhase_PASSTHRU && _muxCount > 1) {
          // Prepare to transmit epilog to mux - first send the stop bit and start bit
          //  (we don't need to reset mux if there is only one.
          TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT)|(1<<TWSTO)|(1<<TWSTA);
          muxPhase = MuxPhase_EPILOG;
          return;
        } else {
          // Finish up.
          TWCR = (1<<TWEN)|(1<<TWINT)|(1<<TWSTO);  // Send Stop
          state = I2C_STATE_COMPLETED;
          muxPhase = MuxPhase_OFF;
          return;
        }
        break;
        
      case TWI_START:             // START has been transmitted  
      case TWI_REP_START:         // Repeated START has been transmitted
        if (muxPhase == MuxPhase_PROLOG || muxPhase == MuxPhase_EPILOG) {
          // Send multiplexer address first
          uint8_t muxAddress = I2C_MUX_BASE_ADDRESS + currentRequest->i2cAddress.muxNumber();
          TWDR = (muxAddress << 1) | 0;   // MUXaddress+Write
          TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT);
          return;
        }
        break;
   
      case TWI_MTX_ADR_NACK:      // SLA+W has been transmitted and NACK received
      case TWI_MRX_ADR_NACK:      // SLA+R has been transmitted and NACK received
      case TWI_MTX_DATA_NACK:     // Data byte has been transmitted and NACK received
        if (muxPhase == MuxPhase_PASSTHRU) {
          // Data transaction was nak'd, update RB status but continue with mux cleardown
          completionStatus = I2C_STATUS_NEGATIVE_ACKNOWLEDGE;
          TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT)|(1<<TWSTO)|(1<<TWSTA);  // Send Stop and start
          muxPhase = MuxPhase_EPILOG;
          return;
        } else if (muxPhase > MuxPhase_EPILOG) {
          // Mux Cleardown was NAK'd, send stop and then finish.
          TWCR = (1<<TWEN)|(1<<TWINT)|(1<<TWSTO);  // Send Stop
          state = I2C_STATE_COMPLETED;
          return;
        }
        break;

    }
  }
  #endif

  // Now the main I2C interrupt handler, used for the device communications.
  //
  // Cases are ordered so that the most frequently used ones are tested first.
  switch (twsr) {
    case TWI_MTX_DATA_ACK:      // Data byte has been transmitted and ACK received
    case TWI_MTX_ADR_ACK:       // SLA+W has been transmitted and ACK received
      if (bytesToSend) {  // Send first.
        if (operation == OPERATION_SEND_P)
          TWDR = GETFLASH(currentRequest->writeBuffer + (txCount++));
        else
          TWDR = currentRequest->writeBuffer[txCount++];
        bytesToSend--;
        TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT);
      } else if (bytesToReceive) {  // All sent, anything to receive?
        // Don't need to wait for stop, as the interface won't send the start until
        // any in-progress stop condition has been sent.
        TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT)|(1<<TWSTA);  // Send Start
      } else {  
         // Nothing left to send or receive
        TWCR = (1<<TWEN)|(1<<TWINT)|(1<<TWEA)|(1<<TWSTO);  // Send Stop
        state = I2C_STATE_COMPLETED;
      }
      break;

    case TWI_MRX_DATA_ACK:      // Data byte has been received and ACK transmitted
      if (bytesToReceive > 0) {
        currentRequest->readBuffer[rxCount++] = TWDR;
        bytesToReceive--;
      }
      /* fallthrough */

    case TWI_MRX_ADR_ACK:      // SLA+R has been sent and ACK received
      if (bytesToReceive <= 1) {
        TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT); // Send NACK after next reception
      } else {
        // send ack
        TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT)|(1<<TWEA);
      }
      break;

    case TWI_MRX_DATA_NACK:     // Data byte has been received and NACK transmitted
      if (bytesToReceive > 0) {
        currentRequest->readBuffer[rxCount++] = TWDR;
        bytesToReceive--;
      }
      TWCR = (1<<TWEN)|(1<<TWINT)|(1<<TWEA)|(1<<TWSTO);  // Send Stop
      state = I2C_STATE_COMPLETED;
      break;

    case TWI_START:             // START has been transmitted  
    case TWI_REP_START:         // Repeated START has been transmitted
      {
        // Set up address and R/W
        uint8_t deviceAddress = currentRequest->i2cAddress;
        if (operation == OPERATION_READ || (operation==OPERATION_REQUEST && !bytesToSend))
          TWDR = (deviceAddress << 1) | 1; // SLA+R
        else
          TWDR = (deviceAddress << 1) | 0; // SLA+W
        TWCR = (1<<TWEN)|ENABLE_TWI_INTERRUPT|(1<<TWINT)|(1<<TWEA);
      }
      break;

    case TWI_MTX_ADR_NACK:      // SLA+W has been transmitted and NACK received
    case TWI_MRX_ADR_NACK:      // SLA+R has been transmitted and NACK received
    case TWI_MTX_DATA_NACK:     // Data byte has been transmitted and NACK received
      TWCR = (1<<TWEN)|(1<<TWINT)|(1<<TWEA)|(1<<TWSTO);  // Send Stop
      completionStatus = I2C_STATUS_NEGATIVE_ACKNOWLEDGE;
      state = I2C_STATE_COMPLETED;
      break;

    case TWI_ARB_LOST:          // Arbitration lost
      // Restart transaction from start.
      I2C_sendStart();
      break;

    case TWI_BUS_ERROR:         // Bus error due to an illegal START or STOP condition
    default:
      TWDR = 0xff;  // Default condition = SDA released
      TWCR = (1<<TWEN)|(1<<TWINT)|(1<<TWEA)|(1<<TWSTO);  // Send Stop
      completionStatus = I2C_STATUS_TRANSMIT_ERROR;
      state = I2C_STATE_COMPLETED;
  }
}

#if defined(I2C_USE_INTERRUPTS)
ISR(TWI_vect) {
  I2CManagerClass::handleInterrupt();
}
#endif

#endif /* I2CMANAGER_AVR_H */
