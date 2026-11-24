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

/**
 * Every emulated frame is recorded, however fast or slow the emulation runs:
 * the picture is piped to one ffmpeg process and the sound to another. The
 * picture is kept at its original size without loss, which is quick enough
 * not to slow down the emulation.
 *
 * When the recording stops, a third ffmpeg run scales the picture up to the
 * chosen size, encodes it, and joins it with the sound into the output file.
 * That run can take a while for large sizes, so it goes on in the background
 * while the emulation continues.
 *
 * The two encoders write Matroska files, which stay readable if Gens64 stops
 * in the middle of a recording.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "video_rec.hpp"

// C includes.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// C++ includes.
#include <string>
#include <vector>
using std::string;
using std::vector;

#ifdef _WIN32
#include "libgsft/w32u/w32u_windows.h"
#include "libgsft/w32u/w32u_libc.h"
#include "libgsft/w32u/w32u_charset.h"
#ifndef CREATE_NO_WINDOW
#define CREATE_NO_WINDOW 0x08000000
#endif
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

// libgsft includes.
#include "libgsft/gsft_szprintf.h"

#include "emulator/g_main.hpp"
#include "gens_core/mem/mem_m68k.h"
#include "gens_core/vdp/vdp_io.h"
#include "gens_core/vdp/vdp_rend.h"
#include "gens_core/vdp/TAB336.h"
#include "util/file/rom.hpp"
#include "gens/gens_window_sync.hpp"

// Video and audio handlers.
#include "video/vdraw.h"
#include "audio/audio.h"

// The picture is always this wide. Narrower modes are centered.
#define VREC_WIDTH 320

// How long the messages stay on the screen, in milliseconds.
#define MSG_SHORT 1500
#define MSG_LONG 5000

int VRec_Recording = 0;
char VRec_FFmpeg_Path[GENS_PATH_MAX] = "ffmpeg";
int VRec_Size = VREC_SIZE_1440P;

// Frame size of each output size. 0 means the picture scaled up 2x.
static const struct
{
	const char *name;
	int width, height;
} sizes[VREC_SIZE_COUNT] =
{
	{"2x", 0, 0},
	{"1080p", 1920, 1080},
	{"1440p", 2560, 1440},
	{"4K", 3840, 2160},
};


// The size, or the default if it is out of range.
static int valid_size(int size)
{
	return (size >= 0 && size < VREC_SIZE_COUNT ? size : VREC_SIZE_1440P);
}


const char *vrec_size_name(int size)
{
	return sizes[valid_size(size)].name;
}


int vrec_size_from_name(const char *name)
{
	for (int i = 0; i < VREC_SIZE_COUNT; i++)
	{
		if (!strcasecmp(sizes[i].name, name))
			return i;
	}
	return -1;
}


/** Child processes. **/

#ifdef _WIN32
typedef HANDLE log_handle_t;
#define NO_LOG INVALID_HANDLE_VALUE

typedef struct
{
	HANDLE process;
	HANDLE input;	// Write end of the child's stdin.
} child_t;
#else
typedef int log_handle_t;
#define NO_LOG (-1)

typedef struct
{
	pid_t pid;
	int input;	// Write end of the child's stdin.
} child_t;

// A write to an ffmpeg that has exited raises SIGPIPE, which Gens64
// otherwise treats as a crash.
static struct sigaction old_sigpipe;
#endif

static const child_t no_child =
#ifdef _WIN32
	{NULL, INVALID_HANDLE_VALUE};
#else
	{-1, -1};
#endif

static string last_error;


/**
 * failed(): Remember why something failed.
 * @return -1.
 */
static int failed(const string& reason)
{
	last_error = reason;
	return -1;
}


static string not_found_message(void)
{
	return string("ffmpeg was not found (\"") + VRec_FFmpeg_Path + "\"). "
	       "Install ffmpeg and put it on the PATH"
#ifdef _WIN32
	       " or next to gens.exe"
#endif
	       ", or set \"FFmpeg Path\" in the [Options] section of gens.cfg.";
}


#ifdef _WIN32

/**
 * append_argument(): Add an argument to a command line, quoted the way that
 * the Microsoft C runtime splits it again.
 */
static void append_argument(string& cmd, const string& arg)
{
	if (!cmd.empty())
		cmd += ' ';

	if (!arg.empty() && arg.find_first_of(" \t\"") == string::npos)
	{
		cmd += arg;
		return;
	}

	cmd += '"';
	size_t backslashes = 0;
	for (size_t i = 0; i < arg.size(); i++)
	{
		const char c = arg[i];
		if (c == '\\')
		{
			backslashes++;
			continue;
		}

		if (c == '"')
		{
			cmd.append((backslashes * 2) + 1, '\\');
		}
		else
		{
			cmd.append(backslashes, '\\');
		}
		cmd += c;
		backslashes = 0;
	}
	cmd.append(backslashes * 2, '\\');
	cmd += '"';
}


static log_handle_t open_log(const string& filename)
{
	wchar_t *wfilename = w32u_UTF8toUTF16(filename.c_str());
	if (!wfilename)
		return NO_LOG;

	// The children inherit this handle as their stdout and stderr.
	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE log = CreateFileW(wfilename, FILE_APPEND_DATA,
				 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				 &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	free(wfilename);
	return log;
}


static void close_log(log_handle_t log)
{
	if (log != NO_LOG)
		CloseHandle(log);
}


/**
 * child_start(): Start a program with a pipe to its stdin.
 * @param background If true, run it at a lower priority than Gens64.
 * @return 0 on success; -1 on error.
 */
static int child_start(child_t *child, const vector<string>& args, log_handle_t log, bool background)
{
	*child = no_child;

	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE read_end, write_end;
	if (!CreatePipe(&read_end, &write_end, &sa, 1024 * 1024))
		return failed("Could not create a pipe for ffmpeg.");

	// Only the read end goes to the child. If the write end were inherited
	// too, the child would never see the end of its input.
	SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);

	string cmd;
	for (size_t i = 0; i < args.size(); i++)
		append_argument(cmd, args[i]);
	wchar_t *wcmd = w32u_UTF8toUTF16(cmd.c_str());

	STARTUPINFOW si;
	memset(&si, 0x00, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = read_end;
	si.hStdOutput = log;
	si.hStdError = log;

	PROCESS_INFORMATION pi;
	const BOOL started = (wcmd != NULL &&
			      CreateProcessW(NULL, wcmd, NULL, NULL, TRUE,
					     CREATE_NO_WINDOW | (background ? BELOW_NORMAL_PRIORITY_CLASS : 0),
					     NULL, NULL, &si, &pi));
	const DWORD err = GetLastError();
	free(wcmd);
	CloseHandle(read_end);

	if (!started)
	{
		CloseHandle(write_end);
		if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
			return failed(not_found_message());

		char msg[128];
		szprintf(msg, sizeof(msg), "Could not start ffmpeg. (Windows error %lu)", (unsigned long)err);
		return failed(msg);
	}

	CloseHandle(pi.hThread);
	child->process = pi.hProcess;
	child->input = write_end;
	return 0;
}


static bool child_write(child_t *child, const void *data, size_t size)
{
	const char *p = (const char*)data;
	while (size > 0)
	{
		DWORD written;
		if (!WriteFile(child->input, p, (DWORD)size, &written, NULL) || written == 0)
			return false;
		p += written;
		size -= written;
	}
	return true;
}


/**
 * child_finish(): Close the child's stdin, and wait for it to exit.
 * @return Exit code, or -1 if it could not be found.
 */
static int child_finish(child_t *child)
{
	if (child->input != INVALID_HANDLE_VALUE)
		CloseHandle(child->input);

	int code = -1;
	if (child->process != NULL)
	{
		WaitForSingleObject(child->process, INFINITE);
		DWORD exit_code;
		if (GetExitCodeProcess(child->process, &exit_code))
			code = (int)exit_code;
		CloseHandle(child->process);
	}

	*child = no_child;
	return code;
}


/**
 * child_exited(): Check whether a child has exited, without waiting.
 * @param code Set to its exit code, as from child_finish(), if it has.
 * @return True if it has exited. The child is then finished.
 */
static bool child_exited(child_t *child, int *code)
{
	if (child->process != NULL && WaitForSingleObject(child->process, 0) == WAIT_TIMEOUT)
		return false;

	*code = child_finish(child);
	return true;
}

#else /* !_WIN32 */

static log_handle_t open_log(const string& filename)
{
	const int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
	if (fd >= 0)
		fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}


static void close_log(log_handle_t log)
{
	if (log != NO_LOG)
		close(log);
}


/**
 * child_start(): Start a program with a pipe to its stdin.
 * @param background If true, run it at a lower priority than Gens64.
 * @return 0 on success; -1 on error.
 */
static int child_start(child_t *child, const vector<string>& args, log_handle_t log, bool background)
{
	*child = no_child;

	// data: the child's stdin.
	// status: closed by a successful exec, or carries its errno.
	int data[2], status[2];
	if (pipe(data) != 0)
		return failed("Could not create a pipe for ffmpeg.");
	if (pipe(status) != 0)
	{
		close(data[0]);
		close(data[1]);
		return failed("Could not create a pipe for ffmpeg.");
	}

	// The write end must not leak into other children, or this child
	// would never see the end of its input.
	fcntl(data[1], F_SETFD, FD_CLOEXEC);
	fcntl(status[0], F_SETFD, FD_CLOEXEC);
	fcntl(status[1], F_SETFD, FD_CLOEXEC);

	vector<char*> argv;
	for (size_t i = 0; i < args.size(); i++)
		argv.push_back(const_cast<char*>(args[i].c_str()));
	argv.push_back(NULL);

	const pid_t pid = fork();
	if (pid == 0)
	{
		// Child process.
		dup2(data[0], STDIN_FILENO);
		if (log != NO_LOG)
		{
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
		}
		if (data[0] > STDERR_FILENO)
			close(data[0]);
		if (background)
		{
			int ignored_nice = nice(10);
			(void)ignored_nice;
		}

		execvp(argv[0], &argv[0]);

		const int err = errno;
		ssize_t ignored = write(status[1], &err, sizeof(err));
		(void)ignored;
		_exit(127);
	}

	close(data[0]);
	close(status[1]);

	if (pid < 0)
	{
		close(data[1]);
		close(status[0]);
		return failed("Could not start ffmpeg.");
	}

	int err = 0;
	ssize_t n;
	do
	{
		n = read(status[0], &err, sizeof(err));
	} while (n < 0 && errno == EINTR);
	close(status[0]);

	if (n > 0)
	{
		// exec failed.
		close(data[1]);
		waitpid(pid, NULL, 0);
		if (err == ENOENT)
			return failed(not_found_message());
		return failed(string("Could not start ffmpeg: ") + strerror(err));
	}

	child->pid = pid;
	child->input = data[1];
	return 0;
}


static bool child_write(child_t *child, const void *data, size_t size)
{
	const char *p = (const char*)data;
	while (size > 0)
	{
		const ssize_t written = write(child->input, p, size);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		p += written;
		size -= (size_t)written;
	}
	return true;
}


/**
 * child_finish(): Close the child's stdin, and wait for it to exit.
 * @return Exit code, or -1 if it could not be found.
 */
static int child_finish(child_t *child)
{
	if (child->input >= 0)
		close(child->input);

	int code = -1;
	if (child->pid > 0)
	{
		int status;
		pid_t rval;
		do
		{
			rval = waitpid(child->pid, &status, 0);
		} while (rval < 0 && errno == EINTR);

		if (rval == child->pid && WIFEXITED(status))
			code = WEXITSTATUS(status);
	}

	*child = no_child;
	return code;
}


/**
 * child_exited(): Check whether a child has exited, without waiting.
 * @param code Set to its exit code, as from child_finish(), if it has.
 * @return True if it has exited. The child is then finished.
 */
static bool child_exited(child_t *child, int *code)
{
	int status = 0;
	pid_t rval = 0;
	if (child->pid > 0)
	{
		do
		{
			rval = waitpid(child->pid, &status, WNOHANG);
		} while (rval < 0 && errno == EINTR);
		if (rval == 0)
			return false;
	}

	// The child is gone now, so child_finish() must not wait for it.
	*code = (rval == child->pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	child->pid = -1;
	child_finish(child);
	return true;
}

#endif /* _WIN32 */


/** Recording state. **/

static struct
{
	child_t video;
	child_t audio;
	log_handle_t log;

	int fps;
	int height;

	bool has_audio;
	int audio_rate;		// Samples per second, as emulated.
	int channels;
	int seg_length;		// Samples per frame.

	string output;
	string video_file;
	string audio_file;
	string log_file;

	vector<uint32_t> canvas;
	unsigned int frames;
} rec;


// A finished recording that ffmpeg is making into the output file.
typedef struct
{
	child_t child;
	log_handle_t log;
	string output;
	string video_file;
	string audio_file;
	string log_file;
} job_t;

// Jobs that run in the background.
static vector<job_t> jobs;


static int frame_rate(void)
{
	return (CPU_Mode ? 50 : 60);
}


static string to_string(int value)
{
	char buf[16];
	szprintf(buf, sizeof(buf), "%d", value);
	return buf;
}


static bool file_exists(const string& filename)
{
	return (access(filename.c_str(), F_OK) == 0);
}


// The file name without its directory, for on-screen messages.
static string base_name(const string& filename)
{
	const size_t slash = filename.find_last_of("/\\");
	return (slash == string::npos ? filename : filename.substr(slash + 1));
}


// The output format is chosen by ffmpeg from the extension.
static bool is_mp4(const string& filename)
{
	const size_t dot = filename.find_last_of('.');
	if (dot == string::npos)
		return false;

	string ext = filename.substr(dot + 1);
	for (size_t i = 0; i < ext.size(); i++)
	{
		if (ext[i] >= 'A' && ext[i] <= 'Z')
			ext[i] += ('a' - 'A');
	}
	return (ext == "mp4" || ext == "m4v" || ext == "mov");
}


static vector<string> ffmpeg_command(void)
{
	vector<string> args;
	args.push_back(VRec_FFmpeg_Path);
	args.push_back("-hide_banner");
	args.push_back("-nostats");
	args.push_back("-loglevel");
	args.push_back("error");
	return args;
}


/**
 * start_encoders(): Start the video and audio encoders.
 * @return 0 on success; -1 on error.
 */
static int start_encoders(void)
{
	// The picture is kept as it is, without loss, until it is scaled up.
	vector<string> args = ffmpeg_command();
	const char *video_args[] =
	{
		"-f", "rawvideo", "-pix_fmt", "bgr0",
		"-video_size", NULL, "-framerate", NULL, "-i", "-",
		"-c:v", "libx264rgb", "-preset", "ultrafast", "-qp", "0",
		"-y", NULL
	};
	const string size = to_string(VREC_WIDTH) + "x" + to_string(rec.height);
	const string fps = to_string(rec.fps);
	video_args[5] = size.c_str();
	video_args[7] = fps.c_str();
	video_args[sizeof(video_args) / sizeof(video_args[0]) - 1] = rec.video_file.c_str();
	args.insert(args.end(), video_args, video_args + (sizeof(video_args) / sizeof(video_args[0])));

	if (child_start(&rec.video, args, rec.log, false) != 0)
		return -1;

	if (!rec.has_audio)
		return 0;

	// The sound rate is given as the number of samples per frame times the
	// frame rate, which keeps the sound in step with the picture when a
	// frame's worth of samples was rounded. ffmpeg resamples it to 48 kHz.
	args = ffmpeg_command();
	const char *audio_args[] =
	{
		"-f", "s16le", "-ar", NULL, "-ac", NULL, "-i", "-",
		"-c:a", "aac", "-b:a", "192k", "-ar", "48000",
		"-y", NULL
	};
	const string rate = to_string(rec.audio_rate);
	const string channels = to_string(rec.channels);
	audio_args[3] = rate.c_str();
	audio_args[5] = channels.c_str();
	audio_args[sizeof(audio_args) / sizeof(audio_args[0]) - 1] = rec.audio_file.c_str();
	args.insert(args.end(), audio_args, audio_args + (sizeof(audio_args) / sizeof(audio_args[0])));

	if (child_start(&rec.audio, args, rec.log, false) != 0)
	{
		child_finish(&rec.video);
		return -1;
	}

	return 0;
}


/**
 * scale_filter(): The filters that scale the recorded picture up to the
 * chosen size, without smoothing, and convert it to video colors.
 */
static string scale_filter(void)
{
	const int size = valid_size(VRec_Size);
	if (sizes[size].height == 0)
	{
		// Standard definition. The 2x scale keeps the pixel edges sharp
		// through the encoder's color subsampling.
		return "scale=iw*2:ih*2:flags=neighbor:out_color_matrix=bt601:out_range=tv,format=yuv420p";
	}

	// High definition: the largest whole-number scale that fits, centered
	// in the frame. YouTube, for one, gives more detail and 60 fps only to
	// videos of 720 lines or more.
	int scale = sizes[size].height / rec.height;
	if (scale * VREC_WIDTH > sizes[size].width)
		scale = sizes[size].width / VREC_WIDTH;

	return "scale=" + to_string(VREC_WIDTH * scale) + ":" + to_string(rec.height * scale) +
	       ":flags=neighbor:out_color_matrix=bt709:out_range=tv,format=yuv420p,pad=" +
	       to_string(sizes[size].width) + ":" + to_string(sizes[size].height) + ":-1:-1";
}


/**
 * start_output(): Start making the output file from the recording.
 * @param job Filled in from the recording.
 * @return 0 on success; -1 if ffmpeg could not be started.
 */
static int start_output(job_t *job)
{
	const bool hd = (sizes[valid_size(VRec_Size)].height != 0);

	vector<string> args = ffmpeg_command();
	args.push_back("-nostdin");
	args.push_back("-i");
	args.push_back(rec.video_file);
	if (rec.has_audio)
	{
		args.push_back("-i");
		args.push_back(rec.audio_file);
	}
	args.push_back("-map");
	args.push_back("0:v:0");
	if (rec.has_audio)
	{
		args.push_back("-map");
		args.push_back("1:a:0");
	}

	// Every frame was recorded, at a fixed rate.
	const char *video_args[] =
	{
		"-vf", NULL, "-r", NULL,
		"-c:v", "libx264", "-preset", "fast", "-crf", "16",
		"-colorspace", NULL, "-color_primaries", NULL, "-color_trc", NULL, "-color_range", "tv",
		"-c:a", "copy",
	};
	const string filter = scale_filter();
	const string fps = to_string(rec.fps);
	const char *colors = (hd ? "bt709" : (rec.fps == 50 ? "bt470bg" : "smpte170m"));
	video_args[1] = filter.c_str();
	video_args[3] = fps.c_str();
	video_args[11] = colors;
	video_args[13] = colors;
	video_args[15] = (hd ? "bt709" : "smpte170m");
	args.insert(args.end(), video_args, video_args + (sizeof(video_args) / sizeof(video_args[0])));

	if (is_mp4(rec.output))
	{
		args.push_back("-video_track_timescale");
		args.push_back(fps);
		args.push_back("-movflags");
		args.push_back("+faststart");
	}

	args.push_back("-y");
	args.push_back(rec.output);

	job->log = rec.log;
	job->output = rec.output;
	job->video_file = rec.video_file;
	job->audio_file = rec.audio_file;
	job->log_file = rec.log_file;
	// The emulation goes on meanwhile, and comes first.
	return child_start(&job->child, args, job->log, true);
}


/**
 * end_job(): Clean up after a finished job, and say how it went.
 * @param code ffmpeg's exit code.
 * @param reason If not NULL, why the recording stopped on its own.
 * @return 0 on success; -1 on error.
 */
static int end_job(job_t *job, int code, const char *reason)
{
	close_log(job->log);
	job->log = NO_LOG;

	string msg;
	int rval = 0;
	if (code == 0)
	{
		unlink(job->video_file.c_str());
		unlink(job->audio_file.c_str());
		unlink(job->log_file.c_str());
		msg = "Video saved: " + base_name(job->output);
	}
	else
	{
		msg = "Video recording failed: ffmpeg could not write the video file. See " +
		      base_name(job->log_file) + ".";
		rval = failed("Video recording failed: ffmpeg could not write the video file. See " +
			      job->log_file + ".");
	}

	if (reason != NULL)
		msg = string(reason) + " " + msg;
	vdraw_text_write(msg.c_str(), (rval == 0 ? MSG_SHORT : MSG_LONG));
	return rval;
}


/**
 * capture_frame(): Copy the emulated picture to the canvas as 32-bit color.
 */
static void capture_frame(void)
{
	uint32_t *canvas = &rec.canvas[0];

	int width = vdp_getHPix();
	int lines = VDP_Lines.Visible.Total;
	if (width <= 0 || width > VREC_WIDTH || lines <= 0 || lines > 240)
		return;

	// Same position as in the MDP_EVENT_POST_FRAME event.
	int offset = TAB336[VDP_Lines.Visible.Border_Size] + 8;
	if (width < VREC_WIDTH)
		offset += ((VREC_WIDTH - width) / 2);

	// The canvas size was set when the recording started.
	// A smaller picture is centered, a taller one cropped.
	int src_y = 0, dst_y = 0, rows = lines;
	if (lines > rec.height)
	{
		src_y = (lines - rec.height) / 2;
		rows = rec.height;
	}
	else
	{
		dst_y = (rec.height - lines) / 2;
	}

	if (width != VREC_WIDTH || lines != rec.height)
		memset(canvas, 0x00, rec.canvas.size() * sizeof(uint32_t));

	const int x0 = (VREC_WIDTH - width) / 2;
	for (int y = 0; y < rows; y++)
	{
		uint32_t *dst = &canvas[((dst_y + y) * VREC_WIDTH) + x0];
		const int src = offset + ((src_y + y) * 336);

		if (bppMD == 32)
		{
			memcpy(dst, &MD_Screen.u32[src], width * sizeof(uint32_t));
			continue;
		}

		const uint16_t *line = &MD_Screen.u16[src];
		for (int x = 0; x < width; x++)
		{
			const unsigned int px = line[x];
			unsigned int r, g, b;
			if (bppMD == 16)
			{
				// RGB565
				r = (px >> 11) & 0x1F;
				g = (px >> 5) & 0x3F;
				b = px & 0x1F;
				g = (g << 2) | (g >> 4);
			}
			else
			{
				// RGB555
				r = (px >> 10) & 0x1F;
				g = (px >> 5) & 0x1F;
				b = px & 0x1F;
				g = (g << 3) | (g >> 2);
			}
			r = (r << 3) | (r >> 2);
			b = (b << 3) | (b >> 2);
			dst[x] = (r << 16) | (g << 8) | b;
		}
	}
}


/** Public functions. **/


int vrec_start(const char *filename)
{
	last_error.clear();

	if (!Game)
		return failed("No game is loaded.");
	if (VRec_Recording)
		return failed("A video is already being recorded.");

#ifdef GENS_OS_WIN32
	// Make sure relative pathnames are handled correctly on Win32.
	// (ffmpeg runs in the same directory.)
	pSetCurrentDirectoryU(PathNames.Gens_Save_Path);
#endif

	// File names.
	string base;
	if (filename != NULL && filename[0] != 0x00)
	{
		rec.output = filename;
		base = rec.output;
	}
	else
	{
		for (int num = 0; ; num++)
		{
			char buf[GENS_PATH_MAX];
			szprintf(buf, sizeof(buf), "%s%s_%03d", PathNames.Video_Dir, ROM_Filename, num);
			base = buf;
			if (!file_exists(base + ".mp4") && !file_exists(base + ".video.mkv"))
				break;
		}
		rec.output = base + ".mp4";
	}
	rec.video_file = base + ".video.mkv";
	rec.audio_file = base + ".audio.mka";
	rec.log_file = base + ".log";

	// Recording format.
	rec.fps = frame_rate();
	rec.height = VDP_Lines.Visible.Total;
	if (rec.height <= 0 || rec.height > 240)
		rec.height = 224;

	rec.has_audio = (audio_get_enabled() && audio_write_sound_buffer != NULL);
	if (rec.has_audio)
	{
		rec.seg_length = audio_seg_length;
		rec.channels = (audio_get_stereo() ? 2 : 1);
		rec.audio_rate = rec.seg_length * rec.fps;
	}

	rec.log = open_log(rec.log_file);
	if (rec.log == NO_LOG)
		return failed("Could not create " + rec.log_file + ".");

#ifndef _WIN32
	struct sigaction ignore;
	memset(&ignore, 0x00, sizeof(ignore));
	ignore.sa_handler = SIG_IGN;
	sigemptyset(&ignore.sa_mask);
	sigaction(SIGPIPE, &ignore, &old_sigpipe);
#endif

	if (start_encoders() != 0)
	{
#ifndef _WIN32
		sigaction(SIGPIPE, &old_sigpipe, NULL);
#endif
		close_log(rec.log);
		rec.log = NO_LOG;
		unlink(rec.log_file.c_str());
		unlink(rec.video_file.c_str());
		unlink(rec.audio_file.c_str());
		return -1;
	}

	rec.canvas.assign(VREC_WIDTH * rec.height, 0);
	rec.frames = 0;
	VRec_Recording = 1;

	vdraw_text_write("Video recording started.", MSG_SHORT);
	Sync_Gens_Window_GraphicsMenu();
	return 0;
}


/**
 * keep_window_alive(): Handle the window's messages while Gens64 waits,
 * so that Windows doesn't mark it as not responding. Input, menu commands,
 * dropped files and close requests are dropped: Gens64 can't act on them
 * until the video is done.
 */
static void keep_window_alive(void)
{
#ifdef _WIN32
	MSG msg;
	while (pPeekMessageU(&msg, NULL, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
		{
			// Leave it for the main loop.
			PostQuitMessage((int)msg.wParam);
			return;
		}

		if ((msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) ||
		    (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) ||
		    (msg.message >= WM_NCMOUSEMOVE && msg.message <= WM_NCXBUTTONDBLCLK) ||
		    msg.message == WM_COMMAND || msg.message == WM_SYSCOMMAND ||
		    msg.message == WM_DROPFILES || msg.message == WM_CLOSE)
		{
			continue;
		}

		TranslateMessage(&msg);
		pDispatchMessageU(&msg);
	}
	Sleep(100);
#else
	usleep(100000);
#endif
}


/**
 * wait_job(): Wait for ffmpeg to make a job's video file.
 * Shows how long it has taken, since a long video takes minutes.
 * @param what What Gens64 is doing.
 * @return ffmpeg's exit code.
 */
static int wait_job(job_t *job, const char *what)
{
	const time_t start = time(NULL);
	time_t shown = (time_t)-1;
	int code;
	while (!child_exited(&job->child, &code))
	{
		const time_t now = time(NULL);
		if (now != shown)
		{
			shown = now;
			const int seconds = (int)(now - start);
			char msg[160];
			szprintf(msg, sizeof(msg), "%s %d:%02d", what, seconds / 60, seconds % 60);
			vdraw_text_write(msg, MSG_LONG);
			vdraw_flip(1);
		}
		keep_window_alive();
	}
	return code;
}


/**
 * finish(): End the recording.
 * @param error If not NULL, the recording failed for this reason, and the
 * files are kept as they are.
 * @param reason If not NULL, why the recording stopped on its own.
 * @param wait If true, wait until the output file is made.
 * @return 0 on success; -1 on error.
 */
static int finish(const char *error, const char *reason, bool wait)
{
	VRec_Recording = 0;

	int code = child_finish(&rec.video);
	if (rec.has_audio)
	{
		const int audio_code = child_finish(&rec.audio);
		if (code == 0)
			code = audio_code;
	}

#ifndef _WIN32
	sigaction(SIGPIPE, &old_sigpipe, NULL);
#endif

	rec.canvas.clear();

	string msg;
	int rval = 0;
	job_t job;
	if (error == NULL && code == 0 && rec.frames == 0)
	{
		// Nothing to keep.
		close_log(rec.log);
		unlink(rec.video_file.c_str());
		unlink(rec.audio_file.c_str());
		unlink(rec.log_file.c_str());
		msg = "No frames were recorded.";
		rval = failed(msg);
	}
	else if (error == NULL && code == 0 && start_output(&job) == 0)
	{
		// The job owns the log now.
		rec.log = NO_LOG;
		if (wait)
		{
			const string what = string("Making the ") + vrec_size_name(VRec_Size) + " video...";
			rval = end_job(&job, wait_job(&job, what.c_str()), reason);
		}
		else
		{
			jobs.push_back(job);
			msg = string("Making the ") + vrec_size_name(VRec_Size) + " video...";
			if (reason != NULL)
				msg = string(reason) + " " + msg;
			vdraw_text_write(msg.c_str(), MSG_SHORT);
		}
		Sync_Gens_Window_GraphicsMenu();
		return rval;
	}
	else
	{
		if (error == NULL)
			error = (code != 0 ? "ffmpeg reported an error" : "ffmpeg could not write the video file");
		close_log(rec.log);
		msg = string("Video recording failed: ") + error + ". See " + base_name(rec.log_file) + ".";
		rval = failed(string("Video recording failed: ") + error + ". See " + rec.log_file + ".");
	}
	rec.log = NO_LOG;

	if (reason != NULL)
		msg = string(reason) + " " + msg;
	vdraw_text_write(msg.c_str(), (rval == 0 ? MSG_SHORT : MSG_LONG));
	Sync_Gens_Window_GraphicsMenu();
	return rval;
}


int vrec_stop(int wait)
{
	last_error.clear();
	if (!VRec_Recording)
		return failed("No video is being recorded.");

	return finish(NULL, NULL, (wait != 0));
}


void vrec_poll(void)
{
	for (size_t i = 0; i < jobs.size(); )
	{
		int code;
		if (!child_exited(&jobs[i].child, &code))
		{
			i++;
			continue;
		}

		job_t job = jobs[i];
		jobs.erase(jobs.begin() + i);
		end_job(&job, code, NULL);
	}
}


void vrec_end(void)
{
	if (VRec_Recording)
		finish(NULL, NULL, true);

	// This can take minutes, so say why Gens64 is still here.
	for (size_t i = 0; i < jobs.size(); i++)
		end_job(&jobs[i], wait_job(&jobs[i], "Finishing the video before exiting..."), NULL);
	jobs.clear();
}


void vrec_update(void)
{
	if (!VRec_Recording)
		return;

	// The encoders can't follow a change of frame rate or sound format.
	const bool has_audio = (audio_get_enabled() && audio_write_sound_buffer != NULL);
	if (frame_rate() != rec.fps || has_audio != rec.has_audio ||
	    (has_audio && (audio_seg_length != rec.seg_length ||
			   (audio_get_stereo() ? 2 : 1) != rec.channels)))
	{
		finish(NULL, "The frame rate or sound settings changed.", false);
		return;
	}

	capture_frame();
	bool ok = child_write(&rec.video, &rec.canvas[0], rec.canvas.size() * sizeof(uint32_t));

	if (ok && rec.has_audio)
	{
		// TODO: Byteswap on big-endian.
		short buf[(882 * 2) + 16];
		audio_write_sound_buffer(buf);
		ok = child_write(&rec.audio, buf, rec.seg_length * rec.channels * sizeof(short));
	}

	if (!ok)
	{
		finish("ffmpeg stopped", NULL, false);
		return;
	}

	rec.frames++;
}


const char *vrec_filename(void)
{
	return rec.output.c_str();
}


const char *vrec_last_error(void)
{
	return last_error.c_str();
}
