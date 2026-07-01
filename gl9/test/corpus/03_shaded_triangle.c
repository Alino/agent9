/* 03_shaded_triangle — per-vertex color (a varying, interpolated by the
 * rasterizer) plus a uniform offset. Exercises attributes, varyings, uniforms. */
#include "glharness.h"

static const char *VS =
	"#version 330 core\n"
	"layout(location=0) in vec2 pos;\n"
	"layout(location=1) in vec3 col;\n"
	"uniform vec2 ofs;\n"
	"out vec3 vcol;\n"
	"void main(){ vcol = col; gl_Position = vec4(pos + ofs, 0.0, 1.0); }\n";
static const char *FS =
	"#version 330 core\n"
	"in vec3 vcol;\n"
	"out vec4 o;\n"
	"void main(){ o = vec4(vcol, 1.0); }\n";

static void
gl9_render(void)
{
	/* pos.xy, col.rgb interleaved */
	static const float v[] = {
		-0.8f, -0.8f,  1.0f, 0.0f, 0.0f,
		 0.8f, -0.8f,  0.0f, 1.0f, 0.0f,
		 0.0f,  0.8f,  0.0f, 0.0f, 1.0f,
	};
	GLuint prog, vao, vbo;

	glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	prog = gl9_program(VS, FS);
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
			      (void *)(2 * sizeof(float)));
	glEnableVertexAttribArray(1);

	glUseProgram(prog);
	glUniform2f(glGetUniformLocation(prog, "ofs"), 0.05f, -0.05f);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}
