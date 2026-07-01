/* 06_depth_blend — two overlapping quads with the depth test and alpha blending
 * on: a near opaque quad and a far semi-transparent quad. Exercises the depth
 * buffer and the blend stage. */
#include "glharness.h"

static const char *VS =
	"#version 330 core\n"
	"layout(location=0) in vec2 pos;\n"
	"uniform float z;\n"
	"void main(){ gl_Position = vec4(pos, z, 1.0); }\n";
static const char *FS =
	"#version 330 core\n"
	"out vec4 o;\n"
	"uniform vec4 c;\n"
	"void main(){ o = c; }\n";

static void
quad(GLuint prog, float x0, float y0, float x1, float y1, float z,
     float r, float g, float b, float a)
{
	float v[] = { x0,y0, x1,y0, x0,y1, x1,y1 };
	GLuint vao, vbo;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);
	glUniform1f(glGetUniformLocation(prog, "z"), z);
	glUniform4f(glGetUniformLocation(prog, "c"), r, g, b, a);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void
gl9_render(void)
{
	GLuint prog;

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClearDepth(1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	prog = gl9_program(VS, FS);
	glUseProgram(prog);
	/* far opaque red at z=0.5, then near translucent blue at z=0.0 overlapping */
	quad(prog, -0.7f, -0.7f, 0.4f, 0.4f, 0.5f, 0.9f, 0.1f, 0.1f, 1.0f);
	quad(prog, -0.4f, -0.4f, 0.7f, 0.7f, 0.0f, 0.1f, 0.2f, 0.9f, 0.5f);
}
