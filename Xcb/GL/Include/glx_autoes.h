/*
 * glx_autoes.h - automatically selects the ES version based on what the application imports (without manual GLX_ES).
 *
 * Reads UNDEFINED (imported) "gl*" symbols from the executable and loaded libraries
 * (the ELF file on disk, .dynsym). ae_detect() result:
 *   1 = fixed-function only (glBegin, glMatrixMode, glLightfv, glVertexPointer, gluPerspective...)  -> ES1
 *   2 = uses shaders (glCreateShader, glUseProgram, glVertexAttribPointer, VAO...)                   -> ES2/ES3
 *   0 = unknown (the application loads its functions with glXGetProcAddress: SDL, GLFW, GLEW, glad)
 * If both types appear, shaders win (ES1 cannot run shaders).
 *
 * Included by glx_shim.c before glx_es_version(). Self-contained (elf.h, link.h).
 */
#ifndef GLX_AUTOES_H
#define GLX_AUTOES_H

#include <elf.h>
#include <link.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static const char* const ae_ff_syms[] = {
    "glBegin", "glEnd", "glMatrixMode", "glLoadIdentity", "glLoadMatrixf", "glLoadMatrixd",
    "glMultMatrixf", "glMultMatrixd", "glPushMatrix", "glPopMatrix", "glTranslatef", "glTranslated",
    "glRotatef", "glRotated", "glScalef", "glScaled", "glFrustum", "glOrtho",
    "gluPerspective", "gluLookAt", "gluOrtho2D",
    "glVertex2f", "glVertex2i", "glVertex2d", "glVertex2fv", "glVertex3f", "glVertex3i", "glVertex3d",
    "glVertex3fv", "glVertex3dv", "glVertex4f", "glNormal3f", "glNormal3fv", "glNormal3d",
    "glColor3f", "glColor3fv", "glColor3ub", "glColor4f", "glColor4fv", "glColor4ub",
    "glTexCoord2f", "glTexCoord2fv", "glTexCoord2d", "glTexCoord2i",
    "glLightfv", "glLightf", "glLightModelfv", "glLightModeli", "glMaterialfv", "glMaterialf",
    "glShadeModel", "glColorMaterial", "glVertexPointer", "glNormalPointer", "glColorPointer",
    "glTexCoordPointer", "glEnableClientState", "glDisableClientState", "glTexEnvi", "glTexEnvf",
    "glTexEnvfv", "glNewList", "glEndList", "glCallList", "glCallLists", "glGenLists", "glDeleteLists",
    "glRasterPos2f", "glRasterPos2i", "glRasterPos3f", "glDrawPixels", "glFogf", "glFogfv", "glFogi",
    "glAlphaFunc", "glTexGeni", "glTexGenfv", "glRectf", "glRecti", "glPushAttrib", "glPopAttrib",
    NULL
};

static const char* const ae_sh_syms[] = {
    "glCreateShader", "glCreateProgram", "glShaderSource", "glCompileShader", "glLinkProgram",
    "glUseProgram", "glAttachShader", "glGetUniformLocation", "glGetAttribLocation",
    "glUniform1i", "glUniform1f", "glUniform3f", "glUniform4f", "glUniform3fv", "glUniform4fv",
    "glUniformMatrix3fv", "glUniformMatrix4fv", "glVertexAttribPointer", "glEnableVertexAttribArray",
    "glBindVertexArray", "glGenVertexArrays", "glBindAttribLocation", "glDrawArraysInstanced",
    "glCreateShaderObjectARB", "glUseProgramObjectARB", "glBindFragDataLocation", "glDispatchCompute",
    NULL
};

/* functions that exist ONLY in OpenGL ES 1.x (not in desktop GL): the application is ES, thus also fixed-function */
static const char* const ae_es_syms[] = {
    "glOrthof", "glFrustumf", "glClipPlanef", "glGetClipPlanef", "glFogx", "glFogxv", "glLightx", "glLightxv",
    "glColor4x", "glTranslatex", "glRotatex", "glScalex", "glMultMatrixx", "glLoadMatrixx", "glMaterialx",
    "glMaterialxv", "glPointSizex", "glNormal3x", "glTexEnvx", "glTexParameterx",
    NULL
};

static int ae_in(const char* n, const char* const* l) {
    for (; *l; l++) if (!strcmp(n, *l)) return 1;
    return 0;
}

/* bit 0 (1): imported fixed-function symbols; bit 1 (2): imported shader symbols */
static int ae_scan_file(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < (off_t)sizeof(Elf64_Ehdr)) { close(fd); return 0; }
    void* m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return 0;

    int r = 0;
    size_t fsz = (size_t)st.st_size;
    const unsigned char* b = m;
    const Elf64_Ehdr* eh = m;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64) goto out;
    if (!eh->e_shoff || eh->e_shentsize != sizeof(Elf64_Shdr) ||
        eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) > fsz) goto out;
    const Elf64_Shdr* sh = (const Elf64_Shdr*)(b + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
        const Elf64_Shdr* strs = &sh[sh[i].sh_link];
        if (sh[i].sh_offset + sh[i].sh_size > fsz || strs->sh_offset + strs->sh_size > fsz) continue;
        const Elf64_Sym* sy = (const Elf64_Sym*)(b + sh[i].sh_offset);
        size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
        const char* str = (const char*)(b + strs->sh_offset);
        for (size_t k = 0; k < n; k++) {
            if (sy[k].st_shndx != SHN_UNDEF || sy[k].st_name >= strs->sh_size) continue;
            const char* nm = str + sy[k].st_name;
            if (nm[0] != 'g' || nm[1] != 'l') continue;
            if (!memchr(nm, 0, strs->sh_size - sy[k].st_name)) continue;     /* unterminated name */
            if (ae_in(nm, ae_ff_syms)) r |= 1;
            else if (ae_in(nm, ae_sh_syms)) r |= 2;
            else if (ae_in(nm, ae_es_syms)) r |= 1 | 4;
        }
    }
out:
    munmap(m, fsz);
    return r;
}

static int ae_cb(struct dl_phdr_info* info, size_t sz, void* data) {
    (void)sz;
    int* acc = data;
    const char* p = info->dlpi_name;
    if (!p || !*p) p = "/proc/self/exe";                                      /* main executable */
    else if (!strncmp(p, "/system/", 8) || !strncmp(p, "/apex/", 6) || !strncmp(p, "/vendor/", 8)) return 0;
    const char* base = strrchr(p, '/');
    base = base ? base + 1 : p;
    /* GL/GLU/GLUT libraries import fixed-function functions themselves: they say nothing about the application */
    if (!strncmp(base, "libGL", 5) || !strncmp(base, "libEGL", 6) || !strncmp(base, "libglut", 7) ||
        !strncmp(base, "libfreeglut", 11) || !strncmp(base, "libglapi", 8)) return 0;
    *acc |= ae_scan_file(p);
    return 0;
}

/* bits: 1 = fixed-function, 2 = shaders, 4 = ES-only symbols. Scanning is performed only once. */
static int ae_bits(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    int acc = 0;
    dl_iterate_phdr(ae_cb, &acc);
    cached = acc;
    if (getenv("GLX_DEBUG"))
        fprintf(stderr, "[glx-auto] gl* imports: fixed-function=%d shaders=%d ES-only=%d\n",
                !!(acc & 1), !!(acc & 2), !!(acc & 4));
    return cached;
}

static int ae_detect(void) {              /* 2 = shaders, 1 = fixed-function, 0 = unknown */
    int b = ae_bits();
    return (b & 2) ? 2 : ((b & 1) ? 1 : 0);
}

static int ae_is_es(void) {               /* the application imports functions that exist only in OpenGL ES */
    return !!(ae_bits() & 4);
}

#endif /* GLX_AUTOES_H */
