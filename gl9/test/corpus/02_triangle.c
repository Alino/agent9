/* 02_triangle — VBO + VAO + a GLSL 330 program drawing one solid triangle. The
 * first real exercise of Mesa's C++ GLSL compiler + NIR + softpipe rasterizer. */
#include "glharness.h"

static const char *VS =
	"#version 330 core\n"
	"layout(location=0) in vec2 pos;\n"
	"void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
	"#version 330 core\n"
	"out vec4 col;\n"
	"void main(){ col = vec4(1.0, 0.8, 0.1, 1.0); }\n";

static void
gl9_render(void)
{
	static const float verts[] = { -0.8f, -0.8f,  0.8f, -0.8f,  0.0f, 0.8f };
	GLuint prog, vao, vbo;

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	prog = gl9_program(VS, FS);
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	glUseProgram(prog);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}
