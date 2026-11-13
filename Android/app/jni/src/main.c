#include <SDL.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>

static const char *vertexShaderSource =
    "attribute vec3 aPosition;"
    "attribute vec3 aColor;"
    "uniform mat4 uMVP;"
    "varying vec3 vColor;"
    "void main() {"
    "    gl_Position = uMVP * vec4(aPosition, 1.0);"
    "    gl_PointSize = 3.0;"
    "    vColor = aColor;"
    "}";

static const char *fragmentShaderSource =
    "precision mediump float;"
    "varying vec3 vColor;"
    "void main() {"
    "    gl_FragColor = vec4(vColor, 1.0);"
    "}";

static const float vertices[] = {
    -1,-1,-1,  1,0,0,  1,-1,-1,  1,0,0,
     1, 1,-1,  1,0,0, -1, 1,-1,  1,0,0,
    -1,-1, 1,  0,1,0,  1,-1, 1,  0,1,0,
     1, 1, 1,  0,1,0, -1, 1, 1,  0,1,0
};

static const GLushort indices[] = {
    0,1,2, 0,2,3, 5,4,7, 5,7,6,
    4,0,3, 4,3,7, 1,5,6, 1,6,2,
    3,2,6, 3,6,7, 4,5,1, 4,1,0
};

/* 5x7 bitmap font: FPS: and digits 0-9. */
static const unsigned char font[14][7] = {
    {14,16,16,14,16,16,16}, /* F */
    {30,17,17,30,16,16,16}, /* P */
    {15,16,16,14,1,1,30},   /* S */
    {0,4,4,0,4,4,0},        /* : */
    {14,17,19,21,25,17,14}, /* 0 */
    {4,12,4,4,4,4,14},      /* 1 */
    {14,17,1,2,4,8,31},     /* 2 */
    {30,1,1,14,1,1,30},     /* 3 */
    {2,6,10,18,31,2,2},     /* 4 */
    {31,16,16,30,1,1,30},   /* 5 */
    {14,16,16,30,17,17,14}, /* 6 */
    {31,1,2,4,8,8,8},       /* 7 */
    {14,17,17,14,17,17,14}, /* 8 */
    {14,17,17,15,1,1,14}    /* 9 */
};

static int glyphIndex(char c)
{
    if (c == 'F') return 0;
    if (c == 'P') return 1;
    if (c == 'S') return 2;
    if (c == ':') return 3;
    if (c >= '0' && c <= '9') return 4 + c - '0';
    return 3;
}

static void multiply(float *out, const float *a, const float *b)
{
    float result[16];
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            result[col * 4 + row] = 0.0f;
            for (int k = 0; k < 4; k++)
                result[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
        }
    for (int i = 0; i < 16; i++) out[i] = result[i];
}

static GLuint compileShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        SDL_Log("Shader error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint createProgram(void)
{
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    if (!vs || !fs) return 0;

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        SDL_Log("Program link error: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static void drawFps(GLuint program, GLuint fpsVbo,
                    GLint position, GLint color, GLint mvpLocation,
                    int width, int height, int fps)
{
    char text[16];
    snprintf(text, sizeof(text), "FPS:%d", fps);

    /* One vertex per lit font pixel: x,y,z,r,g,b. */
    float points[16 * 5 * 7 * 6];
    int count = 0;
    const float cellX = 6.0f / (float)width;
    const float cellY = 6.0f / (float)height;
    const float textWidth = (float)(SDL_strlen(text) * 6) * 3.0f;
    float xStart = 1.0f - (textWidth + 12.0f) * 2.0f / (float)width;
    float yTop = 1.0f - 12.0f * 2.0f / (float)height;

    for (int ch = 0; text[ch] != '\0'; ch++) {
        const unsigned char *rows = font[glyphIndex(text[ch])];
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (rows[row] & (1u << (4 - col))) {
                    points[count++] = xStart + (ch * 6 + col) * cellX;
                    points[count++] = yTop - row * cellY;
                    points[count++] = 0.0f;
                    points[count++] = 1.0f;
                    points[count++] = 1.0f;
                    points[count++] = 1.0f;
                }
            }
        }
    }

    const float identity[16] = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };

    glDisable(GL_DEPTH_TEST);
    glUseProgram(program);
    glUniformMatrix4fv(mvpLocation, 1, GL_FALSE, identity);

    glBindBuffer(GL_ARRAY_BUFFER, fpsVbo);
    glBufferData(GL_ARRAY_BUFFER, count * sizeof(float),
                 points, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(position);
    glEnableVertexAttribArray(color);
    glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE,
                          6 * sizeof(float), (void *)0);
    glVertexAttribPointer(color, 3, GL_FLOAT, GL_FALSE,
                          6 * sizeof(float), (void *)(3 * sizeof(float)));

    glDrawArrays(GL_POINTS, 0, count / 6);
    glEnable(GL_DEPTH_TEST);
}

int main(int argc, char *argv[])
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL initialization failed: %s", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);

    SDL_Window *window = SDL_CreateWindow(
        "JoJo Fighter - 3D Test",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        800, 450,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        SDL_Log("OpenGL context failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetSwapInterval(1);
    GLuint program = createProgram();
    if (!program) {
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    GLuint vbo, ibo, fpsVbo;
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ibo);
    glGenBuffers(1, &fpsVbo);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    GLint position = glGetAttribLocation(program, "aPosition");
    GLint color = glGetAttribLocation(program, "aColor");
    GLint mvpLocation = glGetUniformLocation(program, "uMVP");

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    int running = 1;
    SDL_Event event;
    float angle = 0.0f;
    Uint64 lastCounter = SDL_GetPerformanceCounter();
    Uint64 fpsStart = lastCounter;
    int frameCount = 0;
    int displayedFps = 0;
    double frequency = (double)SDL_GetPerformanceFrequency();

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                event.type == SDL_APP_TERMINATING)
                running = 0;
        }

        int width, height;
        SDL_GL_GetDrawableSize(window, &width, &height);
        if (height < 1) height = 1;
        if (width < 1) width = 1;

        glViewport(0, 0, width, height);
        glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float c = cosf(angle);
        float s = sinf(angle);
        float rotationY[16] = {
             c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1
        };
        float rotationX[16] = {
            1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1
        };
        float translation[16] = {
            1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-4,1
        };

        float model[16], temp[16], projection[16], mvp[16];
        multiply(temp, rotationX, rotationY);
        multiply(model, translation, temp);

        float f = 1.0f / tanf(0.785398f);
        float aspect = (float)width / (float)height;
        for (int i = 0; i < 16; i++) projection[i] = 0.0f;
        projection[0] = f / aspect;
        projection[5] = f;
        projection[10] = -1.002f;
        projection[11] = -1.0f;
        projection[14] = -0.2002f;
        multiply(mvp, projection, model);

        glUseProgram(program);
        glUniformMatrix4fv(mvpLocation, 1, GL_FALSE, mvp);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glEnableVertexAttribArray(position);
        glEnableVertexAttribArray(color);
        glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void *)0);
        glVertexAttribPointer(color, 3, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float), (void *)(3 * sizeof(float)));
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);

        /* Measure rendered frames and refresh the displayed value twice/sec. */
        frameCount++;
        Uint64 now = SDL_GetPerformanceCounter();
        if ((double)(now - fpsStart) / frequency >= 0.5) {
            displayedFps = (int)(frameCount * frequency / (double)(now - fpsStart) + 0.5);
            frameCount = 0;
            fpsStart = now;
        }

        drawFps(program, fpsVbo, position, color, mvpLocation,
                width, height, displayedFps);

        SDL_GL_SwapWindow(window);

        angle += 0.01f;
        if (angle > 6.283185f) angle -= 6.283185f;
        lastCounter = now;
    }

    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ibo);
    glDeleteBuffers(1, &fpsVbo);
    glDeleteProgram(program);
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
