#ifndef DRAWING3D_H
#define DRAWING3D_H

#if RETRO_PLATFORM == RETRO_PS1
// PS1: Sonic 2's half-pipe builds its mesh from script tables each frame: 1,408 vertices and 742 faces (measured,
// docs/30 phase 2.3), plus vertices 4094/4095 as single-point scratch; rings, shadows and the player add 4 vertices a
// face: 1,556 vertices / 784 faces at the first stage's emerald (phase 7 full run) -> 0x680 vertices. Script vertex indices 0xFF0-0xFFF map to the
// last 16 slots (PS1_VTX_SCRATCH); other accesses and counts past these are clamped and counted
// (g_ps1Scene3DOverflow, Script.cpp).
#define PS1_VTX_SCRATCH   (0x10)
#define VERTEXBUFFER_SIZE (0x680 + PS1_VTX_SCRATCH)
#define FACEBUFFER_SIZE   (0x340)
#else
#define VERTEXBUFFER_SIZE (0x1000)
#define FACEBUFFER_SIZE   (0x400)
#endif

enum FaceFlags {
    FACE_FLAG_TEXTURED_3D      = 0,
    FACE_FLAG_TEXTURED_2D      = 1,
    FACE_FLAG_COLORED_3D       = 2,
    FACE_FLAG_COLORED_2D       = 3,
    FACE_FLAG_FADED            = 4,
    FACE_FLAG_TEXTURED_C       = 5,
    FACE_FLAG_TEXTURED_C_BLEND = 6,
    FACE_FLAG_3DSPRITE         = 7
};

enum MatrixTypes {
    MAT_WORLD = 0,
    MAT_VIEW  = 1,
    MAT_TEMP  = 2,
};

struct Matrix {
    int values[4][4];
};

struct Vertex {
    int x;
    int y;
    int z;
    int u;
    int v;
};

struct Face {
    int a;
    int b;
    int c;
    int d;
    uint color;
    int flag;
};

struct DrawListEntry3D {
    int faceID;
    int depth;
};

extern int vertexCount;
extern int faceCount;

extern Matrix matFinal;
extern Matrix matWorld;
extern Matrix matView;
extern Matrix matTemp;

#if RETRO_PLATFORM == RETRO_PS1
// PS1 (docs/30, before phase 9): the Scene3D arrays (faces, vertices, draw list, sort scratch, projections, exact cache:
// ~85 KB) are one heap block, allocated only while a stage whose scripts use them is loaded (Data/Game/PS1Scene3D.bin,
// tools/scripts/scene3d_stages.py; Sonic 2: the special stage): PS1Scene3DPrepare, called first in each stage load's
// bracket. Otherwise they point at 1-entry dummies with capacity 0 (any access clamped + counted, g_ps1Scene3DOverflow).
extern Face *faceBuffer;
extern Vertex *vertexBuffer;
extern int g_ps1VtxCap, g_ps1FaceCap; // VERTEXBUFFER_SIZE / FACEBUFFER_SIZE with the block, 0 without
void PS1Scene3DReadList();
void PS1Scene3DPrepare(const char *folder);
#else
extern Face faceBuffer[FACEBUFFER_SIZE];
extern Vertex vertexBuffer[VERTEXBUFFER_SIZE];
#endif
#if RETRO_PLATFORM == RETRO_PS1
// PS1 (docs/30 phase 7.2b): the GTE transforms + projects every vertex (Scene3D.cpp PS1TransformProject3D) into
// g_ps1Proj: .u = the packed screen position (or PS1_PROJ_MARK: left to the CPU), .v = the depth z. Upstream's exact
// transform (vertexBufferT[i].x/.y/.z in Draw3DScene's general path) is computed on demand into a small per-call cache
// (PS1ExactT): 8 B a vertex instead of vertexBufferT's 20 (-16 KB).
struct PS1ProjEntry {
    uint u;
    int v;
};
extern PS1ProjEntry *g_ps1Proj;
struct PS1VertexXYZ {
    int x, y, z;
};
struct PS1ExactEntry {
    int x, y, z, idx;
    uint frame;
};
extern PS1ExactEntry *g_ps1ExactT; // 256 entries
extern uint g_ps1ExactFrame;
const PS1VertexXYZ &PS1ExactTSlow(int v);
inline PS1VertexXYZ PS1ExactT(int v)
{
    const PS1ExactEntry &e = g_ps1ExactT[v & 255];
    if (e.idx == v && e.frame == g_ps1ExactFrame)
        return PS1VertexXYZ{ e.x, e.y, e.z };
    return PS1ExactTSlow(v);
}
PS1VertexXYZ PS1ExactTOut(int v); // PS1ExactT out of line (the view's call sites stay small)
struct PS1VertexTView {
    PS1VertexXYZ operator[](int v) const { return PS1ExactTOut(v); }
};
#else
extern Vertex vertexBufferT[VERTEXBUFFER_SIZE];
#endif

#if RETRO_PLATFORM == RETRO_PS1
extern DrawListEntry3D *drawList3D;
#else
extern DrawListEntry3D drawList3D[FACEBUFFER_SIZE];
#endif

extern int projectionX;
extern int projectionY;
extern int fogColor;
extern int fogStrength;

extern int faceLineStart[SCREEN_YSIZE];
extern int faceLineEnd[SCREEN_YSIZE];
extern int faceLineStartU[SCREEN_YSIZE];
extern int faceLineEndU[SCREEN_YSIZE];
extern int faceLineStartV[SCREEN_YSIZE];
extern int faceLineEndV[SCREEN_YSIZE];

void SetIdentityMatrix(Matrix *matrix);
void MatrixMultiply(Matrix *matrixA, Matrix *matrixB);
void MatrixTranslateXYZ(Matrix *Matrix, int x, int y, int z);
void MatrixScaleXYZ(Matrix *matrix, int scaleX, int scaleY, int scaleZ);
void MatrixRotateX(Matrix *matrix, int rotationX);
void MatrixRotateY(Matrix *matrix, int rotationY);
void MatrixRotateZ(Matrix *matrix, int rotationZ);
void MatrixRotateXYZ(Matrix *matrix, short rotationX, short rotationY, short rotationZ);
#if !RETRO_REV00
void MatrixInverse(Matrix *matrix);
#endif
void TransformVertexBuffer();
void TransformVertices(Matrix *matrix, int startIndex, int endIndex);
void Sort3DDrawList();
void Draw3DScene(int spriteSheetID);

void ProcessScanEdge(Vertex *vertA, Vertex *vertB);
void ProcessScanEdgeUV(Vertex *vertA, Vertex *vertB);

#endif // !DRAWING3D_H
