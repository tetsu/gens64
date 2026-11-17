/***************************************************************************
 * Gens64: Video recording through ffmpeg.                                 *
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

#ifndef GENS_VIDEO_REC_HPP
#define GENS_VIDEO_REC_HPP

#include "emulator/gens.hpp"

// Non-zero while a video is being recorded.
extern int VRec_Recording;

// The ffmpeg program: a name looked up on the PATH, or a full path.
extern char VRec_FFmpeg_Path[GENS_PATH_MAX];

/**
 * vrec_start(): Start recording a video.
 * @param filename Output file, or NULL for a numbered file in the video directory.
 * @return 0 on success; non-zero on error. (See vrec_last_error().)
 */
int vrec_start(const char *filename);

/**
 * vrec_stop(): Stop recording, and write the finished video file.
 * @return 0 on success; non-zero on error. (See vrec_last_error().)
 */
int vrec_stop(void);

/**
 * vrec_update(): Record the frame that was just emulated. Called once per frame.
 */
void vrec_update(void);

// The output file of the current or last recording.
const char *vrec_filename(void);

// Why the last vrec_start() or vrec_stop() failed.
const char *vrec_last_error(void);

#endif /* GENS_VIDEO_REC_HPP */
