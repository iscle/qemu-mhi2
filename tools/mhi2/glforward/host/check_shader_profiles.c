/* Preserve both branches' masked-compositor pixel behavior during consolidation. */
#define main glhost_main
#include "glhost.c"
#undef main
#include <assert.h>
#include <math.h>

static GLuint compile_profile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok); assert(ok);
    return shader;
}

int main(void)
{
    ensure_window(8,8);glViewport(0,0,8,8);glDisable(GL_DITHER);
    const char *vertex = "attribute vec2 pos;varying mediump vec2 texout,maskout;"
        "void main(){gl_Position=vec4(pos,0.,1.);texout=maskout=vec2(.5);}";
    const GLfloat vertices[] = {-1,-1,3,-1,-1,3};
    const unsigned char texels[2][4] = {{64,128,32,128},{64,64,64,255}};
    GLuint textures[2];glGenTextures(2,textures);
    for(unsigned i=0;i<2;i++){
        glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,textures[i]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,texels[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    }
    for(unsigned audi=0;audi<2;audi++){
        setenv("MHI2_FIRMWARE",audi?"audi-a3":"porsche",1);
        const struct ShaderTranslation *translation=find_shader(UINT64_C(0xa1d1062962f918ff),952);
        const struct ShaderTranslation *vs=find_shader(UINT64_C(0xb372986db48cc454),1412);
        assert(translation && vs);
        assert(!!translation->firmware==audi && !!vs->firmware==audi);
        GLuint program=glCreateProgram();
        glAttachShader(program,compile_profile_shader(GL_VERTEX_SHADER,vertex));
        glAttachShader(program,compile_profile_shader(GL_FRAGMENT_SHADER,translation->source));
        glBindAttribLocation(program,0,"pos");glLinkProgram(program);
        GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(program);
        glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,vertices);
        glUniform1i(glGetUniformLocation(program,"tex"),0);
        glUniform1i(glGetUniformLocation(program,"mask"),1);
        glUniform1f(glGetUniformLocation(program,"opacity"),.5f);
        glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
        struct BlendState state;begin_binary_blend(translation->blend,&state);
        glDrawArrays(GL_TRIANGLES,0,3);end_binary_blend(translation->blend,&state);
        unsigned char pixel[4];glReadPixels(3,3,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        float scale=audi?64.f/255*.5f:128.f/255*(1.f-64.f/255)*.5f;
        for(unsigned c=0;c<4;c++){
            float expected=(!audi && c==3)?scale*255:texels[0][c]*scale;
            assert(fabsf(pixel[c]-expected)<=1.f);
        }
        assert(glGetError()==GL_NO_ERROR);
    }
    puts("PASS: Porsche and Audi masked-compositor variants select and render their original pixels");
}
