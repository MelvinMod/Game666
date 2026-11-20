#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

#define WORLD_SIZE 64
#define CHUNK_SIZE 16
#define CHUNKS_PER_AXIS (WORLD_SIZE / CHUNK_SIZE)
#define CHUNK_COUNT (CHUNKS_PER_AXIS * CHUNKS_PER_AXIS * CHUNKS_PER_AXIS)
#define BLOCK_AIR 0
#define BLOCK_GRASS 1
#define BLOCK_DIRT 2
#define BLOCK_STONE 3
#define BLOCK_WOOD 4
#define BLOCK_LEAVES 5
#define BLOCK_PLANK 6
#define BLOCK_BRICK 7
#define BLOCK_COUNT 8
#define ATLAS_COLS 4
#define ATLAS_ROWS 2
#define TILE_SIZE 16
#define REACH 4.0f
#define AV_HALF 0.3f
#define AV_HEIGHT 1.8f
#define MAX_TASKS 256
#define MAX_PATH 512
#define MAX_SEL 512
#define MAX_CHOP 512
#define WALK_SPEED 4.5f
#define SPRINT_SPEED 7.2f

typedef enum { STATE_MENU, STATE_PLAY, STATE_PAUSE } GameState;
typedef enum { SLOT_PICK, SLOT_AXE, SLOT_BLOCK } SlotKind;
typedef enum { TASK_MOVE, TASK_MINE, TASK_CHOP, TASK_PLACE, TASK_FILL } TaskType;
typedef enum { ACT_NONE, ACT_WALK, ACT_MINE, ACT_CHOP, ACT_MINE_SEL, ACT_CHOP_SEL, ACT_CLEAR, ACT_STOP } MenuAction;
typedef enum { FOLLOW_MOVING, FOLLOW_ARRIVED, FOLLOW_FAILED } FollowResult;

typedef struct { Mesh mesh; bool built; } ChunkMesh;
typedef struct { int x, y, z; } Cell;
typedef struct { bool hit; int x, y, z; int px, py, pz; int face; float dist; Vector3 point; } RayHit;
typedef struct { Vector3 position; float vy; bool onGround; bool moving; bool sprinting; float yaw; float walkPhase; float animBlend; } Avatar;
typedef struct { TaskType type; int bx, by, bz; float fy; int blockId; int tool; bool sprint; int x2, y2, z2; int a, b; float duration; char label[48]; } Task;
typedef struct { char label[40]; int action; } MenuItem;
typedef struct { float feetY; float g; float f; int parent; int x; int z; unsigned char state; } PathNode;

static unsigned char world[WORLD_SIZE][WORLD_SIZE][WORLD_SIZE];
static int worldSeed = 1337;
static Texture2D blockAtlas;
static Material blockMaterial;
static ChunkMesh chunkMeshes[CHUNK_COUNT];
static Avatar avatar;
static Camera3D camera;
static GameState gameState = STATE_MENU;
static bool quitRequested = false;
static float avatarYawTarget = 0.0f;

static const char *blockNames[BLOCK_COUNT] = { "Air", "Grass", "Dirt", "Stone", "Wood", "Leaves", "Plank", "Brick" };
static const Color blockTint[BLOCK_COUNT] = {
    { 255, 255, 255, 255 }, { 95, 159, 53, 255 }, { 134, 96, 67, 255 }, { 128, 128, 128, 255 },
    { 102, 67, 32, 255 }, { 45, 110, 45, 255 }, { 178, 140, 86, 255 }, { 150, 62, 52, 255 }
};
static struct { SlotKind kind; int id; const char *name; } hotbar[9] = {
    { SLOT_PICK, 0, "Pickaxe" }, { SLOT_AXE, 0, "Axe" },
    { SLOT_BLOCK, BLOCK_GRASS, "Grass" }, { SLOT_BLOCK, BLOCK_DIRT, "Dirt" },
    { SLOT_BLOCK, BLOCK_STONE, "Stone" }, { SLOT_BLOCK, BLOCK_WOOD, "Wood" },
    { SLOT_BLOCK, BLOCK_LEAVES, "Leaves" }, { SLOT_BLOCK, BLOCK_PLANK, "Plank" },
    { SLOT_BLOCK, BLOCK_BRICK, "Brick" }
};
static int hotbarSlot = 0;

static char toastBuf[160];
static double toastUntil = 0.0;
static void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toastBuf, sizeof(toastBuf), fmt, ap);
    va_end(ap);
    toastUntil = GetTime() + 2.5;
}

static float hash2(int x, int y) {
    unsigned int n = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u + (unsigned int)worldSeed * 2246822519u;
    n = (n ^ (n >> 13)) * 1274126177u;
    n = n ^ (n >> 16);
    return (float)(n & 0x7fffffffu) / (float)0x7fffffff;
}

static float smoothNoise(float x, float y) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - (float)ix, fy = y - (float)iy;
    float v00 = hash2(ix, iy), v10 = hash2(ix + 1, iy);
    float v01 = hash2(ix, iy + 1), v11 = hash2(ix + 1, iy + 1);
    float u = fx * fx * (3.0f - 2.0f * fx);
    float v = fy * fy * (3.0f - 2.0f * fy);
    return Lerp(Lerp(v00, v10, u), Lerp(v01, v11, u), v);
}

static float terrainHeight(int x, int z) {
    float n = 0.0f, amp = 1.0f, freq = 0.05f;
    for (int i = 0; i < 4; i++) {
        n += smoothNoise((float)x * freq, (float)z * freq) * amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return n * 20.0f + 10.0f;
}

static inline unsigned char getBlock(int x, int y, int z) {
    if (x < 0 || x >= WORLD_SIZE || y < 0 || y >= WORLD_SIZE || z < 0 || z >= WORLD_SIZE) return BLOCK_AIR;
    return world[x][y][z];
}

static inline void setBlock(int x, int y, int z, unsigned char b) {
    if (x >= 0 && x < WORLD_SIZE && y >= 0 && y < WORLD_SIZE && z >= 0 && z < WORLD_SIZE) world[x][y][z] = b;
}

static int w2c(float v) { return (int)floorf(v + 0.5f); }

static float floorYAt(int bx, int bz) {
    for (int y = WORLD_SIZE - 1; y >= 0; y--)
        if (getBlock(bx, y, bz) != BLOCK_AIR) return (float)y + 0.5f;
    return -0.5f;
}

static bool standable(int x, int by, int z) {
    return getBlock(x, by, z) != BLOCK_AIR && getBlock(x, by + 1, z) == BLOCK_AIR && getBlock(x, by + 2, z) == BLOCK_AIR;
}

static float findStand(int x, int z, float refFeet) {
    int top = w2c(refFeet);
    for (int by = top; by >= top - 4 && by >= 0; by--)
        if (standable(x, by, z)) return (float)by + 0.5f;
    return -1.0f;
}

static void generateWorld(void) {
    memset(world, BLOCK_AIR, sizeof(world));
    for (int x = 0; x < WORLD_SIZE; x++) {
        for (int z = 0; z < WORLD_SIZE; z++) {
            int h = (int)terrainHeight(x, z);
            if (h < 1) h = 1;
            if (h > WORLD_SIZE - 8) h = WORLD_SIZE - 8;
            for (int y = 0; y < h; y++) {
                if (y == h - 1) world[x][y][z] = BLOCK_GRASS;
                else if (y > h - 4) world[x][y][z] = BLOCK_DIRT;
                else world[x][y][z] = BLOCK_STONE;
            }
            if ((x % 7 == 0) && (z % 11 == 0) && h > 5) {
                for (int ty = h; ty < h + 4 && ty < WORLD_SIZE; ty++) world[x][ty][z] = BLOCK_WOOD;
                for (int lx = x - 2; lx <= x + 2; lx++)
                    for (int lz = z - 2; lz <= z + 2; lz++)
                        for (int ly = h + 3; ly <= h + 5 && ly < WORLD_SIZE; ly++)
                            if (lx >= 0 && lx < WORLD_SIZE && lz >= 0 && lz < WORLD_SIZE)
                                if (world[lx][ly][lz] == BLOCK_AIR) world[lx][ly][lz] = BLOCK_LEAVES;
            }
        }
    }
}

static int tileForFace(int b, int face) {
    if (b == BLOCK_GRASS) {
        if (face == 0) return 0;
        if (face == 1) return 2;
        return 1;
    }
    switch (b) {
        case BLOCK_DIRT: return 2;
        case BLOCK_STONE: return 3;
        case BLOCK_WOOD: return 4;
        case BLOCK_LEAVES: return 5;
        case BLOCK_PLANK: return 6;
        case BLOCK_BRICK: return 7;
        default: return 2;
    }
}

static Color tileColor(int tile, int px, int py) {
    float n = hash2(px + tile * 137 + 100, py + tile * 211 + 200);
    if (tile == 0) {
        if (n < 0.30f) return (Color){ 70, 130, 40, 255 };
        if (n > 0.85f) return (Color){ 130, 190, 70, 255 };
        return (Color){ 95, 159, 53, 255 };
    }
    if (tile == 1) {
        int edge = 3 + (hash2(px + 51, 7) > 0.5f ? 1 : 0);
        if (py < edge) {
            if (n < 0.30f) return (Color){ 70, 130, 40, 255 };
            return (Color){ 95, 159, 53, 255 };
        }
        if (py == edge && n > 0.55f) return (Color){ 80, 140, 48, 255 };
        if (n < 0.30f) return (Color){ 100, 70, 45, 255 };
        if (n > 0.82f) return (Color){ 160, 120, 85, 255 };
        return (Color){ 134, 96, 67, 255 };
    }
    if (tile == 2) {
        if (n < 0.30f) return (Color){ 100, 70, 45, 255 };
        if (n > 0.82f) return (Color){ 160, 120, 85, 255 };
        return (Color){ 134, 96, 67, 255 };
    }
    if (tile == 3) {
        Color c;
        if (n < 0.20f) c = (Color){ 95, 95, 95, 255 };
        else if (n > 0.82f) c = (Color){ 160, 160, 160, 255 };
        else c = (Color){ 128, 128, 128, 255 };
        if (hash2(px * 3 + 700, py * 3 + 800) < 0.06f) c = (Color){ 70, 70, 70, 255 };
        return c;
    }
    if (tile == 4) {
        int stripe = px % 4;
        Color c;
        if (stripe == 0) c = (Color){ 70, 45, 22, 255 };
        else if (stripe == 1) c = (Color){ 120, 80, 40, 255 };
        else c = (Color){ 102, 67, 32, 255 };
        if (hash2(px + 900, py + 1000) < 0.08f) c = (Color){ 60, 40, 20, 255 };
        return c;
    }
    if (tile == 5) {
        if (n < 0.35f) return (Color){ 30, 80, 30, 255 };
        if (n > 0.72f) return (Color){ 65, 140, 65, 255 };
        return (Color){ 45, 110, 45, 255 };
    }
    if (tile == 6) {
        int band = py / 4;
        if (py % 4 == 0) return (Color){ 110, 80, 45, 255 };
        if ((px + band * 5) % 8 == 0) return (Color){ 120, 90, 52, 255 };
        if (n < 0.25f) return (Color){ 160, 124, 74, 255 };
        if (n > 0.80f) return (Color){ 196, 158, 100, 255 };
        return (Color){ 178, 140, 86, 255 };
    }
    if (tile == 7) {
        int row = py / 4;
        int off = (row % 2) * 4;
        if (py % 4 == 3 || (px + off) % 8 == 7) return (Color){ 188, 180, 170, 255 };
        if (n < 0.25f) return (Color){ 128, 50, 42, 255 };
        if (n > 0.80f) return (Color){ 168, 76, 62, 255 };
        return (Color){ 150, 62, 52, 255 };
    }
    return WHITE;
}

static void generateAtlas(void) {
    int aw = ATLAS_COLS * TILE_SIZE, ah = ATLAS_ROWS * TILE_SIZE;
    Image img = GenImageColor(aw, ah, BLACK);
    for (int tile = 0; tile < ATLAS_COLS * ATLAS_ROWS; tile++) {
        int tx = tile % ATLAS_COLS, ty = tile / ATLAS_COLS;
        for (int py = 0; py < TILE_SIZE; py++)
            for (int px = 0; px < TILE_SIZE; px++)
                ImageDrawPixel(&img, tx * TILE_SIZE + px, ty * TILE_SIZE + py, tileColor(tile, px, py));
    }
    blockAtlas = LoadTextureFromImage(img);
    SetTextureFilter(blockAtlas, TEXTURE_FILTER_POINT);
    UnloadImage(img);
}

static const float faceVerts[6][12] = {
    { -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, -0.5f },
    { -0.5f, -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f },
    { 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f, 0.5f },
    { -0.5f, -0.5f, -0.5f, -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, -0.5f },
    { 0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f },
    { -0.5f, -0.5f, -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, -0.5f, -0.5f }
};
static const float faceUV[6][8] = {
    { 0, 0, 0, 1, 1, 1, 1, 0 },
    { 0, 1, 1, 1, 1, 0, 0, 0 },
    { 0, 1, 1, 1, 1, 0, 0, 0 },
    { 0, 1, 1, 1, 1, 0, 0, 0 },
    { 1, 1, 1, 0, 0, 0, 0, 1 },
    { 0, 1, 0, 0, 1, 0, 1, 1 }
};
static const float faceNormals[6][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const int faceNeighbor[6][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const unsigned char faceShade[6] = { 255, 150, 205, 205, 180, 180 };

static void rebuildChunk(int ci) {
    if (ci < 0 || ci >= CHUNK_COUNT) return;
    if (chunkMeshes[ci].built && chunkMeshes[ci].mesh.vertices) UnloadMesh(chunkMeshes[ci].mesh);
    chunkMeshes[ci].mesh = (Mesh){ 0 };
    chunkMeshes[ci].built = true;
    int cx = ci % CHUNKS_PER_AXIS;
    int cy = (ci / CHUNKS_PER_AXIS) % CHUNKS_PER_AXIS;
    int cz = ci / (CHUNKS_PER_AXIS * CHUNKS_PER_AXIS);
    int x0 = cx * CHUNK_SIZE, y0 = cy * CHUNK_SIZE, z0 = cz * CHUNK_SIZE;
    int faceCount = 0;
    for (int x = x0; x < x0 + CHUNK_SIZE; x++)
        for (int y = y0; y < y0 + CHUNK_SIZE; y++)
            for (int z = z0; z < z0 + CHUNK_SIZE; z++) {
                if (world[x][y][z] == BLOCK_AIR) continue;
                for (int f = 0; f < 6; f++)
                    if (getBlock(x + faceNeighbor[f][0], y + faceNeighbor[f][1], z + faceNeighbor[f][2]) == BLOCK_AIR) faceCount++;
            }
    if (faceCount == 0) return;
    if (faceCount > 16000) faceCount = 16000;
    float *verts = (float *)malloc((size_t)faceCount * 12 * sizeof(float));
    float *texs = (float *)malloc((size_t)faceCount * 8 * sizeof(float));
    float *norms = (float *)malloc((size_t)faceCount * 12 * sizeof(float));
    unsigned char *cols = (unsigned char *)malloc((size_t)faceCount * 16);
    unsigned short *indices = (unsigned short *)malloc((size_t)faceCount * 6 * sizeof(unsigned short));
    if (!verts || !texs || !norms || !cols || !indices) {
        free(verts); free(texs); free(norms); free(cols); free(indices);
        chunkMeshes[ci].built = false;
        return;
    }
    int vi = 0, ti = 0, ni = 0, cbi = 0, ii = 0, base = 0, added = 0;
    for (int x = x0; x < x0 + CHUNK_SIZE && added < faceCount; x++)
        for (int y = y0; y < y0 + CHUNK_SIZE && added < faceCount; y++)
            for (int z = z0; z < z0 + CHUNK_SIZE && added < faceCount; z++) {
                unsigned char b = world[x][y][z];
                if (b == BLOCK_AIR) continue;
                for (int f = 0; f < 6 && added < faceCount; f++) {
                    if (getBlock(x + faceNeighbor[f][0], y + faceNeighbor[f][1], z + faceNeighbor[f][2]) != BLOCK_AIR) continue;
                    int tile = tileForFace(b, f);
                    float u0 = (float)(tile % ATLAS_COLS) / (float)ATLAS_COLS;
                    float v0 = (float)(tile / ATLAS_COLS) / (float)ATLAS_ROWS;
                    float u1 = (float)(tile % ATLAS_COLS + 1) / (float)ATLAS_COLS;
                    float v1 = (float)(tile / ATLAS_COLS + 1) / (float)ATLAS_ROWS;
                    for (int k = 0; k < 4; k++) {
                        verts[vi++] = (float)x + faceVerts[f][k * 3 + 0];
                        verts[vi++] = (float)y + faceVerts[f][k * 3 + 1];
                        verts[vi++] = (float)z + faceVerts[f][k * 3 + 2];
                        float uu = faceUV[f][k * 2 + 0] > 0.5f ? u1 : u0;
                        float vv = faceUV[f][k * 2 + 1] > 0.5f ? v1 : v0;
                        texs[ti++] = uu;
                        texs[ti++] = vv;
                        norms[ni++] = faceNormals[f][0];
                        norms[ni++] = faceNormals[f][1];
                        norms[ni++] = faceNormals[f][2];
                        cols[cbi++] = faceShade[f];
                        cols[cbi++] = faceShade[f];
                        cols[cbi++] = faceShade[f];
                        cols[cbi++] = 255;
                    }
                    indices[ii++] = (unsigned short)(base + 0);
                    indices[ii++] = (unsigned short)(base + 1);
                    indices[ii++] = (unsigned short)(base + 2);
                    indices[ii++] = (unsigned short)(base + 0);
                    indices[ii++] = (unsigned short)(base + 2);
                    indices[ii++] = (unsigned short)(base + 3);
                    base += 4;
                    added++;
                }
            }
    Mesh m = { 0 };
    m.vertexCount = base;
    m.triangleCount = added * 2;
    m.vertices = verts;
    m.texcoords = texs;
    m.normals = norms;
    m.colors = cols;
    m.indices = indices;
    UploadMesh(&m, false);
    chunkMeshes[ci].mesh = m;
}

static void rebuildAllChunks(void) {
    for (int i = 0; i < CHUNK_COUNT; i++) rebuildChunk(i);
}

static void rebuildAroundBlock(int x, int y, int z) {
    int cx = x / CHUNK_SIZE, cy = y / CHUNK_SIZE, cz = z / CHUNK_SIZE;
    int stride = CHUNKS_PER_AXIS, plane = CHUNKS_PER_AXIS * CHUNKS_PER_AXIS;
    rebuildChunk(cx + cy * stride + cz * plane);
    if (x % CHUNK_SIZE == 0 && cx > 0) rebuildChunk((cx - 1) + cy * stride + cz * plane);
    if (x % CHUNK_SIZE == CHUNK_SIZE - 1 && cx < CHUNKS_PER_AXIS - 1) rebuildChunk((cx + 1) + cy * stride + cz * plane);
    if (y % CHUNK_SIZE == 0 && cy > 0) rebuildChunk(cx + (cy - 1) * stride + cz * plane);
    if (y % CHUNK_SIZE == CHUNK_SIZE - 1 && cy < CHUNKS_PER_AXIS - 1) rebuildChunk(cx + (cy + 1) * stride + cz * plane);
    if (z % CHUNK_SIZE == 0 && cz > 0) rebuildChunk(cx + cy * stride + (cz - 1) * plane);
    if (z % CHUNK_SIZE == CHUNK_SIZE - 1 && cz < CHUNKS_PER_AXIS - 1) rebuildChunk(cx + cy * stride + (cz + 1) * plane);
}

static Ray mouseRay(Vector2 mouse) {
    Ray ray;
    ray.position = camera.position;
    Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
    Vector3 up = Vector3CrossProduct(right, forward);
    float aspect = (float)GetScreenWidth() / (float)GetScreenHeight();
    float hh = tanf(camera.fovy * DEG2RAD * 0.5f);
    float hw = hh * aspect;
    float nx = (mouse.x / (float)GetScreenWidth()) * 2.0f - 1.0f;
    float ny = -((mouse.y / (float)GetScreenHeight()) * 2.0f - 1.0f);
    Vector3 dir = Vector3Add(forward, Vector3Add(Vector3Scale(right, nx * hw), Vector3Scale(up, ny * hh)));
    ray.direction = Vector3Normalize(dir);
    return ray;
}

static RayHit castRay(Ray ray, float maxDist) {
    RayHit h;
    memset(&h, 0, sizeof(h));
    Vector3 o = { ray.position.x + 0.5f, ray.position.y + 0.5f, ray.position.z + 0.5f };
    Vector3 d = ray.direction;
    int cx = (int)floorf(o.x), cy = (int)floorf(o.y), cz = (int)floorf(o.z);
    int sx = d.x > 0.0f ? 1 : -1, sy = d.y > 0.0f ? 1 : -1, sz = d.z > 0.0f ? 1 : -1;
    float tdx = fabsf(d.x) > 1e-8f ? fabsf(1.0f / d.x) : 1e30f;
    float tdy = fabsf(d.y) > 1e-8f ? fabsf(1.0f / d.y) : 1e30f;
    float tdz = fabsf(d.z) > 1e-8f ? fabsf(1.0f / d.z) : 1e30f;
    float tmx = tdx >= 1e30f ? 1e30f : (sx > 0 ? ((float)cx + 1.0f - o.x) : (o.x - (float)cx)) * tdx;
    float tmy = tdy >= 1e30f ? 1e30f : (sy > 0 ? ((float)cy + 1.0f - o.y) : (o.y - (float)cy)) * tdy;
    float tmz = tdz >= 1e30f ? 1e30f : (sz > 0 ? ((float)cz + 1.0f - o.z) : (o.z - (float)cz)) * tdz;
    int px = cx, py = cy, pz = cz, face = -1;
    float t = 0.0f;
    for (int i = 0; i < 600; i++) {
        if (cx < -1 || cx > WORLD_SIZE || cy < -1 || cy > WORLD_SIZE || cz < -1 || cz > WORLD_SIZE) break;
        if (getBlock(cx, cy, cz) != BLOCK_AIR) {
            h.hit = true;
            h.x = cx; h.y = cy; h.z = cz;
            h.px = px; h.py = py; h.pz = pz;
            h.face = face;
            h.dist = t;
            h.point = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
            return h;
        }
        px = cx; py = cy; pz = cz;
        if (tmx <= tmy && tmx <= tmz) {
            t = tmx; if (t > maxDist) break;
            cx += sx; tmx += tdx;
            face = sx > 0 ? 3 : 2;
        } else if (tmy <= tmz) {
            t = tmy; if (t > maxDist) break;
            cy += sy; tmy += tdy;
            face = sy > 0 ? 1 : 0;
        } else {
            t = tmz; if (t > maxDist) break;
            cz += sz; tmz += tdz;
            face = sz > 0 ? 5 : 4;
        }
    }
    return h;
}

static bool rayBox(Ray r, Vector3 mn, Vector3 mx, float *tOut) {
    float o[3] = { r.position.x, r.position.y, r.position.z };
    float d[3] = { r.direction.x, r.direction.y, r.direction.z };
    float lo[3] = { mn.x, mn.y, mn.z };
    float hi[3] = { mx.x, mx.y, mx.z };
    float tmin = 0.0f, tmax = 1e30f;
    for (int a = 0; a < 3; a++) {
        if (fabsf(d[a]) < 1e-8f) {
            if (o[a] < lo[a] || o[a] > hi[a]) return false;
        } else {
            float t1 = (lo[a] - o[a]) / d[a];
            float t2 = (hi[a] - o[a]) / d[a];
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    *tOut = tmin;
    return true;
}

static Vector3 avatarCenter(void) {
    return (Vector3){ avatar.position.x, avatar.position.y + 0.9f, avatar.position.z };
}

static bool boxFree(float x, float y, float z) {
    int x0 = w2c(x - AV_HALF), x1 = w2c(x + AV_HALF - 0.0001f);
    int y0 = w2c(y), y1 = w2c(y + AV_HEIGHT - 0.0001f);
    int z0 = w2c(z - AV_HALF), z1 = w2c(z + AV_HALF - 0.0001f);
    for (int bx = x0; bx <= x1; bx++)
        for (int by = y0; by <= y1; by++)
            for (int bz = z0; bz <= z1; bz++)
                if (getBlock(bx, by, bz) != BLOCK_AIR) return false;
    return true;
}

static float stepTopAt(float x, float y, float z) {
    int x0 = w2c(x - AV_HALF), x1 = w2c(x + AV_HALF - 0.0001f);
    int z0 = w2c(z - AV_HALF), z1 = w2c(z + AV_HALF - 0.0001f);
    int y0 = w2c(y), y1 = w2c(y + 1.0f);
    float best = -1.0f;
    for (int bx = x0; bx <= x1; bx++)
        for (int bz = z0; bz <= z1; bz++)
            for (int by = y0; by <= y1; by++) {
                if (getBlock(bx, by, bz) == BLOCK_AIR) continue;
                float top = (float)by + 0.5f;
                if (top > y + 0.001f && top <= y + 1.05f && top > best) best = top;
            }
    return best;
}

static void moveHorizSingle(float dx, float dz) {
    if (dx != 0.0f) {
        float nx = avatar.position.x + dx;
        if (boxFree(nx, avatar.position.y, avatar.position.z)) {
            avatar.position.x = nx;
        } else if (avatar.onGround) {
            float top = stepTopAt(nx, avatar.position.y, avatar.position.z);
            if (top > 0.0f && boxFree(avatar.position.x, top, avatar.position.z) && boxFree(nx, top, avatar.position.z)) {
                avatar.position.y = top;
                avatar.position.x = nx;
            }
        }
    }
    if (dz != 0.0f) {
        float nz = avatar.position.z + dz;
        if (boxFree(avatar.position.x, avatar.position.y, nz)) {
            avatar.position.z = nz;
        } else if (avatar.onGround) {
            float top = stepTopAt(avatar.position.x, avatar.position.y, nz);
            if (top > 0.0f && boxFree(avatar.position.x, top, avatar.position.z) && boxFree(avatar.position.x, top, nz)) {
                avatar.position.y = top;
                avatar.position.z = nz;
            }
        }
    }
    avatar.position.x = Clamp(avatar.position.x, 0.0f, (float)(WORLD_SIZE - 1));
    avatar.position.z = Clamp(avatar.position.z, 0.0f, (float)(WORLD_SIZE - 1));
}

static void moveHorizSteps(float dx, float dz) {
    int n = (int)ceilf(fmaxf(fabsf(dx), fabsf(dz)) / 0.2f);
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) moveHorizSingle(dx / (float)n, dz / (float)n);
}

static void respawnAvatar(void) {
    float feet = findStand(32, 32, 62.0f);
    if (feet < 0.0f) feet = floorYAt(32, 32);
    avatar.position = (Vector3){ 32.0f, feet, 32.0f };
    avatar.vy = 0.0f;
    avatar.onGround = true;
    avatar.moving = false;
    avatar.sprinting = false;
    avatar.yaw = 0.0f;
    avatarYawTarget = 0.0f;
    avatar.walkPhase = 0.0f;
    avatar.animBlend = 0.0f;
}

static void updateVertical(float dt) {
    avatar.vy -= 25.0f * dt;
    if (avatar.vy < -30.0f) avatar.vy = -30.0f;
    float dy = avatar.vy * dt;
    int n = (int)ceilf(fabsf(dy) / 0.2f);
    if (n < 1) n = 1;
    float step = dy / (float)n;
    for (int i = 0; i < n; i++) {
        float ny = avatar.position.y + step;
        if (boxFree(avatar.position.x, ny, avatar.position.z)) {
            avatar.position.y = ny;
            continue;
        }
        if (avatar.vy <= 0.0f) {
            int fy = w2c(ny);
            avatar.position.y = (float)fy + 0.5f;
        } else {
            int hy = w2c(ny + AV_HEIGHT - 0.0001f);
            avatar.position.y = (float)hy - 0.5f - AV_HEIGHT - 0.001f;
        }
        avatar.vy = 0.0f;
        break;
    }
    avatar.onGround = !boxFree(avatar.position.x, avatar.position.y - 0.05f, avatar.position.z);
    if (avatar.position.y < -12.0f) {
        respawnAvatar();
        toast("Respawned");
    }
}

static bool cellOverlapsAvatar(int x, int y, int z) {
    return fabsf((float)x - avatar.position.x) < 0.5f + AV_HALF &&
           (float)y - 0.5f < avatar.position.y + AV_HEIGHT &&
           (float)y + 0.5f > avatar.position.y &&
           fabsf((float)z - avatar.position.z) < 0.5f + AV_HALF;
}

static bool canPlaceAt(int x, int y, int z) {
    if (x < 0 || x >= WORLD_SIZE || y < 0 || y >= WORLD_SIZE || z < 0 || z >= WORLD_SIZE) return false;
    if (getBlock(x, y, z) != BLOCK_AIR) return false;
    if (cellOverlapsAvatar(x, y, z)) return false;
    if (y == 0) return true;
    for (int k = 0; k < 6; k++)
        if (getBlock(x + faceNeighbor[k][0], y + faceNeighbor[k][1], z + faceNeighbor[k][2]) != BLOCK_AIR) return true;
    return false;
}

static PathNode pathNodes[WORLD_SIZE * WORLD_SIZE];
static int heapBuf[16384];
static int heapCount = 0;
static Vector3 activePath[MAX_PATH];
static int activePathLen = 0;
static int pathIndex = 0;
static float totalPathDist = 0.0f;
static int pathGoalX = 0, pathGoalZ = 0;
static float pathGoalRef = 0.0f;

static void heapPush(int idx) {
    if (heapCount >= 16384) return;
    int i = heapCount++;
    heapBuf[i] = idx;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (pathNodes[heapBuf[p]].f <= pathNodes[heapBuf[i]].f) break;
        int tmp = heapBuf[p]; heapBuf[p] = heapBuf[i]; heapBuf[i] = tmp;
        i = p;
    }
}

static int heapPop(void) {
    int top = heapBuf[0];
    heapCount--;
    heapBuf[0] = heapBuf[heapCount];
    int i = 0;
    for (;;) {
        int l = i * 2 + 1, r = l + 1, m = i;
        if (l < heapCount && pathNodes[heapBuf[l]].f < pathNodes[heapBuf[m]].f) m = l;
        if (r < heapCount && pathNodes[heapBuf[r]].f < pathNodes[heapBuf[m]].f) m = r;
        if (m == i) break;
        int tmp = heapBuf[m]; heapBuf[m] = heapBuf[i]; heapBuf[i] = tmp;
        i = m;
    }
    return top;
}

static bool computePath(int gx, int gz, float refFeet) {
    activePathLen = 0;
    pathIndex = 0;
    totalPathDist = 0.0f;
    pathGoalX = gx; pathGoalZ = gz; pathGoalRef = refFeet;
    if (gx < 0 || gx >= WORLD_SIZE || gz < 0 || gz >= WORLD_SIZE) return false;
    int sx = w2c(avatar.position.x), sz = w2c(avatar.position.z);
    float sFeet = findStand(sx, sz, avatar.position.y);
    float gFeet = findStand(gx, gz, refFeet);
    if (sFeet < 0.0f || gFeet < 0.0f) return false;
    memset(pathNodes, 0, sizeof(pathNodes));
    heapCount = 0;
    int si = sx + sz * WORLD_SIZE;
    pathNodes[si].x = sx; pathNodes[si].z = sz;
    pathNodes[si].feetY = sFeet;
    pathNodes[si].g = 0.0f;
    pathNodes[si].f = (float)(abs(gx - sx) + abs(gz - sz));
    pathNodes[si].parent = -1;
    pathNodes[si].state = 1;
    heapPush(si);
    static const int dx4[4] = { 1, -1, 0, 0 };
    static const int dz4[4] = { 0, 0, 1, -1 };
    int goalIdx = -1, expanded = 0;
    while (heapCount > 0 && expanded < 6000) {
        int ci = heapPop();
        PathNode *cn = &pathNodes[ci];
        if (cn->state == 2) continue;
        cn->state = 2;
        expanded++;
        if (cn->x == gx && cn->z == gz) { goalIdx = ci; break; }
        for (int k = 0; k < 4; k++) {
            int nx = cn->x + dx4[k], nz = cn->z + dz4[k];
            if (nx < 0 || nx >= WORLD_SIZE || nz < 0 || nz >= WORLD_SIZE) continue;
            int ni = nx + nz * WORLD_SIZE;
            if (pathNodes[ni].state == 2) continue;
            float nf = findStand(nx, nz, cn->feetY);
            if (nf < 0.0f) continue;
            float ng = cn->g + 1.0f + fabsf(nf - cn->feetY) * 0.4f;
            if (pathNodes[ni].state == 0 || ng < pathNodes[ni].g) {
                pathNodes[ni].x = nx; pathNodes[ni].z = nz;
                pathNodes[ni].feetY = nf;
                pathNodes[ni].g = ng;
                pathNodes[ni].f = ng + (float)(abs(gx - nx) + abs(gz - nz));
                pathNodes[ni].parent = ci;
                pathNodes[ni].state = 1;
                heapPush(ni);
            }
        }
    }
    if (goalIdx < 0) return false;
    Vector3 tmp[MAX_PATH];
    int n = 0;
    for (int i = goalIdx; i >= 0 && n < MAX_PATH; i = pathNodes[i].parent)
        tmp[n++] = (Vector3){ (float)pathNodes[i].x, pathNodes[i].feetY, (float)pathNodes[i].z };
    for (int i = 0; i < n; i++) activePath[i] = tmp[n - 1 - i];
    activePathLen = n;
    for (int i = 1; i < n; i++) totalPathDist += Vector3Distance(activePath[i - 1], activePath[i]);
    return true;
}

static float remainingPathDist(void) {
    if (pathIndex >= activePathLen) return 0.0f;
    float d = Vector3Distance(avatar.position, activePath[pathIndex]);
    for (int i = pathIndex + 1; i < activePathLen; i++) d += Vector3Distance(activePath[i - 1], activePath[i]);
    return d;
}

static Task tasks[MAX_TASKS];
static int taskCount = 0;
static bool taskActive = false;
static bool taskApproach = false;
static float taskWork = 0.0f;
static int repathCount = 0;
static float stuckTimer = 0.0f;
static Cell chopBlocks[MAX_CHOP];
static int chopWoodN = 0, chopLeafN = 0, chopDone = 0;
static int fillTotal = 0, fillDone = 0;
static unsigned char chopMark[WORLD_SIZE][WORLD_SIZE][WORLD_SIZE];

static FollowResult followPath(float speed, float dt) {
    if (pathIndex >= activePathLen) return FOLLOW_ARRIVED;
    Vector3 wp = activePath[pathIndex];
    float ddx = wp.x - avatar.position.x, ddz = wp.z - avatar.position.z;
    float dist = sqrtf(ddx * ddx + ddz * ddz);
    if (dist < 0.18f) {
        pathIndex++;
        return pathIndex >= activePathLen ? FOLLOW_ARRIVED : FOLLOW_MOVING;
    }
    float dirx = ddx / dist, dirz = ddz / dist;
    float step = speed * dt;
    if (step > dist) step = dist;
    Vector3 before = avatar.position;
    moveHorizSteps(dirx * step, dirz * step);
    avatar.moving = true;
    avatar.sprinting = tasks[0].sprint;
    avatarYawTarget = atan2f(dirx, dirz);
    float moved = sqrtf((avatar.position.x - before.x) * (avatar.position.x - before.x) +
                        (avatar.position.z - before.z) * (avatar.position.z - before.z));
    if (moved < step * 0.25f) {
        stuckTimer += dt;
        if (stuckTimer > 0.7f) {
            stuckTimer = 0.0f;
            if (repathCount < 2) {
                repathCount++;
                if (!computePath(pathGoalX, pathGoalZ, pathGoalRef)) return FOLLOW_FAILED;
            } else {
                return FOLLOW_FAILED;
            }
        }
    } else {
        stuckTimer = 0.0f;
    }
    return FOLLOW_MOVING;
}

static float mineDuration(int b, int tool) {
    if (b == BLOCK_STONE || b == BLOCK_BRICK) return tool == SLOT_PICK ? 1.1f : 2.6f;
    if (b == BLOCK_WOOD || b == BLOCK_PLANK) return tool == SLOT_AXE ? 0.7f : 1.4f;
    if (b == BLOCK_LEAVES) return 0.25f;
    return 0.55f;
}

static int cellCmpY(const void *a, const void *b) {
    const Cell *ca = (const Cell *)a;
    const Cell *cb = (const Cell *)b;
    return ca->y - cb->y;
}

static void buildChopList(int bx, int by, int bz) {
    memset(chopMark, 0, sizeof(chopMark));
    chopWoodN = 0;
    chopLeafN = 0;
    Cell q[MAX_CHOP];
    int qh = 0, qt = 0;
    q[qt++] = (Cell){ bx, by, bz };
    chopMark[bx][by][bz] = 1;
    while (qh < qt && chopWoodN < 128) {
        Cell c = q[qh++];
        if (getBlock(c.x, c.y, c.z) != BLOCK_WOOD) continue;
        chopBlocks[chopWoodN++] = c;
        for (int k = 0; k < 6; k++) {
            int nx = c.x + faceNeighbor[k][0], ny = c.y + faceNeighbor[k][1], nz = c.z + faceNeighbor[k][2];
            if (nx < 0 || nx >= WORLD_SIZE || ny < 0 || ny >= WORLD_SIZE || nz < 0 || nz >= WORLD_SIZE) continue;
            if (!chopMark[nx][ny][nz] && getBlock(nx, ny, nz) == BLOCK_WOOD && qt < MAX_CHOP) {
                chopMark[nx][ny][nz] = 1;
                q[qt++] = (Cell){ nx, ny, nz };
            }
        }
    }
    qsort(chopBlocks, (size_t)chopWoodN, sizeof(Cell), cellCmpY);
    qh = 0; qt = 0;
    for (int i = 0; i < chopWoodN; i++) {
        Cell c = chopBlocks[i];
        for (int k = 0; k < 6; k++) {
            int nx = c.x + faceNeighbor[k][0], ny = c.y + faceNeighbor[k][1], nz = c.z + faceNeighbor[k][2];
            if (nx < 0 || nx >= WORLD_SIZE || ny < 0 || ny >= WORLD_SIZE || nz < 0 || nz >= WORLD_SIZE) continue;
            if (!chopMark[nx][ny][nz] && getBlock(nx, ny, nz) == BLOCK_LEAVES && qt < MAX_CHOP) {
                chopMark[nx][ny][nz] = 1;
                q[qt++] = (Cell){ nx, ny, nz };
            }
        }
    }
    while (qh < qt && chopWoodN + chopLeafN < MAX_CHOP) {
        Cell c = q[qh++];
        if (getBlock(c.x, c.y, c.z) != BLOCK_LEAVES) continue;
        chopBlocks[chopWoodN + chopLeafN] = c;
        chopLeafN++;
        for (int k = 0; k < 6; k++) {
            int nx = c.x + faceNeighbor[k][0], ny = c.y + faceNeighbor[k][1], nz = c.z + faceNeighbor[k][2];
            if (nx < 0 || nx >= WORLD_SIZE || ny < 0 || ny >= WORLD_SIZE || nz < 0 || nz >= WORLD_SIZE) continue;
            if (!chopMark[nx][ny][nz] && getBlock(nx, ny, nz) == BLOCK_LEAVES && qt < MAX_CHOP) {
                chopMark[nx][ny][nz] = 1;
                q[qt++] = (Cell){ nx, ny, nz };
            }
        }
    }
}

static float chopThreshold(const Task *t, int i) {
    if (i < t->a) return (float)(i + 1) * 0.35f;
    return (float)t->a * 0.35f + (float)(i - t->a + 1) * 0.04f;
}

static Vector3 taskCenter(const Task *t) {
    if (t->type == TASK_MOVE) return (Vector3){ (float)t->bx, t->fy, (float)t->bz };
    if (t->type == TASK_FILL)
        return (Vector3){ (float)(t->bx + t->x2) * 0.5f, (float)(t->by + t->y2) * 0.5f, (float)(t->bz + t->z2) * 0.5f };
    return (Vector3){ (float)t->bx, (float)t->by, (float)t->bz };
}

static bool findApproach(const Task *t, Cell *out) {
    int cx = t->bx, cz = t->bz;
    float refFeet = (float)t->by + 0.5f;
    if (t->type == TASK_FILL) {
        cx = (int)Clamp((float)w2c(avatar.position.x), (float)t->bx, (float)t->x2);
        cz = (int)Clamp((float)w2c(avatar.position.z), (float)t->bz, (float)t->z2);
        refFeet = (float)t->y2 + 0.5f;
    }
    float bestScore = 1e30f;
    bool found = false;
    int rad = t->type == TASK_FILL ? 1 : 2;
    Vector3 center = taskCenter(t);
    for (int dz = -rad; dz <= rad; dz++) {
        for (int dx = -rad; dx <= rad; dx++) {
            int nx = cx + dx, nz = cz + dz;
            if (nx < 0 || nx >= WORLD_SIZE || nz < 0 || nz >= WORLD_SIZE) continue;
            float feet = findStand(nx, nz, refFeet);
            if (feet < 0.0f) continue;
            if (t->type != TASK_FILL) {
                float d = Vector3Distance((Vector3){ (float)nx, feet + 0.9f, (float)nz }, center);
                if (d > REACH - 0.4f) continue;
            }
            float score = fabsf((float)nx - avatar.position.x) + fabsf((float)nz - avatar.position.z) +
                          fabsf(feet - avatar.position.y) * 0.5f;
            if (score < bestScore) {
                bestScore = score;
                *out = (Cell){ nx, (int)(feet - 0.5f), nz };
                found = true;
            }
        }
    }
    return found;
}

static void shiftQueue(void) {
    if (taskCount > 1) memmove(&tasks[0], &tasks[1], (size_t)(taskCount - 1) * sizeof(Task));
    if (taskCount > 0) taskCount--;
    taskActive = false;
    activePathLen = 0;
    pathIndex = 0;
}

static void completeTask(void) { shiftQueue(); }

static void failTask(const char *msg) {
    toast("%s", msg);
    shiftQueue();
}

static void clearQueue(void) {
    taskCount = 0;
    taskActive = false;
    activePathLen = 0;
    pathIndex = 0;
}

static void cancelTask(int i) {
    if (i < 0 || i >= taskCount) return;
    if (i == 0) {
        taskActive = false;
        activePathLen = 0;
        pathIndex = 0;
    }
    if (i < taskCount - 1) memmove(&tasks[i], &tasks[i + 1], (size_t)(taskCount - i - 1) * sizeof(Task));
    taskCount--;
    toast("Task cancelled");
}

static bool enqueueTask(Task t) {
    if (taskCount >= MAX_TASKS) {
        toast("Action queue full");
        return false;
    }
    tasks[taskCount++] = t;
    return true;
}

static void enqueueMove(int bx, int bz, float refFeet, bool sprint) {
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_MOVE;
    t.bx = bx; t.bz = bz;
    t.fy = refFeet;
    t.sprint = sprint;
    t.duration = 0.0f;
    snprintf(t.label, sizeof(t.label), "%s", sprint ? "Move (sprint)" : "Move");
    enqueueTask(t);
}

static void enqueueMine(int x, int y, int z) {
    int b = getBlock(x, y, z);
    if (b == BLOCK_AIR) return;
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_MINE;
    t.bx = x; t.by = y; t.bz = z;
    t.tool = hotbar[hotbarSlot].kind == SLOT_BLOCK ? SLOT_BLOCK : hotbarSlot;
    snprintf(t.label, sizeof(t.label), "Mine %s", blockNames[b]);
    enqueueTask(t);
}

static void enqueueChop(int x, int y, int z) {
    if (getBlock(x, y, z) != BLOCK_WOOD) return;
    for (int i = 0; i < taskCount; i++)
        if (tasks[i].type == TASK_CHOP && tasks[i].bx == x && tasks[i].bz == z) return;
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_CHOP;
    t.bx = x; t.by = y; t.bz = z;
    t.tool = SLOT_AXE;
    snprintf(t.label, sizeof(t.label), "Chop tree");
    enqueueTask(t);
}

static void enqueuePlace(int x, int y, int z, int blockId) {
    if (!canPlaceAt(x, y, z)) return;
    for (int i = 0; i < taskCount; i++)
        if (tasks[i].type == TASK_PLACE && tasks[i].bx == x && tasks[i].by == y && tasks[i].bz == z) return;
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_PLACE;
    t.bx = x; t.by = y; t.bz = z;
    t.blockId = blockId;
    t.duration = 0.12f;
    snprintf(t.label, sizeof(t.label), "Place %s", blockNames[blockId]);
    enqueueTask(t);
}

static void enqueueFill(int x1, int y1, int z1, int x2, int y2, int z2, int blockId) {
    long vol = (long)(x2 - x1 + 1) * (long)(y2 - y1 + 1) * (long)(z2 - z1 + 1);
    if (vol <= 0) return;
    if (vol > 4096) {
        toast("Fill region too large (max 4096 blocks)");
        return;
    }
    Task t;
    memset(&t, 0, sizeof(t));
    t.type = TASK_FILL;
    t.bx = x1; t.by = y1; t.bz = z1;
    t.x2 = x2; t.y2 = y2; t.z2 = z2;
    t.blockId = blockId;
    snprintf(t.label, sizeof(t.label), "Fill %dx%dx%d %s", x2 - x1 + 1, y2 - y1 + 1, z2 - z1 + 1, blockNames[blockId]);
    enqueueTask(t);
}

static bool fillNextBlock(Task *t) {
    for (int y = t->by; y <= t->y2; y++)
        for (int x = t->bx; x <= t->x2; x++)
            for (int z = t->bz; z <= t->z2; z++) {
                if (getBlock(x, y, z) != BLOCK_AIR) continue;
                if (cellOverlapsAvatar(x, y, z)) continue;
                setBlock(x, y, z, (unsigned char)t->blockId);
                rebuildAroundBlock(x, y, z);
                return true;
            }
    return false;
}

static void activateTask(void) {
    Task *t = &tasks[0];
    taskActive = true;
    taskWork = 0.0f;
    repathCount = 0;
    stuckTimer = 0.0f;
    activePathLen = 0;
    pathIndex = 0;
    taskApproach = false;
    if (t->type == TASK_MOVE) {
        if (!computePath(t->bx, t->bz, t->fy)) {
            failTask("Can't reach destination");
            return;
        }
        taskApproach = true;
        return;
    }
    if (t->type == TASK_MINE || t->type == TASK_CHOP) {
        if (getBlock(t->bx, t->by, t->bz) == BLOCK_AIR) {
            completeTask();
            return;
        }
    }
    if (t->type == TASK_MINE) {
        t->duration = mineDuration(getBlock(t->bx, t->by, t->bz), t->tool);
    }
    if (t->type == TASK_CHOP) {
        buildChopList(t->bx, t->by, t->bz);
        chopDone = 0;
        t->a = chopWoodN;
        t->b = chopLeafN;
        if (chopWoodN + chopLeafN == 0) {
            completeTask();
            return;
        }
        t->duration = chopThreshold(t, chopWoodN + chopLeafN - 1);
    }
    if (t->type == TASK_PLACE) {
        t->duration = 0.12f;
        if (getBlock(t->bx, t->by, t->bz) != BLOCK_AIR) {
            completeTask();
            return;
        }
    }
    if (t->type == TASK_FILL) {
        fillTotal = 0;
        fillDone = 0;
        for (int y = t->by; y <= t->y2; y++)
            for (int x = t->bx; x <= t->x2; x++)
                for (int z = t->bz; z <= t->z2; z++)
                    if (getBlock(x, y, z) == BLOCK_AIR && !cellOverlapsAvatar(x, y, z)) fillTotal++;
        if (fillTotal == 0) {
            completeTask();
            return;
        }
        t->duration = (float)fillTotal * 0.05f;
    }
    if (Vector3Distance(avatarCenter(), taskCenter(t)) <= REACH) return;
    Cell ap;
    if (findApproach(t, &ap) && computePath(ap.x, ap.z, (float)ap.y + 0.5f)) {
        taskApproach = true;
        return;
    }
    failTask("Can't reach target");
}

static void updateTasks(float dt) {
    avatar.moving = false;
    avatar.sprinting = false;
    if (taskCount == 0) {
        taskActive = false;
        return;
    }
    if (!taskActive) activateTask();
    if (taskCount == 0 || !taskActive) return;
    Task *t = &tasks[0];
    if (taskApproach) {
        float spd = t->sprint ? SPRINT_SPEED : WALK_SPEED;
        FollowResult r = followPath(spd, dt);
        if (r == FOLLOW_FAILED) {
            failTask("Path blocked");
            return;
        }
        if (r == FOLLOW_MOVING) return;
        if (t->type == TASK_MOVE) {
            completeTask();
            return;
        }
        taskApproach = false;
        taskWork = 0.0f;
    }
    taskWork += dt;
    if (t->type == TASK_MINE) {
        if (taskWork >= t->duration) {
            if (getBlock(t->bx, t->by, t->bz) != BLOCK_AIR) {
                setBlock(t->bx, t->by, t->bz, BLOCK_AIR);
                rebuildAroundBlock(t->bx, t->by, t->bz);
            }
            completeTask();
        }
    } else if (t->type == TASK_CHOP) {
        int total = t->a + t->b;
        while (chopDone < total && taskWork >= chopThreshold(t, chopDone)) {
            Cell c = chopBlocks[chopDone];
            if (getBlock(c.x, c.y, c.z) != BLOCK_AIR) {
                setBlock(c.x, c.y, c.z, BLOCK_AIR);
                rebuildAroundBlock(c.x, c.y, c.z);
            }
            chopDone++;
        }
        if (chopDone >= total) completeTask();
    } else if (t->type == TASK_PLACE) {
        if (taskWork >= t->duration) {
            if (canPlaceAt(t->bx, t->by, t->bz)) {
                setBlock(t->bx, t->by, t->bz, (unsigned char)t->blockId);
                rebuildAroundBlock(t->bx, t->by, t->bz);
            } else {
                toast("Can't place there");
            }
            completeTask();
        }
    } else if (t->type == TASK_FILL) {
        bool finished = false;
        while (!finished && taskWork >= (float)(fillDone + 1) * 0.05f) {
            if (fillNextBlock(t)) fillDone++;
            else finished = true;
        }
        if (finished || fillDone >= fillTotal) completeTask();
    }
}

static float taskProgress(const Task *t) {
    if (t->type == TASK_MOVE) {
        if (!taskApproach) return 1.0f;
        float frac = totalPathDist > 0.0f ? 1.0f - remainingPathDist() / totalPathDist : 1.0f;
        return Clamp(frac, 0.0f, 1.0f);
    }
    if (taskApproach) {
        float frac = totalPathDist > 0.0f ? 1.0f - remainingPathDist() / totalPathDist : 1.0f;
        return Clamp(frac, 0.0f, 1.0f) * 0.4f;
    }
    float wf = t->duration > 0.0f ? taskWork / t->duration : 1.0f;
    return 0.4f + 0.6f * Clamp(wf, 0.0f, 1.0f);
}

static float taskETA(const Task *t) {
    float spd = t->sprint ? SPRINT_SPEED : WALK_SPEED;
    float travel = taskApproach ? remainingPathDist() / spd : 0.0f;
    if (t->type == TASK_MOVE) return travel;
    float work = taskApproach ? t->duration : fmaxf(0.0f, t->duration - taskWork);
    return travel + work;
}

static void updateAnim(float dt) {
    float target = avatar.moving ? 1.0f : 0.0f;
    avatar.animBlend += (target - avatar.animBlend) * fminf(1.0f, dt * 10.0f);
    if (avatar.moving) avatar.walkPhase += dt * (avatar.sprinting ? 21.0f : 13.0f);
    float d = avatarYawTarget - avatar.yaw;
    while (d > PI) d -= 2.0f * PI;
    while (d < -PI) d += 2.0f * PI;
    avatar.yaw += d * fminf(1.0f, dt * 12.0f);
}

static Cell selBlocks[MAX_SEL];
static int selCount = 0;
static bool selAvatar = false;
static bool menuOpen = false;
static Vector2 menuPos = { 0, 0 };
static MenuItem menuItems[8];
static int menuCount = 0;
static Cell menuTarget = { 0, 0, 0 };
static bool buildMode = false;
static bool buildDragging = false;
static Cell buildAnchor = { 0, 0, 0 };
static Cell buildCur = { 0, 0, 0 };
static int buildHeight = 1;
static bool helpVisible = true;
static bool leftPressing = false;
static bool boxSelecting = false;
static Vector2 pressPos = { 0, 0 };
static Vector2 boxCur = { 0, 0 };
static double lastRightClick = -10.0;
static Vector2 lastRightPos = { 0, 0 };
static bool paintActive = false;
static Cell lastPaintCell = { -999, -999, -999 };
static RayHit hover;
static Rectangle queuePanelRect = { 0, 0, 0, 0 };
static Rectangle queueRowRects[8];
static int queueRowTask[8];
static int queueRowCount = 0;
static Rectangle hotbarRects[9];
static Vector3 camFocus = { 32, 12, 32 };
static float camYaw = 0.7f, camPitch = 55.0f, camDist = 24.0f, camDistTarget = 24.0f;

static bool selectionContains(Cell c) {
    for (int i = 0; i < selCount; i++)
        if (selBlocks[i].x == c.x && selBlocks[i].y == c.y && selBlocks[i].z == c.z) return true;
    return false;
}

static int cellCmpDist(const void *a, const void *b) {
    const Cell *ca = (const Cell *)a;
    const Cell *cb = (const Cell *)b;
    float da = Vector3Distance(avatar.position, (Vector3){ (float)ca->x, (float)ca->y, (float)ca->z });
    float db = Vector3Distance(avatar.position, (Vector3){ (float)cb->x, (float)cb->y, (float)cb->z });
    return da < db ? -1 : (da > db ? 1 : 0);
}

static void batchMineSelection(void) {
    if (selCount == 0) return;
    Cell tmp[MAX_SEL];
    memcpy(tmp, selBlocks, (size_t)selCount * sizeof(Cell));
    qsort(tmp, (size_t)selCount, sizeof(Cell), cellCmpDist);
    int n = 0;
    for (int i = 0; i < selCount; i++) {
        if (getBlock(tmp[i].x, tmp[i].y, tmp[i].z) == BLOCK_AIR) continue;
        enqueueMine(tmp[i].x, tmp[i].y, tmp[i].z);
        n++;
    }
    toast("Queued %d mining tasks", n);
}

static void batchChopSelection(void) {
    int n = 0;
    for (int i = 0; i < selCount; i++) {
        Cell c = selBlocks[i];
        if (getBlock(c.x, c.y, c.z) != BLOCK_WOOD) continue;
        bool dup = false;
        for (int j = 0; j < i; j++)
            if (selBlocks[j].x == c.x && selBlocks[j].z == c.z && getBlock(selBlocks[j].x, selBlocks[j].y, selBlocks[j].z) == BLOCK_WOOD) dup = true;
        if (dup) continue;
        enqueueChop(c.x, c.y, c.z);
        n++;
    }
    toast("Queued %d tree chops", n);
}

static void openMenuBlock(Vector2 mouse, Cell c) {
    menuTarget = c;
    menuCount = 0;
    snprintf(menuItems[menuCount].label, sizeof(menuItems[0].label), "Walk here");
    menuItems[menuCount++].action = ACT_WALK;
    if (getBlock(c.x, c.y, c.z) == BLOCK_WOOD) {
        snprintf(menuItems[menuCount].label, sizeof(menuItems[0].label), "Chop tree");
        menuItems[menuCount++].action = ACT_CHOP;
    }
    snprintf(menuItems[menuCount].label, sizeof(menuItems[0].label), "Mine block");
    menuItems[menuCount++].action = ACT_MINE;
    menuPos = (Vector2){ mouse.x + 10.0f, mouse.y + 6.0f };
    menuOpen = true;
}

static void openMenuBatch(Vector2 mouse) {
    menuCount = 0;
    snprintf(menuItems[menuCount].label, sizeof(menuItems[0].label), "Mine selected (%d)", selCount);
    menuItems[menuCount++].action = ACT_MINE_SEL;
    snprintf(menuItems[menuCount].label, sizeof(menuItems[0].label), "Chop selected trees");
    menuItems[menuCount++].action = ACT_CHOP_SEL;
    snprintf(menuItems[menuCount].label, sizeof(menuItems[0].label), "Clear selection");
    menuItems[menuCount++].action = ACT_CLEAR;
    menuPos = (Vector2){ mouse.x + 10.0f, mouse.y + 6.0f };
    menuOpen = true;
}

static void openMenuAvatar(Vector2 mouse) {
    menuCount = 0;
    snprintf(menuItems[menuCount].label, sizeof(menuItems[0].label), "Stop all tasks");
    menuItems[menuCount++].action = ACT_STOP;
    snprintf(menuItems[menuCount].label, sizeof(menuItems[0].label), "Clear selection");
    menuItems[menuCount++].action = ACT_CLEAR;
    menuPos = (Vector2){ mouse.x + 10.0f, mouse.y + 6.0f };
    menuOpen = true;
}

static Rectangle menuRect(void) {
    float w = 190.0f, h = (float)menuCount * 26.0f + 8.0f;
    float x = Clamp(menuPos.x, 0.0f, (float)GetScreenWidth() - w);
    float y = Clamp(menuPos.y, 0.0f, (float)GetScreenHeight() - h);
    return (Rectangle){ x, y, w, h };
}

static int menuIndexAt(Vector2 mouse) {
    Rectangle r = menuRect();
    if (!CheckCollisionPointRec(mouse, r)) return -1;
    int idx = (int)((mouse.y - r.y - 4.0f) / 26.0f);
    if (idx < 0 || idx >= menuCount) return -1;
    return idx;
}

static void execMenuAction(int action) {
    if (action == ACT_WALK) {
        clearQueue();
        enqueueMove(menuTarget.x, menuTarget.z, (float)menuTarget.y + 0.5f, false);
    } else if (action == ACT_MINE) {
        enqueueMine(menuTarget.x, menuTarget.y, menuTarget.z);
    } else if (action == ACT_CHOP) {
        enqueueChop(menuTarget.x, menuTarget.y, menuTarget.z);
    } else if (action == ACT_MINE_SEL) {
        batchMineSelection();
    } else if (action == ACT_CHOP_SEL) {
        batchChopSelection();
    } else if (action == ACT_CLEAR) {
        selCount = 0;
        selAvatar = false;
    } else if (action == ACT_STOP) {
        clearQueue();
        toast("All tasks stopped");
    }
}

static bool pointInFront(Vector3 p) {
    Vector3 fwd = Vector3Subtract(camera.target, camera.position);
    Vector3 rel = Vector3Subtract(p, camera.position);
    return fwd.x * rel.x + fwd.y * rel.y + fwd.z * rel.z > 0.0f;
}

static void clickSelect(Vector2 mouse) {
    Ray r = mouseRay(mouse);
    Vector3 mn = { avatar.position.x - 0.45f, avatar.position.y, avatar.position.z - 0.45f };
    Vector3 mx = { avatar.position.x + 0.45f, avatar.position.y + 1.9f, avatar.position.z + 0.45f };
    float tAv = 0.0f;
    bool hitAv = rayBox(r, mn, mx, &tAv);
    if (hitAv && (!hover.hit || tAv < hover.dist)) {
        selAvatar = true;
        selCount = 0;
        openMenuAvatar(mouse);
        return;
    }
    if (hover.hit) {
        Cell c = { hover.x, hover.y, hover.z };
        if (selCount > 1 && selectionContains(c)) {
            openMenuBatch(mouse);
        } else {
            selCount = 1;
            selBlocks[0] = c;
            selAvatar = false;
            openMenuBlock(mouse, c);
        }
    } else {
        selCount = 0;
        selAvatar = false;
        menuOpen = false;
    }
}

static void finishBoxSelect(void) {
    float x1 = fminf(pressPos.x, boxCur.x), x2 = fmaxf(pressPos.x, boxCur.x);
    float y1 = fminf(pressPos.y, boxCur.y), y2 = fmaxf(pressPos.y, boxCur.y);
    selCount = 0;
    selAvatar = false;
    Vector3 ac = avatarCenter();
    if (pointInFront(ac)) {
        Vector2 sp = GetWorldToScreen(ac, camera);
        if (sp.x >= x1 && sp.x <= x2 && sp.y >= y1 && sp.y <= y2) selAvatar = true;
    }
    for (int x = 0; x < WORLD_SIZE; x++) {
        for (int z = 0; z < WORLD_SIZE; z++) {
            int top = -1;
            for (int y = WORLD_SIZE - 1; y >= 0; y--)
                if (world[x][y][z] != BLOCK_AIR) { top = y; break; }
            if (top < 0) continue;
            Cell c = { x, top, z };
            if (world[x][top][z] == BLOCK_LEAVES) {
                for (int y = top - 1; y >= top - 8 && y >= 0; y--)
                    if (world[x][y][z] == BLOCK_WOOD) { c.y = y; break; }
            }
            Vector3 p = { (float)c.x, (float)c.y, (float)c.z };
            if (!pointInFront(p)) continue;
            Vector2 sp = GetWorldToScreen(p, camera);
            if (sp.x >= x1 && sp.x <= x2 && sp.y >= y1 && sp.y <= y2) {
                if (selCount < MAX_SEL) selBlocks[selCount++] = c;
            }
        }
    }
    if (selCount > 0 || selAvatar) openMenuBatch((Vector2){ x2, y2 });
}

static void finishBuildFill(void) {
    int x1 = buildAnchor.x < buildCur.x ? buildAnchor.x : buildCur.x;
    int x2 = buildAnchor.x > buildCur.x ? buildAnchor.x : buildCur.x;
    int z1 = buildAnchor.z < buildCur.z ? buildAnchor.z : buildCur.z;
    int z2 = buildAnchor.z > buildCur.z ? buildAnchor.z : buildCur.z;
    int y1 = buildAnchor.y;
    int y2 = y1 + buildHeight - 1;
    if (y1 < 0) y1 = 0;
    if (y2 >= WORLD_SIZE) y2 = WORLD_SIZE - 1;
    if (y1 > y2) return;
    int blockId = hotbar[hotbarSlot].kind == SLOT_BLOCK ? hotbar[hotbarSlot].id : BLOCK_PLANK;
    enqueueFill(x1, y1, z1, x2, y2, z2, blockId);
}

static void tryPlaceAtHover(void) {
    if (!hover.hit) return;
    Cell c = { hover.px, hover.py, hover.pz };
    if (paintActive && c.x == lastPaintCell.x && c.y == lastPaintCell.y && c.z == lastPaintCell.z) return;
    if (canPlaceAt(c.x, c.y, c.z)) {
        enqueuePlace(c.x, c.y, c.z, hotbar[hotbarSlot].id);
        lastPaintCell = c;
    }
}

static void rightCommand(RayHit h, bool dbl) {
    if (dbl) {
        clearQueue();
        enqueueMove(h.x, h.z, (float)h.y + 0.5f, true);
        return;
    }
    bool topFace = h.face == 0;
    int b = getBlock(h.x, h.y, h.z);
    if (hotbarSlot == SLOT_AXE && b == BLOCK_WOOD) {
        enqueueChop(h.x, h.y, h.z);
        return;
    }
    if (hotbarSlot == SLOT_PICK && !topFace) {
        enqueueMine(h.x, h.y, h.z);
        return;
    }
    clearQueue();
    enqueueMove(h.x, h.z, (float)h.y + 0.5f, false);
}

static void escAction(void) {
    if (menuOpen) { menuOpen = false; return; }
    if (buildMode || buildDragging) { buildMode = false; buildDragging = false; return; }
    if (selCount > 0 || selAvatar) { selCount = 0; selAvatar = false; return; }
    if (taskCount > 0) { clearQueue(); toast("Queue cleared"); return; }
    gameState = STATE_PAUSE;
}

static void updateCameraRTS(float dt) {
    float pan = camDist * 0.85f * dt;
    float s = sinf(camYaw), c = cosf(camYaw);
    if (IsKeyDown(KEY_W)) { camFocus.x -= s * pan; camFocus.z -= c * pan; }
    if (IsKeyDown(KEY_S)) { camFocus.x += s * pan; camFocus.z += c * pan; }
    if (IsKeyDown(KEY_D)) { camFocus.x += c * pan; camFocus.z -= s * pan; }
    if (IsKeyDown(KEY_A)) { camFocus.x -= c * pan; camFocus.z += s * pan; }
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        if (buildMode && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))) {
            buildHeight = (int)Clamp((float)buildHeight + (wheel > 0.0f ? 1.0f : -1.0f), 1.0f, 16.0f);
        } else {
            camDistTarget = Clamp(camDistTarget * (1.0f - wheel * 0.12f), 8.0f, 70.0f);
        }
    }
    if (IsMouseButtonDown(MOUSE_MIDDLE_BUTTON)) {
        Vector2 d = GetMouseDelta();
        camYaw -= d.x * 0.006f;
        camPitch = Clamp(camPitch + d.y * 0.25f, 15.0f, 85.0f);
    }
    camDist += (camDistTarget - camDist) * fminf(1.0f, dt * 10.0f);
    camFocus.x = Clamp(camFocus.x, 0.0f, (float)(WORLD_SIZE - 1));
    camFocus.z = Clamp(camFocus.z, 0.0f, (float)(WORLD_SIZE - 1));
    float fy = floorYAt(w2c(camFocus.x), w2c(camFocus.z));
    camFocus.y += (fy - camFocus.y) * fminf(1.0f, dt * 6.0f);
    s = sinf(camYaw);
    c = cosf(camYaw);
    float pr = camPitch * DEG2RAD;
    Vector3 off = { s * cosf(pr) * camDist, sinf(pr) * camDist, c * cosf(pr) * camDist };
    camera.position = Vector3Add(camFocus, off);
    camera.target = camFocus;
}

static void handlePlayInput(float dt) {
    updateCameraRTS(dt);
    for (int i = 0; i < 9; i++)
        if (IsKeyPressed(KEY_ONE + i)) hotbarSlot = i;
    if (IsKeyPressed(KEY_C)) {
        buildMode = !buildMode;
        buildDragging = false;
        if (buildMode) menuOpen = false;
    }
    if (IsKeyPressed(KEY_H)) helpVisible = !helpVisible;
    if (IsKeyPressed(KEY_F)) camFocus = avatar.position;
    if (IsKeyPressed(KEY_DELETE) && selCount > 0) batchMineSelection();
    if (IsKeyPressed(KEY_ESCAPE)) {
        escAction();
        return;
    }
    Vector2 mouse = GetMousePosition();
    hover = castRay(mouseRay(mouse), 220.0f);
    bool uiLeft = false, uiRight = false;
    if (menuOpen) {
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            uiLeft = true;
            int idx = menuIndexAt(mouse);
            if (idx >= 0) execMenuAction(menuItems[idx].action);
            menuOpen = false;
        }
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
            uiRight = true;
            menuOpen = false;
        }
    }
    if (taskCount > 0 && CheckCollisionPointRec(mouse, queuePanelRect)) {
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
            uiRight = true;
            for (int i = 0; i < queueRowCount; i++)
                if (CheckCollisionPointRec(mouse, queueRowRects[i])) {
                    cancelTask(queueRowTask[i]);
                    break;
                }
        }
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) uiLeft = true;
    }
    for (int i = 0; i < 9; i++)
        if (CheckCollisionPointRec(mouse, hotbarRects[i]) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            hotbarSlot = i;
            uiLeft = true;
        }
    if (!uiLeft) {
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (buildMode) {
                if (hover.hit) {
                    buildAnchor = (Cell){ hover.px, hover.py, hover.pz };
                    buildCur = buildAnchor;
                    buildDragging = true;
                }
            } else {
                leftPressing = true;
                boxSelecting = false;
                pressPos = mouse;
                boxCur = mouse;
            }
        }
        if (leftPressing && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            boxCur = mouse;
            if (fabsf(mouse.x - pressPos.x) + fabsf(mouse.y - pressPos.y) > 6.0f) boxSelecting = true;
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
            if (buildDragging) {
                finishBuildFill();
                buildDragging = false;
            } else if (leftPressing) {
                if (boxSelecting) finishBoxSelect();
                else clickSelect(mouse);
            }
            leftPressing = false;
            boxSelecting = false;
        }
    } else {
        leftPressing = false;
        boxSelecting = false;
    }
    if (!uiRight) {
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
            if (hotbar[hotbarSlot].kind == SLOT_BLOCK) {
                paintActive = true;
                lastPaintCell = (Cell){ -999, -999, -999 };
                tryPlaceAtHover();
            } else {
                double now = GetTime();
                bool dbl = (now - lastRightClick < 0.30) &&
                           fabsf(mouse.x - lastRightPos.x) < 24.0f && fabsf(mouse.y - lastRightPos.y) < 24.0f;
                lastRightClick = now;
                lastRightPos = mouse;
                if (hover.hit) rightCommand(hover, dbl);
            }
        }
        if (paintActive && IsMouseButtonDown(MOUSE_RIGHT_BUTTON) && hotbar[hotbarSlot].kind == SLOT_BLOCK)
            tryPlaceAtHover();
        if (IsMouseButtonReleased(MOUSE_RIGHT_BUTTON)) paintActive = false;
    } else {
        paintActive = false;
    }
}

static void drawAvatar(void) {
    float blend = avatar.animBlend;
    float s1 = sinf(avatar.walkPhase), s2 = sinf(avatar.walkPhase * 0.5f + 1.3f);
    float armAng = s1 * 150.0f * blend;
    float armSide = s2 * 28.0f * blend;
    float legAng = s1 * 55.0f * blend;
    float bob = fabsf(s1) * 0.06f * blend;
    Color skin = { 224, 178, 138, 255 };
    Color shirt = { 0, 145, 160, 255 };
    Color sleeve = { 0, 115, 130, 255 };
    Color pants = { 58, 66, 140, 255 };
    Color shoe = { 70, 50, 40, 255 };
    Color hair = { 80, 50, 30, 255 };
    Color eye = { 40, 40, 60, 255 };
    rlPushMatrix();
    rlTranslatef(avatar.position.x, avatar.position.y + bob, avatar.position.z);
    rlRotatef(avatar.yaw * RAD2DEG, 0, 1, 0);
    for (int side = 0; side < 2; side++) {
        float sx = side == 0 ? -0.13f : 0.13f;
        float ang = side == 0 ? legAng : -legAng;
        rlPushMatrix();
        rlTranslatef(sx, 0.75f, 0);
        rlRotatef(ang, 1, 0, 0);
        DrawCube((Vector3){ 0, -0.33f, 0 }, 0.24f, 0.66f, 0.26f, pants);
        DrawCube((Vector3){ 0, -0.70f, 0.02f }, 0.25f, 0.10f, 0.30f, shoe);
        rlPopMatrix();
    }
    DrawCube((Vector3){ 0, 1.06f, 0 }, 0.50f, 0.62f, 0.28f, shirt);
    for (int side = 0; side < 2; side++) {
        float sx = side == 0 ? -0.36f : 0.36f;
        float ang = armAng * (side == 0 ? 1.0f : 0.92f);
        float sz = side == 0 ? armSide : -armSide;
        rlPushMatrix();
        rlTranslatef(sx, 1.32f, 0);
        rlRotatef(ang, 1, 0, 0);
        rlRotatef(sz, 0, 0, 1);
        DrawCube((Vector3){ 0, -0.26f, 0 }, 0.20f, 0.52f, 0.22f, sleeve);
        DrawCube((Vector3){ 0, -0.58f, 0 }, 0.20f, 0.14f, 0.22f, skin);
        rlPopMatrix();
    }
    rlPushMatrix();
    rlTranslatef(0, 1.38f, 0);
    rlRotatef(s2 * 7.0f * blend, 0, 0, 1);
    DrawCube((Vector3){ 0, 0.24f, 0 }, 0.45f, 0.45f, 0.45f, skin);
    DrawCube((Vector3){ 0, 0.45f, 0 }, 0.46f, 0.09f, 0.46f, hair);
    DrawCube((Vector3){ -0.10f, 0.28f, 0.226f }, 0.08f, 0.09f, 0.02f, eye);
    DrawCube((Vector3){ 0.10f, 0.28f, 0.226f }, 0.08f, 0.09f, 0.02f, eye);
    rlPopMatrix();
    rlPopMatrix();
    if (selAvatar)
        DrawCubeWires((Vector3){ avatar.position.x, avatar.position.y + 0.95f, avatar.position.z }, 0.95f, 1.95f, 0.95f, YELLOW);
}

static void drawFaceHighlight(void) {
    if (!hover.hit || menuOpen) return;
    Vector3 c = { (float)hover.x, (float)hover.y, (float)hover.z };
    DrawCubeWires(c, 1.02f, 1.02f, 1.02f, WHITE);
    if (hover.face < 0) return;
    Vector3 n = { faceNormals[hover.face][0], faceNormals[hover.face][1], faceNormals[hover.face][2] };
    Vector3 fc = Vector3Add(c, Vector3Scale(n, 0.505f));
    rlPushMatrix();
    rlTranslatef(fc.x, fc.y, fc.z);
    if (hover.face == 1) rlRotatef(180, 1, 0, 0);
    else if (hover.face == 2) rlRotatef(-90, 0, 0, 1);
    else if (hover.face == 3) rlRotatef(90, 0, 0, 1);
    else if (hover.face == 4) rlRotatef(90, 1, 0, 0);
    else if (hover.face == 5) rlRotatef(-90, 1, 0, 0);
    DrawPlane((Vector3){ 0, 0, 0 }, (Vector2){ 0.98f, 0.98f }, Fade(WHITE, 0.25f));
    rlPopMatrix();
}

static void drawPathMarkers(void) {
    if (taskCount == 0) return;
    const Task *t = &tasks[0];
    if (activePathLen > 0) {
        DrawSphere(activePath[0], 0.22f, BLUE);
        for (int i = pathIndex; i < activePathLen; i++) {
            DrawSphere(activePath[i], 0.11f, YELLOW);
            if (i > pathIndex) DrawLine3D(activePath[i - 1], activePath[i], YELLOW);
        }
        if (pathIndex < activePathLen) DrawLine3D(avatar.position, activePath[pathIndex], YELLOW);
    }
    Vector3 goal = taskCenter(t);
    DrawSphere(goal, 0.18f, RED);
    DrawCubeWires(goal, 0.6f, 0.6f, 0.6f, RED);
    for (int i = 1; i < taskCount && i < 8; i++)
        DrawCubeWires(taskCenter(&tasks[i]), 0.4f, 0.4f, 0.4f, Fade(RED, 0.6f));
}

static void drawGhost(void) {
    if (buildMode || menuOpen) return;
    if (hotbar[hotbarSlot].kind != SLOT_BLOCK) return;
    if (!hover.hit) return;
    int x = hover.px, y = hover.py, z = hover.pz;
    if (x < 0 || x >= WORLD_SIZE || y < 0 || y >= WORLD_SIZE || z < 0 || z >= WORLD_SIZE) return;
    bool ok = canPlaceAt(x, y, z);
    Vector3 c = { (float)x, (float)y, (float)z };
    DrawCube(c, 0.98f, 0.98f, 0.98f, Fade(blockTint[hotbar[hotbarSlot].id], 0.45f));
    DrawCubeWires(c, 1.0f, 1.0f, 1.0f, ok ? GREEN : RED);
}

static void drawBuildPreview(void) {
    if (!buildMode) return;
    Cell a, b;
    if (buildDragging) { a = buildAnchor; b = buildCur; }
    else if (hover.hit) { a = (Cell){ hover.px, hover.py, hover.pz }; b = a; }
    else return;
    int x1 = a.x < b.x ? a.x : b.x, x2 = a.x > b.x ? a.x : b.x;
    int z1 = a.z < b.z ? a.z : b.z, z2 = a.z > b.z ? a.z : b.z;
    int y1 = a.y, y2 = a.y + buildHeight - 1;
    if (y2 >= WORLD_SIZE) y2 = WORLD_SIZE - 1;
    Vector3 mn = { (float)x1 - 0.5f, (float)y1 - 0.5f, (float)z1 - 0.5f };
    Vector3 mx = { (float)x2 + 0.5f, (float)y2 + 0.5f, (float)z2 + 0.5f };
    Vector3 ctr = Vector3Scale(Vector3Add(mn, mx), 0.5f);
    Vector3 sz = Vector3Subtract(mx, mn);
    DrawCube(ctr, sz.x, sz.y, sz.z, Fade(SKYBLUE, 0.30f));
    DrawCubeWires(ctr, sz.x, sz.y, sz.z, BLUE);
}

static void drawWorld3D(void) {
    BeginMode3D(camera);
    for (int i = 0; i < CHUNK_COUNT; i++) {
        if (!chunkMeshes[i].built) continue;
        if (chunkMeshes[i].mesh.vertexCount == 0) continue;
        DrawMesh(chunkMeshes[i].mesh, blockMaterial, MatrixIdentity());
    }
    drawAvatar();
    if (gameState == STATE_PLAY) {
        drawFaceHighlight();
        for (int i = 0; i < selCount; i++)
            DrawCubeWires((Vector3){ (float)selBlocks[i].x, (float)selBlocks[i].y, (float)selBlocks[i].z }, 1.04f, 1.04f, 1.04f, YELLOW);
        drawPathMarkers();
        drawGhost();
        drawBuildPreview();
    }
    EndMode3D();
}

static void drawQueuePanel(void) {
    queueRowCount = 0;
    queuePanelRect = (Rectangle){ 0, 0, 0, 0 };
    if (taskCount == 0) return;
    int rows = taskCount < 7 ? taskCount : 7;
    float h = 26.0f + (float)rows * 26.0f + (taskCount > 7 ? 16.0f : 0.0f);
    queuePanelRect = (Rectangle){ 10, 64, 300, h };
    DrawRectangleRec(queuePanelRect, Fade(BLACK, 0.55f));
    DrawRectangleLinesEx(queuePanelRect, 1, Fade(RAYWHITE, 0.4f));
    DrawText(TextFormat("Action Queue (%d)  RMB row = cancel", taskCount), 16, 70, 12, RAYWHITE);
    for (int i = 0; i < rows; i++) {
        float y = 64.0f + 26.0f + (float)i * 26.0f;
        Rectangle rr = { 14, y, 292, 24 };
        queueRowRects[queueRowCount] = rr;
        queueRowTask[queueRowCount] = i;
        queueRowCount++;
        DrawRectangleRec(rr, i == 0 ? Fade(DARKGREEN, 0.55f) : Fade(DARKGRAY, 0.45f));
        const Task *t = &tasks[i];
        if (i == 0) {
            DrawText(TextFormat("%s%s", t->label, taskApproach ? " (moving)" : ""), 18, (int)y + 3, 12, WHITE);
            float p = taskProgress(t);
            DrawRectangle(18, (int)y + 17, 200, 4, Fade(BLACK, 0.6f));
            DrawRectangle(18, (int)y + 17, (int)(200.0f * p), 4, GREEN);
            DrawText(TextFormat("%.1fs", taskETA(t)), 250, (int)y + 3, 12, LIME);
        } else {
            DrawText(TextFormat("%d. %s", i, t->label), 18, (int)y + 5, 12, LIGHTGRAY);
        }
    }
    if (taskCount > 7) DrawText(TextFormat("+%d more", taskCount - 7), 18, (int)(64.0f + 26.0f + (float)rows * 26.0f), 12, GRAY);
}

static void drawHotbar(void) {
    int sw = 46, gap = 6;
    int total = 9 * sw + 8 * gap;
    int x0 = (GetScreenWidth() - total) / 2;
    int y0 = GetScreenHeight() - 58;
    for (int i = 0; i < 9; i++) {
        Rectangle r = { (float)(x0 + i * (sw + gap)), (float)y0, (float)sw, (float)sw };
        hotbarRects[i] = r;
        DrawRectangleRec(r, Fade(BLACK, i == hotbarSlot ? 0.75f : 0.5f));
        DrawRectangleLinesEx(r, i == hotbarSlot ? 2.0f : 1.0f, i == hotbarSlot ? GOLD : Fade(RAYWHITE, 0.35f));
        DrawText(TextFormat("%d", i + 1), (int)r.x + 3, (int)r.y + 2, 10, RAYWHITE);
        if (hotbar[i].kind == SLOT_BLOCK) {
            int tile = tileForFace(hotbar[i].id, 0);
            Rectangle src = { (float)(tile % ATLAS_COLS) * TILE_SIZE, (float)(tile / ATLAS_COLS) * TILE_SIZE, (float)TILE_SIZE, (float)TILE_SIZE };
            Rectangle dst = { r.x + 8, r.y + 12, 30, 30 };
            DrawTexturePro(blockAtlas, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        } else {
            float cx = r.x + 23, cy = r.y + 26;
            DrawLineEx((Vector2){ cx - 9, cy + 11 }, (Vector2){ cx + 7, cy - 9 }, 4.0f, BROWN);
            if (hotbar[i].kind == SLOT_PICK) {
                DrawTriangle((Vector2){ cx - 13, cy - 4 }, (Vector2){ cx + 13, cy - 12 }, (Vector2){ cx + 9, cy - 2 }, LIGHTGRAY);
                DrawTriangle((Vector2){ cx - 13, cy - 4 }, (Vector2){ cx - 9, cy - 14 }, (Vector2){ cx + 13, cy - 12 }, GRAY);
            } else {
                DrawTriangle((Vector2){ cx + 2, cy - 14 }, (Vector2){ cx + 14, cy - 6 }, (Vector2){ cx + 4, cy + 2 }, LIGHTGRAY);
            }
        }
    }
    const char *name = hotbar[hotbarSlot].name;
    int tw = MeasureText(name, 14);
    DrawText(name, (GetScreenWidth() - tw) / 2, y0 - 20, 14, WHITE);
}

static void drawContextMenu(void) {
    if (!menuOpen) return;
    Rectangle r = menuRect();
    DrawRectangleRec(r, Fade(BLACK, 0.80f));
    DrawRectangleLinesEx(r, 1, Fade(RAYWHITE, 0.5f));
    Vector2 mouse = GetMousePosition();
    for (int i = 0; i < menuCount; i++) {
        Rectangle ir = { r.x + 4, r.y + 4 + (float)i * 26.0f, r.width - 8, 24 };
        bool hov = CheckCollisionPointRec(mouse, ir);
        if (hov) DrawRectangleRec(ir, Fade(SKYBLUE, 0.35f));
        DrawText(menuItems[i].label, (int)ir.x + 8, (int)ir.y + 5, 13, hov ? WHITE : LIGHTGRAY);
    }
}

static void drawHelp(void) {
    if (!helpVisible) return;
    static const char *lines[] = {
        "WASD  pan camera", "Wheel  zoom height", "MMB drag  rotate / tilt",
        "F  focus avatar", "LMB  select + context menu", "LMB drag  box select",
        "RMB ground  walk here", "RMB double  sprint", "RMB wall + pickaxe  mine",
        "RMB trunk + axe  chop tree", "1-9  tools / blocks", "RMB + block  place / paint",
        "C  build mode (LMB drag fill)", "Shift+Wheel  fill height", "Del  mine selection",
        "Esc  cancel / pause", "H  toggle this help"
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    float w = 280, h = (float)n * 17.0f + 30.0f;
    Rectangle r = { (float)GetScreenWidth() - w - 10, 40, w, h };
    DrawRectangleRec(r, Fade(BLACK, 0.55f));
    DrawRectangleLinesEx(r, 1, Fade(RAYWHITE, 0.35f));
    DrawText("Controls", (int)r.x + 10, (int)r.y + 6, 14, GOLD);
    for (int i = 0; i < n; i++) DrawText(lines[i], (int)r.x + 10, (int)r.y + 26 + i * 17, 12, RAYWHITE);
}

static void drawHUD(void) {
    DrawText("game666 0.0.2 preview", 10, 10, 20, DARKGRAY);
    DrawText(TextFormat("Mode: %s   Slot: %s   Selected: %d%s", buildMode ? "BUILD" : "RTS",
             hotbar[hotbarSlot].name, selCount, selAvatar ? " + avatar" : ""), 10, 36, 14, DARKGRAY);
    if (buildMode) DrawText(TextFormat("Fill height: %d (Shift+Wheel)", buildHeight), 10, 52, 12, DARKBLUE);
    drawQueuePanel();
    drawHelp();
    drawHotbar();
    drawContextMenu();
    if (boxSelecting) {
        float x = fminf(pressPos.x, boxCur.x), y = fminf(pressPos.y, boxCur.y);
        float w = fabsf(boxCur.x - pressPos.x), h = fabsf(boxCur.y - pressPos.y);
        DrawRectangle((int)x, (int)y, (int)w, (int)h, Fade(SKYBLUE, 0.18f));
        DrawRectangleLines((int)x, (int)y, (int)w, (int)h, BLUE);
    }
    if (GetTime() < toastUntil) {
        int tw = MeasureText(toastBuf, 14);
        DrawRectangle(8, GetScreenHeight() - 92, tw + 16, 24, Fade(BLACK, 0.6f));
        DrawText(toastBuf, 16, GetScreenHeight() - 87, 14, GOLD);
    }
    DrawFPS(GetScreenWidth() - 90, 10);
}

static bool drawButton(Rectangle r, const char *label) {
    Vector2 m = GetMousePosition();
    bool hov = CheckCollisionPointRec(m, r);
    DrawRectangleRec(r, Fade(hov ? GRAY : DARKGRAY, 0.9f));
    DrawRectangleLinesEx(r, 2, hov ? GOLD : RAYWHITE);
    int tw = MeasureText(label, 20);
    DrawText(label, (int)(r.x + (r.width - (float)tw) * 0.5f), (int)r.y + 12, 20, RAYWHITE);
    return hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

static void newWorld(void) {
    worldSeed = GetRandomValue(1, 2000000000);
    generateWorld();
    rebuildAllChunks();
    clearQueue();
    selCount = 0;
    selAvatar = false;
    menuOpen = false;
    buildMode = false;
    buildDragging = false;
    respawnAvatar();
    camFocus = avatar.position;
    toast("New world generated");
}

static void drawMainMenu(void) {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.35f));
    const char *title = "GAME666";
    int tw = MeasureText(title, 64);
    DrawText(title, (GetScreenWidth() - tw) / 2, 110, 64, RAYWHITE);
    const char *sub = "RTS voxel prototype - inspired by TimeCraft";
    int sw = MeasureText(sub, 16);
    DrawText(sub, (GetScreenWidth() - sw) / 2, 185, 16, LIGHTGRAY);
    float bw = 280, bh = 46;
    float bx = (float)GetScreenWidth() * 0.5f - bw * 0.5f;
    float by = (float)GetScreenHeight() * 0.45f;
    if (drawButton((Rectangle){ bx, by, bw, bh }, "Play")) {
        gameState = STATE_PLAY;
        camFocus = avatar.position;
        camDist = 24.0f;
        camDistTarget = 24.0f;
    }
    if (drawButton((Rectangle){ bx, by + 58, bw, bh }, "New World")) newWorld();
    if (drawButton((Rectangle){ bx, by + 116, bw, bh }, "Quit")) quitRequested = true;
    DrawText("game666 0.0.2 preview - github.com/MelvinMod/Game666", 10, GetScreenHeight() - 24, 14, LIGHTGRAY);
}

static void drawPauseMenu(void) {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.45f));
    const char *title = "Paused";
    int tw = MeasureText(title, 40);
    DrawText(title, (GetScreenWidth() - tw) / 2, 120, 40, RAYWHITE);
    float bw = 280, bh = 46;
    float bx = (float)GetScreenWidth() * 0.5f - bw * 0.5f;
    float by = (float)GetScreenHeight() * 0.35f;
    if (drawButton((Rectangle){ bx, by, bw, bh }, "Resume") || IsKeyPressed(KEY_ESCAPE)) gameState = STATE_PLAY;
    if (drawButton((Rectangle){ bx, by + 58, bw, bh }, "New World")) { newWorld(); gameState = STATE_PLAY; }
    if (drawButton((Rectangle){ bx, by + 116, bw, bh }, "Main Menu")) gameState = STATE_MENU;
    if (drawButton((Rectangle){ bx, by + 174, bw, bh }, "Quit")) quitRequested = true;
}

static void updateMenuCamera(void) {
    float t = (float)GetTime() * 0.12f;
    camera.position = (Vector3){ 32.0f + cosf(t) * 40.0f, 36.0f, 32.0f + sinf(t) * 40.0f };
    camera.target = (Vector3){ 32.0f, 14.0f, 32.0f };
}

static void initGame(void) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "game666");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    generateWorld();
    generateAtlas();
    blockMaterial = LoadMaterialDefault();
    blockMaterial.maps[MATERIAL_MAP_DIFFUSE].texture = blockAtlas;
    rebuildAllChunks();
    respawnAvatar();
    camera.up = (Vector3){ 0, 1, 0 };
    camera.fovy = 55.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    camFocus = avatar.position;
    gameState = STATE_MENU;
}

static void drawFrame(void) {
    BeginDrawing();
    ClearBackground((Color){ 135, 206, 235, 255 });
    drawWorld3D();
    if (gameState == STATE_PLAY) drawHUD();
    else if (gameState == STATE_MENU) drawMainMenu();
    else drawPauseMenu();
    EndDrawing();
}

int main(void) {
    initGame();
    while (!WindowShouldClose() && !quitRequested) {
        float dt = GetFrameTime();
        if (dt > 0.05f) dt = 0.05f;
        if (gameState == STATE_MENU) {
            updateMenuCamera();
        } else if (gameState == STATE_PLAY) {
            handlePlayInput(dt);
            if (gameState == STATE_PLAY) {
                updateTasks(dt);
                updateVertical(dt);
                updateAnim(dt);
            }
        }
        drawFrame();
    }
    for (int i = 0; i < CHUNK_COUNT; i++)
        if (chunkMeshes[i].built && chunkMeshes[i].mesh.vertices) UnloadMesh(chunkMeshes[i].mesh);
    UnloadTexture(blockAtlas);
    CloseWindow();
    return 0;
}
