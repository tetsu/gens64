/***************************************************************************
 * Gens: Miscellaneous functions.                                          *
 *                                                                         *
 * Copyright (c) 1999-2002 by Stéphane Dallongeville                       *
 * Copyright (c) 2003-2004 by Stéphane Akhoun                              *
 * Copyright (c) 2008-2010 by David Korth                                  *
 * C port for 64-bit builds: Copyright (c) 2026 by Tetsuro Mori            *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.           *
 ***************************************************************************/

/**
 * C port of misc.asm.
 */

#include <stdint.h>

#include "misc.h"
#include "segacd/lc89510.h"

/**
 * CDD_Export_Status(): Copy the CDD status into the CDD receive buffer
 * and compute its checksum.
 *
 * CDD.Rcv_Status holds little-endian 16-bit copies of Status, Minute,
 * Seconde and Frame (bytes 0-7). Byte 8 is the checksum and byte 9 is
 * the low byte of CDD.Ext.
 */
void CDD_Export_Status(void)
{
	const uint16_t status  = (uint16_t)CDD.Status;
	const uint16_t minute  = (uint16_t)CDD.Minute;
	const uint16_t seconde = (uint16_t)CDD.Seconde;
	const uint16_t frame   = (uint16_t)CDD.Frame;
	const uint8_t  ext     = (uint8_t)CDD.Ext;
	uint8_t sum;

	CDD.Rcv_Status[0] = (uint8_t)status;
	CDD.Rcv_Status[1] = (uint8_t)(status >> 8);
	CDD.Rcv_Status[2] = (uint8_t)minute;
	CDD.Rcv_Status[3] = (uint8_t)(minute >> 8);
	CDD.Rcv_Status[4] = (uint8_t)seconde;
	CDD.Rcv_Status[5] = (uint8_t)(seconde >> 8);

	// "and byte [CDD.Control], 0x3": only the low byte is modified.
	CDD.Control &= 0xFFFFFF03U;

	// 8-bit checksum of all the status bytes.
	sum = (uint8_t)(status + (status >> 8) +
			minute + (minute >> 8) +
			seconde + (seconde >> 8) +
			frame + (frame >> 8) +
			ext);

	CDD.Rcv_Status[6] = (uint8_t)frame;
	CDD.Rcv_Status[7] = (uint8_t)(frame >> 8);
	CDD.Rcv_Status[8] = (uint8_t)(~sum & 0x0F);
	CDD.Rcv_Status[9] = ext;
}
