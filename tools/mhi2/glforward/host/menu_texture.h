/* Optional local Porsche artwork: retain RGB lost by TargetImageInfoHome's
 * LA declaration. The original image table and guest binaries stay intact.
 * Use RGBA storage for LA textures while enabled; unmatched pixels retain
 * exactly the GLES luminance/alpha semantics. Match every B/A byte of the
 * replacement PNG, not a texture ID, screen coordinate, or approximate hash. */
static uint8_t menu_rgba[154 * 168 * 4];
static uint8_t menu_pressed_rgba[154 * 168 * 4];
static int menu_texture_enabled;
static int menu_pressed_enabled;

struct MenuDrawSignature {
    GLint program, framebuffer, viewport[4], scissor[4], scissor_enabled;
    GLfloat modelview[16], projection[16], color[3];
};
struct MenuTexture {
    GLuint idle, pressed;
    int previous_valid;
    struct MenuDrawSignature previous;
};

static void menu_texture_init(void)
{
    const char *path = getenv("MHI2_PORSCHE_MENU_RGBA");
    if (!path || !*path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(menu_rgba, 1, sizeof(menu_rgba), f);
    int extra = fgetc(f);
    fclose(f);
    menu_texture_enabled = n == sizeof(menu_rgba) && extra == EOF;
    if (!menu_texture_enabled)
        fprintf(stderr, "Porsche menu: invalid RGBA companion %s\n", path);
    path = getenv("MHI2_PORSCHE_MENU_PRESSED_RGBA");
    if (!menu_texture_enabled || !path || !*path) return;
    f = fopen(path, "rb");
    if (!f) return;
    n = fread(menu_pressed_rgba, 1, sizeof(menu_pressed_rgba), f);
    extra = fgetc(f); fclose(f);
    menu_pressed_enabled = n == sizeof(menu_pressed_rgba) && extra == EOF;
}

static void menu_texture_reset_draw(void)
{
    if (blend->menu) blend->menu->previous_valid = 0;
}

static void menu_texture_deleted(GLuint texture)
{
    struct MenuTexture *m = blend->menu;
    if (!m || m->idle != texture) return;
    glDeleteTextures(1, &m->pressed);
    free(m); blend->menu = NULL;
}

static void menu_texture_register(void)
{
    if (!menu_pressed_enabled) return;
    GLint binding;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    struct MenuTexture *m = blend->menu;
    if (m && m->idle == (GLuint)binding) return;
    if (m) menu_texture_deleted(m->idle);
    m = calloc(1, sizeof(*m));
    if (!m) return;
    blend->menu = m; m->idle = binding;
    glGenTextures(1, &m->pressed);
    glBindTexture(GL_TEXTURE_2D, m->pressed);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 154, 168, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, menu_pressed_rgba);
    glBindTexture(GL_TEXTURE_2D, binding);
}

/* Require the captured native quad layout, including client-array bounds.
 * Unknown draw layouts retain normal rendering instead of guessing. */
static int menu_texture_array(GLuint program, const char *name,
                              unsigned components, const float *expected,
                              unsigned bytes)
{
    GLint index = glGetAttribLocation(program, name), v;
    if (index < 0 || index >= 16 || blend->client_array_bytes[index] < bytes)
        return 0;
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &v);
    if (!v) return 0;
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &v);
    if (v) return 0;
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &v);
    if (v != components) return 0;
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &v);
    if (v != GL_FLOAT) return 0;
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &v);
    if (v) return 0;
    void *pointer;
    glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
    return pointer && pointer == blend->client_arrays[index] &&
           !memcmp(pointer, expected, bytes);
}

/* GlowButtonRenderer draws idle then glow at identical geometry. K5126
 * erroneously shares their bitmap ID, but preserves this pair of draws and
 * the native glow opacity. Substitute only the second member of that pair.
 * No mouse coordinates, press timers, firmware table edits or screen overlay.
 * Return the binding to restore after the caller executes its normal draw. */
static GLuint menu_texture_begin_draw(unsigned op, const uint8_t *p, unsigned len)
{
    struct MenuTexture *m = blend->menu;
    if (!m) return 0;
    int previous_valid = m->previous_valid;
    m->previous_valid = 0;
    static const uint16_t indices[] = {0,1,2,1,2,3};
    if (op != 129 || len != 28 || u32(p,0) != GL_TRIANGLES ||
        u32(p,4) != 6 || u32(p,8) != GL_UNSIGNED_SHORT ||
        memcmp(p+16,indices,sizeof(indices))) return 0;
    GLint active, binding;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    if (active != GL_TEXTURE0) return 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    if ((GLuint)binding != m->idle) return 0;
    struct MenuDrawSignature s = {0};
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    static const float vertices[] = {0,0,0,154,0,0,0,168,0,154,168,0};
    static const float uv[] = {0,0,1,0,0,1,1,1};
    if (!menu_texture_array(s.program,"a_vertex",3,vertices,sizeof(vertices)) ||
        !menu_texture_array(s.program,"a_texCoords",2,uv,sizeof(uv))) return 0;
    GLint model = glGetUniformLocation(s.program,"u_modelviewMatrix");
    GLint projection = glGetUniformLocation(s.program,"u_projectionMatrix");
    GLint color = glGetUniformLocation(s.program,"u_modulateColor");
    GLint sampler = glGetUniformLocation(s.program,"u_sampler"), unit;
    if (model < 0 || projection < 0 || color < 0 || sampler < 0) return 0;
    glGetUniformiv(s.program,sampler,&unit);
    if (unit != 0) return 0;
    glGetUniformfv(s.program,model,s.modelview);
    glGetUniformfv(s.program,projection,s.projection);
    float rgba[4]; glGetUniformfv(s.program,color,rgba);
    memcpy(s.color,rgba,sizeof(s.color));
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&s.framebuffer);
    glGetIntegerv(GL_VIEWPORT,s.viewport);
    glGetIntegerv(GL_SCISSOR_BOX,s.scissor);
    s.scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    if (!previous_valid || memcmp(&s,&m->previous,sizeof(s))) {
        m->previous = s; m->previous_valid = 1;
        return 0;
    }
    /* Copy filtering/wrapping from the current native texture each time. */
    static const GLenum names[] = {GL_TEXTURE_MIN_FILTER,GL_TEXTURE_MAG_FILTER,
                                   GL_TEXTURE_WRAP_S,GL_TEXTURE_WRAP_T};
    GLint params[4];
    for (unsigned i=0;i<4;i++) glGetTexParameteriv(GL_TEXTURE_2D,names[i],&params[i]);
    glBindTexture(GL_TEXTURE_2D,m->pressed);
    for (unsigned i=0;i<4;i++) glTexParameteri(GL_TEXTURE_2D,names[i],params[i]);
    return m->idle;
}

static int menu_texture_match(const uint8_t *la, unsigned w, unsigned h,
                              size_t stride)
{
    if (w != 154 || h != 168) return 0;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            const uint8_t *p = la + y * stride + x * 2;
            const uint8_t *q = menu_rgba + (y * w + x) * 4;
            if (p[0] != q[2] || p[1] != q[3]) return 0;
        }
    return 1;
}

/* Returns 1 when handled, including rejected malformed LA packets. */
static int menu_texture_upload(unsigned op, const uint8_t *p, unsigned len)
{
    if (!menu_texture_enabled || len < 36 || u32(p,24) != GL_LUMINANCE_ALPHA ||
        u32(p,28) != GL_UNSIGNED_BYTE) return 0;
    unsigned w = u32(p,op == 52 ? 12 : 16);
    unsigned h = u32(p,op == 52 ? 16 : 20);
    unsigned n = u32(p,32);
    if (w > 8192 || h > 8192 || len < 36 || n > len - 36) return 1;
    GLint alignment;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    size_t stride = ((size_t)w * 2 + alignment - 1) & ~(size_t)(alignment - 1);
    size_t required = h ? (h - 1) * stride + w * 2 : 0;
    uint8_t *expanded = NULL;
    const uint8_t *pixels = NULL;
    int matched = 0;
    if (n) {
        if (n < required) return 1;
        if (menu_texture_match(p + 36, w, h, stride)) {
            pixels = menu_rgba;
            matched = 1;
            fprintf(stderr, "Porsche menu: restored RGB for Android Auto icon\n");
        } else {
            expanded = malloc((size_t)w * h * 4);
            if (!expanded) return 1;
            for (unsigned y = 0; y < h; y++)
                for (unsigned x = 0; x < w; x++) {
                    const uint8_t *s = p + 36 + y * stride + x * 2;
                    uint8_t *d = expanded + (y * w + x) * 4;
                    d[0] = d[1] = d[2] = s[0]; d[3] = s[1];
                }
            pixels = expanded;
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (op == 52)
        glTexImage2D(u32(p,0), i32(p,4), GL_RGBA, w, h, i32(p,20),
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    else if (pixels)
        glTexSubImage2D(u32(p,0), i32(p,4), i32(p,8), i32(p,12), w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (matched) menu_texture_register();
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    free(expanded);
    return 1;
}
