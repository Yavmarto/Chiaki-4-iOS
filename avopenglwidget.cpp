// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <avopenglwidget.h>
#include <streamsession.h>

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLDebugLogger>
#include <QTimer>

#include <cstring>

extern "C"
{
#include <libavutil/pixdesc.h>
}

#define MOUSE_TIMEOUT_MS 1000

//#define DEBUG_OPENGL

static const char *shader_vert_glsl = R"glsl(
#version 300 es

in vec2 pos_attr;

out vec2 uv_var;

void main()
{
    uv_var = pos_attr;
    gl_Position = vec4(pos_attr * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
)glsl";

static const char *yuv420p_shader_frag_glsl = R"glsl(
#version 300 es

precision mediump float;

uniform sampler2D plane1; // Y
uniform sampler2D plane2; // U
uniform sampler2D plane3; // V

in vec2 uv_var;

out vec4 out_color;

void main()
{
    vec3 yuv = vec3(
        (texture(plane1, uv_var).r - (16.0 / 255.0)) / ((235.0 - 16.0) / 255.0),
        (texture(plane2, uv_var).r - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5,
        (texture(plane3, uv_var).r - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5);
    vec3 rgb = mat3(
        1.0,        1.0,        1.0,
        0.0,        -0.21482,   2.12798,
        1.28033,    -0.38059,   0.0) * yuv;
    out_color = vec4(rgb, 1.0);
}
)glsl";

static const char *nv12_shader_frag_glsl = R"glsl(
#version 300 es

precision mediump float;

uniform sampler2D plane1; // Y
uniform sampler2D plane2; // interlaced UV

in vec2 uv_var;

out vec4 out_color;

void main()
{
    vec3 yuv = vec3(
        (texture(plane1, uv_var).r - (16.0 / 255.0)) / ((235.0 - 16.0) / 255.0),
        (texture(plane2, uv_var).r - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5,
        (texture(plane2, uv_var).g - (16.0 / 255.0)) / ((240.0 - 16.0) / 255.0) - 0.5
    );
    vec3 rgb = mat3(
        1.0,        1.0,        1.0,
        0.0,        -0.21482,   2.12798,
        1.28033,    -0.38059,   0.0) * yuv;
    out_color = vec4(rgb, 1.0);
}
)glsl";

ConversionConfig conversion_configs[] = {
	{
		AV_PIX_FMT_YUV420P,
		shader_vert_glsl,
		yuv420p_shader_frag_glsl,
		3,
		{
			{ 1, 1, 1, GL_R8, GL_RED },
			{ 2, 2, 1, GL_R8, GL_RED },
			{ 2, 2, 1, GL_R8, GL_RED }
		}
	},
	{
		AV_PIX_FMT_NV12,
		shader_vert_glsl,
		nv12_shader_frag_glsl,
		2,
		{
			{ 1, 1, 1, GL_R8, GL_RED },
			{ 2, 2, 2, GL_RG8, GL_RG }
		}
	}
};

static const float vert_pos[] = {
	0.0f, 0.0f,
	0.0f, 1.0f,
	1.0f, 0.0f,
	1.0f, 1.0f
};

QSurfaceFormat AVOpenGLWidget::CreateSurfaceFormat()
{
	QSurfaceFormat format;
	format.setDepthBufferSize(0);
	format.setStencilBufferSize(0);
	format.setVersion(3, 0);
	format.setProfile(QSurfaceFormat::CoreProfile);
#ifdef DEBUG_OPENGL
	format.setOption(QSurfaceFormat::DebugContext, true);
#endif
	return format;
}

AVOpenGLWidget::AVOpenGLWidget(StreamSession *session, QWidget *parent, TransformMode transform_mode)
	: QOpenGLWidget(parent),
	session(session), transform_mode(transform_mode)
{
	enum AVPixelFormat pixel_format = chiaki_ffmpeg_decoder_get_pixel_format(session->GetFfmpegDecoder());
	conversion_config = nullptr;
	for(auto &cc : conversion_configs)
	{
		if(pixel_format == cc.pixel_format)
		{
			conversion_config = &cc;
			break;
		}
	}

	if(!conversion_config)
		throw Exception("No matching video conversion config can be found");

	setFormat(CreateSurfaceFormat());

	memset(tex, 0, sizeof(tex));
	frame_width = frame_height = 0;
	gl_ok = false;
	frames_shown = 0;

	CHIAKI_LOGI(session->GetChiakiLog(), "Video: pixel format %s", av_get_pix_fmt_name(pixel_format));
	// Decoder signals from the network thread; queued to the GUI thread, update() coalesces.
	connect(session, &StreamSession::FfmpegFrameAvailable, this, QOverload<>::of(&QWidget::update), Qt::QueuedConnection);

	setMouseTracking(true);
	mouse_timer = new QTimer(this);
	connect(mouse_timer, &QTimer::timeout, this, &AVOpenGLWidget::HideMouse);
	ResetMouseTimeout();
}

AVOpenGLWidget::~AVOpenGLWidget()
{
	if(gl_ok && context())
	{
		makeCurrent();
		auto f = context()->extraFunctions();
		f->glDeleteTextures(conversion_config->planes, tex);
		f->glDeleteBuffers(1, &vbo);
		f->glDeleteVertexArrays(1, &vao);
		f->glDeleteProgram(program);
		doneCurrent();
	}
}

void AVOpenGLWidget::mouseMoveEvent(QMouseEvent *event)
{
	QOpenGLWidget::mouseMoveEvent(event);
	ResetMouseTimeout();
}

void AVOpenGLWidget::ResetMouseTimeout()
{
	unsetCursor();
	mouse_timer->start(MOUSE_TIMEOUT_MS);
}

void AVOpenGLWidget::HideMouse()
{
	setCursor(Qt::BlankCursor);
}

bool AVOpenGLWidget::UploadFrame(AVFrame *frame)
{
	auto f = QOpenGLContext::currentContext()->extraFunctions();

	if(frame->format != conversion_config->pixel_format)
	{
		CHIAKI_LOGE(session->GetChiakiLog(), "Video: got frame with format %s, expected %s",
				av_get_pix_fmt_name((AVPixelFormat)frame->format), av_get_pix_fmt_name(conversion_config->pixel_format));
		return false;
	}

	f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	for(unsigned int i=0; i<conversion_config->planes; i++)
	{
		const PlaneConfig &pc = conversion_config->plane_configs[i];
		int w = (frame->width + pc.width_divider - 1) / pc.width_divider;
		int h = (frame->height + pc.height_divider - 1) / pc.height_divider;
		// ES3 row length lets GL skip the decoder's line padding, no copy needed
		f->glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[i] / pc.data_per_pixel);
		f->glBindTexture(GL_TEXTURE_2D, tex[i]);
		if(frame_width == (unsigned)frame->width && frame_height == (unsigned)frame->height)
			f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, pc.format, GL_UNSIGNED_BYTE, frame->data[i]);
		else
			f->glTexImage2D(GL_TEXTURE_2D, 0, pc.internal_format, w, h, 0, pc.format, GL_UNSIGNED_BYTE, frame->data[i]);
	}
	f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

	frame_width = frame->width;
	frame_height = frame->height;
	return true;
}

void AVOpenGLWidget::initializeGL()
{
	auto f = QOpenGLContext::currentContext()->extraFunctions();

	const char *gl_version = (const char *)f->glGetString(GL_VERSION);
	CHIAKI_LOGI(session->GetChiakiLog(), "OpenGL initialized with version \"%s\"", gl_version ? gl_version : "(null)");

#ifdef DEBUG_OPENGL
	auto logger = new QOpenGLDebugLogger(this);
	logger->initialize();
	connect(logger, &QOpenGLDebugLogger::messageLogged, this, [](const QOpenGLDebugMessage &msg) {
		qDebug() << msg;
	});
	logger->startLogging();
#endif

	auto CheckShaderCompiled = [&](GLuint shader) {
		GLint compiled = 0;
		f->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if(compiled == GL_TRUE)
			return;
		GLint info_log_size = 0;
		f->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_size);
		QVector<GLchar> info_log(info_log_size);
		f->glGetShaderInfoLog(shader, info_log_size, &info_log_size, info_log.data());
		f->glDeleteShader(shader);
		CHIAKI_LOGE(session->GetChiakiLog(), "Failed to Compile Shader:\n%s", info_log.data());
	};

	GLuint shader_vert = f->glCreateShader(GL_VERTEX_SHADER);
	f->glShaderSource(shader_vert, 1, &conversion_config->shader_vert_glsl, nullptr);
	f->glCompileShader(shader_vert);
	CheckShaderCompiled(shader_vert);

	GLuint shader_frag = f->glCreateShader(GL_FRAGMENT_SHADER);
	f->glShaderSource(shader_frag, 1, &conversion_config->shader_frag_glsl, nullptr);
	f->glCompileShader(shader_frag);
	CheckShaderCompiled(shader_frag);

	program = f->glCreateProgram();
	f->glAttachShader(program, shader_vert);
	f->glAttachShader(program, shader_frag);
	f->glBindAttribLocation(program, 0, "pos_attr");
	f->glLinkProgram(program);

	GLint linked = 0;
	f->glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if(linked != GL_TRUE)
	{
		GLint info_log_size = 0;
		f->glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_log_size);
		QVector<GLchar> info_log(info_log_size);
		f->glGetProgramInfoLog(program, info_log_size, &info_log_size, info_log.data());
		f->glDeleteProgram(program);
		CHIAKI_LOGE(session->GetChiakiLog(), "Failed to Link Shader Program:\n%s", info_log.data());
		return;
	}

	frame_width = frame_height = 0; // fresh 1x1 textures below, next frame must re-specify them
	f->glGenTextures(conversion_config->planes, tex);
	uint8_t uv_default[] = {0x7f, 0x7f};
	for(unsigned int j=0; j<conversion_config->planes; j++)
	{
		f->glBindTexture(GL_TEXTURE_2D, tex[j]);
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		f->glTexImage2D(GL_TEXTURE_2D, 0, conversion_config->plane_configs[j].internal_format, 1, 1, 0, conversion_config->plane_configs[j].format, GL_UNSIGNED_BYTE, j > 0 ? uv_default : nullptr);
	}

	f->glUseProgram(program);

	// bind only as many planes as we need
	const char *plane_names[] = {"plane1", "plane2", "plane3"};
	for(int i=0; i<sizeof(plane_names)/sizeof(char *); i++)
	{
		f->glUniform1i(f->glGetUniformLocation(program, plane_names[i]), i);
	}

	f->glGenVertexArrays(1, &vao);
	f->glBindVertexArray(vao);

	f->glGenBuffers(1, &vbo);
	f->glBindBuffer(GL_ARRAY_BUFFER, vbo);
	f->glBufferData(GL_ARRAY_BUFFER, sizeof(vert_pos), vert_pos, GL_STATIC_DRAW);

	f->glBindBuffer(GL_ARRAY_BUFFER, vbo);
	f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	f->glEnableVertexAttribArray(0);

	f->glCullFace(GL_BACK);
	f->glEnable(GL_CULL_FACE);
	f->glClearColor(0.0, 0.0, 0.0, 0.0);

	gl_ok = true;
}

void AVOpenGLWidget::paintGL()
{
	auto f = QOpenGLContext::currentContext()->extraFunctions();

	f->glClear(GL_COLOR_BUFFER_BIT);
	if(!gl_ok)
		return;

	if(AVFrame *next_frame = chiaki_ffmpeg_decoder_pull_frame(session->GetFfmpegDecoder()))
	{
		if(UploadFrame(next_frame) && frames_shown++ == 0)
			CHIAKI_LOGI(session->GetChiakiLog(), "Video: first frame %dx%d uploaded (GL error 0x%x)", next_frame->width, next_frame->height, f->glGetError());
		av_frame_free(&next_frame);
	}

	int widget_width = (int)(width() * devicePixelRatioF());
	int widget_height = (int)(height() * devicePixelRatioF());

	GLsizei vp_width, vp_height;
	if(!frame_width || !frame_height)
	{
		vp_width = widget_width;
		vp_height = widget_height;
	}
	else if(transform_mode == TransformMode::Fit)
	{
		float aspect = (float)frame_width / (float)frame_height;
		if(widget_height && aspect < (float)widget_width / (float)widget_height)
		{
			vp_height = widget_height;
			vp_width = (GLsizei)(vp_height * aspect);
		}
		else
		{
			vp_width = widget_width;
			vp_height = (GLsizei)(vp_width / aspect);
		}
	}
	else if(transform_mode == TransformMode::Zoom)
	{
		float aspect = (float)frame_width / (float)frame_height;
		if(widget_height && aspect < (float)widget_width / (float)widget_height)
		{
			vp_width = widget_width;
			vp_height = (GLsizei)(vp_width / aspect);
		}
		else
		{
			vp_height = widget_height;
			vp_width = (GLsizei)(vp_height * aspect);
		}
	}
	else // transform_mode == TransformMode::Stretch
	{
		float aspect = (float)frame_width / (float)frame_height;
		if(widget_height && aspect < (float)widget_width / (float)widget_height)
		{
			vp_height = widget_height;
			vp_width = widget_width;
		}
		else
		{
			vp_width = widget_width;
			vp_height = widget_height;
		}
	}

	f->glViewport((widget_width - vp_width) / 2, (widget_height - vp_height) / 2, vp_width, vp_height);

	f->glUseProgram(program);
	f->glBindVertexArray(vao);
	for(unsigned int i=0; i<conversion_config->planes; i++)
	{
		f->glActiveTexture(GL_TEXTURE0 + i);
		f->glBindTexture(GL_TEXTURE_2D, tex[i]);
	}

	f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	f->glBindVertexArray(0);
	f->glActiveTexture(GL_TEXTURE0);
}
