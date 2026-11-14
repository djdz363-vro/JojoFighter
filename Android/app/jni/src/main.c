#include <SDL.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

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
 GLuint t=0;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,r->w,r->h,0,GL_RGBA,GL_UNSIGNED_BYTE,r->pixels);SDL_Log("Texture OK: %dx%d",r->w,r->h);SDL_FreeSurface(r);return t;
}
static void drawFPS(GLuint prog,GLuint vbo,GLint pos,GLint col,GLint mvp,GLint mode,int w,int h,int fps){
 char str[16];snprintf(str,sizeof(str),"FPS:%d",fps);float a[16*5*7*6];int n=0;float dx=6.0f/w,dy=6.0f/h,x0=1.0f-((float)SDL_strlen(str)*18+12)*2/w,y0=1.0f-24.0f/h;
 for(int ch=0;str[ch];ch++)for(int y=0;y<7;y++)for(int x=0;x<5;x++)if(font[glyph(str[ch])][y]&(1u<<(4-x))){a[n++]=x0+(ch*6+x)*dx;a[n++]=y0-y*dy;a[n++]=0;a[n++]=1;a[n++]=1;a[n++]=1;}
 const float id[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};glDisable(GL_DEPTH_TEST);glUseProgram(prog);glUniformMatrix4fv(mvp,1,GL_FALSE,id);glUniform1f(mode,1);
 glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,n*sizeof(float),a,GL_DYNAMIC_DRAW);glEnableVertexAttribArray(pos);glEnableVertexAttribArray(col);glVertexAttribPointer(pos,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)0);glVertexAttribPointer(col,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(3*sizeof(float)));glDrawArrays(GL_POINTS,0,n/6);glEnable(GL_DEPTH_TEST);
}

int main(int argc,char **argv){
 if(SDL_Init(SDL_INIT_VIDEO)!=0){SDL_Log("SDL: %s",SDL_GetError());return 1;}
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,16);
 SDL_Window *win=SDL_CreateWindow("JoJo Fighter - OBJ Test",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,800,450,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
 if(!win){SDL_Log("Window: %s",SDL_GetError());SDL_Quit();return 1;}SDL_GLContext ctx=SDL_GL_CreateContext(win);if(!ctx){SDL_Log("Context: %s",SDL_GetError());SDL_DestroyWindow(win);SDL_Quit();return 1;}SDL_GL_SetSwapInterval(1);
 GLuint prog=programCreate();if(!prog){SDL_GL_DeleteContext(ctx);SDL_DestroyWindow(win);SDL_Quit();return 1;}
 Mesh mesh={0};int ok=loadOBJ("model0.obj",&mesh);GLuint tex=loadBMP("model0.bmp");
 GLuint meshVBO=0,fpsVBO=0;glGenBuffers(1,&meshVBO);glGenBuffers(1,&fpsVBO);if(ok){glBindBuffer(GL_ARRAY_BUFFER,meshVBO);glBufferData(GL_ARRAY_BUFFER,mesh.count*sizeof(Vertex),mesh.v,GL_STATIC_DRAW);}
 GLint pos=glGetAttribLocation(prog,"aPosition"),uv=glGetAttribLocation(prog,"aUV"),normal=glGetAttribLocation(prog,"aNormal"),col=glGetAttribLocation(prog,"aColor");
 GLint mvpLoc=glGetUniformLocation(prog,"uMVP"),modelLoc=glGetUniformLocation(prog,"uModel"),texLoc=glGetUniformLocation(prog,"uTexture"),modeLoc=glGetUniformLocation(prog,"uTextMode");
 glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);int run=1;SDL_Event e;float angle=0;Uint64 start=SDL_GetPerformanceCounter();int frames=0,fps=0;double freq=(double)SDL_GetPerformanceFrequency();
 while(run){
  while(SDL_PollEvent(&e))if(e.type==SDL_QUIT||e.type==SDL_APP_TERMINATING)run=0;
  int w,h;SDL_GL_GetDrawableSize(win,&w,&h);if(w<1)w=1;if(h<1)h=1;glViewport(0,0,w,h);glClearColor(.08f,.08f,.12f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  if(ok&&tex){
   float c=cosf(angle),s=sinf(angle);float model[16]={c,0,-s,0,0,1,0,0,s,0,c,0,0,0,0,1},trans[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,-3.5f,1},vm[16],pr[16]={0},mvp[16];mm(vm,trans,model);
   float f=1.0f/tanf(1.047198f*0.5f),aspect=(float)w/h;pr[0]=f/aspect;pr[5]=f;pr[10]=-1.002f;pr[11]=-1;pr[14]=-.2002f;mm(mvp,pr,vm);
   glUseProgram(prog);glUniform1f(modeLoc,0);glUniformMatrix4fv(mvpLoc,1,GL_FALSE,mvp);glUniformMatrix4fv(modelLoc,1,GL_FALSE,model);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,tex);glUniform1i(texLoc,0);
   glBindBuffer(GL_ARRAY_BUFFER,meshVBO);glEnableVertexAttribArray(pos);glEnableVertexAttribArray(uv);glEnableVertexAttribArray(normal);
   glVertexAttribPointer(pos,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)offsetof(Vertex,p));glVertexAttribPointer(uv,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)offsetof(Vertex,uv));glVertexAttribPointer(normal,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)offsetof(Vertex,n));glDrawArrays(GL_TRIANGLES,0,(GLsizei)mesh.count);
  }
  frames++;Uint64 now=SDL_GetPerformanceCounter();if((double)(now-start)/freq>=.5){fps=(int)(frames*freq/(double)(now-start)+.5);frames=0;start=now;}
  drawFPS(prog,fpsVBO,pos,col,mvpLoc,modeLoc,w,h,fps);SDL_GL_SwapWindow(win);angle+=.008f;if(angle>6.283185f)angle-=6.283185f;
 }
 glDeleteBuffers(1,&meshVBO);glDeleteBuffers(1,&fpsVBO);if(tex)glDeleteTextures(1,&tex);glDeleteProgram(prog);free(mesh.v);SDL_GL_DeleteContext(ctx);SDL_DestroyWindow(win);SDL_Quit();return 0;
}
