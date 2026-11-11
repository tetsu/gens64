/***************************************************************************
 * Gens64: Remote control over a named pipe. (Win32)                       *
 *                                                                         *
 * Copyright (c) 2026 by Tetsuro Mori                                      *
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

#ifndef GENS_CONTROL_WIN32_HPP
#define GENS_CONTROL_WIN32_HPP

/**
 * control_init(): Open the control pipe, \\.\pipe\<name>.
 * @param name Pipe name. (Letters, digits, '_', '-' and '.'; 63 characters at most.)
 * @return 0 on success; non-zero on error.
 */
int control_init(const char *name);

/**
 * control_update(): Handle client commands. Called once per main loop iteration.
 * @return Non-zero if a client is holding the emulation, in which case
 *         the main loop must not run or redraw the game on its own.
 */
int control_update(void);

/**
 * control_end(): Close the control pipe.
 */
void control_end(void);

#endif /* GENS_CONTROL_WIN32_HPP */
