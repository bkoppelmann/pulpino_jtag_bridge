/* cable_common.c -- Interface to the low-level cable drivers
   Copyright (C) 2001 Marko Mlinar, markom@opencores.org
   Copyright (C) 2004 Gy�rgy Jeney, nog@sdf.lonestar.org
   Copyright (C) 2008 - 2010 Nathan Yawn, nathan.yawn@opencores.org

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */



#include <stdio.h>
#include <string.h>


#include "cable_common.h"

#include "cable_ft2232.h"

#include "errcodes.h"

#define debug(...) if(0) fprintf(stderr, __VA_ARGS__)

jtag_cable_t *jtag_cables;

/////////////////////////////////////////////////////////////////////////////////////
// Cable subsystem / init functions

void cable_setup(void)
{
  jtag_cables = cable_ftdi_get_driver();
}

/* Calls the init function of the cable
 */
int cable_init()
{
  return jtag_cables->init_func();
}

/* Parses command-line options specific to the selected cable */
int cable_parse_opt(int c, char *str)
{
  return jtag_cables->opt_func(c, str);
}

const char *cable_get_args()
{
  if(jtag_cables != NULL)
    return jtag_cables->opts;
  else
    return NULL;
}

/////////////////////////////////////////////////////////////////////////////////
// Cable API Functions

int cable_write_stream(uint32_t *stream, int len_bits, int set_last_bit)
{
  return jtag_cables->stream_out_func(stream, len_bits, set_last_bit);
}

int cable_read_write_stream(uint32_t *outstream, uint32_t *instream, int len_bits, int set_last_bit)
{
  return jtag_cables->stream_inout_func(outstream, instream, len_bits, set_last_bit);
}

int cable_write_bit(uint8_t packet)
{
  return jtag_cables->bit_out_func(packet);
}

int cable_read_write_bit(uint8_t packet_out, uint8_t *bit_in)
{
  return jtag_cables->bit_inout_func(packet_out, bit_in);
}

int cable_flush(void)
{
  if(jtag_cables->flush_func != NULL)
    return jtag_cables->flush_func();
  return APP_ERR_NONE;
}


/////////////////////////////////////////////////////////////////////////////////////
// Common functions which may or may not be used by individual drivers


/* Note that these make no assumption as to the starting state of the clock,
 * and they leave the clock HIGH.  But, these need to interface with other routines (like
 * the byte-shift mode in the USB-Blaster), which begin by assuming that a new
 * data bit is available at TDO, which only happens after a FALLING edge of TCK.
 * So, routines which assume new data is available will need to start by dropping
 * the clock.
 */
int cable_common_write_bit(uint8_t packet)
{
  uint8_t data = TRST_BIT;  // TRST is active low, don't clear unless /set/ in 'packet'
  int err = APP_ERR_NONE;

  /* Write data, drop clock */
  if(packet & TDO) data |= TDI_BIT;
  if(packet & TMS) data |= TMS_BIT;
  if(packet & TRST) data &= ~TRST_BIT;

  err |= jtag_cables->out_func(data);

  /* raise clock, to do write */
  err |= jtag_cables->out_func(data | TCLK_BIT);

  return err;
}

int cable_common_read_write_bit(uint8_t packet_out, uint8_t *bit_in)
{
  uint8_t data = TRST_BIT;  //  TRST is active low, don't clear unless /set/ in 'packet'
  int err = APP_ERR_NONE;

  /* Write data, drop clock */
  if(packet_out & TDO) data |= TDI_BIT;
  if(packet_out & TMS) data |= TMS_BIT;
  if(packet_out & TRST) data &= ~TRST_BIT;

  err |= jtag_cables->out_func(data);  // drop the clock to make data available, set the out data
  err |= jtag_cables->inout_func((data | TCLK_BIT), bit_in);  // read in bit, clock high for out bit.

  return err;
}


/* Writes bitstream via bit-bang. Can be used by any driver which does not have a high-speed transfer function.
 * Transfers LSB to MSB of stream[0], then LSB to MSB of stream[1], etc.
 */
int cable_common_write_stream(uint32_t *stream, int len_bits, int set_last_bit)
{
  int i;
  int index = 0;
  int bits_this_index = 0;
  uint8_t out;
  int err = APP_ERR_NONE;

  debug("writeStrm%d(", len_bits);
  for(i = 0; i < len_bits - 1; i++)
  {
    out = (stream[index] >> bits_this_index) & 1;
    err |= cable_write_bit(out);
    debug("%i", out);
    bits_this_index++;
    if(bits_this_index >= 32)
    {
      index++;
      bits_this_index = 0;
    }
  }

  out = (stream[index] >>(len_bits - 1)) & 0x1;
  if(set_last_bit) out |= TMS;
  err |= cable_write_bit(out);
  debug("%i)\n", out);
  return err;
}

/* Gets bitstream via bit-bang.  Can be used by any driver which does not have a high-speed transfer function.
 * Transfers LSB to MSB of stream[0], then LSB to MSB of stream[1], etc.
 */
int cable_common_read_stream(uint32_t *outstream, uint32_t *instream, int len_bits, int set_last_bit)
{
  int i;
  int index = 0;
  int bits_this_index = 0;
  uint8_t inval, outval;
  int err = APP_ERR_NONE;

  instream[0] = 0;

  debug("readStrm%d(", len_bits);
  for(i = 0; i < (len_bits - 1); i++)
  {
    outval = (outstream[index] >> bits_this_index) & 0x1;
    err |= cable_read_write_bit(outval, &inval);
    debug("%i", inval);
    instream[index] |= (inval << bits_this_index);
    bits_this_index++;
    if(bits_this_index >= 32)
    {
      index++;
      bits_this_index = 0;
      instream[index] = 0;  // It's safe to do this, because there's always at least one more bit
    }
  }

  if (set_last_bit)
    outval = ((outstream[index] >> (len_bits - 1)) & 1) | TMS;
  else
    outval = (outstream[index] >> (len_bits - 1)) & 1;

  err |= cable_read_write_bit(outval, &inval);
  debug("%i", inval);
  instream[index] |= (inval << bits_this_index);

  debug(") = 0x%x\n", instream[0]);

  return err;
}



