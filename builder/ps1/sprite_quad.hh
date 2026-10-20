#pragma once
// Quad math of the sprite paths (ps1/render.cpp): the screen corners of a rotated / zoomed piece and the texel-edge UVs
// of rotated, scaled and mirrored pieces. Pure integer code without psyqo, so the host regression test
// (../tests/host/test_v4_sprite_quads.py; the same header as RSDKv3-ps1's) checks it against a model of the GPU's rasterizer.
//
// Conventions (psx-spx GPU "Render Polygon"): corners A B C D = the piece's local top-left, top-right, bottom-left,
// bottom-right; the GPU draws a quad as triangles ABC and BCD, samples each pixel's texel at its top-left point and
// leaves out the right / bottom edges. UVs are texel edges; u0 / v0 belong to corner A's side.

struct PS1QuadCorners {
    int x[4], y[4];
};
struct PS1QuadUV {
    int u0, u1, v0, v1;
};

// Corners of a piece (local rect ox, oy, w, h of the frame) rotated / zoomed about (pivotX, pivotY) and placed at
// (XPos, YPos); sn / cs = sin / cos * scale (512 = 1.0), fx = mirrored. A, B, C are upstream's (HW path) corners; D is
// B + C - A, so the quad is an exact parallelogram (V4-103, as RSDKv3-ps1's CD-093): the GPU's two triangles then share one texture gradient.
// With D rounded on its own (as before), ABC and BCD had different gradients and the texture sheared along the BC
// diagonal, a shear that changed with every angle and position (the "varying UVs" of rotated sprites).
static inline PS1QuadCorners PS1AffineCorners(int XPos, int YPos, int pivotX, int pivotY, int sn, int cs, bool fx, int ox, int oy,
                                              int w, int h)
{
    PS1QuadCorners q;
    const int lx[3] = { ox, ox + w, ox };
    const int ly[3] = { oy, oy, oy + h };
    for (int k = 0; k < 3; ++k) {
        int x  = fx ? pivotX - lx[k] : lx[k] - pivotX;
        int y  = ly[k] - pivotY;
        q.x[k] = XPos + ((x * cs + y * sn) >> 9);
        q.y[k] = YPos + ((y * cs - x * sn) >> 9);
    }
    q.x[3] = q.x[1] + q.x[2] - q.x[0];
    q.y[3] = q.y[1] + q.y[2] - q.y[0];
    return q;
}

// UV edges of a rotated / zoomed piece (texture at u, v, size w x h) for the corners q; rightAngle = the rotation is a
// multiple of 90 degrees at scale 1 (RSDKv4-ps1 0a3f95f; Sonic CD's CD-016). The GPU takes each pixel's texel
// at its top-left point and leaves out the right / bottom edges, so a texture axis running left or up on screen read
// its far edge u + w / v + h: the next piece in the atlas (the white L along a sprite turned ~180 degrees, the column
// beside a mirrored one). At right angles the quad is axis-aligned and exact when such an axis has both edges pulled
// back one texel (as the 1:1 mirror); other angles and scales can't be exact with integer UVs, so the far edges come one
// texel in and no pixel reads a neighbouring piece.
static inline PS1QuadUV PS1AffineUV(const PS1QuadCorners &q, int u, int v, int w, int h, bool rightAngle)
{
    PS1QuadUV uv = { u, u + w, v, v + h };
    if (rightAngle) { // u runs from corner A to B, v from A to C
        int ux = q.x[1] - q.x[0], uy = q.y[1] - q.y[0], vx = q.x[2] - q.x[0], vy = q.y[2] - q.y[0];
        if (ux < 0 || (ux == 0 && uy < 0))
            --uv.u0, --uv.u1;
        if (vx < 0 || (vx == 0 && vy < 0))
            --uv.v0, --uv.v1;
    }
    else
        --uv.u1, --uv.v1;
    return uv;
}

// UV edges of an axis-aligned piece (texture pu, pv, size pw x ph) drawn at w x h screen pixels, optionally mirrored.
// A mirrored axis must not start at its far edge (pu + pw: the next piece, 0a3f95f): 1:1 mirrors sample texel
// u + w - 1 - k at column k (UV edges u + w - 1 .. u - 1, exact); scaled mirrors take u + w - 1 .. u (one texel narrower:
// a scaled edge has no exact integer UV); unmirrored pieces the full [u, u + w) range.
static inline PS1QuadUV PS1RectUV(int pu, int pv, int pw, int ph, int w, int h, bool flipX, bool flipY)
{
    PS1QuadUV uv = { pu, pu + pw, pv, pv + ph };
    if (flipX) {
        uv.u0 = pu + pw - 1;
        uv.u1 = w == pw ? pu - 1 : pu;
    }
    if (flipY) {
        uv.v0 = pv + ph - 1;
        uv.v1 = h == ph ? pv - 1 : pv;
    }
    return uv;
}

// Texcoord of a 1:1 mirrored piece (u, v, w x h) drawn as a flip rectangle (GP0(E1h) bit 12 X / bit 13 Y flip, new GPUs
// only: nocash psx-spx "GPU Render Rectangle Commands"): the rectangle decrements its texcoords on a flipped axis, so it
// starts at the piece's last texel (u + w - 1 / v + h - 1). Returns false where the quad stays:
// - X flip ignores Texcoord.X's LSB and starts at an odd X: u + w - 1 must be odd (u + w even);
// - an unflipped X axis wants an even Texcoord.X (odd ones glitch every 4th / 8th pixel on hardware), so u even there;
// - the quad it replaces (PS1RectUV, 1:1) must be exact: every UV edge within a byte (0 .. 255), else the quad clamps and
//   the two would differ.
#define PS1_FLIP_RECT_TEX 1 // the host test (../tests/host/cd_sprite_quads.cpp) checks it when present
static inline bool PS1FlipRectTex(int u, int v, int w, int h, bool flipX, bool flipY, int *tu, int *tv)
{
    int u0 = u - flipX, u1 = u + w - flipX, v0 = v - flipY, v1 = v + h - flipY; // the quad's UV edges (PS1RectUV, 1:1)
    *tu = flipX ? u1 : u;
    *tv = flipY ? v1 : v;
    return !((u0 | u1 | v0 | v1) & ~0xFF) && (*tu & 1) == flipX; // edges in a byte; X flip odd, else even
}

// Texel edge at screen offset pos of a piece edge spanning len pixels / texels texels from base (mirrored: from the far
// edge), for pieces cut to the screen (PS1ChainPieceRectClipped).
static inline int PS1ClipEdge(int base, int texels, int pos, int len, bool flip)
{
    int t = (int)((long long)pos * texels / len);
    return flip ? base + texels - t : base + t;
}

#define PS1_SPRITE_QUAD_CLIPUV 1 // (the host test checks PS1ClipRectUV when present)
// UV edges of a piece at screen rect (x, y, w, h) cut to [cx0, cx1) x [cy0, cy1) (PS1ChainPieceRectClipped: a zoomed piece
// beyond the GPU limits); a mirrored axis never starts at its far edge (PS1RectUV).
static inline PS1QuadUV PS1ClipRectUV(int pu, int pv, int pw, int ph, int x, int y, int w, int h, int cx0, int cx1, int cy0, int cy1,
                                      bool flipX, bool flipY)
{
    PS1QuadUV uv = { PS1ClipEdge(pu, pw, cx0 - x, w, flipX), PS1ClipEdge(pu, pw, cx1 - x, w, flipX), PS1ClipEdge(pv, ph, cy0 - y, h, flipY),
                     PS1ClipEdge(pv, ph, cy1 - y, h, flipY) };
    if (flipX && uv.u0 > pu + pw - 1)
        uv.u0 = pu + pw - 1;
    if (flipY && uv.v0 > pv + ph - 1)
        uv.v0 = pv + ph - 1;
    return uv;
}
