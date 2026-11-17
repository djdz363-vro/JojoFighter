#include <SDL.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "fbx_loader.h"
#include "jfa_skin.h"
#include "jfa_gpu.h"

/* ------------------------------------------------------------------ */
/* JFA character state (Stage B). Loaded once; all per-frame work      */
/* reuses these buffers - no allocation in the render loop.            */
/* ------------------------------------------------------------------ */
static JFACharacter jfa;
static JFASkinned jfaSkin;
static int jfaOK = 0;
static int jfaClip = JFA_IDLE;
static double jfaClipStart = 0.0;   /* animationTime when clip began */
static double jfaClipDur = 0.0;     /* seconds of the current clip */
static float jfaScale = 1.0f;
static float jfaCenter[3] = { 0, 0, 0 };
/* PERF accumulators (like fbx_loader.c): averaged over 60 frames. */
static double jfaAccSkin = 0.0, jfaAccUpload = 0.0, jfaAccDraw = 0.0;
static int jfaAccN = 0;

typedef struct {
    float position[3];
    float uv[2];
    float normal[3];
    float color[3];
} FloorVertex;

typedef struct { float p[3], uv[2], n[3]; } Vertex;
typedef struct { Vertex *v; size_t count, cap; } Mesh;

static const char *vs =
"attribute vec3 aPosition; attribute vec2 aUV; attribute vec3 aNormal; attribute vec3 aColor;"
"uniform mat4 uMVP; uniform mat4 uModel; varying vec2 vUV; varying float vLight; varying vec3 vColor;"
"void main(){ vec3 n=normalize(mat3(uModel)*aNormal);"
"vLight=0.35+0.65*max(dot(n,normalize(vec3(-0.4,0.8,0.6))),0.0);"
"vUV=aUV; vColor=aColor; gl_Position=uMVP*vec4(aPosition,1.0); gl_PointSize=3.0; }";
static const char *fs =
"precision mediump float; uniform sampler2D uTexture; uniform float uTextMode;"
"varying vec2 vUV; varying float vLight; varying vec3 vColor;"
"void main(){ if(uTextMode>0.5) gl_FragColor=vec4(vColor,1.0);"
"else { vec4 t=texture2D(uTexture,vUV); gl_FragColor=vec4(t.rgb*vLight,t.a); } }";

static const unsigned char font[14][7] = {
{14,16,16,14,16,16,16},{30,17,17,30,16,16,16},{15,16,16,14,1,1,30},{0,4,4,0,4,4,0},
{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},
{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}};
static int glyph(char c){ if(c=='F')return 0;if(c=='P')return 1;if(c=='S')return 2;if(c==':')return 3;if(c>='0'&&c<='9')return 4+c-'0';return 3; }

static GLuint shader(GLenum type,const char *src){
 GLuint s=glCreateShader(type);glShaderSource(s,1,&src,NULL);glCompileShader(s);GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
 if(!ok){char log[512];glGetShaderInfoLog(s,sizeof(log),NULL,log);SDL_Log("Shader: %s",log);glDeleteShader(s);return 0;}return s;
}
static GLuint programCreate(void){
 GLuint a=shader(GL_VERTEX_SHADER,vs),b=shader(GL_FRAGMENT_SHADER,fs);if(!a||!b)return 0;
 GLuint p=glCreateProgram();glAttachShader(p,a);glAttachShader(p,b);glLinkProgram(p);glDeleteShader(a);glDeleteShader(b);
 GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char log[512];glGetProgramInfoLog(p,sizeof(log),NULL,log);SDL_Log("Link: %s",log);glDeleteProgram(p);return 0;}return p;
}
static void mm(float *o,const float *a,const float *b){
 float r[16];for(int c=0;c<4;c++)for(int y=0;y<4;y++){r[c*4+y]=0;for(int k=0;k<4;k++)r[c*4+y]+=a[k*4+y]*b[c*4+k];}memcpy(o,r,sizeof(r));
}
static int push(Mesh *m,Vertex v){
 if(m->count==m->cap){size_t n=m->cap?m->cap*2:4096;Vertex *p=(Vertex*)realloc(m->v,n*sizeof(Vertex));if(!p)return 0;m->v=p;m->cap=n;}m->v[m->count++]=v;return 1;
}
static int idx(int x,int n){return x>0?x-1:(x<0?n+x:-1);}

static int loadOBJ(const char *path,Mesh *m){
 size_t len=0;char *buf=(char*)SDL_LoadFile(path,&len);if(!buf){SDL_Log("OBJ read: %s",SDL_GetError());return 0;}
 float *p=NULL,*uv=NULL,*no=NULL;int np=0,nu=0,nn=0,cp=0,cu=0,cn=0;
 float mn[3]={1e30f,1e30f,1e30f},mx[3]={-1e30f,-1e30f,-1e30f};char *outer=NULL;
 for(char *line=strtok_r(buf,"\n",&outer);line;line=strtok_r(NULL,"\n",&outer)){
  if(line[0]=='v'&&line[1]==' '){float x,y,z;if(sscanf(line+2,"%f %f %f",&x,&y,&z)==3){
   if(np+3>cp){cp=cp?cp*2:4096;p=(float*)realloc(p,cp*sizeof(float));}if(!p)goto bad;
   p[np++]=x;p[np++]=y;p[np++]=z;if(x<mn[0])mn[0]=x;if(y<mn[1])mn[1]=y;if(z<mn[2])mn[2]=z;if(x>mx[0])mx[0]=x;if(y>mx[1])mx[1]=y;if(z>mx[2])mx[2]=z;
  }}else if(line[0]=='v'&&line[1]=='t'){float u,v;if(sscanf(line+2,"%f %f",&u,&v)==2){
   if(nu+2>cu){cu=cu?cu*2:4096;uv=(float*)realloc(uv,cu*sizeof(float));}if(!uv)goto bad;uv[nu++]=u;uv[nu++]=v;
  }}else if(line[0]=='v'&&line[1]=='n'){float x,y,z;if(sscanf(line+2,"%f %f %f",&x,&y,&z)==3){
   if(nn+3>cn){cn=cn?cn*2:4096;no=(float*)realloc(no,cn*sizeof(float));}if(!no)goto bad;no[nn++]=x;no[nn++]=y;no[nn++]=z;
  }}else if(line[0]=='f'&&line[1]==' '){
   int pi[8],ti[8],ni[8],count=0;char *inner=NULL;
   for(char *t=strtok_r(line+2," \t\r",&inner);t&&count<8;t=strtok_r(NULL," \t\r",&inner)){
    int a=0,b=0,c=0;if(sscanf(t,"%d/%d/%d",&a,&b,&c)!=3){count=0;break;}pi[count]=idx(a,np/3);ti[count]=idx(b,nu/2);ni[count]=idx(c,nn/3);count++;
   }
   for(int tri=1;tri<count-1;tri++){int ids[3]={0,tri,tri+1};for(int k=0;k<3;k++){
    int q=ids[k],a=pi[q],b=ti[q],c=ni[q];if(a<0||b<0||c<0||a>=np/3||b>=nu/2||c>=nn/3)goto bad;
    Vertex v={{p[a*3],p[a*3+1],p[a*3+2]},{uv[b*2],1.0f-uv[b*2+1]},{no[c*3],no[c*3+1],no[c*3+2]}};
    if(!push(m,v))goto bad;
   }}
  }
 }
 if(!m->count||np<3)goto bad;
 float center[3];for(int j=0;j<3;j++)center[j]=(mn[j]+mx[j])*0.5f;
 float extent=fmaxf(mx[0]-mn[0],fmaxf(mx[1]-mn[1],mx[2]-mn[2]));if(extent<0.0001f)goto bad;
 float scale=2.2f/extent;for(size_t i=0;i<m->count;i++)for(int j=0;j<3;j++)m->v[i].p[j]=(m->v[i].p[j]-center[j])*scale;
 SDL_free(buf);free(p);free(uv);free(no);SDL_Log("OBJ OK: %d verts, %d UVs, %d normals, %u triangle vertices",np/3,nu/2,nn/3,(unsigned)m->count);return 1;
bad: SDL_Log("OBJ parse failed");SDL_free(buf);free(p);free(uv);free(no);free(m->v);memset(m,0,sizeof(*m));return 0;
}
static GLuint loadBMP(const char *path){
 SDL_Surface *s=SDL_LoadBMP(path);if(!s){SDL_Log("BMP load: %s",SDL_GetError());return 0;}
 SDL_Surface *r=SDL_ConvertSurfaceFormat(s,SDL_PIXELFORMAT_RGBA32,0);SDL_FreeSurface(s);if(!r){SDL_Log("BMP convert: %s",SDL_GetError());return 0;}
 GLuint t=0;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,r->w,r->h,0,GL_RGBA,GL_UNSIGNED_BYTE,r->pixels);SDL_Log("Texture OK: %dx%d",r->w,r->h);SDL_FreeSurface(r);return t;
}
static void drawFPS(GLuint prog,GLuint vbo,GLint pos,GLint col,GLint mvp,GLint mode,int w,int h,int fps){
 char str[16];snprintf(str,sizeof(str),"FPS:%d",fps);float a[16*5*7*6];int n=0;float dx=6.0f/w,dy=6.0f/h,x0=1.0f-((float)SDL_strlen(str)*18+12)*2/w,y0=1.0f-24.0f/h;
 for(int ch=0;str[ch];ch++)for(int y=0;y<7;y++)for(int x=0;x<5;x++)if(font[glyph(str[ch])][y]&(1u<<(4-x))){a[n++]=x0+(ch*6+x)*dx;a[n++]=y0-y*dy;a[n++]=0;a[n++]=1;a[n++]=1;a[n++]=1;}
 const float id[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};glDisable(GL_DEPTH_TEST);glUseProgram(prog);glUniformMatrix4fv(mvp,1,GL_FALSE,id);glUniform1f(mode,1);
 glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,n*sizeof(float),a,GL_DYNAMIC_DRAW);glEnableVertexAttribArray(pos);glEnableVertexAttribArray(col);glVertexAttribPointer(pos,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)0);glVertexAttribPointer(col,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(3*sizeof(float)));glDrawArrays(GL_POINTS,0,n/6);glEnable(GL_DEPTH_TEST);
}
static GLuint createArenaFloor(float y) {
    const float half = 8.0f;

    const FloorVertex floorVertices[] = {
        {{-half, y, -half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}},
        {{-half, y,  half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}},
        {{ half, y,  half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}},

        {{-half, y, -half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}},
        {{ half, y,  half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}},
        {{ half, y, -half}, {0, 0}, {0, 1, 0}, {0.13f, 0.16f, 0.21f}}
    };

    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glBufferData(
        GL_ARRAY_BUFFER,
        sizeof(floorVertices),
        floorVertices,
        GL_STATIC_DRAW
    );

    return vbo;
}

static void addArenaFace(
    FloorVertex *vertices,
    size_t *count,
    const float a[3],
    const float b[3],
    const float c[3],
    const float d[3],
    float r, float g, float blue
) {
    const FloorVertex face[6] = {
        {{a[0], a[1], a[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}},
        {{b[0], b[1], b[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}},
        {{c[0], c[1], c[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}},
        {{a[0], a[1], a[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}},
        {{c[0], c[1], c[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}},
        {{d[0], d[1], d[2]}, {0, 0}, {0, 1, 0}, {r, g, blue}}
    };

    for (int i = 0; i < 6; i++) {
        vertices[*count + i] = face[i];
    }

    *count += 6;
}

static void addArenaBox(
    FloorVertex *vertices,
    size_t *count,
    float xMin, float yMin, float zMin,
    float xMax, float yMax, float zMax
) {
    const float points[8][3] = {
        {xMin, yMin, zMin},
        {xMax, yMin, zMin},
        {xMax, yMin, zMax},
        {xMin, yMin, zMax},
        {xMin, yMax, zMin},
        {xMax, yMax, zMin},
        {xMax, yMax, zMax},
        {xMin, yMax, zMax}
    };

    static const int faces[6][4] = {
        {0, 3, 2, 1},
        {4, 5, 6, 7},
        {0, 1, 5, 4},
        {3, 7, 6, 2},
        {0, 4, 7, 3},
        {1, 2, 6, 5}
    };

    static const float colors[6][3] = {
        {0.10f, 0.15f, 0.20f},
        {0.31f, 0.48f, 0.64f},
        {0.18f, 0.27f, 0.36f},
        {0.12f, 0.19f, 0.27f},
        {0.15f, 0.23f, 0.32f},
        {0.23f, 0.35f, 0.47f}
    };

    for (int i = 0; i < 6; i++) {
        addArenaFace(
            vertices, count,
            points[faces[i][0]],
            points[faces[i][1]],
            points[faces[i][2]],
            points[faces[i][3]],
            colors[i][0], colors[i][1], colors[i][2]
        );
    }
}

static GLuint createArenaWalls(
    float groundY,
    float wallHeight,
    float half,
    float thickness
) {
    const float inner = half - thickness;
    const float top = groundY + wallHeight;

    FloorVertex vertices[144];
    size_t count = 0;

    // Back wall
    addArenaBox(vertices, &count,
        -half, groundY, -half,
         half, top, -inner);

    // Front wall
    addArenaBox(vertices, &count,
        -half, groundY, inner,
         half, top, half);

    // Left wall
    addArenaBox(vertices, &count,
        -half, groundY, -inner,
        -inner, top, inner);

    // Right wall
    addArenaBox(vertices, &count,
        inner, groundY, -inner,
        half, top, inner);

    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glBufferData(
        GL_ARRAY_BUFFER,
        count * sizeof(FloorVertex),
        vertices,
        GL_STATIC_DRAW
    );

    return vbo;
}


int main(int argc,char **argv){
 SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
 if(SDL_Init(SDL_INIT_VIDEO)!=0){SDL_Log("SDL: %s",SDL_GetError());return 1;}
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,16);
 SDL_Window *win=SDL_CreateWindow("JoJo Fighter - OBJ Test",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,800,450,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
 if(!win){SDL_Log("Window: %s",SDL_GetError());SDL_Quit();return 1;}SDL_GLContext ctx=SDL_GL_CreateContext(win);if(!ctx){SDL_Log("Context: %s",SDL_GetError());SDL_DestroyWindow(win);SDL_Quit();return 1;}SDL_GL_SetSwapInterval(1);
 GLuint prog=programCreate();if(!prog){SDL_GL_DeleteContext(ctx);SDL_DestroyWindow(win);SDL_Quit();return 1;}
FBXModel fbx = {0};
FBXMesh frameMesh = {0};
int fbxOK = 0;
GLuint tex = 0;
GLuint meshVBO = 0, fpsVBO = 0;
float groundY = 0.0f;
float characterTopY = 0.0f;

/* ---- JFA path: load the character once; FBX renderer is the fallback ---- */
{
	char jerr[512] = { 0 };
	jfaOK = jfa_load_all(&jfa, "animations", jerr, sizeof(jerr));
	if (jfaOK) {
		jfa_skin_setup(&jfa, 2.2f, &jfaScale, jfaCenter);
		if (!jfa_skin_alloc(&jfaSkin, (size_t)jfa.nv)) {
			SDL_Log("JFA skin alloc failed - falling back to FBX");
			jfa_free(&jfa);
			jfaOK = 0;
		}
	}
	if (jfaOK) {
		/* UVs never change: fill them once here, not every frame
		 * (same trick as the PSP loader). V is flipped for GLES. */
		for (size_t i = 0; i < (size_t)jfa.nv; i++) {
			jfaSkin.v[i].uv[0] = jfa.v[i].uv[0];
			jfaSkin.v[i].uv[1] = 1.0f - jfa.v[i].uv[1];
		}
		/* posed local space: feet at y=0, height ~2.2 */
		groundY = 0.0f;
		characterTopY = 2.2f;
		SDL_Log("JFA renderer active: %d bones, %d verts, %d clips, scale %.3f",
			jfa.nb, jfa.nv, JFA_CLIP_COUNT, (double)jfaScale);
	} else {
		SDL_Log("JFA load failed (%s) - using FBX fallback",
			jerr[0] ? jerr : "unknown error");
	}
}
if (!jfaOK) {
	fbxOK = fbx_load(&fbx, "Mma Idle.fbx");
	if (fbxOK) fbxOK = fbx_build_mesh(&fbx, 0.0, &frameMesh);
}
tex = loadBMP("model0.bmp");
glGenBuffers(1, &meshVBO);
glGenBuffers(1, &fpsVBO);

if (fbxOK && frameMesh.vertex_count > 0) {
    groundY = frameMesh.vertices[0].position[1];
    characterTopY = groundY;

    for (size_t i = 1; i < frameMesh.vertex_count; i++) {
        float y = frameMesh.vertices[i].position[1];

        if (y < groundY) groundY = y;
        if (y > characterTopY) characterTopY = y;
    }
}

float characterHeight = characterTopY - groundY;
if (characterHeight < 0.001f) characterHeight = 2.0f;

GLuint arenaVBO = createArenaFloor(groundY - 0.02f);
GLuint arenaWallsVBO = createArenaWalls(
    groundY - 0.02f,
    characterHeight * 1.1f,
    4.2f,
    characterHeight * 0.08f
);

if (!jfaOK && fbxOK && frameMesh.vertex_count > 0) {
    glBindBuffer(GL_ARRAY_BUFFER, meshVBO);
    glBufferData(
        GL_ARRAY_BUFFER,
        frameMesh.vertex_count * sizeof(FBXVertex),
        frameMesh.vertices,
        GL_DYNAMIC_DRAW
    );
} else if (jfaOK) {
    /* Reserve the VBO once; every frame only calls glBufferSubData. */
    glBindBuffer(GL_ARRAY_BUFFER, meshVBO);
    glBufferData(
        GL_ARRAY_BUFFER,
        (size_t)jfa.nv * sizeof(JFASkinVertex),
        NULL,
        GL_DYNAMIC_DRAW
    );
}
 GLint pos=glGetAttribLocation(prog,"aPosition"),uv=glGetAttribLocation(prog,"aUV"),normal=glGetAttribLocation(prog,"aNormal"),col=glGetAttribLocation(prog,"aColor");
 GLint mvpLoc=glGetUniformLocation(prog,"uMVP"),modelLoc=glGetUniformLocation(prog,"uModel"),texLoc=glGetUniformLocation(prog,"uTexture"),modeLoc=glGetUniformLocation(prog,"uTextMode");

#ifdef JFA_GPU_SKIN
/* ---- Stage C Substage A: GPU skinning infrastructure. Compiled in
 * ONLY with -DJFA_GPU_SKIN (temporary A/B switch); without it this
 * build is byte-for-byte the CPU behavior. Stage A creates the
 * program, checks the vertex-uniform budget at runtime, and uploads
 * the folded 3x4 matrices as 99 vec4 each frame. Vertex attributes
 * and rendering switch come in Stage B -- the CPU path still
 * renders everything below. ---- */
GLuint jfaGpuProg = 0;
GLint jfaGpuBoneLoc = -1;
int jfaGpuOK = 0;
#if !JFA_GPU_RENDER
static int jfaGpuAccN = 0;
#endif
static float jfaBoneStage[JFA_GPU_BONE_VECS * 4]; /* 99 vec4 staging */
/* Substage B: prepared static GPU vertex data. jfaGpuVerts stays NULL
 * on ANY preparation failure - that only disables the (not yet used)
 * GPU draw path later; the CPU renderer keeps rendering regardless. */
static JFAGpuVertex *jfaGpuVerts = NULL;
static GLuint jfaGpuVbo = 0;
/* Substage C: runtime render-mode switch. JFA_GPU_RENDER (0/1) is a
 * compile-time switch, defaulting to 0:
 *   JFA_GPU_SKIN off               -> CPU-only build (no GPU code)
 *   JFA_GPU_SKIN on, RENDER=0      -> prep + matrix validation only;
 *                                     CPU path renders (Stage A mode)
 *   JFA_GPU_SKIN on, RENDER=1      -> GPU path draws when every
 *                                     runtime check passes; any
 *                                     failure falls back to CPU. */
#ifndef JFA_GPU_RENDER
#define JFA_GPU_RENDER 0
#endif
#if JFA_GPU_RENDER
int jfaGpuRenderReady = 0;
GLint jfaGpuPosLoc = -1, jfaGpuUvLoc = -1, jfaGpuNrmLoc = -1;
GLint jfaGpuBoneIdxLoc = -1, jfaGpuBoneWgtLoc = -1;
GLint jfaGpuMvpLoc = -1, jfaGpuModelLoc = -1, jfaGpuTexLoc = -1,
	jfaGpuModeLoc = -1;
static double jfaGpuDrawAcc = 0.0;
static int jfaGpuDrawN = 0;
#endif
{
	GLint maxVUV = 0;
	glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &maxVUV);
	if (!jfaOK) {
		SDL_Log("JFA GPU skin: skipped (no JFA data)");
	} else if (!jfa_gpu_budget_ok(maxVUV, jfa.nb)) {
		SDL_Log("JFA GPU skin: budget fails (maxVUV=%d, bones=%d, need %d) - CPU path",
			maxVUV, jfa.nb, JFA_GPU_BASE_VECS + JFA_GPU_BONE_VECS);
	} else {
		GLuint va=shader(GL_VERTEX_SHADER,jfa_gpu_vs),vb=shader(GL_FRAGMENT_SHADER,fs);
		if (va && vb) {
			jfaGpuProg=glCreateProgram();glAttachShader(jfaGpuProg,va);glAttachShader(jfaGpuProg,vb);
			glLinkProgram(jfaGpuProg);GLint ok=0;glGetProgramiv(jfaGpuProg,GL_LINK_STATUS,&ok);
			if(!ok){char log[512];glGetProgramInfoLog(jfaGpuProg,sizeof(log),NULL,log);SDL_Log("JFA GPU link: %s",log);glDeleteProgram(jfaGpuProg);jfaGpuProg=0;}
		} else {
			SDL_Log("JFA GPU shader compile failed");
		}
		glDeleteShader(va);glDeleteShader(vb);
		if (jfaGpuProg) {
			jfaGpuBoneLoc=glGetUniformLocation(jfaGpuProg,"uBones");
			if (jfaGpuBoneLoc >= 0) {
				jfaGpuOK = 1;
				SDL_Log("JFA GPU program ready: maxVUV=%d, bones=%d, uBones=%d vec4 (Stage A: matrices only, CPU path renders)",
					maxVUV, jfa.nb, jfa.nb * 3);
			} else {
				SDL_Log("JFA GPU: uBones location not found - CPU path");
				glDeleteProgram(jfaGpuProg);jfaGpuProg=0;
			}
		}
	}
}
/* ---- Stage C Substage B: prepare the static GPU vertex data once.
 * Runs during init, AFTER the Stage A program was created and only if
 * jfaGpuOK (GPU budget + program ready). One allocation, one
 * validated conversion, one upload into a dedicated GL_STATIC_DRAW
 * VBO. Every failure path logs, cleans up after itself and leaves the
 * CPU renderer untouched - jfaGpuVerts/jfaGpuVbo are only consumed by
 * a future draw substage, never bound for drawing here. ---- */
if (jfaGpuOK && jfaOK && jfa.nv > 0) {
	const size_t gnv = (size_t)jfa.nv;
	jfaGpuVerts = malloc(gnv * sizeof(JFAGpuVertex));
	if (!jfaGpuVerts) {
		SDL_Log("JFA GPU verts: alloc failed (%zu x %d bytes) - CPU path unaffected",
			gnv, (int)sizeof(JFAGpuVertex));
	} else if (!jfa_gpu_vertices(&jfa, jfaGpuVerts, gnv)) {
		SDL_Log("JFA GPU verts: conversion rejected input - CPU path unaffected");
		free(jfaGpuVerts);
		jfaGpuVerts = NULL;
	} else {
		glGenBuffers(1, &jfaGpuVbo);
		if (jfaGpuVbo == 0) {
			SDL_Log("JFA GPU verts: glGenBuffers returned 0 - preparation failed, CPU path unaffected");
			free(jfaGpuVerts);
			jfaGpuVerts = NULL;
		} else {
			/* Clear any pre-existing GL error so it is not
			 * misattributed to the upload below. */
			while (glGetError() != GL_NO_ERROR) {
			}
			glBindBuffer(GL_ARRAY_BUFFER, jfaGpuVbo);
			glBufferData(GL_ARRAY_BUFFER,
				gnv * sizeof(JFAGpuVertex), jfaGpuVerts,
				GL_STATIC_DRAW);
			const GLenum gerr = glGetError();
			if (gerr != GL_NO_ERROR) {
				SDL_Log("JFA GPU verts: upload failed (GL error 0x%04x) - preparation failed, CPU path unaffected",
					gerr);
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				glDeleteBuffers(1, &jfaGpuVbo);
				jfaGpuVbo = 0;
				free(jfaGpuVerts);
				jfaGpuVerts = NULL;
			} else {
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				SDL_Log("JFA GPU verts ready: %d verts, %d bytes each (Stage B: preparation only, no draw)",
					jfa.nv, (int)sizeof(JFAGpuVertex));
			}
		}
	}
}
#if JFA_GPU_RENDER
/* Substage C: query the GPU program's attribute and uniform
 * locations and validate the remaining hardware limits. Any failure
 * leaves jfaGpuRenderReady = 0 -> the CPU path renders. */
if (jfaGpuOK && jfaGpuVerts && jfaGpuVbo) {
	GLint maxAttribs = 0;
	glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttribs);
	jfaGpuPosLoc = glGetAttribLocation(jfaGpuProg, "aPosition");
	jfaGpuUvLoc = glGetAttribLocation(jfaGpuProg, "aUV");
	jfaGpuNrmLoc = glGetAttribLocation(jfaGpuProg, "aNormal");
	jfaGpuBoneIdxLoc = glGetAttribLocation(jfaGpuProg, "aBoneIndex");
	jfaGpuBoneWgtLoc = glGetAttribLocation(jfaGpuProg, "aBoneWeight");
	jfaGpuMvpLoc = glGetUniformLocation(jfaGpuProg, "uMVP");
	jfaGpuModelLoc = glGetUniformLocation(jfaGpuProg, "uModel");
	jfaGpuTexLoc = glGetUniformLocation(jfaGpuProg, "uTexture");
	jfaGpuModeLoc = glGetUniformLocation(jfaGpuProg, "uTextMode");
	if (maxAttribs < 5 || jfaGpuPosLoc < 0 || jfaGpuUvLoc < 0 ||
	    jfaGpuNrmLoc < 0 || jfaGpuBoneIdxLoc < 0 ||
	    jfaGpuBoneWgtLoc < 0 || jfaGpuMvpLoc < 0 ||
	    jfaGpuModelLoc < 0 || jfaGpuTexLoc < 0 ||
	    jfaGpuModeLoc < 0) {
		SDL_Log("JFA GPU render: locations/limits fail (maxAttribs=%d, pos=%d, uv=%d, nrm=%d, bidx=%d, bwgt=%d, mvp=%d, model=%d, tex=%d, mode=%d) - CPU path",
			maxAttribs, jfaGpuPosLoc, jfaGpuUvLoc,
			jfaGpuNrmLoc, jfaGpuBoneIdxLoc, jfaGpuBoneWgtLoc,
			jfaGpuMvpLoc, jfaGpuModelLoc, jfaGpuTexLoc,
			jfaGpuModeLoc);
	} else {
		jfaGpuRenderReady = 1;
		SDL_Log("JFA GPU render active: %d verts, attribs %d/%d/%d/%d/%d (Stage C: GPU path draws, CPU is fallback)",
			jfa.nv, jfaGpuPosLoc, jfaGpuUvLoc, jfaGpuNrmLoc,
			jfaGpuBoneIdxLoc, jfaGpuBoneWgtLoc);
	}
}
#endif
#endif
 glEnable(GL_DEPTH_TEST);
glDepthFunc(GL_LEQUAL);

int run = 1;
SDL_Event e;
float angle = 0;

Uint64 start = SDL_GetPerformanceCounter();
Uint64 animationStart = SDL_GetPerformanceCounter();

int frames = 0, fps = 0;
double freq = (double)SDL_GetPerformanceFrequency();
 while(run){
  while(SDL_PollEvent(&e))if(e.type==SDL_QUIT||e.type==SDL_APP_TERMINATING)run=0;
  double animationTime =
    (SDL_GetPerformanceCounter() - animationStart) / freq;

if (!jfaOK) {
    /* FBX fallback: rebuild the mesh on the CPU every frame (existing path). */
    fbx_free_mesh(&frameMesh);

    if (fbxOK) {
        fbxOK = fbx_build_mesh(&fbx, animationTime, &frameMesh);
    }
}
  int w,h;SDL_GL_GetDrawableSize(win,&w,&h);if(w<1)w=1;if(h<1)h=1;glViewport(0,0,w,h);glClearColor(.08f,.08f,.12f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  if(tex && (jfaOK || (fbxOK && frameMesh.vertex_count > 0))){
   float c=cosf(angle),s=sinf(angle);float model[16]={c,0,-s,0,0,1,0,0,s,0,c,0,0,0,0,1},trans[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,-1.1f,-3.5f,1},vm[16],pr[16]={0},mvp[16];mm(vm,trans,model);
   float f=1.0f/tanf(1.047198f*0.5f),aspect=(float)w/h;pr[0]=f/aspect;pr[5]=f;pr[10]=-1.002f;pr[11]=-1;pr[14]=-.2002f;mm(mvp,pr,vm);
   float identity[16] = {
    1,0,0,0,
    0,1,0,0,
    0,0,1,0,
    0,0,0,1
};

float floorMVP[16];
mm(floorMVP, pr, trans);

glUseProgram(prog);
glUniform1f(modeLoc, 1.0f);
glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, floorMVP);
glUniformMatrix4fv(modelLoc, 1, GL_FALSE, identity);

glBindBuffer(GL_ARRAY_BUFFER, arenaVBO);

glEnableVertexAttribArray(pos);
glEnableVertexAttribArray(uv);
glEnableVertexAttribArray(normal);
glEnableVertexAttribArray(col);

glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, position));

glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, uv));

glVertexAttribPointer(normal, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, normal));

glVertexAttribPointer(col, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, color));

glDrawArrays(GL_TRIANGLES, 0, 6);

glBindBuffer(GL_ARRAY_BUFFER, arenaWallsVBO);

glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, position));

glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, uv));

glVertexAttribPointer(normal, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, normal));

glVertexAttribPointer(col, 3, GL_FLOAT, GL_FALSE,
    sizeof(FloorVertex), (void*)offsetof(FloorVertex, color));

glDrawArrays(GL_TRIANGLES, 0, 144);

glUseProgram(prog);glUniform1f(modeLoc,0);
glDisableVertexAttribArray(col);
glVertexAttrib3f(col, 1.0f, 1.0f, 1.0f);
   glUniformMatrix4fv(mvpLoc,1,GL_FALSE,mvp);glUniformMatrix4fv(modelLoc,1,GL_FALSE,model);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,tex);glUniform1i(texLoc,0);
   glBindBuffer(GL_ARRAY_BUFFER, meshVBO);

size_t drawCount = 0;
const JFAClip *jfaCur = NULL;
int af = 0;
const float worldPos[3] = { 0.0f, 0.0f, 0.0f };
if (jfaOK) {
    /* ---- Animation advance (shared by CPU and GPU paths): the
     * clip cycle and sampled frame are computed identically for
     * both render paths - timing never depends on which one draws. ---- */
    jfaCur = &jfa.clips[jfaClip];
    jfaClipDur = (double)jfaCur->frames / (double)jfaCur->fps;
    if (animationTime - jfaClipStart >= jfaClipDur) {
        jfaClip = (jfaClip + 1) % JFA_CLIP_COUNT;
        jfaClipStart = animationTime;
        jfaCur = &jfa.clips[jfaClip];
        jfaClipDur = (double)jfaCur->frames / (double)jfaCur->fps;
        SDL_Log("JFA clip: %s (%d frames @ %u fps)",
            jfa_clip_suffix[jfaClip], jfaCur->frames,
            (unsigned)jfaCur->fps);
    }
    af = jfa_skin_frame_index(jfaCur, animationTime - jfaClipStart);
}
#if JFA_GPU_SKIN && JFA_GPU_RENDER
int gpuDraw = 0;
if (jfaOK && jfaGpuRenderReady) {
    gpuDraw = 1;
    /* ---- Stage C: GPU skinning draw. One draw call from the
     * static jfaGpuVbo; the pose is the same folded matrix set the
     * CPU path uses (Substage A proved the fold bit-identical). ---- */
    Uint64 tGpu0 = SDL_GetPerformanceCounter();
    /* Clear stale GL errors so the post-draw check below is not
     * misattributed. */
    while (glGetError() != GL_NO_ERROR) {
    }
    jfa_gpu_fold(&jfa, jfaClip, af, jfaScale, jfaCenter, worldPos,
        jfaBoneStage);
    glUseProgram(jfaGpuProg);
    glUniformMatrix4fv(jfaGpuMvpLoc, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(jfaGpuModelLoc, 1, GL_FALSE, model);
    glUniform1i(jfaGpuTexLoc, 0);
    glUniform1f(jfaGpuModeLoc, 0.0f);
    glUniform4fv(jfaGpuBoneLoc, jfa.nb * 3, jfaBoneStage);
    glBindBuffer(GL_ARRAY_BUFFER, jfaGpuVbo);
    glEnableVertexAttribArray(jfaGpuPosLoc);
    glEnableVertexAttribArray(jfaGpuUvLoc);
    glEnableVertexAttribArray(jfaGpuNrmLoc);
    glEnableVertexAttribArray(jfaGpuBoneIdxLoc);
    glEnableVertexAttribArray(jfaGpuBoneWgtLoc);
    glVertexAttribPointer(jfaGpuPosLoc, 3, GL_FLOAT, GL_FALSE,
        (GLsizei)sizeof(JFAGpuVertex),
        (void*)offsetof(JFAGpuVertex, position));
    glVertexAttribPointer(jfaGpuUvLoc, 2, GL_FLOAT, GL_FALSE,
        (GLsizei)sizeof(JFAGpuVertex),
        (void*)offsetof(JFAGpuVertex, uv));
    glVertexAttribPointer(jfaGpuNrmLoc, 3, GL_FLOAT, GL_FALSE,
        (GLsizei)sizeof(JFAGpuVertex),
        (void*)offsetof(JFAGpuVertex, normal));
    glVertexAttribPointer(jfaGpuBoneIdxLoc, 4, GL_UNSIGNED_BYTE,
        GL_FALSE, (GLsizei)sizeof(JFAGpuVertex),
        (void*)offsetof(JFAGpuVertex, bone));
    glVertexAttribPointer(jfaGpuBoneWgtLoc, 4, GL_FLOAT, GL_FALSE,
        (GLsizei)sizeof(JFAGpuVertex),
        (void*)offsetof(JFAGpuVertex, weight));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)jfa.nv);
    /* Attribute enable state is GLOBAL in GLES2: the two extra
     * bone/weight arrays must not leak into the CPU/FBX/floor
     * draws. pos/uv/normal are re-pointed by whichever path draws
     * next, exactly as they already are today. */
    glDisableVertexAttribArray(jfaGpuBoneIdxLoc);
    glDisableVertexAttribArray(jfaGpuBoneWgtLoc);
    glUseProgram(prog); /* restore: FPS text renders with prog */
    glBindBuffer(GL_ARRAY_BUFFER, meshVBO);
    {
        GLenum derr = glGetError();
        if (derr != GL_NO_ERROR) {
            SDL_Log("JFA GPU render failed (GL error 0x%04x) - CPU path for the rest of the session",
                derr);
            jfaGpuRenderReady = 0;
        }
    }
    Uint64 tGpu1 = SDL_GetPerformanceCounter();
    jfaGpuDrawAcc += (double)(tGpu1 - tGpu0);
    if (++jfaGpuDrawN == 60) {
        double ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
        SDL_Log("PERF jfa gpu draw %.2f ms (fold+uniforms+attribs+draw)",
            jfaGpuDrawAcc * ms / 60.0);
        jfaGpuDrawAcc = 0.0;
        jfaGpuDrawN = 0;
    }
    drawCount = (size_t)jfa.nv;
} else
#endif
if (jfaOK) {
    /* ---- JFA: CPU skin the current clip frame, then one sub-data upload ---- */
    Uint64 tSkin0 = SDL_GetPerformanceCounter();
    jfa_skin_frame(&jfa, jfaClip, af, jfaScale, jfaCenter, worldPos,
        &jfaSkin);
    Uint64 tSkin1 = SDL_GetPerformanceCounter();

    glBufferSubData(
        GL_ARRAY_BUFFER,
        0,
        (size_t)jfa.nv * sizeof(JFASkinVertex),
        jfaSkin.v
    );
    Uint64 tUpload1 = SDL_GetPerformanceCounter();

    jfaAccSkin += (double)(tSkin1 - tSkin0);
    jfaAccUpload += (double)(tUpload1 - tSkin1);
    jfaAccDraw += (double)(tUpload1 - tSkin0);
    if (++jfaAccN == 60) {
        double ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
        SDL_Log("PERF jfa skin %.2f ms, upload %.2f ms, skin+upload %.2f ms",
            jfaAccSkin * ms / 60.0,
            jfaAccUpload * ms / 60.0,
            jfaAccDraw * ms / 60.0);
        jfaAccSkin = jfaAccUpload = jfaAccDraw = 0.0;
        jfaAccN = 0;
    }
#ifdef JFA_GPU_SKIN
#if !JFA_GPU_RENDER
    /* Stage A validation mode (RENDER=0): fold the same pose into
     * the compact vec4 layout and upload it to the (not rendering)
     * GPU program. In RENDER=1 builds the GPU branch above does
     * the fold + upload itself. */
    if (jfaGpuOK) {
        Uint64 tFold0 = SDL_GetPerformanceCounter();
        jfa_gpu_fold(&jfa, jfaClip, af, jfaScale, jfaCenter, worldPos,
            jfaBoneStage);
        glUseProgram(jfaGpuProg);
        glUniform4fv(jfaGpuBoneLoc, jfa.nb * 3, jfaBoneStage);
        glUseProgram(prog);
        if (++jfaGpuAccN == 60) {
            double ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
            SDL_Log("PERF jfa gpu fold+upload %.2f ms (Stage A, CPU path renders)",
                (double)(SDL_GetPerformanceCounter() - tFold0) * ms / 60.0);
            jfaGpuAccN = 0;
        }
    }
#endif
#endif
    drawCount = (size_t)jfa.nv;
} else {
    glBufferData(
        GL_ARRAY_BUFFER,
        frameMesh.vertex_count * sizeof(FBXVertex),
        frameMesh.vertices,
        GL_DYNAMIC_DRAW
    );
    drawCount = frameMesh.vertex_count;
}

#if JFA_GPU_SKIN && JFA_GPU_RENDER
if (!gpuDraw)
#endif
{
glEnableVertexAttribArray(pos);
glEnableVertexAttribArray(uv);
glEnableVertexAttribArray(normal);
   glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE,
    sizeof(FBXVertex), (void*)offsetof(FBXVertex, position));

glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE,
    sizeof(FBXVertex), (void*)offsetof(FBXVertex, uv));

glVertexAttribPointer(normal, 3, GL_FLOAT, GL_FALSE,
    sizeof(FBXVertex), (void*)offsetof(FBXVertex, normal));

glDrawArrays(GL_TRIANGLES, 0, (GLsizei)drawCount);
}
  }
  frames++;Uint64 now=SDL_GetPerformanceCounter();if((double)(now-start)/freq>=.5){fps=(int)(frames*freq/(double)(now-start)+.5);frames=0;start=now;}
  drawFPS(prog,fpsVBO,pos,col,mvpLoc,modeLoc,w,h,fps);SDL_GL_SwapWindow(win);
 }
 glDeleteBuffers(1, &meshVBO);
glDeleteBuffers(1, &fpsVBO);
glDeleteBuffers(1, &arenaVBO);
glDeleteBuffers(1, &arenaWallsVBO);
if (tex)
    glDeleteTextures(1, &tex);

glDeleteProgram(prog);

if (jfaOK) {
    jfa_skin_free(&jfaSkin);
    jfa_free(&jfa);
}

#ifdef JFA_GPU_SKIN
/* Stage B cleanup: the GPU VBO and the prepared vertex array are
 * owned independently of the CPU renderer state. */
if (jfaGpuVbo) {
	glDeleteBuffers(1, &jfaGpuVbo);
	jfaGpuVbo = 0;
}
free(jfaGpuVerts);
jfaGpuVerts = NULL;
if (jfaGpuProg) {
	glDeleteProgram(jfaGpuProg);
	jfaGpuProg = 0;
}
#endif

fbx_free_mesh(&frameMesh);
fbx_free(&fbx);

SDL_GL_DeleteContext(ctx);
SDL_DestroyWindow(win);
SDL_Quit();

return 0;
}
