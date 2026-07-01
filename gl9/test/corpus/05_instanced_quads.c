/* 05_instanced_quads — glDrawArraysInstanced + gl_InstanceID + a per-instance
 * offset attribute (divisor=1), each quad sampling a shared texture. This is
 * Alacritty's exact draw shape: a glyph-atlas texture drawn as instanced quads. */
#include "glharness.h"

#define N 5

static const char *VS =
	"#version 330 core\n"
	"layout(location=0) in vec2 corner;\n"   /* unit quad corner 0..1 */
	"layout(location=1) in vec2 ioffset;\n"  /* per-instance cell origin (divisor 1) */
	"out vec2 vuv;\n"
	"void main(){\n"
	"  vuv = corner;\n"
	"  vec2 p = ioffset + corner * 0.28;\n"
	"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"  /* [0,1] -> clip */
	"}\n";
static const char *FS =
	"#version 330 core\n"
	"in vec2 vuv;\n"
	"out vec4 o;\n"
	"uniform sampler2D atlas;\n"
	"void main(){ o = texture(atlas, vuv); }\n";

static void
gl9_render(void)
{
	static const float quad[] = { 0,0, 1,0, 0,1, 1,1 };  /* strip */
	float off[N * 2];
	unsigned char tex[4 * 4 * 4];
	int i;
	GLuint prog, vao, vq, vo, t;

	for (i = 0; i < N; i++) { off[i*2] = 0.05f + i * 0.18f; off[i*2+1] = 0.36f; }
	for (i = 0; i < 16; i++) {
		unsigned char *p = &tex[i*4];
		unsigned char c = ((i ^ (i >> 2)) & 1) ? 220 : 60;
		p[0] = c; p[1] = 90; p[2] = (unsigned char)(255 - c); p[3] = 255;
	}

	glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);

	prog = gl9_program(VS, FS);
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	glGenBuffers(1, &vq);
	glBindBuffer(GL_ARRAY_BUFFER, vq);
	glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	glGenBuffers(1, &vo);
	glBindBuffer(GL_ARRAY_BUFFER, vo);
	glBufferData(GL_ARRAY_BUFFER, sizeof off, off, GL_STATIC_DRAW);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(1);
	glVertexAttribDivisor(1, 1);   /* one offset per instance */

	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "atlas"), 0);
	glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, N);
}
