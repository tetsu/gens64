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

// Picture size of the finished video.
enum
{
	VREC_SIZE_2X = 0,	// 640 pixels wide.
	VREC_SIZE_1080P,	// Scaled up as far as it fits, in a 1920x1080 frame.
	VREC_SIZE_1440P,	// The same in 2560x1440.
	VREC_SIZE_4K,		// The same in 3840x2160.
	VREC_SIZE_COUNT
};
extern int VRec_Size;

// The name of a size in gens.cfg, such as "1440p".
const char *vrec_size_name(int size);

// The size with this name (case doesn't matter), or -1.
int vrec_size_from_name(const char *name);

/**
 * vrec_start(): Start recording a video.
 * @param filename Output file, or NULL for a numbered file in the video directory.
 * @return 0 on success; non-zero on error. (See vrec_last_error().)
 */
int vrec_start(const char *filename);

/**
 * vrec_stop(): Stop recording, and make the finished video file.
 * @param wait If zero, the file is made in the background, and a message on
 * the screen says when it is done. Otherwise this returns when it is done.
 * @return 0 on success; non-zero on error. (See vrec_last_error().)
 */
int vrec_stop(int wait);

/**
 * vrec_update(): Record the frame that was just emulated. Called once per frame.
 */
void vrec_update(void);

/**
 * vrec_poll(): Check on the videos being made in the background.
 * Called from the main loop.
 */
void vrec_poll(void);

/**
 * vrec_end(): Wait until the videos being made in the background are done.
 * Called when Gens64 exits.
 */
void vrec_end(void);

// The output file of the current or last recording.
const char *vrec_filename(void);

// Why the last vrec_start() or vrec_stop() failed.
const char *vrec_last_error(void);

#endif /* GENS_VIDEO_REC_HPP */
