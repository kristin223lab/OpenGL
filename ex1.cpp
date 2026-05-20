/*  
  內容說明：
    1.使用 OpenGL + GLUI 建立 3D 物件操作介面
      支援 Teapot / ColorCube / Model 三種物件切換顯示
      並可從外部 .tri 檔讀取模型資料（頂點、法向量、三角面）
      載入後自動置中與等比例縮放，建立 display list 進行渲染
    2.物件Translation、Rotation、Scaling控制 並支援 Uniform Scaling
    3.支援 Orthographic / Perspective 兩種投影視角切換，並可即時更新畫面
    4.三盞光源 Light0 / Light1 / Light2，可各自獨立開關，並可調整 Diffuse / Specular 顏色參數
    5.每盞光源支援 Directional Light 與 Spot Light 兩種模式切換
      Spot Light 可透過 GLUI Rotation 旋轉控制照射方向，且光源位置固定於世界座標
    6.材質系統（Gold / Pewter / Silver / Copper / Chrome） 並可調整 Shininess 參數，同時支援 Emission 發光效果
    7.Teapot / ColorCube / Model 皆能正確受到光照影響
    8.右鍵選單統一提供 Object Size 與 Object Type（Wire / Solid / Smooth）設定，且所有物件皆可使用右鍵選單快速調整
*/
#include <windows.h>
#include <commdlg.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <cctype>
#include <direct.h>
#include <algorithm>
#include <cstring>

#include "Include/FreeGLUT/freeglut.h"
#include "Include/GL/glui.h"

#define timer_interval 16
#define WORLD_ 1
#define LOCAL_ 2

static char gModelPath[MAX_PATH] = "";
static int  lastObjType = 0;   // 用來取消選檔時回復上一個物件
static GLUI_StaticText* modelPathText = nullptr;

static inline float clampf(float v, float a, float b) { return (v < a) ? a : (v > b) ? b : v; }

static inline void normalize3(float v[3]) {
    float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len < 1e-8f) { v[0] = 0; v[1] = 0; v[2] = -1; return; }
    v[0] /= len; v[1] /= len; v[2] /= len;
}

static inline void mulMat3Vec3(const float m[16], const float in[3], float out[3]) {
    out[0] = m[0]*in[0] + m[4]*in[1] + m[8]*in[2];
    out[1] = m[1]*in[0] + m[5]*in[1] + m[9]*in[2];
    out[2] = m[2]*in[0] + m[6]*in[1] + m[10]*in[2];
}

static inline void getSpotDirFromMatrixAndPos(const float m[16], float lx, float ly, float lz, float dir[3]) {
    float base[3] = { -lx, -ly, -lz };
    normalize3(base);
    mulMat3Vec3(m, base, dir);
    normalize3(dir);
}

static void printCWD() {
    char buf[4096];
    if (_getcwd(buf, sizeof(buf))) std::printf("[CWD] %s\n", buf);
    else std::printf("[CWD] _getcwd failed\n");
}

static bool fileExists(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

struct Vec3 { float x, y, z; };
struct TriFace { int v[3]; };

static std::vector<Vec3> gV;
static std::vector<Vec3> gN;
static std::vector<TriFace> gF;
static std::vector<Vec3> gC;        // vertex color (if exist)
static std::vector<Vec3> gFaceC;    // face color (fallback)

static bool gHasVertexColor = false;
static bool gHasFaceColor   = false;

static GLuint gModelList = 0;
static float  gModelScale = 1.0f;
static Vec3   gModelCenter = {0,0,0};

extern int window;

static bool OpenTriFileDialog(char outPath[MAX_PATH]) {
    outPath[0] = '\0';

    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));

    char fileName[MAX_PATH] = "";

    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = nullptr;
    ofn.lpstrFile    = fileName;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrFilter  = "TRI Models (*.tri)\0*.tri\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        std::strncpy(outPath, fileName, MAX_PATH - 1);
        outPath[MAX_PATH - 1] = '\0';
        return true;
    }
    return false; // 使用者按取消
}

static inline void norm3(Vec3& v) {
    float len = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    if (len < 1e-8f) { v = {0,0,1}; return; }
    v.x /= len; v.y /= len; v.z /= len;
}

static std::vector<double> extractNumbers(const std::string& s) {
    std::vector<double> out;
    const char* p = s.c_str();
    char* endp = nullptr;
    while (*p) {
        if ((*p >= '0' && *p <= '9') || *p=='-' || *p=='+' || *p=='.') {
            double v = std::strtod(p, &endp);
            if (endp != p) { out.push_back(v); p = endp; continue; }
        }
        ++p;
    }
    return out;
}

static void computeCenterAndScale() {
    if (gV.empty()) { gModelCenter = {0,0,0}; gModelScale = 1.0f; return; }

    float minx = gV[0].x, maxx = gV[0].x;
    float miny = gV[0].y, maxy = gV[0].y;
    float minz = gV[0].z, maxz = gV[0].z;

    for (const auto& v : gV) {
        minx = std::min(minx, v.x); maxx = std::max(maxx, v.x);
        miny = std::min(miny, v.y); maxy = std::max(maxy, v.y);
        minz = std::min(minz, v.z); maxz = std::max(maxz, v.z);
    }

    gModelCenter.x = (minx + maxx) * 0.5f;
    gModelCenter.y = (miny + maxy) * 0.5f;
    gModelCenter.z = (minz + maxz) * 0.5f;

    float dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
    float m = std::max(dx, std::max(dy, dz));

    const float TARGET = 2.0f;
    gModelScale = (m > 1e-6f) ? (TARGET / m) : 1.0f;
}

static bool tryReadHeaderSmart(std::ifstream& fin, int& nTri, int& mVert) {
    nTri = 0; mVert = 0;

    auto toUpper = [](std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    };

    auto isMostlyNumeric = [](const std::string& s)->bool{
        bool hasDigit = false;
        for (unsigned char c : s) {
            if (std::isdigit(c)) { hasDigit = true; continue; }
            if (std::isspace(c)) continue;
            if (c=='+'||c=='-'||c=='.'||c=='e'||c=='E') continue;
            return false;
        }
        return hasDigit;
    };

    fin.clear();
    fin.seekg(0);

    std::string line;
    while (std::getline(fin, line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        std::string u = toUpper(line);

        if (u.find("SIMPLE") != std::string::npos || u.find("COLOR") != std::string::npos) {
            auto nums = extractNumbers(line);
            if (!nums.empty()) { nTri = (int)nums[0]; break; }
        }
        if (u.find("TRIANGLE") != std::string::npos || u.find("FACES") != std::string::npos || u.find("NUMTRI") != std::string::npos) {
            auto nums = extractNumbers(line);
            if (!nums.empty()) { nTri = (int)nums[0]; break; }
        }
        if (isMostlyNumeric(line)) {
            auto nums = extractNumbers(line);

            if (nums.size() >= 2 && nums[0] > 0 && nums[1] > 0) {
                int a = (int)nums[0];
                int b = (int)nums[1];

                if (a >= b) { nTri = a; mVert = b; }
                else        { nTri = b; mVert = a; }

                return true; 
            }

            if (nums.size() == 1 && nums[0] > 0) { nTri = (int)nums[0]; break; }
        }
    }
    if (nTri <= 0) return false;

    while (std::getline(fin, line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        std::string u = toUpper(line);

        if (u.find("VERTEX") != std::string::npos || u.find("VERTICES") != std::string::npos ||
            u.find("POINT")  != std::string::npos || u.find("NUMVERT")   != std::string::npos) {
            auto nums = extractNumbers(line);
            if (!nums.empty()) { mVert = (int)nums[0]; break; }
        }
        if (isMostlyNumeric(line)) {
            auto nums = extractNumbers(line);

            if (nums.size() >= 2 && nums[0] > 0 && nums[1] > 0) {
                int a = (int)nums[0];
                int b = (int)nums[1];

                if (a >= b) { nTri = a; mVert = b; }
                else        { nTri = b; mVert = a; }

                return true;
            }

            if (nums.size() == 1 && nums[0] > 0) { nTri = (int)nums[0]; break; }
        }
    }
    return (mVert > 0);
}

static bool loadTRI_ASCII(const char* path) {
    std::ifstream fin(path);
    if (!fin.is_open()) return false;

    int nTri = 0, mVert = 0;
    if (!tryReadHeaderSmart(fin, nTri, mVert)) {
        std::printf("[TRI] header nTri/mVert missing/<=0\n");
        return false;
    }

    gF.assign(nTri, TriFace{});
    gV.assign(mVert, Vec3{0,0,0});
    gN.assign(mVert, Vec3{0,0,1});
    gC.assign(mVert, Vec3{1,1,1});
    gFaceC.assign(nTri, Vec3{1,1,1});

    gHasVertexColor = false;
    gHasFaceColor   = false;

    bool oneBased = false;
    auto idxOK0 = [&](int id)->bool { return id >= 0 && id < mVert; };
    auto idxOK1 = [&](int id)->bool { return id >= 1 && id <= mVert; };

    std::string line;

    // ---------- read faces ----------
    int readTri = 0;
    while (readTri < nTri && std::getline(fin, line)) {
        auto nums = extractNumbers(line);
        if (nums.size() < 3) continue;

        int tid = -1, v1 = -1, v2 = -1, v3 = -1;
        bool hasFaceColor = false;
        float fr = 1.0f, fg = 1.0f, fb = 1.0f;

        if (nums.size() >= 4) {
            // either: tid v1 v2 v3 [r g b]
            tid = (int)nums[0];
            v1  = (int)nums[1];
            v2  = (int)nums[2];
            v3  = (int)nums[3];

            if (nums.size() >= 7) {
                fr = (float)nums[4];
                fg = (float)nums[5];
                fb = (float)nums[6];
                hasFaceColor = true;
            }
        } else {
            // v1 v2 v3 [r g b]  (rare)
            tid = readTri;
            v1  = (int)nums[0];
            v2  = (int)nums[1];
            v3  = (int)nums[2];

            if (nums.size() >= 6) {
                fr = (float)nums[3];
                fg = (float)nums[4];
                fb = (float)nums[5];
                hasFaceColor = true;
            }
        }

        bool ok0 = idxOK0(v1) && idxOK0(v2) && idxOK0(v3);
        bool ok1 = idxOK1(v1) && idxOK1(v2) && idxOK1(v3);
        if (!ok0 && ok1) oneBased = true;
        else if (!ok0 && !ok1) continue;

        if (oneBased) { v1--; v2--; v3--; }
        if (tid < 0 || tid >= nTri) tid = readTri;

        gF[tid].v[0] = v1;
        gF[tid].v[1] = v2;
        gF[tid].v[2] = v3;

        if (hasFaceColor) {
            if (fr > 1.5f || fg > 1.5f || fb > 1.5f) { fr/=255.0f; fg/=255.0f; fb/=255.0f; }
            fr = clampf(fr, 0.0f, 1.0f);
            fg = clampf(fg, 0.0f, 1.0f);
            fb = clampf(fb, 0.0f, 1.0f);
            gFaceC[tid] = { fr, fg, fb };
            gHasFaceColor = true;
        }

        readTri++;
    }
    if (readTri != nTri) {
        std::printf("[TRI] face readTri=%d != nTri=%d\n", readTri, nTri);
        return false;
    }

    // ---------- read vertices ----------
    int readV = 0;
    while (readV < mVert && std::getline(fin, line)) {
        auto nums = extractNumbers(line);
        if (nums.size() < 6) continue;

        int vid = -1;
        float x, y, z, nx, ny, nz;
        bool hasVertexColor = false;
        float r = 1.0f, gg = 1.0f, bb = 1.0f;

        if (nums.size() >= 7) {
            // id x y z nx ny nz [r g b]
            vid = (int)nums[0];
            x  = (float)nums[1]; y  = (float)nums[2]; z  = (float)nums[3];
            nx = (float)nums[4]; ny = (float)nums[5]; nz = (float)nums[6];

            if (nums.size() >= 10) {
                r  = (float)nums[7];
                gg = (float)nums[8];
                bb = (float)nums[9];
                hasVertexColor = true;
            }
        } else {
            // x y z nx ny nz [r g b]
            vid = readV;
            x  = (float)nums[0]; y  = (float)nums[1]; z  = (float)nums[2];
            nx = (float)nums[3]; ny = (float)nums[4]; nz = (float)nums[5];

            if (nums.size() >= 9) {
                r  = (float)nums[6];
                gg = (float)nums[7];
                bb = (float)nums[8];
                hasVertexColor = true;
            }
        }

        if (oneBased) {
            if (vid >= 1 && vid <= mVert) vid--;
            else if (vid != readV) continue;
        } else {
            if (vid < 0 || vid >= mVert) {
                if (vid != readV) continue;
                vid = readV;
            }
        }

        gV[vid] = {x,y,z};
        gN[vid] = {nx,ny,nz};
        norm3(gN[vid]);

        if (hasVertexColor) {
            if (r > 1.5f || gg > 1.5f || bb > 1.5f) { r/=255.0f; gg/=255.0f; bb/=255.0f; }
            r  = clampf(r,  0.0f, 1.0f);
            gg = clampf(gg, 0.0f, 1.0f);
            bb = clampf(bb, 0.0f, 1.0f);
            gC[vid] = {r,gg,bb};
            gHasVertexColor = true;
        } else {
            gC[vid] = {1,1,1};
        }

        readV++;
    }
    if (readV != mVert) {
        std::printf("[TRI] vertex readV=%d != mVert=%d\n", readV, mVert);
        return false;
    }

    computeCenterAndScale();

    std::printf("[TRI] parsed OK. nTri=%d mVert=%d oneBased=%s vColor=%s fColor=%s\n",
        nTri, mVert, oneBased ? "YES" : "NO",
        gHasVertexColor ? "YES" : "NO",
        gHasFaceColor ? "YES" : "NO");

    return true;
}

static void rebuildModelListFromTRI() {
    if (gModelList != 0) {
        glDeleteLists(gModelList, 1);
        gModelList = 0;
    }

    if (glutGetWindow() != window) glutSetWindow(window);

    gModelList = glGenLists(1);
    std::printf("[TRI] glGenLists -> %u\n", (unsigned)gModelList);
    if (gModelList == 0) {
        std::printf("[TRI] ERROR: glGenLists failed (no valid GL context?)\n");
        return;
    }

    glNewList(gModelList, GL_COMPILE);

    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);

    glBegin(GL_TRIANGLES);
    for (int fi = 0; fi < (int)gF.size(); ++fi) {
        const TriFace& f = gF[fi];

        Vec3 fc = {1,1,1};
        if (fi >= 0 && fi < (int)gFaceC.size()) fc = gFaceC[fi];

        for (int k = 0; k < 3; k++) {
            int id = f.v[k];
            if (id < 0 || id >= (int)gV.size()) continue;

            const Vec3& n = gN[id];
            const Vec3& v = gV[id];

            // ★優先 vertex color，沒有才用 face color，最後白色
            if (gHasVertexColor && id >= 0 && id < (int)gC.size()) {
                const Vec3& vc = gC[id];
                glColor3f(vc.x, vc.y, vc.z);
            } else if (gHasFaceColor) {
                glColor3f(fc.x, fc.y, fc.z);
            } else {
                glColor3f(1,1,1);
            }

            glNormal3f(n.x, n.y, n.z);
            glVertex3f(v.x - gModelCenter.x, v.y - gModelCenter.y, v.z - gModelCenter.z);
        }
    }
    glEnd();

    glEndList();

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) std::printf("[TRI] rebuild list GL error = 0x%X\n", err);
}

static bool loadTRI(const char* path) {
    printCWD();
    std::printf("[TRI] try open: %s\n", path);
    std::printf("[TRI] exists? %s\n", fileExists(path) ? "YES" : "NO");

    if (!fileExists(path)) return false;

    if (loadTRI_ASCII(path)) {
        if (glutGetWindow() != window) glutSetWindow(window);

        rebuildModelListFromTRI();
        std::printf("[TRI] loaded OK. tri=%d, v=%d list=%u scale=%.4f\n",
            (int)gF.size(), (int)gV.size(), (unsigned)gModelList, gModelScale);
        return (gModelList != 0);
    }

    std::printf("[TRI] load FAILED (unknown format): %s\n", path);
    return false;
}

typedef struct {
    GLfloat amb[4];
    GLfloat dif[4];
    GLfloat spec[4];
    GLfloat shin;
} Material;

Material MATS[5] = {
    { {0.24725f,0.19950f,0.07450f,1.0f}, {0.75164f,0.60648f,0.22648f,1.0f}, {0.628281f,0.555802f,0.366065f,1.0f}, 51.2f },
    { {0.10588f,0.058824f,0.113725f,1.0f}, {0.427451f,0.470588f,0.541176f,1.0f}, {0.3333f,0.3333f,0.521569f,1.0f}, 9.84615f },
    { {0.19225f,0.19225f,0.19225f,1.0f}, {0.50754f,0.50754f,0.50754f,1.0f}, {0.508273f,0.508273f,0.508273f,1.0f}, 51.2f },
    { {0.19125f,0.07350f,0.02250f,1.0f}, {0.70380f,0.27048f,0.08280f,1.0f}, {0.256777f,0.137622f,0.086014f,1.0f}, 12.8f },
    { {0.25f,0.25f,0.25f,1.0f}, {0.40f,0.40f,0.40f,1.0f}, {0.774597f,0.774597f,0.774597f,1.0f}, 76.8f }
};

int window = 0;
GLUI* glui = nullptr;

GLUI_Rotation* rotation_UI = nullptr;
GLUI_Translation* scaling_X = nullptr;
GLUI_Translation* scaling_Y = nullptr;
GLUI_Translation* scaling_Z = nullptr;

GLUI_Rollout* Light0_rollout0 = nullptr;
GLUI_Rollout* Light1_rollout0 = nullptr;
GLUI_Rollout* Light2_rollout0 = nullptr;

GLUI_Rotation* spot0_rotation = nullptr;
GLUI_Rotation* spot1_rotation = nullptr;
GLUI_Rotation* spot2_rotation = nullptr;

GLUI_Button* Reset_spot0 = nullptr;
GLUI_Button* Reset_spot1 = nullptr;
GLUI_Button* Reset_spot2 = nullptr;

GLUI_Spinner* shininess_spinner = nullptr;
GLUI_RadioGroup* mate_types = nullptr;

GLUI_Checkbox* cb_material = nullptr;
GLUI_Checkbox* cb_emission = nullptr;

GLint width = 1000, height = 900;
GLfloat aspect = 1.0f;

enum { Red = 0, Green, Blue, None = 9 };
int myColor = Red;

int WhenClick_X = 0;
GLclampf background = 0.5f;
GLfloat bef_background = 0.0f;

int if_uniform = 0;

int if_Light0 = 1, if_Light1 = 1, if_Light2 = 0;
int lightType0 = 0, lightType1 = 0, lightType2 = 0;

int if_animate = 1;
int if_material = 1;
int if_emission = 1;

int shadingType = 2;
int objType = 0;
int materialID = 0;

int if_adjust_shininess = 1;
GLfloat shininess = 0.0f;

GLfloat tran_X = 0, tran_Y = 0, tran_Z = 0;
GLfloat scale_C_X = 1.0f, scale_C_Y = 1.0f, scale_C_Z = 1.0f;
GLfloat scale_X = 1.0f, scale_Y = 1.0f, scale_Z = 1.0f;
GLfloat af_X = 1.0f, af_Y = 1.0f, af_Z = 1.0f;
float uniBaseX = 1.0f, uniBaseY = 1.0f, uniBaseZ = 1.0f;

GLfloat teapot_posX = 0, teapot_posY = 0, teapot_posZ = 0;
GLfloat rotateAngle = 0.0f;
GLfloat rotateSpeed = 1.0f;

GLfloat teapot_size = 1.0f;
GLfloat cube_size = 1.0f;
GLfloat model_size = 1.0f;
int cube_wire = 0;

int viewType = 1;

GLclampf diff_R0 = 0.25f, diff_G0 = 0.02f, diff_B0 = 0.02f;
GLclampf spec_R0 = 0.20f, spec_G0 = 0.02f, spec_B0 = 0.02f;

GLclampf diff_R1 = 0.02f, diff_G1 = 0.25f, diff_B1 = 0.02f;
GLclampf spec_R1 = 0.02f, spec_G1 = 0.20f, spec_B1 = 0.02f;

GLclampf diff_R2 = 0.02f, diff_G2 = 0.02f, diff_B2 = 0.20f;
GLclampf spec_R2 = 0.02f, spec_G2 = 0.02f, spec_B2 = 0.18f;

bool needRedisplay = true;
int  lastTickMs = 0;

float rotation_matrix[16] = {
    1.0f,0.0f,0.0f,0.0f,
    0.0f,1.0f,0.0f,0.0f,
    0.0f,0.0f,1.0f,0.0f,
    0.0f,0.0f,0.0f,1.0f
};

float rotation_matrix_reset[16] = {
    1.0f,0.0f,0.0f,0.0f,
    0.0f,1.0f,0.0f,0.0f,
    0.0f,0.0f,1.0f,0.0f,
    0.0f,0.0f,0.0f,1.0f
};

float spotlight_matrix0[16] = {
    1.0f,0.0f,0.0f,0.0f,
    0.0f,1.0f,0.0f,0.0f,
    0.0f,0.0f,1.0f,0.0f,
    0.0f,0.0f,0.0f,1.0f
};
float spotlight_matrix1[16] = {
    1.0f,0.0f,0.0f,0.0f,
    0.0f,1.0f,0.0f,0.0f,
    0.0f,0.0f,1.0f,0.0f,
    0.0f,0.0f,0.0f,1.0f
};
float spotlight_matrix2[16] = {
    1.0f,0.0f,0.0f,0.0f,
    0.0f,1.0f,0.0f,0.0f,
    0.0f,0.0f,1.0f,0.0f,
    0.0f,0.0f,0.0f,1.0f
};

enum {
    CB_RESET_TRANS = 0,
    CB_RESET_SCALE = 1,
    CB_RESET_ROT   = 3,
    CB_UNIFORM     = 4,

    CB_L0_ENABLE   = 5,
    CB_L0_RESETROT = 6,

    CB_L1_ENABLE   = 7,
    CB_L1_RESETROT = 8,

    CB_L2_ENABLE   = 9,
    CB_L2_RESETROT = 10,

    CB_ANIMATE     = 11,
    CB_MATERIAL    = 12,
    CB_VIEW        = 13,

    CB_ADJUST_SHIN = 20,
    CB_OBJECT      = 21,

    CB_L0_TYPE     = 50,
    CB_L1_TYPE     = 70,
    CB_L2_TYPE     = 90,

    CB_EMISSION = 201,
};

static inline void applyLightCommon(GLenum lightId, GLfloat dr,GLfloat dg,GLfloat db, GLfloat sr,GLfloat sg,GLfloat sb) {
    GLfloat amb[] = { 0.0f,0.0f,0.0f,1.0f };
    GLfloat dif[] = { dr,dg,db,1.0f };
    GLfloat spc[] = { sr,sg,sb,1.0f };
    glLightfv(lightId, GL_AMBIENT,  amb);
    glLightfv(lightId, GL_DIFFUSE,  dif);
    glLightfv(lightId, GL_SPECULAR, spc);
}

void Light0(int state) {
    if (!state) { glDisable(GL_LIGHT0); return; }
    applyLightCommon(GL_LIGHT0, diff_R0,diff_G0,diff_B0, spec_R0,spec_G0,spec_B0);

    const float lx = 3.0f, ly = 0.0f, lz = 0.0f;
    if (lightType0 == 0) {
        GLfloat pos[] = { lx, ly, lz, 0.0f };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 180.0f);
    } else {
        GLfloat pos[] = { lx, ly, lz, 1.0f };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);

        float m[16]; spot0_rotation->get_float_array_val(m);
        GLfloat dir[3]; getSpotDirFromMatrixAndPos(m, lx, ly, lz, dir);
        glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, dir);
        glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 30.0f);
        glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 30.0f);
    }
    glEnable(GL_LIGHT0);
}

void Light1(int state) {
    if (!state) { glDisable(GL_LIGHT1); return; }
    applyLightCommon(GL_LIGHT1, diff_R1,diff_G1,diff_B1, spec_R1,spec_G1,spec_B1);

    const float lx = 0.0f, ly = 3.0f, lz = 0.0f;
    if (lightType1 == 0) {
        GLfloat pos[] = { lx, ly, lz, 0.0f };
        glLightfv(GL_LIGHT1, GL_POSITION, pos);
        glLightf(GL_LIGHT1, GL_SPOT_CUTOFF, 180.0f);
    } else {
        GLfloat pos[] = { lx, ly, lz, 1.0f };
        glLightfv(GL_LIGHT1, GL_POSITION, pos);

        float m[16]; spot1_rotation->get_float_array_val(m);
        GLfloat dir[3]; getSpotDirFromMatrixAndPos(m, lx, ly, lz, dir);
        glLightfv(GL_LIGHT1, GL_SPOT_DIRECTION, dir);
        glLightf(GL_LIGHT1, GL_SPOT_CUTOFF, 30.0f);
        glLightf(GL_LIGHT1, GL_SPOT_EXPONENT, 30.0f);
    }
    glEnable(GL_LIGHT1);
}

void Light2(int state) {
    if (!state) { glDisable(GL_LIGHT2); return; }
    applyLightCommon(GL_LIGHT2, diff_R2,diff_G2,diff_B2, spec_R2,spec_G2,spec_B2);

    const float lx = 0.0f, ly = 0.0f, lz = 3.0f;
    if (lightType2 == 0) {
        GLfloat pos[] = { lx, ly, lz, 0.0f };
        glLightfv(GL_LIGHT2, GL_POSITION, pos);
        glLightf(GL_LIGHT2, GL_SPOT_CUTOFF, 180.0f);
    } else {
        GLfloat pos[] = { lx, ly, lz, 1.0f };
        glLightfv(GL_LIGHT2, GL_POSITION, pos);

        float m[16]; spot2_rotation->get_float_array_val(m);
        GLfloat dir[3]; getSpotDirFromMatrixAndPos(m, lx, ly, lz, dir);
        glLightfv(GL_LIGHT2, GL_SPOT_DIRECTION, dir);
        glLightf(GL_LIGHT2, GL_SPOT_CUTOFF, 30.0f);
        glLightf(GL_LIGHT2, GL_SPOT_EXPONENT, 30.0f);
    }
    glEnable(GL_LIGHT2);
}

void callmaterial(int ID) {
    if (ID < 0 || ID > 4) ID = 0;
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT,  MATS[ID].amb);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE,  MATS[ID].dif);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, MATS[ID].spec);
}

void callemission(int ID) {
    GLfloat e[4] = {0,0,0,1};
    if (if_emission) {
        e[0] = MATS[ID].dif[0];
        e[1] = MATS[ID].dif[1];
        e[2] = MATS[ID].dif[2];
        e[3] = 1.0f;
    }
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, e);
}

// ====== ColorCube ======
void drawColorCube(float size) {
    float s = size * 0.5f;
    const float inv = 1.0f / (2.0f * s);

    auto setColorByPos = [&](float x, float y, float z) {
        float r = x * inv + 0.5f;
        float g = y * inv + 0.5f;
        float b = z * inv + 0.5f;
        glColor3f(r, g, b);
    };

    glBegin(GL_QUADS);

    glNormal3f(0,0,1);
    setColorByPos(-s,-s, s); glVertex3f(-s,-s, s);
    setColorByPos( s,-s, s); glVertex3f( s,-s, s);
    setColorByPos( s, s, s); glVertex3f( s, s, s);
    setColorByPos(-s, s, s); glVertex3f(-s, s, s);

    glNormal3f(0,0,-1);
    setColorByPos(-s,-s,-s); glVertex3f(-s,-s,-s);
    setColorByPos(-s, s,-s); glVertex3f(-s, s,-s);
    setColorByPos( s, s,-s); glVertex3f( s, s,-s);
    setColorByPos( s,-s,-s); glVertex3f( s,-s,-s);

    glNormal3f(0,1,0);
    setColorByPos(-s, s,-s); glVertex3f(-s, s,-s);
    setColorByPos(-s, s, s); glVertex3f(-s, s, s);
    setColorByPos( s, s, s); glVertex3f( s, s, s);
    setColorByPos( s, s,-s); glVertex3f( s, s,-s);

    glNormal3f(0,-1,0);
    setColorByPos(-s,-s,-s); glVertex3f(-s,-s,-s);
    setColorByPos( s,-s,-s); glVertex3f( s,-s,-s);
    setColorByPos( s,-s, s); glVertex3f( s,-s, s);
    setColorByPos(-s,-s, s); glVertex3f(-s,-s, s);

    glNormal3f(1,0,0);
    setColorByPos( s,-s,-s); glVertex3f( s,-s,-s);
    setColorByPos( s, s,-s); glVertex3f( s, s,-s);
    setColorByPos( s, s, s); glVertex3f( s, s, s);
    setColorByPos( s,-s, s); glVertex3f( s,-s, s);

    glNormal3f(-1,0,0);
    setColorByPos(-s,-s,-s); glVertex3f(-s,-s,-s);
    setColorByPos(-s,-s, s); glVertex3f(-s,-s, s);
    setColorByPos(-s, s, s); glVertex3f(-s, s, s);
    setColorByPos(-s, s,-s); glVertex3f(-s, s,-s);

    glEnd();
}

void callViewType(int) {
    int vx, vy, vw, vh;
    GLUI_Master.get_viewport_area(&vx, &vy, &vw, &vh);
    if (vh <= 0) vh = 1;

    aspect = (float)vw / (float)vh;
    glViewport(vx, vy, vw, vh);

    const float eyeX = 3.0f, eyeY = 3.0f, eyeZ = 3.0f;
    const float atX  = 0.0f, atY  = 0.0f, atZ  = 0.0f;
    const float fovy = 60.0f;
    const float zNear = 0.1f, zFar = 50.0f;

    float dx = eyeX - atX, dy = eyeY - atY, dz = eyeZ - atZ;
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    const float PI = 3.14159265358979323846f;

    float halfH = dist * std::tan((fovy * 0.5f) * PI / 180.0f);
    float halfW = halfH * aspect;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    if (viewType == 0) glOrtho(-halfW, halfW, -halfH, halfH, -zFar, zFar);
    else gluPerspective(fovy, aspect, zNear, zFar);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(eyeX, eyeY, eyeZ, atX, atY, atZ, 0.0f, 1.0f, 0.0f);
}

void scale_function(int) {
    float base = 1.0f;
    if (objType == 0) base = teapot_size;
    else if (objType == 1) base = cube_size;
    else base = model_size;

    float autoScale = (objType == 2) ? gModelScale : 1.0f;

    if (if_uniform) {
        float u = scale_C_X;
        glScalef(base * autoScale * uniBaseX * u,
                 base * autoScale * uniBaseY * u,
                 base * autoScale * uniBaseZ * u);
    } else {
        glScalef(base * autoScale * scale_C_X,
                 base * autoScale * scale_C_Y,
                 base * autoScale * scale_C_Z);
    }
}

void updateRightClickMenu();

void call_back_function(int ID) {
    switch (ID) {
    case CB_RESET_TRANS:
        teapot_posX = teapot_posY = teapot_posZ = tran_X = tran_Y = tran_Z = 0.0f;
        break;

    case CB_RESET_SCALE:
        scale_C_X = scale_C_Y = scale_C_Z = 1.0f;
        uniBaseX = uniBaseY = uniBaseZ = 1.0f;
        break;

    case CB_RESET_ROT:
        rotation_UI->set_float_array_val(rotation_matrix_reset);
        break;

    case CB_UNIFORM:{
        if (if_uniform) {
            uniBaseX = scale_C_X;
            uniBaseY = scale_C_Y;
            uniBaseZ = scale_C_Z;

            scale_C_X = 1.0f;
            scale_C_Y = 1.0f;
            scale_C_Z = 1.0f;

            scaling_Y->disable();
            scaling_Z->disable();
        } else {
            float u = scale_C_X;

            scale_C_X = uniBaseX * u;
            scale_C_Y = uniBaseY * u;
            scale_C_Z = uniBaseZ * u;

            uniBaseX = uniBaseY = uniBaseZ = 1.0f;

            scaling_Y->enable();
            scaling_Z->enable();
        }
        break;
    }

    case CB_L0_ENABLE:
        if (if_Light0) Light0_rollout0->enable();
        else { glDisable(GL_LIGHT0); Light0_rollout0->disable(); }
        if (lightType0 == 1) { spot0_rotation->enable(); Reset_spot0->enable(); }
        else { spot0_rotation->disable(); Reset_spot0->disable(); }
        break;

    case CB_L1_ENABLE:
        if (if_Light1) Light1_rollout0->enable();
        else { glDisable(GL_LIGHT1); Light1_rollout0->disable(); }
        if (lightType1 == 1) { spot1_rotation->enable(); Reset_spot1->enable(); }
        else { spot1_rotation->disable(); Reset_spot1->disable(); }
        break;

    case CB_L2_ENABLE:
        if (if_Light2) Light2_rollout0->enable();
        else { glDisable(GL_LIGHT2); Light2_rollout0->disable(); }
        if (lightType2 == 1) { spot2_rotation->enable(); Reset_spot2->enable(); }
        else { spot2_rotation->disable(); Reset_spot2->disable(); }
        break;

    case CB_L0_RESETROT:
        spot0_rotation->set_float_array_val(rotation_matrix_reset);
        break;

    case CB_L1_RESETROT:
        spot1_rotation->set_float_array_val(rotation_matrix_reset);
        break;

    case CB_L2_RESETROT:
        spot2_rotation->set_float_array_val(rotation_matrix_reset);
        break;

    case CB_L0_TYPE:
        if (lightType0 == 1) { spot0_rotation->enable(); Reset_spot0->enable(); }
        else { spot0_rotation->disable(); Reset_spot0->disable(); }
        break;

    case CB_L1_TYPE:
        if (lightType1 == 1) { spot1_rotation->enable(); Reset_spot1->enable(); }
        else { spot1_rotation->disable(); Reset_spot1->disable(); }
        break;

    case CB_L2_TYPE:
        if (lightType2 == 1) { spot2_rotation->enable(); Reset_spot2->enable(); }
        else { spot2_rotation->disable(); Reset_spot2->disable(); }
        break;

    case CB_ANIMATE:
        break;

    case CB_MATERIAL:
        // ★ material 關：emission 不能選 + 強制關閉 emission
        if (!if_material) {
            if_emission = 0;
            if (cb_emission) {
                cb_emission->set_int_val(0);
                cb_emission->disable();
            }
        } else {
            if (cb_emission) cb_emission->enable();
        }

        if (!if_adjust_shininess) {
            shininess = MATS[materialID].shin;
            if (shininess_spinner) shininess_spinner->set_float_val(shininess);
        }
        break;

    case CB_ADJUST_SHIN:
        if (if_adjust_shininess) {
            if (shininess_spinner) shininess_spinner->enable();
        } else {
            if (shininess_spinner) shininess_spinner->disable();
            shininess = MATS[materialID].shin;
            if (shininess_spinner) shininess_spinner->set_float_val(shininess);
        }
        break;

    case CB_OBJECT: {
        int newType = objType;

        if (newType == 2) { // DataModel
            char picked[MAX_PATH];
            if (OpenTriFileDialog(picked)) {
                std::strncpy(gModelPath, picked, MAX_PATH - 1);
                gModelPath[MAX_PATH - 1] = '\0';

                std::printf("[TRI] user picked: %s\n", gModelPath);

                if (!loadTRI(gModelPath)) {
                    std::printf("[TRI] load FAILED: %s\n", gModelPath);
                    objType = lastObjType;
                }
            } else {
                objType = lastObjType;
            }

            if (modelPathText) {
                if (gModelPath[0] == '\0') {
                    modelPathText->set_text("Model: (none)");
                } else {
                    const char* p = strrchr(gModelPath, '\\');
                    const char* name = (p ? p + 1 : gModelPath);
                    std::string s = std::string("Model: ") + name;
                    modelPathText->set_text(s.c_str());
                }
            }
        }

        lastObjType = objType;
        updateRightClickMenu();
        break;
    }

    case CB_EMISSION:
        // emission checkbox 已由 CB_MATERIAL 控制 enable/disable
        break;

    default:
        break;
    }

    if (glui) glui->sync_live();
    needRedisplay = true;

    if (glutGetWindow() != window) glutSetWindow(window);
    glutPostRedisplay();
}

void initUI() {
    GLUI_Panel* Instance_Transformation = glui->add_panel("Instance Transformation");
    GLUI_Panel* trans_rotate = glui->add_panel_to_panel(Instance_Transformation, "", 0);

    GLUI_Panel* trans_panel = glui->add_panel_to_panel(trans_rotate, "Translation");
    GLUI_Panel* trans_panel2 = glui->add_panel_to_panel(trans_panel, "", GLUI_PANEL_NONE);
    glui->add_translation_to_panel(trans_panel2, "X", GLUI_TRANSLATION_X, &tran_X);
    glui->add_column_to_panel(trans_panel2, false);
    glui->add_translation_to_panel(trans_panel2, "Y", GLUI_TRANSLATION_Y, &tran_Y);
    glui->add_column_to_panel(trans_panel2, false);
    glui->add_translation_to_panel(trans_panel2, "Z", GLUI_TRANSLATION_Z, &tran_Z);
    glui->add_button_to_panel(trans_panel, "Reset Translation", CB_RESET_TRANS, call_back_function);

    glui->add_column_to_panel(trans_rotate, false);

    GLUI_Panel* rotate_panel = glui->add_panel_to_panel(trans_rotate, "Rotation");
    rotation_UI = glui->add_rotation_to_panel(rotate_panel, "Rotation", rotation_matrix);
    glui->add_button_to_panel(rotate_panel, "Reset Rotation", CB_RESET_ROT, call_back_function);

    GLUI_Panel* scale_ani_material_view = glui->add_panel_to_panel(Instance_Transformation, "", GLUI_PANEL_RAISED);

    GLUI_Panel* scale_panel = glui->add_panel_to_panel(scale_ani_material_view, "Scaling");
    GLUI_Panel* scale_panel2 = glui->add_panel_to_panel(scale_panel, "", GLUI_PANEL_NONE);
    scaling_X = glui->add_translation_to_panel(scale_panel2, "X", GLUI_TRANSLATION_X, &scale_C_X);
    scaling_X->set_speed(0.01f);
    glui->add_column_to_panel(scale_panel2, false);
    scaling_Y = glui->add_translation_to_panel(scale_panel2, "Y", GLUI_TRANSLATION_Y, &scale_C_Y);
    scaling_Y->set_speed(0.01f);
    glui->add_column_to_panel(scale_panel2, false);
    scaling_Z = glui->add_translation_to_panel(scale_panel2, "Z", GLUI_TRANSLATION_Z, &scale_C_Z);
    scaling_Z->set_speed(0.01f);

    GLUI_Panel* scale_panel3 = glui->add_panel_to_panel(scale_panel, "", false);
    glui->add_checkbox_to_panel(scale_panel3, "Uniform Scaling", &if_uniform, CB_UNIFORM, call_back_function);
    glui->add_button_to_panel(scale_panel, "Reset Scaling", CB_RESET_SCALE, call_back_function);

    glui->add_column_to_panel(scale_ani_material_view, false);

    GLUI_Panel* ani = glui->add_panel_to_panel(scale_ani_material_view, "Animation");
    glui->add_checkbox_to_panel(ani, "Animate", &if_animate, CB_ANIMATE, call_back_function);

    GLUI_Panel* material = glui->add_panel_to_panel(scale_ani_material_view, "Material");
    cb_material = glui->add_checkbox_to_panel(material, "material", &if_material, CB_MATERIAL, call_back_function);
    cb_emission = glui->add_checkbox_to_panel(material, "emission", &if_emission, CB_EMISSION, call_back_function);

    // ★初始狀態：material 關 -> emission disable
    if (!if_material) {
        if_emission = 0;
        cb_emission->set_int_val(0);
        cb_emission->disable();
    }

    GLUI_Panel* view = glui->add_panel_to_panel(scale_ani_material_view, "Viewing");
    GLUI_RadioGroup* chooseView = glui->add_radiogroup_to_panel(view, &viewType, CB_VIEW, call_back_function);
    glui->add_radiobutton_to_group(chooseView, "Ortho");
    glui->add_radiobutton_to_group(chooseView, "Perspec");

    GLUI_Panel* shad_mate = glui->add_panel("Shading and Materials");
    GLUI_Panel* shad_obj = glui->add_panel_to_panel(shad_mate, "", GLUI_PANEL_RAISED);

    GLUI_Panel* shad_type = glui->add_panel_to_panel(shad_obj, "Shading Type");
    GLUI_RadioGroup* shad_types = glui->add_radiogroup_to_panel(shad_type, &shadingType);
    glui->add_radiobutton_to_group(shad_types, "Wire");
    glui->add_radiobutton_to_group(shad_types, "Flat");
    glui->add_radiobutton_to_group(shad_types, "Smooth");

    glui->add_column_to_panel(shad_obj, false);

    GLUI_Panel* objs = glui->add_panel_to_panel(shad_obj, "Object");
    GLUI_RadioGroup* object_types = glui->add_radiogroup_to_panel(objs, &objType, CB_OBJECT, call_back_function);
    glui->add_radiobutton_to_group(object_types, "Teapot");
    glui->add_radiobutton_to_group(object_types, "ColorCube");
    glui->add_radiobutton_to_group(object_types, "DataModel");
    modelPathText = glui->add_statictext_to_panel(objs, "Model: (none)");

    glui->add_checkbox_to_panel(shad_mate, "Adjust Shininess", &if_adjust_shininess, CB_ADJUST_SHIN, call_back_function);
    shininess_spinner = glui->add_spinner_to_panel(shad_mate, "Shininess", GLUI_SPINNER_FLOAT, &shininess);
    shininess_spinner->set_float_limits(0.0f, 128.0f);

    glui->add_column_to_panel(shad_mate, false);

    GLUI_Panel* mate_type = glui->add_panel_to_panel(shad_mate, "Material type");
    mate_types = glui->add_radiogroup_to_panel(mate_type, &materialID, CB_MATERIAL, call_back_function);
    glui->add_radiobutton_to_group(mate_types, "Gold");
    glui->add_radiobutton_to_group(mate_types, "Pewter");
    glui->add_radiobutton_to_group(mate_types, "Silver");
    glui->add_radiobutton_to_group(mate_types, "Copper");
    glui->add_radiobutton_to_group(mate_types, "Chrome");

    GLUI_Panel* lighting = glui->add_panel("Lighting");
    GLUI_Panel* lights = glui->add_panel_to_panel(lighting, "", GLUI_PANEL_RAISED);
    glui->add_checkbox_to_panel(lights, "Light 0", &if_Light0, CB_L0_ENABLE, call_back_function);
    glui->add_column_to_panel(lights, false);
    glui->add_checkbox_to_panel(lights, "Light 1", &if_Light1, CB_L1_ENABLE, call_back_function);
    glui->add_column_to_panel(lights, false);
    glui->add_checkbox_to_panel(lights, "Light 2", &if_Light2, CB_L2_ENABLE, call_back_function);

    Light0_rollout0 = glui->add_rollout_to_panel(lighting, "Light 0", if_Light0);
    GLUI_Panel* L0_left = glui->add_panel_to_panel(Light0_rollout0, "", GLUI_PANEL_NONE);
    glui->add_column_to_panel(Light0_rollout0, false);
    GLUI_Panel* L0_right = glui->add_panel_to_panel(Light0_rollout0, "", GLUI_PANEL_NONE);
    GLUI_RadioGroup* rg0 = glui->add_radiogroup_to_panel(L0_left, &lightType0, CB_L0_TYPE, call_back_function);
    glui->add_radiobutton_to_group(rg0, "Directional");
    glui->add_radiobutton_to_group(rg0, "Spot");
    spot0_rotation = glui->add_rotation_to_panel(L0_left, "Rotate", spotlight_matrix0);
    Reset_spot0    = glui->add_button_to_panel(L0_left, "Reset", CB_L0_RESETROT, call_back_function);
    glui->add_spinner_to_panel(L0_right, "Diffuse: Red",    GLUI_SPINNER_FLOAT, &diff_R0)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L0_right, "Diffuse: Green",  GLUI_SPINNER_FLOAT, &diff_G0)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L0_right, "Diffuse: Blue",   GLUI_SPINNER_FLOAT, &diff_B0)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L0_right, "Specular: Red",   GLUI_SPINNER_FLOAT, &spec_R0)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L0_right, "Specular: Green", GLUI_SPINNER_FLOAT, &spec_G0)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L0_right, "Specular: Blue",  GLUI_SPINNER_FLOAT, &spec_B0)->set_float_limits(0,1);

    Light1_rollout0 = glui->add_rollout_to_panel(lighting, "Light 1", if_Light1);
    GLUI_Panel* L1_left = glui->add_panel_to_panel(Light1_rollout0, "", GLUI_PANEL_NONE);
    glui->add_column_to_panel(Light1_rollout0, false);
    GLUI_Panel* L1_right = glui->add_panel_to_panel(Light1_rollout0, "", GLUI_PANEL_NONE);
    GLUI_RadioGroup* rg1 = glui->add_radiogroup_to_panel(L1_left, &lightType1, CB_L1_TYPE, call_back_function);
    glui->add_radiobutton_to_group(rg1, "Directional");
    glui->add_radiobutton_to_group(rg1, "Spot");
    spot1_rotation = glui->add_rotation_to_panel(L1_left, "Rotate", spotlight_matrix1);
    Reset_spot1    = glui->add_button_to_panel(L1_left, "Reset", CB_L1_RESETROT, call_back_function);
    glui->add_spinner_to_panel(L1_right, "Diffuse: Red",    GLUI_SPINNER_FLOAT, &diff_R1)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L1_right, "Diffuse: Green",  GLUI_SPINNER_FLOAT, &diff_G1)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L1_right, "Diffuse: Blue",   GLUI_SPINNER_FLOAT, &diff_B1)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L1_right, "Specular: Red",   GLUI_SPINNER_FLOAT, &spec_R1)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L1_right, "Specular: Green", GLUI_SPINNER_FLOAT, &spec_G1)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L1_right, "Specular: Blue",  GLUI_SPINNER_FLOAT, &spec_B1)->set_float_limits(0,1);

    Light2_rollout0 = glui->add_rollout_to_panel(lighting, "Light 2", if_Light2);
    GLUI_Panel* L2_left = glui->add_panel_to_panel(Light2_rollout0, "", GLUI_PANEL_NONE);
    glui->add_column_to_panel(Light2_rollout0, false);
    GLUI_Panel* L2_right = glui->add_panel_to_panel(Light2_rollout0, "", GLUI_PANEL_NONE);
    GLUI_RadioGroup* rg2 = glui->add_radiogroup_to_panel(L2_left, &lightType2, CB_L2_TYPE, call_back_function);
    glui->add_radiobutton_to_group(rg2, "Directional");
    glui->add_radiobutton_to_group(rg2, "Spot");
    spot2_rotation = glui->add_rotation_to_panel(L2_left, "Rotate", spotlight_matrix2);
    Reset_spot2    = glui->add_button_to_panel(L2_left, "Reset", CB_L2_RESETROT, call_back_function);
    glui->add_spinner_to_panel(L2_right, "Diffuse: Red",    GLUI_SPINNER_FLOAT, &diff_R2)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L2_right, "Diffuse: Green",  GLUI_SPINNER_FLOAT, &diff_G2)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L2_right, "Diffuse: Blue",   GLUI_SPINNER_FLOAT, &diff_B2)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L2_right, "Specular: Red",   GLUI_SPINNER_FLOAT, &spec_R2)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L2_right, "Specular: Green", GLUI_SPINNER_FLOAT, &spec_G2)->set_float_limits(0,1);
    glui->add_spinner_to_panel(L2_right, "Specular: Blue",  GLUI_SPINNER_FLOAT, &spec_B2)->set_float_limits(0,1);

    glui->add_button("Exit", 0, (GLUI_Update_CB)exit);

    call_back_function(CB_ADJUST_SHIN);
    call_back_function(CB_L0_ENABLE);
    call_back_function(CB_L1_ENABLE);
    call_back_function(CB_L2_ENABLE);
    call_back_function(CB_MATERIAL);
    call_back_function(CB_L0_TYPE);
    call_back_function(CB_L1_TYPE);
    call_back_function(CB_L2_TYPE);
}

// ================================
// Right click menu
// ================================
enum {
    MENU_EXIT = 0,
    OBJ_SIZE_1 = 10,
    OBJ_SIZE_2,
    OBJ_SIZE_3,
    OBJ_WIRE  = 20,
    OBJ_SOLID,
    OBJ_SMOOTH
};

int menu_main_object = -1;

void menu(int id) {
    switch (id) {
    case OBJ_SIZE_1:
        if (objType == 0) teapot_size = 1.0f;
        else if (objType == 1) cube_size = 1.0f;
        else model_size = 1.0f;
        break;

    case OBJ_SIZE_2:
        if (objType == 0) teapot_size = 1.5f;
        else if (objType == 1) cube_size = 1.5f;
        else model_size = 1.5f;
        break;

    case OBJ_SIZE_3:
        if (objType == 0) teapot_size = 2.0f;
        else if (objType == 1) cube_size = 2.0f;
        else model_size = 2.0f;
        break;

    case OBJ_WIRE:   shadingType = 0; break;
    case OBJ_SOLID:  shadingType = 1; break;
    case OBJ_SMOOTH: shadingType = 2; break;

    case MENU_EXIT: exit(0);
    default: break;
    }

    if (glui) glui->sync_live();
    if (glutGetWindow() != window) glutSetWindow(window);
    glutPostRedisplay();
}

void buildMenus() {
    int menu_size = glutCreateMenu(menu);
    glutAddMenuEntry("1.0", OBJ_SIZE_1);
    glutAddMenuEntry("1.5", OBJ_SIZE_2);
    glutAddMenuEntry("2.0", OBJ_SIZE_3);

    int menu_type = glutCreateMenu(menu);
    glutAddMenuEntry("Wire",   OBJ_WIRE);
    glutAddMenuEntry("Solid",  OBJ_SOLID);
    glutAddMenuEntry("Smooth", OBJ_SMOOTH);

    menu_main_object = glutCreateMenu(menu);
    glutAddSubMenu("Object Size", menu_size);
    glutAddSubMenu("Object Type", menu_type);
    glutAddMenuEntry("Exit", MENU_EXIT);
}

void updateRightClickMenu() {
    glutSetMenu(menu_main_object);
    glutAttachMenu(GLUT_RIGHT_BUTTON);
}

void init() {
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE);

    glClearColor(0.2f, 0.2f, 0.2f, 0.0f);
    GLfloat globalAmb[] = { 0.15f, 0.15f, 0.15f, 1.0f };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, globalAmb);

    glNewList(WORLD_, GL_COMPILE);
    glBegin(GL_LINES);
    glColor3f(1.0, 0.0, 0.0); glVertex3f(0.0, 0.0, 0.0); glVertex3f(3.0, 0.0, 0.0);
    glColor3f(0.0, 1.0, 0.0); glVertex3f(0.0, 0.0, 0.0); glVertex3f(0.0, 3.0, 0.0);
    glColor3f(0.0, 0.0, 1.0); glVertex3f(0.0, 0.0, 0.0); glVertex3f(0.0, 0.0, 3.0);
    glEnd();
    glEndList();

    glNewList(LOCAL_, GL_COMPILE);
    glBegin(GL_LINES);
    glColor3f(1.0, 0.0, 0.0); glVertex3f(0.0, 0.0, 0.0); glVertex3f(1.0, 0.0, 0.0);
    glColor3f(0.0, 1.0, 0.0); glVertex3f(0.0, 0.0, 0.0); glVertex3f(0.0, 1.0, 0.0);
    glColor3f(0.0, 0.0, 1.0); glVertex3f(0.0, 0.0, 0.0); glVertex3f(0.0, 0.0, 1.0);
    glEnd();
    glEndList();

    gModelPath[0] = '\0';
}

void timer(int val) {
    int now = glutGet(GLUT_ELAPSED_TIME);
    if (lastTickMs == 0) lastTickMs = now;
    float dt = (now - lastTickMs) * 0.001f;
    lastTickMs = now;

    if (if_animate) {
        rotateAngle += rotateSpeed * 60.0f * dt;
        if (rotateAngle > 360.0f) rotateAngle -= 360.0f;
    }

    needRedisplay = true;
    glutTimerFunc(timer_interval, timer, 0);
}

void reshape(int w, int h) {
    if (h <= 0) h = 1;
    width = w;
    height = h;
    aspect = w * 1.0f / h;
    glViewport(0, 0, w, h);
}

void mouse(int button, int state, int x, int) {
    if (button == GLUT_LEFT_BUTTON && state == GLUT_DOWN) {
        bef_background = background;
        WhenClick_X = x;
    }
}

void mouse_moving(int x, int) {
    background = clampf((x - WhenClick_X) * 0.005f + bef_background, 0.0f, 1.0f);
    needRedisplay = true;
}

void keyboard(unsigned char key, int, int) {
    switch (key) {
    case 'r': case 'R': myColor = Red; break;
    case 'g': case 'G': myColor = Green; break;
    case 'b': case 'B': myColor = Blue; break;
    case 27: exit(0);
    default: break;
    }
    needRedisplay = true;
}

void display() {
    glClearColor(background, background, background, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    callViewType(viewType);

    Light0(if_Light0);
    Light1(if_Light1);
    Light2(if_Light2);

    glDisable(GL_LIGHTING);
    glCallList(WORLD_);

    glPushMatrix();

    glTranslatef(teapot_posX + tran_X * 0.01f,
                 teapot_posY + tran_Y * 0.01f,
                 teapot_posZ + tran_Z * 0.01f);

    glMultMatrixf(rotation_matrix);
    if (if_animate) glRotatef(rotateAngle, 0.0f, 1.0f, 0.0f);

    scale_function(if_uniform);

    glCallList(LOCAL_);

    glEnable(GL_LIGHTING);
    glShadeModel(shadingType == 1 ? GL_FLAT : GL_SMOOTH);

    if (objType == 0) {
        // ====== TEAPOT ======
        if (if_material) {
            glDisable(GL_COLOR_MATERIAL);

            callmaterial(materialID);
            glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, shininess);

            callemission(materialID);

            if (shadingType == 0) glutWireTeapot(teapot_size);
            else glutSolidTeapot(teapot_size);
        } else {
            GLfloat e0[4] = {0,0,0,1};
            glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, e0);

            if (shadingType == 0) {
                glDisable(GL_LIGHTING);
                glDisable(GL_COLOR_MATERIAL);

                glColor3f(1.0f, 0.0f, 0.0f);
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glutSolidTeapot(teapot_size);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

                glEnable(GL_LIGHTING);
            } else {
                glEnable(GL_LIGHTING);
                glEnable(GL_COLOR_MATERIAL);
                glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

                glColor3f(1.0f, 0.0f, 0.0f);

                GLfloat spc[4] = {0.8f, 0.8f, 0.8f, 1.0f};
                glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spc);
                glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 48.0f);

                GLfloat e00[4] = {0,0,0,1};
                glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, e00);

                GLfloat oldGlobalAmb[4];
                glGetFloatv(GL_LIGHT_MODEL_AMBIENT, oldGlobalAmb);

                GLfloat brightGlobalAmb[4] = {0.65f, 0.65f, 0.65f, 1.0f};
                glLightModelfv(GL_LIGHT_MODEL_AMBIENT, brightGlobalAmb);

                glutSolidTeapot(teapot_size);

                glLightModelfv(GL_LIGHT_MODEL_AMBIENT, oldGlobalAmb);
            }
        }
    }
    else if (objType == 1) {
        // ====== COLOR CUBE ======
        if (if_material) {
            glDisable(GL_COLOR_MATERIAL);

            callmaterial(materialID);
            glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, shininess);

            callemission(materialID);

            if (cube_wire || shadingType == 0) {
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                drawColorCube(cube_size);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            } else {
                drawColorCube(cube_size);
            }
        } else {
            // material off => no emission
            GLfloat e0[4] = {0,0,0,1};
            glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, e0);

            glEnable(GL_LIGHTING);
            glEnable(GL_COLOR_MATERIAL);
            glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

            GLfloat spc[4] = {0.8f,0.8f,0.8f,1.0f};
            glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spc);
            glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 48.0f);

            GLfloat oldGlobalAmb[4];
            glGetFloatv(GL_LIGHT_MODEL_AMBIENT, oldGlobalAmb);

            GLfloat brightGlobalAmb[4] = {0.65f,0.65f,0.65f,1.0f};
            glLightModelfv(GL_LIGHT_MODEL_AMBIENT, brightGlobalAmb);

            if (cube_wire || shadingType == 0) {
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                drawColorCube(cube_size);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            } else {
                drawColorCube(cube_size);
            }

            glLightModelfv(GL_LIGHT_MODEL_AMBIENT, oldGlobalAmb);
        }
    }
    else {
        // ====== MODEL ======
        if (gModelList != 0) {
            glEnable(GL_COLOR_MATERIAL);
            glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

            if (if_material) {
                glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, MATS[materialID].spec);
                glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, shininess);
            } else {
                GLfloat spc[4] = {0.15f, 0.15f, 0.15f, 1.0f};
                glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spc);
                glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 24.0f);
            }

            if (if_material && if_emission) {
                callemission(materialID);
            } else {
                GLfloat e0[4] = {0,0,0,1};
                glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, e0);
            }

            if (shadingType == 0) {
                glDisable(GL_CULL_FACE);
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glCallList(gModelList);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            } else {
                glCallList(gModelList);
            }
        }
    }

    glPopMatrix();
    glutSwapBuffers();
}


void glutIdle() {
    if (!needRedisplay) return;
    if (glutGetWindow() != window) glutSetWindow(window);
    glutPostRedisplay();
    needRedisplay = false;
}

int main(int argc, char* argv[]) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGB | GLUT_DEPTH | GLUT_DOUBLE);
    glutInitWindowSize(width, height);
    glutInitWindowPosition(0, 0);
    window = glutCreateWindow("U11316014");

    glui = GLUI_Master.create_glui_subwindow(window, GLUI_SUBWINDOW_RIGHT);
    glui->set_main_gfx_window(window);

    init();
    initUI();

    shininess = MATS[materialID].shin;
    if (shininess_spinner) shininess_spinner->set_float_val(shininess);
    call_back_function(CB_ADJUST_SHIN);

    glutDisplayFunc(display);
    GLUI_Master.set_glutReshapeFunc(reshape);
    GLUI_Master.set_glutKeyboardFunc(keyboard);
    GLUI_Master.set_glutMouseFunc(mouse);
    GLUI_Master.set_glutIdleFunc(glutIdle);
    glutMotionFunc(mouse_moving);

    glutTimerFunc(timer_interval, timer, 0);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_NORMALIZE);

    buildMenus();
    updateRightClickMenu();

    glutMainLoop();
    return 0;
}
