// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_AVOPENGLWIDGET_H
#define CHIAKI_AVOPENGLWIDGET_H

#include "transformmode.h"

#include <chiaki/log.h>

#include <QOpenGLWidget>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#define MAX_PANES 3

class StreamSession;

struct PlaneConfig
{
	unsigned int width_divider;
	unsigned int height_divider;
	unsigned int data_per_pixel;
	GLint internal_format;
	GLenum format;
};

struct ConversionConfig
{
	enum AVPixelFormat pixel_format;
	const char *shader_vert_glsl;
	const char *shader_frag_glsl;
	unsigned int planes;
	struct PlaneConfig plane_configs[MAX_PANES];
};

class AVOpenGLWidget: public QOpenGLWidget
{
	Q_OBJECT

	private:
		StreamSession *session;

		GLuint program;
		GLuint vbo;
		GLuint vao;

		// ponytail: frames are pulled and uploaded on the GUI GL context in paintGL.
		// The old background uploader thread + shared EAGL context showed no video on iOS.
		GLuint tex[MAX_PANES];
		unsigned int frame_width;
		unsigned int frame_height;
		bool gl_ok;
		unsigned long frames_shown;

		bool UploadFrame(AVFrame *frame);

		QTimer *mouse_timer;

		ConversionConfig *conversion_config;

	public:
		static QSurfaceFormat CreateSurfaceFormat();

		explicit AVOpenGLWidget(StreamSession *session, QWidget *parent = nullptr, TransformMode transform_mode = TransformMode::Fit);
		~AVOpenGLWidget() override;

		void SetTransformMode(TransformMode mode) { transform_mode = mode; }
		TransformMode GetTransformMode() const { return transform_mode; }

	protected:
		TransformMode transform_mode;
		void mouseMoveEvent(QMouseEvent *event) override;

		void initializeGL() override;
		void paintGL() override;

	public slots:
		void ResetMouseTimeout();
		void HideMouse();
};

#endif // CHIAKI_AVOPENGLWIDGET_H
