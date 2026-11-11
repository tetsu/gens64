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

/**
 * Line-based protocol: the client sends one command per line, and gets one
 * reply line starting with "OK" or "ERR". See README.txt, section 1.2.
 *
 * The pipe is non-blocking and is only touched from the main loop, so
 * commands always run between emulated frames.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "control_win32.hpp"

// C includes.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// C++ includes.
#include <string>
using std::string;

#include "gens.hpp"
#include "g_main.hpp"
#include "g_md.hpp"
#include "g_mcd.hpp"
#include "g_32x.hpp"
#include "g_update.hpp"
#include "options.hpp"

#include "macros/log_msg.h"
#include "debugger/debugger.hpp"
#include "audio/audio.h"
#include "gens_core/io/io.h"
#include "gens_core/vdp/vdp_io.h"
#include "input/input_update.h"
#include "mdp/mdp_error.h"
#include "mdp/mdp_mem.h"
#include "plugins/mdp_host_gens_mem.h"
#include "util/file/rom.hpp"
#include "util/file/save.hpp"
#include "util/gfx/imageutil.hpp"
#include "util/sound/gym.hpp"
#include "video/vdraw.h"
#include "gens/gens_window_sync.hpp"

// Win32 includes.
#include "libgsft/w32u/w32u_windows.h"
#include "libgsft/w32u/w32u_libc.h"

// libgsft includes.
#include "libgsft/gsft_szprintf.h"
#include "libgsft/gsft_unused.h"

// Not defined with the _WIN32_WINNT value used by this project.
#ifndef FILE_FLAG_FIRST_PIPE_INSTANCE
#define FILE_FLAG_FIRST_PIPE_INSTANCE 0x00080000
#endif
#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

#define PIPE_BUFFER_SIZE	65536
#define MAX_LINE_LENGTH		(16 * 1024)
#define MAX_READ_LENGTH		65536
#define MAX_STEP_FRAMES		1000000
#define WRITE_CHUNK_SIZE	4096
#define SEND_TIMEOUT_MS		5000

// Frames to run per main loop iteration, so the window stays responsive.
#define FRAMES_PER_ITERATION	8

static HANDLE hPipe = INVALID_HANDLE_VALUE;
static bool client_connected = false;

// Data received from the client. A line that doesn't fit is rejected.
static char rx_buf[MAX_LINE_LENGTH];
static size_t rx_len = 0;
static bool rx_discard = false;

// Emulation state. It stays in place when the client disconnects,
// so that each command can come from a separate client connection.
static bool held = false;
static unsigned int pending_frames = 0;
static unsigned int frames_run = 0;


/** Pipe I/O. **/


static void drop_client(void)
{
	DisconnectNamedPipe(hPipe);
	client_connected = false;
	rx_len = 0;
	rx_discard = false;
}


/**
 * write_all(): Write data to the client.
 * A client that stops reading is dropped, so that it can't stall the emulator.
 * @return True on success; false if the client was dropped.
 */
static bool write_all(const char *data, size_t len)
{
	DWORD last_progress = GetTickCount();

	while (len > 0)
	{
		const DWORD chunk = (DWORD)(len > WRITE_CHUNK_SIZE ? WRITE_CHUNK_SIZE : len);
		DWORD written = 0;

		if (!WriteFile(hPipe, data, chunk, &written, NULL))
		{
			drop_client();
			return false;
		}

		if (written == 0)
		{
			// The pipe is full.
			if ((GetTickCount() - last_progress) >= SEND_TIMEOUT_MS)
			{
				drop_client();
				return false;
			}
			Sleep(1);
			continue;
		}

		data += written;
		len -= written;
		last_progress = GetTickCount();
	}

	return true;
}


static void send_line(const char *text)
{
	if (client_connected && write_all(text, strlen(text)))
		write_all("\n", 1);
}


static void reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void reply(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vszprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	send_line(buf);
}


/** Command argument helpers. **/


/**
 * next_token(): Split the next space-separated token off of a string.
 * @param s [in/out] String position.
 * @return Token, or NULL if there are no more tokens.
 */
static char *next_token(char **s)
{
	char *p = *s;
	while (*p == ' ' || *p == '\t')
		p++;

	if (*p == 0x00)
	{
		*s = p;
		return NULL;
	}

	char *token = p;
	while (*p != 0x00 && *p != ' ' && *p != '\t')
		p++;
	if (*p != 0x00)
		*p++ = 0x00;

	*s = p;
	return token;
}


/**
 * rest_of_line(): Get a string argument that may contain spaces, such as a filename.
 * @param s String position.
 * @return Argument, with the surrounding whitespace removed.
 */
static char *rest_of_line(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;

	size_t len = strlen(s);
	while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t'))
		s[--len] = 0x00;

	return s;
}


/**
 * parse_number(): Parse a non-negative number. (Decimal, or hexadecimal with "0x".)
 * @return True on success.
 */
static bool parse_number(const char *s, unsigned long *value)
{
	if (s == NULL || *s < '0' || *s > '9')
		return false;

	const bool is_hex = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
	char *end;
	*value = strtoul(s, &end, (is_hex ? 16 : 10));
	return (*end == 0x00);
}


static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}


/**
 * need_game(): Check that a game is loaded, and reply with an error if it isn't.
 */
static bool need_game(void)
{
	if (Game != NULL)
		return true;

	reply("ERR no game loaded");
	return false;
}


/**
 * parse_memory_id(): Get the MDP memory ID for a memory name.
 * @return Memory ID, or MDP_MEM_NULL after replying with an error.
 */
static int parse_memory_id(const char *name)
{
	static const struct
	{
		const char *name;
		int id;
	} memories[] =
	{
		{"rom",		MDP_MEM_MD_ROM},
		{"ram",		MDP_MEM_MD_RAM},
		{"vram",	MDP_MEM_MD_VRAM},
		{"cram",	MDP_MEM_MD_CRAM},
	};

	if (name != NULL)
	{
		for (size_t i = 0; i < sizeof(memories) / sizeof(memories[0]); i++)
		{
			if (!strcmp(name, memories[i].name))
				return memories[i].id;
		}
	}

	reply("ERR memory must be rom, ram, vram or cram");
	return MDP_MEM_NULL;
}


/**
 * file_argument(): Get a filename argument.
 * @param args Command arguments.
 * @param command Command name, for the usage error.
 * @return Filename, or NULL after replying with an error.
 */
static const char *file_argument(char *args, const char *command)
{
	const char *filename = rest_of_line(args);
	if (*filename != 0x00)
		return filename;
	
	reply("ERR usage: %s <file>", command);
	return NULL;
}


/**
 * redraw_held_frame(): Redraw the game after a command changed it between frames.
 * Without this, the window and the next screenshot would show the old frame.
 */
static void redraw_held_frame(void)
{
	if (!held || Game == NULL)
		return;
	
	if (_32X_Started)
		Do_32X_VDP_Only();
	else
		Do_VDP_Only();
	
	vdraw_flip(1);
}


static void hold_emulation(void)
{
	if (held)
		return;

	// Nothing fills the looping sound buffer while the emulation is held.
	held = true;
	audio_clear_sound_buffer();
}


/** Commands. **/


static void cmd_help(char *args)
{
	GSFT_UNUSED_PARAMETER(args);
	reply("OK ping status load close hold resume step reset pad read write "
	      "screenshot savestate loadstate quit help");
}


static void cmd_ping(char *args)
{
	GSFT_UNUSED_PARAMETER(args);
	reply("OK " GENS_APPNAME " " GENS_GS_VERSION);
}


static void cmd_status(char *args)
{
	GSFT_UNUSED_PARAMETER(args);

	const char *system;
	if (Game == NULL)
		system = "none";
	else if (SegaCD_Started)
		system = "mcd";
	else if (_32X_Started)
		system = "32x";
	else
		system = "md";

	reply("OK system=%s held=%d paused=%d frames=%u rom=%s",
	      system, (held ? 1 : 0), (Settings.Paused ? 1 : 0), frames_run,
	      (Game != NULL ? ROM_Filename : ""));
}


static void cmd_load(char *args)
{
	const char *filename = file_argument(args, "load");
	if (filename == NULL)
		return;

	// Gives a more specific error than a failed load.
	FILE *f = fopen(filename, "rb");
	if (!f)
	{
		reply("ERR cannot open file");
		return;
	}
	fclose(f);

	if (audio_get_gym_playing())
		gym_play_stop();

	const int started = ROM::openROM(filename);
	Sync_Gens_Window();
	frames_run = 0;

	if (held)
		audio_clear_sound_buffer();

	if (started <= 0 || Game == NULL)
		reply("ERR could not start the game");
	else
		reply("OK rom=%s", ROM_Filename);
}


static void cmd_close(char *args)
{
	GSFT_UNUSED_PARAMETER(args);

	ROM::freeROM(Game);
	Sync_Gens_Window();
	frames_run = 0;
	reply("OK");
}


static void cmd_hold(char *args)
{
	GSFT_UNUSED_PARAMETER(args);

	hold_emulation();
	reply("OK");
}


static void cmd_resume(char *args)
{
	GSFT_UNUSED_PARAMETER(args);

	if (held)
	{
		held = false;
		Reset_Update_Timers();
	}
	reply("OK");
}


static void cmd_step(char *args)
{
	if (!need_game())
		return;

	unsigned long count = 1;
	const char *arg = next_token(&args);
	if (arg != NULL && !parse_number(arg, &count))
	{
		reply("ERR usage: step [frames]");
		return;
	}
	if (count < 1 || count > MAX_STEP_FRAMES)
	{
		reply("ERR frames must be between 1 and %d", MAX_STEP_FRAMES);
		return;
	}

	if (IS_DEBUGGING())
	{
		reply("ERR the debugger is active");
		return;
	}

	// The reply is sent from control_update() when the frames are done.
	hold_emulation();
	pending_frames = (unsigned int)count;
}


static void cmd_reset(char *args)
{
	GSFT_UNUSED_PARAMETER(args);

	if (!need_game())
		return;

	Options::systemReset();
	frames_run = 0;
	redraw_held_frame();
	reply("OK");
}


static void cmd_pad(char *args)
{
	static const struct
	{
		const char *name;
		unsigned int mask;
	} buttons[] =
	{
		{"up",		CONTROLLER_UP},
		{"down",	CONTROLLER_DOWN},
		{"left",	CONTROLLER_LEFT},
		{"right",	CONTROLLER_RIGHT},
		{"start",	CONTROLLER_START},
		{"a",		CONTROLLER_A},
		{"b",		CONTROLLER_B},
		{"c",		CONTROLLER_C},
		{"mode",	CONTROLLER_MODE},
		{"x",		CONTROLLER_X},
		{"y",		CONTROLLER_Y},
		{"z",		CONTROLLER_Z},
	};

	unsigned long port;
	if (!parse_number(next_token(&args), &port) || port < 1 || port > 2)
	{
		reply("ERR usage: pad <1|2> off|none|<buttons...>");
		return;
	}

	char *token = next_token(&args);
	if (token == NULL)
	{
		reply("ERR usage: pad <1|2> off|none|<buttons...>");
		return;
	}

	if (!strcmp(token, "off"))
	{
		// Return the pad to the keyboard / joystick.
		input_set_forced_buttons((int)port - 1, 0, 0);
		reply("OK");
		return;
	}

	unsigned int pressed = 0;
	if (strcmp(token, "none") != 0)
	{
		for (; token != NULL; token = next_token(&args))
		{
			size_t i;
			for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
			{
				if (!strcmp(token, buttons[i].name))
				{
					pressed |= buttons[i].mask;
					break;
				}
			}

			if (i == sizeof(buttons) / sizeof(buttons[0]))
			{
				reply("ERR unknown button: %s", token);
				return;
			}
		}
	}

	input_set_forced_buttons((int)port - 1, 1, pressed);
	reply("OK");
}


static void cmd_read(char *args)
{
	if (!need_game())
		return;

	const int memID = parse_memory_id(next_token(&args));
	if (memID == MDP_MEM_NULL)
		return;

	unsigned long address, length;
	if (!parse_number(next_token(&args), &address) ||
	    !parse_number(next_token(&args), &length) ||
	    length < 1 || length > MAX_READ_LENGTH)
	{
		reply("ERR usage: read <memory> <address> <length> (length: 1 to %d)", MAX_READ_LENGTH);
		return;
	}

	static const char hex[] = "0123456789ABCDEF";
	string text = "OK ";
	text.reserve(3 + (length * 2));

	for (unsigned long i = 0; i < length; i++)
	{
		uint8_t value;
		if (mdp_host_mem_read_8(memID, (uint32_t)(address + i), &value) != MDP_ERR_OK)
		{
			reply("ERR memory read failed");
			return;
		}
		text += hex[value >> 4];
		text += hex[value & 0x0F];
	}

	send_line(text.c_str());
}


static void cmd_write(char *args)
{
	if (!need_game())
		return;

	const int memID = parse_memory_id(next_token(&args));
	if (memID == MDP_MEM_NULL)
		return;

	unsigned long address;
	const char *data = NULL;
	bool valid = parse_number(next_token(&args), &address);
	if (valid)
		data = next_token(&args);

	const size_t digits = (data != NULL ? strlen(data) : 0);
	valid = (valid && digits > 0 && (digits % 2) == 0);
	for (size_t i = 0; valid && i < digits; i++)
		valid = (hex_digit(data[i]) >= 0);

	if (!valid)
	{
		reply("ERR usage: write <memory> <address> <hex bytes>");
		return;
	}

	for (size_t i = 0; i < digits / 2; i++)
	{
		const uint8_t value = (uint8_t)((hex_digit(data[i*2]) << 4) | hex_digit(data[(i*2)+1]));
		if (mdp_host_mem_write_8(NULL, memID, (uint32_t)(address + i), value) != MDP_ERR_OK)
		{
			reply("ERR memory write failed");
			return;
		}
	}
	
	redraw_held_frame();
	reply("OK");
}


static void cmd_screenshot(char *args)
{
	if (!need_game())
		return;

	const char *filename = file_argument(args, "screenshot");
	if (filename == NULL)
		return;

	if (ImageUtil::ScreenShot(filename) != 0)
		reply("ERR could not save the screenshot");
	else
		reply("OK");
}


static void cmd_savestate(char *args)
{
	if (!need_game())
		return;

	const char *filename = file_argument(args, "savestate");
	if (filename == NULL)
		return;

	if (Savestate::SaveState(filename) != 0)
		reply("ERR could not save the state");
	else
		reply("OK");
}


static void cmd_loadstate(char *args)
{
	if (!need_game())
		return;

	const char *filename = file_argument(args, "loadstate");
	if (filename == NULL)
		return;

	if (Savestate::LoadState(filename) != 0)
	{
		reply("ERR could not load the state");
		return;
	}
	
	redraw_held_frame();
	reply("OK");
}


static void cmd_quit(char *args)
{
	GSFT_UNUSED_PARAMETER(args);

	reply("OK");
	close_gens();
}


static void handle_line(char *line)
{
	static const struct
	{
		const char *name;
		void (*fn)(char *args);
	} commands[] =
	{
		{"help",	cmd_help},
		{"ping",	cmd_ping},
		{"status",	cmd_status},
		{"load",	cmd_load},
		{"close",	cmd_close},
		{"hold",	cmd_hold},
		{"resume",	cmd_resume},
		{"step",	cmd_step},
		{"reset",	cmd_reset},
		{"pad",		cmd_pad},
		{"read",	cmd_read},
		{"write",	cmd_write},
		{"screenshot",	cmd_screenshot},
		{"savestate",	cmd_savestate},
		{"loadstate",	cmd_loadstate},
		{"quit",	cmd_quit},
	};

	char *args = line;
	const char *verb = next_token(&args);
	if (verb == NULL)
		return;

	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
	{
		if (!strcmp(verb, commands[i].name))
		{
			// A modal message box would block the main loop, and with it
			// the client. Errors are reported in the reply instead.
			log_msgbox_disabled = 1;
			commands[i].fn(args);
			log_msgbox_disabled = 0;
			return;
		}
	}

	reply("ERR unknown command: %s", verb);
}


/**
 * service_client(): Accept a client, and run the commands it has sent.
 */
static void service_client(void)
{
	if (!client_connected)
	{
		if (!ConnectNamedPipe(hPipe, NULL))
		{
			const DWORD err = GetLastError();
			if (err == ERROR_NO_DATA)
			{
				// The client has already closed its end.
				DisconnectNamedPipe(hPipe);
				return;
			}
			if (err != ERROR_PIPE_CONNECTED)
				return;
		}

		client_connected = true;
		rx_len = 0;
		rx_discard = false;
	}

	// Commands run in order, so stop while a "step" is still running.
	while (client_connected && pending_frames == 0)
	{
		char *newline = (char*)memchr(rx_buf, '\n', rx_len);
		if (newline != NULL)
		{
			const size_t line_len = (size_t)(newline - rx_buf);
			*newline = 0x00;
			if (line_len > 0 && rx_buf[line_len-1] == '\r')
				rx_buf[line_len-1] = 0x00;

			if (rx_discard)
				rx_discard = false;
			else
				handle_line(rx_buf);

			if (!client_connected)
				return;

			rx_len -= (line_len + 1);
			memmove(rx_buf, newline + 1, rx_len);
			continue;
		}

		if (rx_len == sizeof(rx_buf))
		{
			// Keep discarding until the end of this line.
			if (!rx_discard)
				send_line("ERR line too long");
			rx_discard = true;
			rx_len = 0;
			continue;
		}

		DWORD received = 0;
		if (!ReadFile(hPipe, &rx_buf[rx_len], (DWORD)(sizeof(rx_buf) - rx_len), &received, NULL))
		{
			if (GetLastError() != ERROR_NO_DATA)
				drop_client();
			return;
		}

		if (received == 0)
			return;
		rx_len += received;
	}
}


/** Public functions. **/


int control_init(const char *name)
{
	const size_t len = strlen(name);
	bool valid = (len >= 1 && len <= 63);
	for (size_t i = 0; valid && i < len; i++)
	{
		const char c = name[i];
		valid = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.');
	}

	if (!valid)
	{
		LOG_MSG(gens, LOG_MSG_LEVEL_ERROR, "Invalid control pipe name.");
		return -1;
	}

	char path[128];
	szprintf(path, sizeof(path), "\\\\.\\pipe\\%s", name);

	// The default security descriptor only allows the current user and
	// administrators to write to the pipe.
	hPipe = CreateNamedPipeA(path,
				 PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
				 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT |
				 PIPE_REJECT_REMOTE_CLIENTS,
				 1, PIPE_BUFFER_SIZE, PIPE_BUFFER_SIZE, 0, NULL);
	if (hPipe == INVALID_HANDLE_VALUE)
	{
		LOG_MSG(gens, LOG_MSG_LEVEL_ERROR,
			"Could not create the control pipe '%s'. (Error %lu)",
			path, GetLastError());
		return -2;
	}

	return 0;
}


int control_update(void)
{
	if (hPipe == INVALID_HANDLE_VALUE)
		return 0;

	service_client();

	if (pending_frames > 0 && (Game == NULL || IS_DEBUGGING()))
	{
		// The game was closed from the window, or the debugger was started.
		pending_frames = 0;
		reply("ERR step interrupted");
	}

	if (!held || Game == NULL || IS_DEBUGGING())
		return 0;

	if (pending_frames == 0)
	{
		Sleep(1);
		return 1;
	}

	log_msgbox_disabled = 1;
	for (int i = 0; i < FRAMES_PER_ITERATION && pending_frames > 0; i++)
	{
		input_update_controllers();
		Update_Frame();

		// Nothing plays this frame's sound. WAV dumps have already got it.
		memset(Seg_L, 0x00, sizeof(Seg_L));
		memset(Seg_R, 0x00, sizeof(Seg_R));

		pending_frames--;
		frames_run++;
	}
	log_msgbox_disabled = 0;

	vdraw_flip(1);

	if (pending_frames == 0)
		reply("OK frames=%u", frames_run);

	return 1;
}


void control_end(void)
{
	if (hPipe == INVALID_HANDLE_VALUE)
		return;

	if (client_connected)
		DisconnectNamedPipe(hPipe);
	client_connected = false;

	CloseHandle(hPipe);
	hPipe = INVALID_HANDLE_VALUE;
}
