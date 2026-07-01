/* cube_demo — the strong demonstration: a perspective-projected, depth-tested,
 * Phong-lit spinning cube, animated frame-by-frame to gl9win. Exercises the full
 * modern-GL path the parity suite verified (VBO/VAO/GLSL/uniforms/depth) plus a
 * mat4 pipeline and per-pixel lighting with specular highlights — unambiguously
 * real 3D OpenGL, on 9front softpipe. `cube_demo | gl9win`, GALLIUM_NOSSE=1. */
#define GL9_W 480
#define GL9_H 480
#define GL9_NO_MAIN
#include "glharness.h"
#include <math.h>

/* --- minimal column-major mat4 (GL layout, transpose=GL_FALSE) --------------- */
static void mident(float *m){ int i; for(i=0;i<16;i++) m[i]=(i%5)?0.0f:1.0f; }
static void mmul(float *r, const float *a, const float *b){
	float t[16]; int c, row, k;
	for(c=0;c<4;c++) for(row=0;row<4;row++){
		float s=0; for(k=0;k<4;k++) s += a[k*4+row]*b[c*4+k];
		t[c*4+row]=s;
	}
	for(k=0;k<16;k++) r[k]=t[k];
}
static void mpersp(float *m, float fovy, float aspect, float n, float f){
	float t=1.0f/tanf(fovy*0.5f); int i;
	for(i=0;i<16;i++) m[i]=0;
	m[0]=t/aspect; m[5]=t; m[10]=(f+n)/(n-f); m[11]=-1.0f; m[14]=(2*f*n)/(n-f);
}
static void mroty(float *m, float a){ float c=cosf(a), s=sinf(a);
	mident(m); m[0]=c; m[2]=-s; m[8]=s; m[10]=c; }
static void mrotx(float *m, float a){ float c=cosf(a), s=sinf(a);
	mident(m); m[5]=c; m[6]=s; m[9]=-s; m[10]=c; }
static void mtrans(float *m, float x, float y, float z){
	mident(m); m[12]=x; m[13]=y; m[14]=z; }

static const char *VS =
	"#version 330 core\n"
	"layout(location=0) in vec3 pos;\n"
	"layout(location=1) in vec3 nrm;\n"
	"layout(location=2) in vec3 col;\n"
	"uniform mat4 mvp; uniform mat4 model;\n"
	"out vec3 vn; out vec3 vc; out vec3 vp;\n"
	"void main(){\n"
	"  gl_Position = mvp * vec4(pos,1.0);\n"
	"  vn = mat3(model) * nrm; vc = col; vp = (model*vec4(pos,1.0)).xyz;\n"
	"}\n";
static const char *FS =
	"#version 330 core\n"
	"in vec3 vn; in vec3 vc; in vec3 vp; out vec4 o;\n"
	"void main(){\n"
	"  vec3 N=normalize(vn), L=normalize(vec3(0.5,0.8,0.7));\n"
	"  vec3 V=normalize(vec3(0.0,0.0,3.4)-vp), R=reflect(-L,N);\n"
	"  float d=max(dot(N,L),0.0), s=pow(max(dot(R,V),0.0),28.0);\n"
	"  o=vec4(vc*(0.22+0.78*d)+vec3(1.0)*s*0.6, 1.0);\n"
	"}\n";

int
main(void)
{
	/* 36 vertices, precomputed: unit cube [-1,1], per-face normal + color. */
	static const float V[] = {
	  /* +Z (red) */
	 -1,-1, 1, 0,0,1, 1,0,0,  1,-1, 1, 0,0,1, 1,0,0,  1, 1, 1, 0,0,1, 1,0,0,
	 -1,-1, 1, 0,0,1, 1,0,0,  1, 1, 1, 0,0,1, 1,0,0, -1, 1, 1, 0,0,1, 1,0,0,
	  /* -Z (green) */
	  1,-1,-1, 0,0,-1, 0,1,0, -1,-1,-1, 0,0,-1, 0,1,0, -1, 1,-1, 0,0,-1, 0,1,0,
	  1,-1,-1, 0,0,-1, 0,1,0, -1, 1,-1, 0,0,-1, 0,1,0,  1, 1,-1, 0,0,-1, 0,1,0,
	  /* +X (blue) */
	  1,-1, 1, 1,0,0, 0.2,0.4,1,  1,-1,-1, 1,0,0, 0.2,0.4,1,  1, 1,-1, 1,0,0, 0.2,0.4,1,
	  1,-1, 1, 1,0,0, 0.2,0.4,1,  1, 1,-1, 1,0,0, 0.2,0.4,1,  1, 1, 1, 1,0,0, 0.2,0.4,1,
	  /* -X (yellow) */
	 -1,-1,-1, -1,0,0, 1,0.85,0.1, -1,-1, 1, -1,0,0, 1,0.85,0.1, -1, 1, 1, -1,0,0, 1,0.85,0.1,
	 -1,-1,-1, -1,0,0, 1,0.85,0.1, -1, 1, 1, -1,0,0, 1,0.85,0.1, -1, 1,-1, -1,0,0, 1,0.85,0.1,
	  /* +Y (magenta) */
	 -1, 1, 1, 0,1,0, 1,0.2,0.8,  1, 1, 1, 0,1,0, 1,0.2,0.8,  1, 1,-1, 0,1,0, 1,0.2,0.8,
	 -1, 1, 1, 0,1,0, 1,0.2,0.8,  1, 1,-1, 0,1,0, 1,0.2,0.8, -1, 1,-1, 0,1,0, 1,0.2,0.8,
	  /* -Y (cyan) */
	 -1,-1,-1, 0,-1,0, 0.1,0.9,0.9,  1,-1,-1, 0,-1,0, 0.1,0.9,0.9,  1,-1, 1, 0,-1,0, 0.1,0.9,0.9,
	 -1,-1,-1, 0,-1,0, 0.1,0.9,0.9,  1,-1, 1, 0,-1,0, 0.1,0.9,0.9, -1,-1, 1, 0,-1,0, 0.1,0.9,0.9,
	};
	GLuint prog, vao, vbo;
	GLint uMVP, uModel;
	int frame;

	if (!gl9_init())
		return 1;
	glEnable(GL_DEPTH_TEST);

	prog = gl9_program(VS, FS);
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof V, V, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9*sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9*sizeof(float), (void*)(3*sizeof(float)));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 9*sizeof(float), (void*)(6*sizeof(float)));
	glEnableVertexAttribArray(2);
	glUseProgram(prog);
	uMVP = glGetUniformLocation(prog, "mvp");
	uModel = glGetUniformLocation(prog, "model");

	for (frame = 0; frame < 100000; frame++) {
		float a = frame * 0.10f;
		float model[16], ry[16], rx[16], view[16], proj[16], mv[16], mvp[16];
		mroty(ry, a); mrotx(rx, a*0.6f); mmul(model, ry, rx);
		mtrans(view, 0, 0, -3.4f);
		mpersp(proj, 0.9f, (float)GL9_W/GL9_H, 0.1f, 100.0f);
		mmul(mv, view, model); mmul(mvp, proj, mv);

		glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp);
		glUniformMatrix4fv(uModel, 1, GL_FALSE, model);
		glBindVertexArray(vao);
		glDrawArrays(GL_TRIANGLES, 0, 36);
		glFinish();
		gl9_present(1);
		fprintf(stderr, "frame %d\n", frame);
	}
	return 0;
}
