/* Local pixel regression for Tegra's baked blending on GLES2 drivers. */
#define main glhost_main
#include "glhost.c"
#undef main
#include <math.h>
#include <assert.h>

static GLuint compile(GLenum type, const char *source)
{
    GLuint shader=glCreateShader(type);GLint ok=0;
    glShaderSource(shader,1,&source,NULL);glCompileShader(shader);
    glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    check_shader("test",shader,0);assert(ok);return shader;
}

int main(void)
{
    ensure_window(8,8);
    const char *vertex="attribute vec2 pos;varying mediump vec2 texout,v_texCoord;"
        "varying mediump float v_fogFactor;void main(){gl_Position=vec4(pos,0.,1.);"
        "texout=v_texCoord=vec2(.5);v_fogFactor=.25;}";
    const GLfloat vertices[]={-1,-1,3,-1,-1,3};
    GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    const unsigned char texel[]={64,128,32,128};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,texel);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glDisable(GL_DITHER);
    unsigned cases=0;
    for(unsigned n=0;n<sizeof(shaders)/sizeof(shaders[0]);n++) {
        unsigned mode=shaders[n].blend;if(!mode)continue;
        if(shaders[n].hash!=UINT64_C(0xb768a6623dd44225) &&
           shaders[n].hash!=UINT64_C(0x456f4843de8e81e) &&
           shaders[n].hash!=UINT64_C(0x7d495213a2eb81e3))continue;
        GLuint program=glCreateProgram();
        glAttachShader(program,compile(GL_VERTEX_SHADER,vertex));
        glAttachShader(program,compile(GL_FRAGMENT_SHADER,shaders[n].source));
        glBindAttribLocation(program,0,"pos");glLinkProgram(program);
        GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);assert(linked);
        glUseProgram(program);glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,vertices);
        glUniform1i(glGetUniformLocation(program,"uTex"),0);
        glUniform1i(glGetUniformLocation(program,"s_texture0"),0);
        for(unsigned variant=0;variant<3;variant++) {
            float opacity=variant*.5f;
            glUniform1f(glGetUniformLocation(program,"uOpacity"),opacity);
            glUniform4f(glGetUniformLocation(program,"u_color"),.8,.4,.2,opacity);
            glUniform3f(glGetUniformLocation(program,"u_fogColor"),.2,.3,.4);
            for(unsigned enabled=0;enabled<2;enabled++) {
                if(enabled)glEnable(GL_BLEND);else glDisable(GL_BLEND);
                glBlendFuncSeparate(GL_DST_COLOR,GL_SRC_COLOR,GL_DST_ALPHA,GL_SRC_ALPHA);
                glBlendEquationSeparate(GL_FUNC_SUBTRACT,GL_FUNC_REVERSE_SUBTRACT);
                glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
                glClearColor(.1,.2,.3,.4);glClear(GL_COLOR_BUFFER_BIT);
                unsigned char initial[4];glReadPixels(3,3,1,1,GL_RGBA,GL_UNSIGNED_BYTE,initial);
                float expected[4];for(int c=0;c<4;c++)expected[c]=initial[c]/255.f;
                for(unsigned repeat=0;repeat<2;repeat++) {
                    struct BlendState saved;begin_binary_blend(mode,&saved);
                    if(repeat){const GLushort indices[]={0,1,2};glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_SHORT,indices);}
                    else glDrawArrays(GL_TRIANGLES,0,3);
                    end_binary_blend(mode,&saved);
                    assert(glIsEnabled(GL_BLEND)==enabled);
                    GLint state;glGetIntegerv(GL_BLEND_SRC_RGB,&state);assert(state==GL_DST_COLOR);
                    glGetIntegerv(GL_BLEND_DST_RGB,&state);assert(state==GL_SRC_COLOR);
                    glGetIntegerv(GL_BLEND_SRC_ALPHA,&state);assert(state==GL_DST_ALPHA);
                    glGetIntegerv(GL_BLEND_DST_ALPHA,&state);assert(state==GL_SRC_ALPHA);
                    glGetIntegerv(GL_BLEND_EQUATION_RGB,&state);assert(state==GL_FUNC_SUBTRACT);
                    glGetIntegerv(GL_BLEND_EQUATION_ALPHA,&state);assert(state==GL_FUNC_REVERSE_SUBTRACT);
                    float source[4];
                    if(mode==2){source[0]=.35f*opacity;source[1]=.325f*opacity;source[2]=.35f*opacity;source[3]=opacity;}
                    else {float scale=shaders[n].hash==UINT64_C(0xb768a6623dd44225)?opacity:1;
                        for(int c=0;c<4;c++)source[c]=texel[c]/255.f*scale;}
                    for(int c=0;c<4;c++)if(mode!=2 || c!=3)
                        expected[c]=source[c]+expected[c]*(1-source[3]);
                    unsigned char pixel[4];glReadPixels(3,3,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    for(int c=0;c<4;c++) {
                        if(fabsf(pixel[c]-expected[c]*255)>2){
                            fprintf(stderr,"blend=%u variant=%u repeat=%u channel=%d got=%u expected=%.2f\n",
                                    mode,variant,repeat,c,pixel[c],expected[c]*255);return 1;
                        }
                    }
                    assert(glGetError()==GL_NO_ERROR);cases++;
                }
            }
        }
    }
    fprintf(stderr,"PASS: %u pixel/state checks, arrays and indexed draws\n",cases);
    return 0;
}
