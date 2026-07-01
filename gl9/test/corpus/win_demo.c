/* win_demo — the Phase 2 windowed demo. Renders a 480x480 modern-GL scene with
 * OSMesa and streams it to gl9win over stdout (`win_demo | gl9win`). Distinct R/G/B
 * triangle corners double as a byte-order check for the ABGR32 blit. After
 * presenting it sleeps so the window persists for a screendump, then EOFs. */
#define GL9_W 480
#define GL9_H 480
#define GL9_NO_MAIN
#include "glharness.h"

static const char *VS =
	"#version 330 core\n"
	"layout(location=0) in vec2 pos;\n"
	"layout(location=1) in vec3 col;\n"
	"out vec3 vcol;\n"
	"void main(){ vcol = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
	"#version 330 core\n"
	"in vec3 vcol;\n"
	"out vec4 o;\n"
	"void main(){ o = vec4(vcol, 1.0); }\n";

int
main(void)
{
	static const float v[] = {
		-0.85f, -0.85f,  1.0f, 0.0f, 0.0f,   /* red   bottom-left */
		 0.85f, -0.85f,  0.0f, 1.0f, 0.0f,   /* green bottom-right */
		 0.0f,   0.85f,  0.0f, 0.0f, 1.0f,   /* blue  top */
	};
	GLuint prog, vao, vbo;

	if (!gl9_init())
		return 1;

	glClearColor(0.10f, 0.10f, 0.14f, 1.0f);
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
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();

	gl9_present(1);			/* frame -> gl9win (which holds the window) */
	fprintf(stderr, "presented %dx%d\n", GL9_W, GL9_H);
	return 0;
}
