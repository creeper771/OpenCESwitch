/*
HOST_GL.C

The guest's window and OpenGL ES, on switch-mesa's EGL.

The renderer (port/linux/src/d3d8_gl.c, built for OpenGL ES 3) opens its
window and context through SDL3 (sdl_platform.c): the SDL calls the guest
image bridges arrive here, and there is one window, the Switch's default
(nwindowGetDefault, 1280x720, which the system scales when docked). The
host's text console draws on that window too, so it closes once the guest
makes its context; the log then goes to host.txt only.

The guest's generated OpenGL ES entry points import hostgl_<function>,
which host_gl_resolve finds through eglGetProcAddress. host_gl_get_string,
host_gl_has_extension, host_gl_read_buffer, host_gl_fence_frame,
host_gl_wait_frame and host_gl_buffer_write are the Android host's
(port/android/host/host_gl.c), for what a guest pointer cannot hold.
*/

#include "host.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

/* SDL3's SDL_GLAttr values the guest sets */
enum
{
	SDL_GL_RED_SIZE = 0,
	SDL_GL_GREEN_SIZE = 1,
	SDL_GL_BLUE_SIZE = 2,
	SDL_GL_ALPHA_SIZE = 3,
	SDL_GL_DEPTH_SIZE = 6,
	SDL_GL_STENCIL_SIZE = 7,
	SDL_GL_CONTEXT_MAJOR_VERSION = 17,
	SDL_GL_CONTEXT_MINOR_VERSION = 18,
	SDL_GL_CONTEXT_FLAGS = 19,
	SDL_GL_ATTRIBUTE_COUNT = 28
};

/* the guest's window and context handles */
#define WINDOW_HANDLE 1
#define CONTEXT_HANDLE 1

static int attributes[SDL_GL_ATTRIBUTE_COUNT] =
{
	[SDL_GL_RED_SIZE] = 8,
	[SDL_GL_GREEN_SIZE] = 8,
	[SDL_GL_BLUE_SIZE] = 8,
	[SDL_GL_ALPHA_SIZE] = 8,
	[SDL_GL_DEPTH_SIZE] = 16,
	[SDL_GL_CONTEXT_MAJOR_VERSION] = 3,
	[SDL_GL_CONTEXT_MINOR_VERSION] = 0,
};

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;
static EGLConfig config;
static int window_made;
/* Mesa's GL thread (port/switch/mesa: built with port/switch/mesa/build_mesa.sh),
which takes the driver's work off the game's main thread; on unless
sdmc:/switch/opence/no_glthread exists. (It needs the renderer's persistently
mapped streaming buffers, host_gl_persistent_buffer: copied through its
queue, each frame's vertices, about 5 MB, filled it and made the campaign
slower than without it.) */
static int glthread;
#define NO_GLTHREAD_PATH "sdmc:/switch/opence/no_glthread"
/* under the GL thread, glBufferSubData is queued without waiting for the
thread only below this size (Mesa's MARSHAL_MAX_CMD_SIZE, less its command) */
#define QUEUED_WRITE_BYTES 8000u
/* the game's swap interval (display.vsync), and when the next frame may be
shown under the host's limit */
static int wanted_interval = 1;
static uint64_t next_frame;

/* Mesa's Switch surface has two buffers, and with a swap interval of 1 a
frame that misses a refresh waits for the next to get one back
(nwindowDequeueBuffer): a frame of 17 ms took 33, the game dropped from 60
frames a second to 30 and back, and the campaign's profile spent 15% of
its main thread there. The window's interval is therefore 0, and the host
keeps the game to 60 frames a second itself: a quicker frame waits for its
turn, a slower one is shown when it is done. */
#define FRAME_NANOSECONDS 16666667ULL

void *host_gl_resolve(const char *name)
{
	return (void *)eglGetProcAddress(name);
}

/* ---------- the window and context (the guest's SDL video calls) */

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	(void)title;
	(void)flags;
	host_logf(HOST_LOG_INFO, "window: the game asked for %dx%d; the Switch's is %dx%d", width, height,
		WINDOW_WIDTH, WINDOW_HEIGHT);
	window_made = 1;
	return WINDOW_HANDLE;
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	(void)window;
	*width = window_made ? WINDOW_WIDTH : 0;
	*height = window_made ? WINDOW_HEIGHT : 0;
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	(void)window;
	(void)enabled;
	return 1;
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	if (attribute < 0 || attribute >= SDL_GL_ATTRIBUTE_COUNT)
		return 0;
	attributes[attribute] = value;
	return 1;
}

/* EGL on the window, with a configuration from the guest's attributes */
static int egl_start(void)
{
	EGLint count = 0;
	const EGLint config_attributes[] =
	{
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, attributes[SDL_GL_RED_SIZE],
		EGL_GREEN_SIZE, attributes[SDL_GL_GREEN_SIZE],
		EGL_BLUE_SIZE, attributes[SDL_GL_BLUE_SIZE],
		EGL_ALPHA_SIZE, attributes[SDL_GL_ALPHA_SIZE],
		EGL_DEPTH_SIZE, attributes[SDL_GL_DEPTH_SIZE],
		EGL_STENCIL_SIZE, attributes[SDL_GL_STENCIL_SIZE],
		EGL_NONE
	};

	if (display != EGL_NO_DISPLAY)
		return 1;
	/* (the console drew on the window EGL now takes) */
	host_console_close();
	display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL))
	{
		host_logf(HOST_LOG_ERROR, "EGL cannot start: 0x%x", eglGetError());
		display = EGL_NO_DISPLAY;
		return 0;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count < 1)
	{
		host_logf(HOST_LOG_ERROR, "EGL has no configuration for the window: 0x%x", eglGetError());
		return 0;
	}
	surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)nwindowGetDefault(), NULL);
	if (surface == EGL_NO_SURFACE)
	{
		host_logf(HOST_LOG_ERROR, "EGL cannot draw on the window: 0x%x", eglGetError());
		return 0;
	}
	return 1;
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	const EGLint context_attributes[] =
	{
		EGL_CONTEXT_MAJOR_VERSION, attributes[SDL_GL_CONTEXT_MAJOR_VERSION],
		EGL_CONTEXT_MINOR_VERSION, attributes[SDL_GL_CONTEXT_MINOR_VERSION],
		EGL_NONE
	};

	if (window != WINDOW_HANDLE || !egl_start())
		return 0;
	if (context != EGL_NO_CONTEXT)
		return CONTEXT_HANDLE;
	{
		FILE *flag = fopen(NO_GLTHREAD_PATH, "r");

		glthread = flag == NULL;
		if (flag)
			fclose(flag);
		setenv("mesa_glthread", glthread ? "true" : "false", 1);
	}
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
	if (context == EGL_NO_CONTEXT)
	{
		host_logf(HOST_LOG_WARN, "EGL cannot make an OpenGL ES %d.%d context: 0x%x",
			attributes[SDL_GL_CONTEXT_MAJOR_VERSION], attributes[SDL_GL_CONTEXT_MINOR_VERSION], eglGetError());
		return 0;
	}
	host_logf(HOST_LOG_INFO, "OpenGL ES %d.%d context%s", attributes[SDL_GL_CONTEXT_MAJOR_VERSION],
		attributes[SDL_GL_CONTEXT_MINOR_VERSION], glthread ? ", with Mesa's GL thread (if this Mesa has it)" : "");
	return CONTEXT_HANDLE;
}

int host_sdl_gl_make_current(uint32_t window, uint32_t handle)
{
	(void)window;
	if (display == EGL_NO_DISPLAY)
		return 0;
	if (!eglMakeCurrent(display, handle ? surface : EGL_NO_SURFACE, handle ? surface : EGL_NO_SURFACE,
		handle ? context : EGL_NO_CONTEXT))
	{
		host_logf(HOST_LOG_ERROR, "EGL cannot make the context current: 0x%x", eglGetError());
		return 0;
	}
	if (handle)
	{
		host_logf(HOST_LOG_INFO, "OpenGL ES: %s; %s; %s", (const char *)glGetString(GL_VENDOR),
			(const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
	}
	return 1;
}

int host_sdl_gl_set_swap_interval(int interval)
{
	wanted_interval = interval;
	return display != EGL_NO_DISPLAY && eglSwapInterval(display, 0);
}

int host_sdl_gl_swap_window(uint32_t window)
{
	uint64_t start = host_now();
	int result;

	(void)window;
	if (wanted_interval > 0)
	{
		if (start < next_frame)
		{
			svcSleepThread((int64_t)(next_frame - start));
			next_frame += FRAME_NANOSECONDS;
		}
		else
		{
			/* (behind: the next frame keeps its full time) */
			next_frame = start + FRAME_NANOSECONDS;
		}
	}
	result = display != EGL_NO_DISPLAY && eglSwapBuffers(display, surface);
	HOST_ADD(swap_time, host_now() - start);
	return result;
}

/* the shutdown, on the thread that drew (the game's main thread): EGL lets
go of the window and of the GPU's memory */
void host_gl_stop(void)
{
	if (display == EGL_NO_DISPLAY)
		return;
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (context != EGL_NO_CONTEXT)
		eglDestroyContext(display, context);
	if (surface != EGL_NO_SURFACE)
		eglDestroySurface(display, surface);
	eglTerminate(display);
	context = EGL_NO_CONTEXT;
	surface = EGL_NO_SURFACE;
	display = EGL_NO_DISPLAY;
}

/* a message for the player: the host's log (there is no message box yet) */
int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	(void)flags;
	host_logf(HOST_LOG_WARN, "message: %s: %s", title ? title : "", message ? message : "");
	return 1;
}

/* ---------- as the Android host's */

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	const GLubyte *text = index >= 0 ? glGetStringi(name, (GLuint)index) : glGetString(name);

	if (!size)
		return;
	buffer[0] = 0;
	if (text)
	{
		strncpy(buffer, (const char *)text, size - 1);
		buffer[size - 1] = 0;
	}
}

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

/* copies size bytes of a buffer object: ES has no glGetBufferSubData */
void host_gl_read_buffer(uint32_t buffer, uint32_t offset, uint32_t size, void *data)
{
	uint64_t start = host_now();
	const void *mapping;

	glBindBuffer(GL_COPY_READ_BUFFER, buffer);
	mapping = glMapBufferRange(GL_COPY_READ_BUFFER, offset, size, GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(data, mapping, size);
		glUnmapBuffer(GL_COPY_READ_BUFFER);
	}
	else
	{
		memset(data, 0, size);
	}
	glBindBuffer(GL_COPY_READ_BUFFER, 0);
	HOST_ADD(readback_time, host_now() - start);
}

/* a fence at the end of each frame's work, so that the renderer writes a
ring buffer again only once the GPU is done with it (d3d8_gl.c) */
#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

void host_gl_fence_frame(uint32_t slot)
{
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void host_gl_wait_frame(uint32_t slot)
{
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	/* at most a second: a lost context must not hang the game */
	{
		uint64_t start = host_now();

		glClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
		HOST_ADD(fence_time, host_now() - start);
	}
	glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

/* writes data into the buffer bound to target, unsynchronized: the renderer
only writes ranges no queued draw reads (the Android host's comment explains
why without invalidation) */
void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	void *mapping;

	/* the GL thread would stop for the mapping, and for a larger
	glBufferSubData: queued pieces instead */
	if (glthread)
	{
		const uint8_t *bytes = data;

		while (size)
		{
			uint32_t piece = size < QUEUED_WRITE_BYTES ? size : QUEUED_WRITE_BYTES;

			glBufferSubData(target, offset, piece, bytes);
			offset += piece;
			bytes += piece;
			size -= piece;
		}
		return;
	}
	mapping = glMapBufferRange(target, offset, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);

	if (!mapping)
	{
		glBufferSubData(target, offset, size, data);
		return;
	}
	memcpy(mapping, data, size);
	glUnmapBuffer(target);
}

/* ---------- persistently mapped buffers

The renderer's streaming buffers (d3d8_gl.c), where the extension is there:
their storage is mapped once, for good, so that each draw's vertices are a
copy into it rather than a mapping of the buffer, or a glBufferSubData,
per draw. The renderer only writes ranges the GPU no longer reads (its ring
of buffers and their fences). */
#define PERSISTENT_BUFFERS 16
#define PERSISTENT_ACCESS (GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT)

static struct
{
	GLuint name;
	uint8_t *mapping;
	uint32_t size;
} persistent_buffers[PERSISTENT_BUFFERS];
static int persistent_count;

uint32_t host_gl_persistent_buffer(uint32_t target, uint32_t size)
{
	static PFNGLBUFFERSTORAGEEXTPROC buffer_storage;
	static int checked;
	GLuint name = 0;
	void *mapping;

	if (!checked)
	{
		checked = 1;
		if (host_gl_has_extension("GL_EXT_buffer_storage"))
			buffer_storage = (PFNGLBUFFERSTORAGEEXTPROC)eglGetProcAddress("glBufferStorageEXT");
	}
	if (!buffer_storage || persistent_count == PERSISTENT_BUFFERS)
		return 0;
	glGenBuffers(1, &name);
	glBindBuffer(target, name);
	buffer_storage(target, size, NULL, PERSISTENT_ACCESS);
	mapping = glMapBufferRange(target, 0, size, PERSISTENT_ACCESS);
	if (!mapping)
	{
		glBindBuffer(target, 0);
		glDeleteBuffers(1, &name);
		return 0;
	}
	persistent_buffers[persistent_count].name = name;
	persistent_buffers[persistent_count].mapping = mapping;
	persistent_buffers[persistent_count].size = size;
	persistent_count++;
	return name;
}

void host_gl_persistent_write(uint32_t buffer, uint32_t offset, uint32_t size, const void *data)
{
	int index;

	for (index = 0; index < persistent_count; index++)
	{
		if (persistent_buffers[index].name == buffer)
		{
			if (offset <= persistent_buffers[index].size && size <= persistent_buffers[index].size - offset)
				memcpy(persistent_buffers[index].mapping + offset, data, size);
			return;
		}
	}
}
