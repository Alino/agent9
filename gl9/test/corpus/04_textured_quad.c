/* 04_textured_quad — upload an 8x8 checkerboard texture and sample it across a
 * full quad (two triangles via a triangle strip). Exercises textures + samplers. */
#include "glharness.h"

static const char *VS =
	"#version 330 core\n"
	"layout(location=0) in vec2 pos;\n"
	"layout(location=1) in vec2 uv;\n"
	"out vec2 vuv;\n"
	"void main(){ vuv = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
	"#version 330 core\n"
	"in vec2 vuv;\n"
	"out vec4 o;\n"
	"uniform sampler2D tex;\n"
	"void main(){ o = texture(tex, vuv); }\n";

static void
gl9_render(void)
{
	/* pos.xy, uv.xy for a triangle strip covering the frame */
	static const float v[] = {
		-0.9f, -0.9f, 0.0f, 0.0f,
		 0.9f, -0.9f, 1.0f, 0.0f,
		-0.9f,  0.9f, 0.0f, 1.0f,
		 0.9f,  0.9f, 1.0f, 1.0f,
	};
	unsigned char tex[8 * 8 * 4];
	int i, j;
	GLuint prog, vao, vbo, t;

	for (j = 0; j < 8; j++)
		for (i = 0; i < 8; i++) {
			unsigned char *p = &tex[(j * 8 + i) * 4];
			unsigned char c = ((i ^ j) & 1) ? 230 : 40;
			p[0] = c; p[1] = (unsigned char)(255 - c); p[2] = 128; p[3] = 255;
		}

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);

	prog = gl9_program(VS, FS);
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
			      (void *)(2 * sizeof(float)));
	glEnableVertexAttribArray(1);

	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "tex"), 0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
