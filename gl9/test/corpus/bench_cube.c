/* bench_cube.c — llvmpipe vs softpipe throughput on the SAME binary (the driver
 * is chosen at runtime by GALLIUM_DRIVER), so the only variable is the rasterizer.
 *
 * Workload = cube_demo's per-pixel Phong (normalize + reflect + pow specular) at
 * 512x512: fragment-shading bound, which is exactly where a JIT-vectorized
 * rasterizer should beat the scalar reference one. 64x64 (the parity corpus size)
 * would be swamped by setup/JIT and measure nothing.
 *
 * Reports the two numbers separately and honestly:
 *   first frame  — includes llvmpipe's one-time shader JIT (softpipe has none)
 *   steady state — mean ms/frame afterwards = the throughput we actually care about
 * Also prints the mean-pixel signature: if the two drivers disagree there, they
 * did different work and the timing comparison is meaningless.
 *
 * env: BENCH_FRAMES (default 20). crt0 has no real argv, so config comes via /env.
 */
#define GL9_W 512
#define GL9_H 512
#define GL9_NO_MAIN     /* own main: timing loop, not the corpus render->PPM one */
#include "glharness.h"
#include <time.h>
#include <math.h>
#include <string.h>

static void mident(float *m){ int i; for(i=0;i<16;i++) m[i]=(i%5)?0.0f:1.0f; }
static void mmul(float *r, const float *a, const float *b){
	float t[16]; int c,row,k;
	for(c=0;c<4;c++) for(row=0;row<4;row++){
		float s=0; for(k=0;k<4;k++) s += a[k*4+row]*b[c*4+k];
		t[c*4+row]=s;
	}
	for(k=0;k<16;k++) r[k]=t[k];
}
static void mpersp(float *m, float fovy, float asp, float zn, float zf){
	float f = 1.0f/(float)tan(fovy*0.5); int i;
	for(i=0;i<16;i++) m[i]=0;
	m[0]=f/asp; m[5]=f; m[10]=(zf+zn)/(zn-zf); m[11]=-1; m[14]=(2*zf*zn)/(zn-zf);
}
static void mrotx(float *m, float a){ float c=(float)cos(a), s=(float)sin(a);
	mident(m); m[5]=c; m[6]=s; m[9]=-s; m[10]=c; }
static void mroty(float *m, float a){ float c=(float)cos(a), s=(float)sin(a);
	mident(m); m[0]=c; m[2]=-s; m[8]=s; m[10]=c; }
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

static double now_ms(void){
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void draw(int frame, GLint uMVP, GLint uModel, GLuint vao){
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
}

int
main(void)
{
	static const float V[] = {
	 -1,-1, 1, 0,0,1, 1,0,0,  1,-1, 1, 0,0,1, 1,0,0,  1, 1, 1, 0,0,1, 1,0,0,
	 -1,-1, 1, 0,0,1, 1,0,0,  1, 1, 1, 0,0,1, 1,0,0, -1, 1, 1, 0,0,1, 1,0,0,
	  1,-1,-1, 0,0,-1, 0,1,0, -1,-1,-1, 0,0,-1, 0,1,0, -1, 1,-1, 0,0,-1, 0,1,0,
	  1,-1,-1, 0,0,-1, 0,1,0, -1, 1,-1, 0,0,-1, 0,1,0,  1, 1,-1, 0,0,-1, 0,1,0,
	  1,-1, 1, 1,0,0, 0.2,0.4,1,  1,-1,-1, 1,0,0, 0.2,0.4,1,  1, 1,-1, 1,0,0, 0.2,0.4,1,
	  1,-1, 1, 1,0,0, 0.2,0.4,1,  1, 1,-1, 1,0,0, 0.2,0.4,1,  1, 1, 1, 1,0,0, 0.2,0.4,1,
	 -1,-1,-1, -1,0,0, 1,0.85,0.1, -1,-1, 1, -1,0,0, 1,0.85,0.1, -1, 1, 1, -1,0,0, 1,0.85,0.1,
	 -1,-1,-1, -1,0,0, 1,0.85,0.1, -1, 1, 1, -1,0,0, 1,0.85,0.1, -1, 1,-1, -1,0,0, 1,0.85,0.1,
	 -1, 1, 1, 0,1,0, 1,0.2,0.8,  1, 1, 1, 0,1,0, 1,0.2,0.8,  1, 1,-1, 0,1,0, 1,0.2,0.8,
	 -1, 1, 1, 0,1,0, 1,0.2,0.8,  1, 1,-1, 0,1,0, 1,0.2,0.8, -1, 1,-1, 0,1,0, 1,0.2,0.8,
	 -1,-1,-1, 0,-1,0, 0.1,0.9,0.9,  1,-1,-1, 0,-1,0, 0.1,0.9,0.9,  1,-1, 1, 0,-1,0, 0.1,0.9,0.9,
	 -1,-1,-1, 0,-1,0, 0.1,0.9,0.9,  1,-1, 1, 0,-1,0, 0.1,0.9,0.9, -1,-1, 1, 0,-1,0, 0.1,0.9,0.9,
	};
	GLuint prog, vao, vbo;
	GLint uMVP, uModel;
	int frame, nframes;
	double t0, t_first, t_steady;
	const char *e;
	long rs = 0, i, npx = (long)GL9_W * GL9_H;

	e = getenv("BENCH_FRAMES");
	nframes = e ? atoi(e) : 20;
	if (nframes < 1) nframes = 20;

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

	/* frame 0 alone: llvmpipe compiles its shaders here (softpipe does not) */
	t0 = now_ms();
	draw(0, uMVP, uModel, vao);
	t_first = now_ms() - t0;

	/* steady state */
	t0 = now_ms();
	for (frame = 1; frame <= nframes; frame++)
		draw(frame, uMVP, uModel, vao);
	t_steady = (now_ms() - t0) / nframes;

	/* signature: both drivers must agree, else we timed different work */
	for (i = 0; i < npx; i++) rs += gl9_buf[i*4];
	printf("BENCH %dx%d frames=%d first=%.1fms steady=%.2fms fps=%.2f meanR=%ld\n",
	       GL9_W, GL9_H, nframes, t_first, t_steady, 1000.0 / t_steady, rs / npx);
	return 0;
}
