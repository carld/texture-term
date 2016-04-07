#define GLEW_STATIC
#include <GL/glew.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_GLX
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>

#include <xkbcommon/xkbcommon.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "font/bdf.h"
#include "libtsm.h"
#include "shl_pty.h"
#include "external/xkbcommon-keysyms.h"

#include "terminal.h"
#include "display.h"
#include "shader.h"

float vertices[] = {
    //  Position  Color             Texcoords
    -1.0f,  1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, // Top-left
     1.0f,  1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, // Top-right
     1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, // Bottom-right
    -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f  // Bottom-left
};

GLuint elements[] = {
    0, 1, 2,
    2, 3, 0
};

GLuint create_program(struct shader *shaders, int l) {
  GLuint program;
  program = shader_program(shaders, l);
  glBindFragDataLocation(program, 0, "outColor");
  glLinkProgram(program);

  glUseProgram(program);

  GLint posAttrib = glGetAttribLocation(program, "position");
  glEnableVertexAttribArray(posAttrib);
  glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 7*sizeof(float), 0);

  GLint colAttrib = glGetAttribLocation(program, "color");
  glEnableVertexAttribArray(colAttrib);
  glVertexAttribPointer(colAttrib, 3, GL_FLOAT, GL_FALSE, 7*sizeof(float), (void*)(2*sizeof(float)));

  GLint texAttrib = glGetAttribLocation(program, "texcoord");
  glEnableVertexAttribArray(texAttrib);
  glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 7*sizeof(float), (void*)(5*sizeof(float)));

  return program;
}

void setup() {
  GLuint vertexBuffer;
  glGenBuffers(1, &vertexBuffer); // vbo
  glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);

  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);

  GLuint ebo;
  glGenBuffers(1, &ebo);

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(elements), elements, GL_STATIC_DRAW);

  glClearColor(0.3,0.3,0.3,1.0);
  glColor4ub(255,255,255,255);
}

void update_texture(struct display *disp, GLenum filter) {
  glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
  glBindTexture(GL_TEXTURE_2D, disp->tex_id);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
    disp->width, disp->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, disp->pixels);
}

void render() {
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

void info() {
  printf("OpenGL          %s\n", glGetString(GL_VERSION));
  printf("GLSL            %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
  printf("Vendor          %s\n", glGetString(GL_VENDOR));
  printf("Renderer        %s\n", glGetString(GL_RENDERER));
  //printf("Extensions\n%s\n", glGetString(GL_EXTENSIONS));
}

void display_stats(struct display *disp) {
  printf("glyphs rendered:  %d  (skipped %d)\n", disp->glyphs_rendered, disp->glyphs_clean_skipped);
}

static Display *x_display = NULL;

static int draw_cb(struct tsm_screen *screen, uint32_t id,
                   const uint32_t *ch, size_t len,
                   unsigned int cwidth, unsigned int posx,
                   unsigned int posy,
                   const struct tsm_screen_attr *attr,
                   tsm_age_t age, void *data)
{
  uint8_t fg[4], bg[4];
  struct display *disp = (struct display *)data;
  int skip;

  skip = age && disp->age && age <= disp->age;
  if (skip) return 0;

  if (attr->inverse) {
    fg[0] = attr->br; fg[1] = attr->bg; fg[2] = attr->bb; fg[3] = 255;
    bg[0] = attr->fr; bg[1] = attr->fg; bg[2] = attr->fb; bg[3] = 255;
  } else {
    fg[0] = attr->fr; fg[1] = attr->fg; fg[2] = attr->fb; fg[3] = 255;
    bg[0] = attr->br; bg[1] = attr->bg; bg[2] = attr->bb; bg[3] = 255;
  }

  if (!len) {
    display_put(disp, ' ', posx, posy, fg, bg);
  } else {
    int i;
    for(i = 0; i < len; i++)
      display_put(disp, ch[i], posx, posy, fg, bg);
  }
  return 0;
}

static void key_callback(GLFWwindow* window, int key /*glfw*/, int scancode, int action, int mods) {
  struct terminal *term = glfwGetWindowUserPointer(window);
  int m = 0;
  unsigned int ucs4 = 0;
  assert(term != NULL);

  if (action == GLFW_RELEASE) return;

//  if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
//    glfwSetWindowShouldClose(window, GL_TRUE);
  if (mods & GLFW_MOD_CONTROL)  m |= TSM_CONTROL_MASK;
  if (mods & GLFW_MOD_SHIFT) m |= TSM_SHIFT_MASK;
  if (mods & GLFW_MOD_ALT)   m |= TSM_ALT_MASK;
  if (mods & GLFW_MOD_SUPER)  m |= TSM_LOGO_MASK;

  key = XkbKeycodeToKeysym(x_display, scancode, 0, mods & GLFW_MOD_SHIFT ? 1 : 0);
  ucs4 = xkb_keysym_to_utf32(key);
  if (!ucs4) ucs4 = TSM_VTE_INVALID;
  tsm_vte_handle_keyboard(term->vte, key, XKB_KEY_NoSymbol, m, ucs4);
}

static void waitEvents(Display *xdisplay, struct terminal *term) {
  int fdlist[2];
  
  fdlist[0] = ConnectionNumber(xdisplay);
  fdlist[1] = shl_pty_get_fd(term->pty);
  /* ignoring joystick fd */

  assert(fdlist[0] > 0);
  assert(fdlist[1] > 0);

  while( select_fd_array(NULL, fdlist, 2) <= 0 )
    ;

  glfwPollEvents();
  shl_pty_dispatch(term->pty);
}

static void pollEvents(Display *xdisplay, struct terminal *term) {
  glfwPollEvents();
  shl_pty_dispatch(term->pty);
}

int main(int argc, char *argv[], char *envp[])
{
  GLFWwindow *window;
  GLFWmonitor *monitor;
  const GLFWvidmode *mode;
  struct terminal *terminal = NULL;
  struct display *display = NULL;

  /* default resolution for 9x15 font at 80 cols and 25 rows */
  GLuint screenSize[2] = {80*9,25*15};
  GLuint displaySize[2] = {80*9,25*15};

  GLuint program;
  const char *fontfile = "9x15.bdf";
  int opt;
  struct shader shaders[2];
  int dot_stretch = 1, wait_events = 1, full_screen = 0, show_pointer = 1;
  GLenum texture_filter = GL_NEAREST;
  
  shaders[0].filename = "crt-lottes.glsl";
  shaders[0].type     = GL_FRAGMENT_SHADER;
  shaders[1].filename = "vertex.glsl";
  shaders[1].type     = GL_VERTEX_SHADER;

  while ((opt = getopt(argc, argv, "f:s:g:w:ldphm")) != -1) {
    switch (opt) {
    case 'f':
      fontfile = optarg;
      break;
    case 's':
      shaders[0].filename = optarg;
      break;    
    case 'd':
      dot_stretch ^= 1;
      break;      
    case 'g':
      displaySize[0] = atoi(strtok(optarg,"x"));
      displaySize[1] = atoi(strtok(NULL,"x"));
      break;
    case 'w':
      screenSize[0] = atoi(strtok(optarg,"x"));
      screenSize[1] = atoi(strtok(NULL,"x"));
      full_screen ^= 1;
      show_pointer ^= 1;
      break;
    case 'l':
      texture_filter = GL_LINEAR;
      break;
    case 'p':
      wait_events ^= 1; 
      break;
    case 'm':
      full_screen ^= 1;
      screenSize[0] = 0;
      screenSize[1] = 0;
      break;
    case 'h':
    default: /* '?' */
       printf("Usage: %s [-f bdf file] [-s glsl shader] [-g w x h] [-w w x h] [-d] [-l] [-p] [-m]\n", argv[0]);
       exit(EXIT_SUCCESS);
    }
  }

  /* displaySize is the size of the CRT monitor / character terminal texture */
  display_create(&display, displaySize[0], displaySize[1], fontfile, dot_stretch);
  terminal_create(&terminal, display->cols, display->rows);

  if (!glfwInit())
    return -1;

  monitor = glfwGetPrimaryMonitor();
  mode    = glfwGetVideoMode(monitor); 

  /* screenSize is the size of the window being rendered to */
  if (screenSize[0] == 0 || screenSize[1] == 0) {
    screenSize[0] = mode->width;
    screenSize[1] = mode->height;
  }

  glfwWindowHint(GLFW_RED_BITS, mode->redBits);
  glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
  glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
  glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

  glfwWindowHint(GLFW_DECORATED, GL_FALSE);

  window = glfwCreateWindow(screenSize[0], screenSize[1], argv[0], full_screen ? monitor : NULL, NULL);
  if (!window) {
    glfwTerminate();
    return -1;
  }

  printf("Resolution      %d x %d, %d x %d, %d x %d\n",
    screenSize[0], screenSize[1],
    display->width, display->height,
    display->cols, display->rows);

  glfwMakeContextCurrent(window);
  glfwSetWindowUserPointer(window, terminal);
  glfwSwapInterval(1);
  glfwSetKeyCallback(window, key_callback);
  if (show_pointer == 0)
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);

  x_display = glfwGetX11Display();

  info();

  glewExperimental = GL_TRUE;
  GLenum err = glewInit();
  if (err != GLEW_OK) exit(1);
  if (!GLEW_VERSION_3_2) exit(1);

  setup();
  glGenTextures(1, & display->tex_id);
  program = create_program(shaders, 2);

  if (shaders[0].status == 0) exit(-3);

  GLint sourceSize = glGetUniformLocation(program, "sourceSize");
  GLint targetSize = glGetUniformLocation(program, "targetSize");
  GLint appTime = glGetUniformLocation(program, "appTime");
  glUniform2f(sourceSize, display->width, display->height);
  glUniform2f(targetSize, screenSize[0], screenSize[1]);

  while (!glfwWindowShouldClose(window)) {
    glUniform1f(appTime, glfwGetTime());

    display->age = tsm_screen_draw(terminal->screen, draw_cb, display);
    display_update(display);
    update_texture(display, texture_filter);

    render();

    glfwSwapBuffers(window);
    if (wait_events) {
      waitEvents(x_display, terminal);
    } else  { 
      pollEvents(x_display, terminal);
    }
  }
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

