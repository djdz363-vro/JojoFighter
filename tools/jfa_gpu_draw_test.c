/*
 * jfa_gpu_draw_test.c - optional desktop smoke test for M2 Stage C
 * Substage C (GPU skinning rendering integration).
 *
 * Creates a hidden SDL window with a GLES2 context (desktop EGL/
 * Mesa when available), compiles the REAL Android shader sources
 * (jfa_gpu_vs + fs), links the program exactly like main.c, and
 * verifies:
 *   1. the shader compiles and links,
 *   2. all 5 attribute locations are valid,
 *   3. all 5 uniform locations (uMVP, uModel, uTexture, uTextMode,
 *      uBones) are valid,
 *   4. the vertex-uniform budget check passes for 33 bones,
 *   5. a draw with the 52-byte JFAGpuVertex layout produces no GL
 *      error (attribute pointers + one glDrawArrays + uniform
 *      upload, exactly the Substage C draw sequence).
 *
 * Skips gracefully (exit 0 with SKIP messages) when no GLES2 context
 * can be created - the implementation never depends on a desktop GL
 * environment; real validation happens on the Android device.
 */

#include "../Android/app/jni/src/jfa_gpu.h"

#include <SDL.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what)
{
	printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
	if (!cond)
		failures++;
}

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		printf("  [FAIL] shader compile: %s\n", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

int main(void)
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		printf("[SKIP] SDL_Init: %s\n", SDL_GetError());
		return 0;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
			    SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

	SDL_Window *win = SDL_CreateWindow("jfa_gpu_draw_test",
					   SDL_WINDOWPOS_CENTERED,
					   SDL_WINDOWPOS_CENTERED, 64, 64,
					   SDL_WINDOW_OPENGL |
					   SDL_WINDOW_HIDDEN);
	SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : NULL;
	if (!ctx) {
		printf("[SKIP] no GLES2 context on this host: %s\n",
		       SDL_GetError());
		if (win)
			SDL_DestroyWindow(win);
		SDL_Quit();
		return 0;
	}
	printf("GLES2 context: %s\n", glGetString(GL_RENDERER));

	/* 1. compile + link exactly like main.c's Stage A path. */
	GLuint va = compile(GL_VERTEX_SHADER, jfa_gpu_vs);
	GLuint vb = compile(GL_FRAGMENT_SHADER,
			    "precision mediump float;"
			    "uniform sampler2D uTexture; uniform float uTextMode;"
			    "varying vec2 vUV; varying float vLight; varying vec3 vColor;"
			    "void main(){ if(uTextMode>0.5) gl_FragColor=vec4(vColor,1.0);"
			    "else { vec4 t=texture2D(uTexture,vUV); gl_FragColor=vec4(t.rgb*vLight,t.a); } }");
	check(va != 0, "GPU vertex shader compiles");
	check(vb != 0, "fragment shader compiles");
	GLuint p = 0;
	if (va && vb) {
		p = glCreateProgram();
		glAttachShader(p, va);
		glAttachShader(p, vb);
		glLinkProgram(p);
		GLint ok = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) {
			char log[1024];
			glGetProgramInfoLog(p, sizeof(log), NULL, log);
			printf("  [FAIL] link: %s\n", log);
			p = 0;
		}
	}
	check(p != 0, "GPU program links");

	/* 2+3. attribute and uniform locations. */
	if (p) {
		const GLint pos = glGetAttribLocation(p, "aPosition");
		const GLint uv = glGetAttribLocation(p, "aUV");
		const GLint nrm = glGetAttribLocation(p, "aNormal");
		const GLint bidx = glGetAttribLocation(p, "aBoneIndex");
		const GLint bwgt = glGetAttribLocation(p, "aBoneWeight");
		const GLint mvp = glGetUniformLocation(p, "uMVP");
		const GLint model = glGetUniformLocation(p, "uModel");
		const GLint tex = glGetUniformLocation(p, "uTexture");
		const GLint mode = glGetUniformLocation(p, "uTextMode");
		const GLint bones = glGetUniformLocation(p, "uBones");
		check(pos >= 0 && uv >= 0 && nrm >= 0 && bidx >= 0 &&
			      bwgt >= 0,
		      "all 5 attribute locations valid");
		check(mvp >= 0 && model >= 0 && tex >= 0 && mode >= 0 &&
			      bones >= 0,
		      "all 5 uniform locations valid");

		/* 4. budget check for the real 33-bone skeleton. */
		GLint maxVUV = 0, maxAttribs = 0;
		glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &maxVUV);
		glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttribs);
		printf("  host: maxVUV=%d maxAttribs=%d\n", maxVUV,
		       maxAttribs);
		check(jfa_gpu_budget_ok(maxVUV, 33),
		      "33-bone budget check passes on this host");
		check(maxAttribs >= 5, "at least 5 vertex attribs");

		/* 5. full draw sequence with the 52-byte layout. */
		JFAGpuVertex v[3];
		memset(v, 0, sizeof(v));
		v[0].position[0] = -1; v[0].weight[0] = 1; v[0].bone[0] = 0;
		v[1].position[1] = 1; v[1].weight[0] = 1; v[1].bone[0] = 0;
		v[2].position[2] = -1; v[2].weight[0] = 1; v[2].bone[0] = 0;
		GLuint vbo = 0;
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
		float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
				0, 0, 0, 1 };
		float stage[JFA_GPU_FLOATS];
		memset(stage, 0, sizeof(stage));
		for (int b = 0; b < JFA_GPU_MAX_BONES; b++) {
			stage[b * 12 + 0] = 1;
			stage[b * 12 + 5] = 1;
			stage[b * 12 + 10] = 1;
		}
		while (glGetError() != GL_NO_ERROR) {
		}
		glUseProgram(p);
		glUniformMatrix4fv(mvp, 1, GL_FALSE, m);
		glUniformMatrix4fv(model, 1, GL_FALSE, m);
		glUniform1i(tex, 0);
		glUniform1f(mode, 0.0f);
		glUniform4fv(bones, JFA_GPU_BONE_VECS, stage);
		glEnableVertexAttribArray(pos);
		glEnableVertexAttribArray(uv);
		glEnableVertexAttribArray(nrm);
		glEnableVertexAttribArray(bidx);
		glEnableVertexAttribArray(bwgt);
		glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE,
				      (GLsizei)sizeof(JFAGpuVertex),
				      (void *)0);
		glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE,
				      (GLsizei)sizeof(JFAGpuVertex),
				      (void *)12);
		glVertexAttribPointer(nrm, 3, GL_FLOAT, GL_FALSE,
				      (GLsizei)sizeof(JFAGpuVertex),
				      (void *)20);
		glVertexAttribPointer(bidx, 4, GL_UNSIGNED_BYTE, GL_FALSE,
				      (GLsizei)sizeof(JFAGpuVertex),
				      (void *)32);
		glVertexAttribPointer(bwgt, 4, GL_FLOAT, GL_FALSE,
				      (GLsizei)sizeof(JFAGpuVertex),
				      (void *)36);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glDisableVertexAttribArray(bidx);
		glDisableVertexAttribArray(bwgt);
		const GLenum err = glGetError();
		check(err == GL_NO_ERROR, "draw sequence: GL_NO_ERROR");
		if (err != GL_NO_ERROR)
			printf("  GL error: 0x%04x\n", err);
		glDeleteBuffers(1, &vbo);
	}
	if (p)
		glDeleteProgram(p);

	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(win);
	SDL_Quit();

	printf("\n%s (%d failure%s)\n",
	       failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures,
	       failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
