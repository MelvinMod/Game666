#include "raylib.h"
#include "raymath.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#define WORLD_SIZE 64
#define BLOCK_AIR 0
#define BLOCK_GRASS 1
#define BLOCK_DIRT 2
#define BLOCK_STONE 3
#define BLOCK_WOOD 4
#define BLOCK_LEAVES 5

static float hash2(int x, int y) {
    int n = x * 374761393 + y * 668265263;
    n = (n ^ (n >> 13)) * 1274126177;
    return (float)(n & 0x7fffffff) / (float)0x7fffffff;
}

static float smoothNoise(float x, float y) {
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    float fx = x - ix;
    float fy = y - iy;
    float v00 = hash2(ix, iy);
    float v10 = hash2(ix + 1, iy);
    float v01 = hash2(ix, iy + 1);
    float v11 = hash2(ix + 1, iy + 1);
    float u = fx * fx * (3 - 2 * fx);
    float v = fy * fy * (3 - 2 * fy);
    return Lerp(Lerp(v00, v10, u), Lerp(v01, v11, u), v);
}

static float terrainHeight(int x, int z) {
    float n = 0.0f;
    float amp = 1.0f;
    float freq = 0.05f;
    for (int i = 0; i < 4; i++) {
        n += smoothNoise(x * freq, z * freq) * amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return n * 20.0f + 10.0f;
}

static unsigned char world[WORLD_SIZE][WORLD_SIZE][WORLD_SIZE];

static void generateWorld(void) {
    memset(world, BLOCK_AIR, sizeof(world));
    for (int x = 0; x < WORLD_SIZE; x++) {
        for (int z = 0; z < WORLD_SIZE; z++) {
            int h = (int)terrainHeight(x, z);
            for (int y = 0; y < h; y++) {
                if (y == h - 1) world[x][y][z] = BLOCK_GRASS;
                else if (y > h - 4) world[x][y][z] = BLOCK_DIRT;
                else world[x][y][z] = BLOCK_STONE;
            }
            if ((x % 7 == 0) && (z % 11 == 0) && h > 5) {
                for (int ty = h; ty < h + 4 && ty < WORLD_SIZE; ty++)
                    world[x][ty][z] = BLOCK_WOOD;
                for (int lx = x - 2; lx <= x + 2; lx++)
                    for (int lz = z - 2; lz <= z + 2; lz++)
                        for (int ly = h + 3; ly <= h + 5 && ly < WORLD_SIZE; ly++)
                            if (lx >= 0 && lx < WORLD_SIZE && lz >= 0 && lz < WORLD_SIZE)
                                if (world[lx][ly][lz] == BLOCK_AIR)
                                    world[lx][ly][lz] = BLOCK_LEAVES;
            }
        }
    }
}

static inline unsigned char getBlock(int x, int y, int z) {
    if (x < 0 || x >= WORLD_SIZE || y < 0 || y >= WORLD_SIZE || z < 0 || z >= WORLD_SIZE)
        return BLOCK_AIR;
    return world[x][y][z];
}

static inline void setBlock(int x, int y, int z, unsigned char b) {
    if (x >= 0 && x < WORLD_SIZE && y >= 0 && y < WORLD_SIZE && z >= 0 && z < WORLD_SIZE)
        world[x][y][z] = b;
}

static void drawBlock(int x, int y, int z, unsigned char type) {
    if (type == BLOCK_AIR) return;
    Color col;
    switch (type) {
        case BLOCK_GRASS:  col = (Color){95, 159, 53, 255}; break;
        case BLOCK_DIRT:   col = (Color){134, 96, 67, 255}; break;
        case BLOCK_STONE:  col = (Color){128, 128, 128, 255}; break;
        case BLOCK_WOOD:   col = (Color){102, 67, 32, 255}; break;
        case BLOCK_LEAVES: col = (Color){45, 110, 45, 255}; break;
        default:           col = WHITE; break;
    }
    Vector3 pos = { (float)x, (float)y, (float)z };
    if (getBlock(x, y + 1, z) == BLOCK_AIR) DrawCubeV((Vector3){pos.x, pos.y + 0.5f, pos.z}, (Vector3){1,1,1}, col);
    if (getBlock(x, y - 1, z) == BLOCK_AIR) DrawCubeV((Vector3){pos.x, pos.y - 0.5f, pos.z}, (Vector3){1,1,1}, col);
    if (getBlock(x + 1, y, z) == BLOCK_AIR) DrawCubeV((Vector3){pos.x + 0.5f, pos.y, pos.z}, (Vector3){1,1,1}, col);
    if (getBlock(x - 1, y, z) == BLOCK_AIR) DrawCubeV((Vector3){pos.x - 0.5f, pos.y, pos.z}, (Vector3){1,1,1}, col);
    if (getBlock(x, y, z + 1) == BLOCK_AIR) DrawCubeV((Vector3){pos.x, pos.y, pos.z + 0.5f}, (Vector3){1,1,1}, col);
    if (getBlock(x, y, z - 1) == BLOCK_AIR) DrawCubeV((Vector3){pos.x, pos.y, pos.z - 0.5f}, (Vector3){1,1,1}, col);
}

static Ray getMouseRaySimple(Vector2 mouse, Camera3D cam) {
    Ray ray = { 0 };
    ray.position = cam.position;

    Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 right   = Vector3Normalize(Vector3CrossProduct(forward, cam.up));
    Vector3 up      = Vector3CrossProduct(right, forward);

    float aspect     = (float)GetScreenWidth() / (float)GetScreenHeight();
    float fovRad     = cam.fovy * DEG2RAD;
    float halfHeight = tanf(fovRad * 0.5f);
    float halfWidth  = halfHeight * aspect;

    float nx = (mouse.x / (float)GetScreenWidth()) * 2.0f - 1.0f;
    float ny = -((mouse.y / (float)GetScreenHeight()) * 2.0f - 1.0f);

    Vector3 dir = Vector3Add(forward,
                    Vector3Add(Vector3Scale(right, nx * halfWidth),
                               Vector3Scale(up,    ny * halfHeight)));
    ray.direction = Vector3Normalize(dir);
    return ray;
}

typedef struct {
    Vector3 position;
    Vector3 target;
    bool moving;
    float speed;
} Avatar;

static Avatar avatar = { {32, 20, 32}, {32, 20, 32}, false, 4.0f };
static Camera3D camera;

#define MAX_ACTIONS 8
static int actionQueue[MAX_ACTIONS];
static int actionCount = 0;

static void enqueueAction(int action) {
    if (actionCount < MAX_ACTIONS)
        actionQueue[actionCount++] = action;
}

static void initGame(void) {
    InitWindow(1280, 720, "game666");
    EnableCursor();
    SetTargetFPS(60);
    generateWorld();
    camera.position = (Vector3){ 32.0f, 35.0f, 42.0f };
    camera.target   = avatar.position;
    camera.up       = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy     = 70.0f;
    camera.projection = CAMERA_PERSPECTIVE;
}

static void updateGame(void) {
    float speed = 10.0f * GetFrameTime();
    if (IsKeyDown(KEY_W)) camera.position.z -= speed;
    if (IsKeyDown(KEY_S)) camera.position.z += speed;
    if (IsKeyDown(KEY_A)) camera.position.x -= speed;
    if (IsKeyDown(KEY_D)) camera.position.x += speed;
    if (IsKeyDown(KEY_SPACE)) camera.position.y += speed;
    if (IsKeyDown(KEY_LEFT_SHIFT)) camera.position.y -= speed;

    Ray ray = getMouseRaySimple(GetMousePosition(), camera);

    if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
        for (int i = 0; i < 200; i++) {
            Vector3 p = Vector3Add(ray.position, Vector3Scale(ray.direction, (float)i * 0.25f));
            int bx = (int)floorf(p.x);
            int by = (int)floorf(p.y);
            int bz = (int)floorf(p.z);
            if (getBlock(bx, by, bz) != BLOCK_AIR) {
                avatar.target = (Vector3){ (float)bx + 0.5f, (float)by + 1.0f, (float)bz + 0.5f };
                avatar.moving = true;
                enqueueAction(1);
                break;
            }
        }
    }

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        for (int i = 0; i < 200; i++) {
            Vector3 p = Vector3Add(ray.position, Vector3Scale(ray.direction, (float)i * 0.25f));
            int bx = (int)floorf(p.x);
            int by = (int)floorf(p.y);
            int bz = (int)floorf(p.z);
            if (getBlock(bx, by, bz) != BLOCK_AIR) {
                avatar.target = (Vector3){ (float)bx + 0.5f, (float)by + 0.5f, (float)bz + 0.5f };
                avatar.moving = true;
                enqueueAction(2);
                break;
            }
        }
    }

    if (avatar.moving) {
        Vector3 dir = Vector3Subtract(avatar.target, avatar.position);
        float dist = Vector3Length(dir);
        if (dist > 0.1f) {
            dir = Vector3Normalize(dir);
            avatar.position = Vector3Add(avatar.position, Vector3Scale(dir, avatar.speed * GetFrameTime()));
        } else {
            avatar.moving = false;
            if (actionCount > 0) {
                int act = actionQueue[0];
                for (int i = 1; i < actionCount; i++) actionQueue[i-1] = actionQueue[i];
                actionCount--;
                if (act == 2) {
                    int bx = (int)floorf(avatar.target.x);
                    int by = (int)floorf(avatar.target.y);
                    int bz = (int)floorf(avatar.target.z);
                    if (getBlock(bx, by, bz) != BLOCK_AIR)
                        setBlock(bx, by, bz, BLOCK_AIR);
                }
            }
        }
    }

    Vector3 desired = Vector3Add(avatar.position, (Vector3){0, 15, 10});
    camera.position = Vector3Lerp(camera.position, desired, 0.05f);
    camera.target   = avatar.position;
}

static void drawGame(void) {
    BeginDrawing();
    ClearBackground((Color){135, 206, 235, 255});
    BeginMode3D(camera);
    for (int x = 0; x < WORLD_SIZE; x++)
        for (int y = 0; y < WORLD_SIZE; y++)
            for (int z = 0; z < WORLD_SIZE; z++)
                if (world[x][y][z] != BLOCK_AIR)
                    drawBlock(x, y, z, world[x][y][z]);
    DrawCapsule(avatar.position, Vector3Add(avatar.position, (Vector3){0, 1.8f, 0}), 0.4f, 8, 8, RED);
    if (avatar.moving)
        DrawSphere(avatar.target, 0.3f, YELLOW);
    EndMode3D();
    DrawText("game666", 10, 10, 20, DARKGRAY);
    DrawText("RTS mode – WASD pan, Space/Shift up/down, RMB move avatar, LMB mine", 10, 40, 16, DARKGRAY);
    DrawText(TextFormat("Actions in queue: %d", actionCount), 10, 70, 16, DARKGRAY);
    EndDrawing();
}

int main(void) {
    initGame();
    while (!WindowShouldClose()) {
        updateGame();
        drawGame();
    }
    CloseWindow();
    return 0;
}
