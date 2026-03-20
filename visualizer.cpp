#define _USE_MATH_DEFINES
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_syswm.h>
#include <fftw3.h>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
#include <deque>
#include <random>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <ctime>
#include <ios>
#include <iomanip>
namespace fs = std::filesystem;

// ── CRASH LOGGER ──────────────────────────────────────────────────────────────
static std::string g_exeDir;
void logErr(const std::string& msg){
    std::ofstream f(g_exeDir+"crash.log", std::ios::app);
    f << msg << "\n"; f.flush();
}

LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep){
    std::ofstream f(g_exeDir+"crash.log", std::ios::app);
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    f << "\n=== CRASH " << std::ctime(&t);
    if(ep && ep->ExceptionRecord){
        f << "Exception code: 0x" << std::hex << ep->ExceptionRecord->ExceptionCode << "\n";
        f << "Address:        0x" << ep->ExceptionRecord->ExceptionAddress << "\n";
    }
    f << "Check crash.log for details. Rebuild with -g for symbols.\n";
    f.flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

static constexpr int   FPS       = 60;
static constexpr int   FFT_N     = 4096;
static constexpr int   FREQ_BINS = FFT_N / 2;
static constexpr int   KSLICES   = 8;
static constexpr float SANG      = 2.f*(float)M_PI/KSLICES;

static TTF_Font*  g_font    = nullptr;
static TTF_Font*  g_fontSm  = nullptr;
static float      g_pcmBuf[FFT_N*2]={};
static int        g_pcmWrite=0;
static SDL_mutex* g_pcmMutex=nullptr;

void audioCaptureCallback(void*,Uint8* stream,int len){
    int n=len/sizeof(float); SDL_LockMutex(g_pcmMutex);
    float* f=(float*)stream;
    for(int i=0;i<n;++i){g_pcmBuf[g_pcmWrite%(FFT_N*2)]=f[i];++g_pcmWrite;}
    SDL_UnlockMutex(g_pcmMutex);
}

bool initFonts(){
    const char* paths[]={
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        "C:/Windows/Fonts/verdana.ttf",
        nullptr
    };
    for(int i=0;paths[i];++i){
        g_font  =TTF_OpenFont(paths[i],15);
        g_fontSm=TTF_OpenFont(paths[i],12);
        if(g_font&&g_fontSm)return true;
        if(g_font)  {TTF_CloseFont(g_font);  g_font=nullptr;}
        if(g_fontSm){TTF_CloseFont(g_fontSm);g_fontSm=nullptr;}
    }
    return false;
}

void drawText(SDL_Renderer* r,TTF_Font* font,const std::string& text,
              int x,int y,SDL_Color c,bool centered=false){
    if(!font||text.empty())return;
    SDL_Surface* surf=TTF_RenderText_Blended(font,text.c_str(),c);
    if(!surf)return;
    SDL_Texture* tex=SDL_CreateTextureFromSurface(r,surf);
    SDL_FreeSurface(surf);
    if(!tex)return;
    int tw,th; SDL_QueryTexture(tex,nullptr,nullptr,&tw,&th);
    SDL_Rect dst={centered?x-tw/2:x, y, tw, th};
    SDL_SetTextureBlendMode(tex,SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(tex,c.a);
    SDL_RenderCopy(r,tex,nullptr,&dst);
    SDL_DestroyTexture(tex);
}

std::string openFilePicker(){
    char fn[MAX_PATH]={};OPENFILENAMEA ofn={};ofn.lStructSize=sizeof(ofn);
    ofn.lpstrFilter="Audio Files\0*.mp3;*.ogg;*.wav;*.flac\0All Files\0*.*\0";
    ofn.lpstrFile=fn;ofn.nMaxFile=MAX_PATH;ofn.lpstrTitle="Select a music file";
    ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(GetOpenFileNameA(&ofn))return std::string(fn);return"";
}
// Async version — runs dialog on background thread, keeps render loop alive
// Posts SDL_USEREVENT code=3 with result string* when done
static std::atomic<bool> g_filePickerOpen{false};
void openFilePickerAsync(){
    if(g_filePickerOpen.exchange(true)) return; // already open
    std::thread([](){
        std::string result = openFilePicker();
        g_filePickerOpen = false;
        std::string* res = new std::string(result);
        SDL_Event e{}; e.type=SDL_USEREVENT;
        e.user.code=3; e.user.data1=res;
        SDL_PushEvent(&e);
    }).detach();
}
inline float lerp(float a,float b,float t){return a+(b-a)*t;}
inline float clamp01(float x){return x<0?0:x>1?1:x;}
inline float smoothstep(float x){x=clamp01(x);return x*x*(3-2*x);}
// Spring easing: pronounced overshoot then settles — gives menu items a bouncy pop
inline float springEase(float t){
    t=clamp01(t);
    return 1.f - expf(-5.5f*t)*cosf(t*14.f);
}

SDL_Color hsv(float h,float s,float v,float a=1.f){
    h=fmod(h,360.f);if(h<0)h+=360.f;
    float c=v*s,x=c*(1.f-fabsf(fmod(h/60.f,2.f)-1.f)),m=v-c;
    float r=0,g=0,b=0;
    if(h<60){r=c;g=x;}else if(h<120){r=x;g=c;}else if(h<180){g=c;b=x;}
    else if(h<240){g=x;b=c;}else if(h<300){r=x;b=c;}else{r=c;b=x;}
    return{(Uint8)((r+m)*255),(Uint8)((g+m)*255),(Uint8)((b+m)*255),(Uint8)(a*255)};
}


// ═══════════════════════════════════════════════════════════════════════════════
// THEME SYSTEM
// ═══════════════════════════════════════════════════════════════════════════════
// THEME SYSTEM
// ═══════════════════════════════════════════════════════════════════════════════
enum EffectID {
    EFF_KALEIDOSCOPE=0, EFF_LIGHTNING, EFF_DNA, EFF_NOVA, EFF_TENDRILS,
    EFF_CYMATICS, EFF_VORTEX, EFF_GEOMETRY, EFF_FILAMENTS, EFF_GRAVITY,
    EFF_AURORA, EFF_GALAXY, EFF_RIBBON, EFF_NEURAL, EFF_SHOCKWAVE,
    EFF_TUNNEL, EFF_RAIN, EFF_MEMBRANE, EFF_SPECTRUM, EFF_HYPNO,
    EFF_SINE, EFF_WORMHOLE, EFF_LISSAJOUS, EFF_FRACTAL, EFF_CUBE, EFF_GEOSHAPES,
    EFF_COUNT
};
static const char* EFF_NAMES[EFF_COUNT]={
    "Kaleidoscope","Lightning","DNA Helix","Nova","Tendrils",
    "Cymatics","Vortex","Geometry","Filaments","Gravity Lens",
    "Aurora","Galaxy","Ribbon Tornado","Neural Net","Shockwaves",
    "Tunnel","Pixel Rain","Sub Membrane","Spectrum Ring","Hypno Spiral",
    "Sine Waves","Wormhole","Mega Lissajous","Fractal Burst","Beat Cube","Geo Shapes"
};

struct Theme {
    std::string name;
    float hueOffset, satMult, valMult, intensity, uiHue;
    float bgHue1, bgHue2, bgSat, bgVal, bgGradientAngle;
    bool  bgGradient, custom;
    bool  effHueOverride[EFF_COUNT];
    float effHue[EFF_COUNT], effSat[EFF_COUNT];
    bool  effVisible[EFF_COUNT];
};

Theme makeDefaultTheme(const std::string& nm="Custom"){
    Theme t{};
    t.name=nm; t.hueOffset=0.f; t.satMult=1.f; t.valMult=1.f;
    t.intensity=1.f; t.uiHue=120.f;
    t.bgHue1=0.f; t.bgHue2=240.f; t.bgSat=0.f; t.bgVal=0.f;
    t.bgGradientAngle=90.f; t.bgGradient=false; t.custom=true;
    for(int i=0;i<EFF_COUNT;++i){
        t.effHueOverride[i]=false;
        t.effHue[i]=(float)i/EFF_COUNT*360.f;
        t.effSat[i]=1.f; t.effVisible[i]=true;
    }
    t.effVisible[EFF_CUBE]=false; // off by default — it interferes with other effects
    return t;
}

static Theme PRESET_THEMES[8];
static bool  g_presetsInit=false;
static const int NUM_PRESETS=8;

void initPresets(){
    if(g_presetsInit) return;
    // Each preset has a completely different personality
    // ho=hueOffset, sm=satMult, vm=valMult, in=intensity, ui=uiHue
    // bh=bgHue1, bh2=bgHue2, bs=bgSat, bv=bgVal, bg=gradient
    const struct{ const char* n; float ho,sm,vm,in,ui,bh,bh2,bs,bv; bool bg; }
    P[]={
        // Acid — full rainbow, maximum everything, pure chaos
        {"Acid",     0.f,  1.2f, 1.0f, 1.0f, 120.f,   0.f,  0.f, 0.f,  0.f,  false},
        // Void — desaturated deep purple, ghostly, very low intensity, dark bg
        {"Void",   260.f,  0.15f,0.5f, 0.4f, 270.f, 260.f,200.f, 0.8f, 0.06f, true},
        // Ember — locked to red/orange, high sat, warm bg, mid intensity
        {"Ember",  330.f,  1.3f, 1.1f, 1.1f,  15.f,  10.f, 40.f, 1.0f, 0.08f, true},
        // Ocean — cool blue/cyan only, calm, deep blue bg
        {"Ocean",  185.f,  1.0f, 0.8f, 0.7f, 195.f, 200.f,240.f, 0.9f, 0.05f, true},
        // Neon — hypersaturated pink/green, overdriven brightness, black bg
        {"Neon",   295.f,  1.5f, 1.3f, 1.4f, 300.f,   0.f,  0.f, 0.f,  0.f,  false},
        // Mono — pure white/grey, zero saturation, all effects become luminous white
        {"Mono",     0.f,  0.0f, 1.2f, 0.9f,   0.f,   0.f,  0.f, 0.f,  0.f,  false},
        // Sakura — soft pink/rose, gentle pastel, light bloom
        {"Sakura", 315.f,  0.6f, 0.9f, 0.65f,325.f, 310.f,340.f, 0.5f, 0.04f, true},
        // Nuclear — searing yellow/green, max intensity, radioactive bg
        {"Nuclear",  50.f, 1.4f, 1.3f, 1.6f,  65.f,  55.f, 80.f, 0.8f, 0.1f,  true},
    };
    for(int i=0;i<8;++i){
        PRESET_THEMES[i]=makeDefaultTheme(P[i].n);
        PRESET_THEMES[i].hueOffset=P[i].ho;  PRESET_THEMES[i].satMult=P[i].sm;
        PRESET_THEMES[i].valMult =P[i].vm;   PRESET_THEMES[i].intensity=P[i].in;
        PRESET_THEMES[i].uiHue  =P[i].ui;    PRESET_THEMES[i].bgHue1=P[i].bh;
        PRESET_THEMES[i].bgHue2 =P[i].bh2;   PRESET_THEMES[i].bgSat=P[i].bs;
        PRESET_THEMES[i].bgVal  =P[i].bv;    PRESET_THEMES[i].bgGradient=P[i].bg;
        PRESET_THEMES[i].custom =false;
        // Beat Cube off by default in all presets — too distracting
        PRESET_THEMES[i].effVisible[EFF_CUBE]=false;
        // Per-theme effect visibility tweaks for extra personality
        if(i==1){ // Void — hide the most chaotic effects
            PRESET_THEMES[i].effVisible[EFF_LIGHTNING]=false;
            PRESET_THEMES[i].effVisible[EFF_NOVA]=false;
            PRESET_THEMES[i].effVisible[EFF_CYMATICS]=false;
        }
        if(i==5){ // Mono — hide colourful effects, keep geometric ones
            PRESET_THEMES[i].effVisible[EFF_AURORA]=false;
            PRESET_THEMES[i].effVisible[EFF_FILAMENTS]=false;
            PRESET_THEMES[i].effVisible[EFF_GALAXY]=false;
        }
        if(i==6){ // Sakura — hide harsh effects
            PRESET_THEMES[i].effVisible[EFF_LIGHTNING]=false;
            PRESET_THEMES[i].effVisible[EFF_SHOCKWAVE]=false;
            PRESET_THEMES[i].effVisible[EFF_GEOMETRY]=false;
        }
        if(i==7){ // Nuclear — hide calm effects, max out violent ones
            PRESET_THEMES[i].effVisible[EFF_AURORA]=false;
            PRESET_THEMES[i].effVisible[EFF_FILAMENTS]=false;
            PRESET_THEMES[i].effVisible[EFF_HYPNO]=false;
        }
    }
    g_presetsInit=true;
}

static Theme g_theme;
static Theme g_themePrev;
static int   g_themeIdx=0;
static float g_themeBlend=1.f;
static Theme g_customThemes[4];
static int   g_numCustomThemes=0;

// Theme editor state
static bool  g_themeOpen=false;
static float g_themePanelT=0.f;
static bool  g_themeEditing=false;
static int   g_themeEditSlot=0;
static Theme g_themeEditBuf;
static int   g_themeEditorTab=0;   // 0=Colors 1=Effects 2=Background
static float g_themeEditorT=0.f;   // fullscreen build-in timer
static int   g_themeHover=-1;
static int   g_effScrollOffset=0;
// Slider drag state for theme editor
static int   g_themeDragSlider=-1;  // which slider is being dragged (-1=none)
static int   g_themeDragTab=-1;     // which tab the drag started in
static int   g_themeDragEffIdx=-1;  // for effect hue bars
static int   g_themeDragX=0;        // x of slider track start
static int   g_themeDragW=0;        // width of slider track
static float g_themeDragMin=0.f;
static float g_themeDragMax=1.f;
static float* g_themeDragVal=nullptr;

SDL_Color thsv(float h,float s,float v,float a=1.f){
    float b=g_themeBlend;
    float hOff=g_themePrev.hueOffset*(1.f-b)+g_theme.hueOffset*b;
    float sm=g_themePrev.satMult*(1.f-b)+g_theme.satMult*b;
    float vm=g_themePrev.valMult*(1.f-b)+g_theme.valMult*b;
    return hsv(fmod(h+hOff,360.f),std::min(1.f,s*sm),std::min(1.f,v*vm),a);
}
float themeIntensity(){
    float b=g_themeBlend;
    return g_themePrev.intensity*(1.f-b)+g_theme.intensity*b;
}
SDL_Color themeBg(){
    auto& th=g_theme;
    if(!th.bgGradient) return {0,0,0,255};
    SDL_Color c=thsv(th.bgHue1,th.bgSat,th.bgVal);
    return {c.r,c.g,c.b,255};
}
bool effVisible(int e){ return g_theme.effVisible[e]; }

void applyTheme(int idx){
    g_themePrev=g_theme; g_themeBlend=0.f;
    if(idx<NUM_PRESETS) g_theme=PRESET_THEMES[idx];
    else g_theme=g_customThemes[idx-NUM_PRESETS];
    g_themeIdx=idx;
}

void saveThemes(){
    std::ofstream f(g_exeDir+"themes.dat");
    f<<"CUSTOM "<<g_numCustomThemes<<"\n";
    for(int i=0;i<g_numCustomThemes;++i){
        auto& t=g_customThemes[i];
        f<<t.name<<"\n"<<t.hueOffset<<" "<<t.satMult<<" "<<t.valMult<<" "<<t.intensity<<" "<<t.uiHue<<"\n";
        f<<t.bgHue1<<" "<<t.bgHue2<<" "<<t.bgSat<<" "<<t.bgVal<<" "<<t.bgGradientAngle<<" "<<(int)t.bgGradient<<"\n";
        for(int e=0;e<EFF_COUNT;++e)
            f<<(int)t.effHueOverride[e]<<" "<<t.effHue[e]<<" "<<t.effSat[e]<<" "<<(int)t.effVisible[e]<<"\n";
    }
    f<<"ACTIVE "<<g_themeIdx<<"\n";
}

void loadThemes(){
    std::ifstream f(g_exeDir+"themes.dat");
    if(!f) return;
    std::string line; int n=0;
    if(std::getline(f,line)){ std::istringstream ss(line); std::string l; ss>>l>>n; g_numCustomThemes=std::min(n,4); }
    for(int i=0;i<g_numCustomThemes;++i){
        g_customThemes[i]=makeDefaultTheme();
        auto& t=g_customThemes[i]; std::string nm; std::getline(f,nm); t.name=nm;
        std::string l1,l2;
        if(std::getline(f,l1)){ std::istringstream ss(l1); ss>>t.hueOffset>>t.satMult>>t.valMult>>t.intensity>>t.uiHue; }
        if(std::getline(f,l2)){ std::istringstream ss(l2); int bg; ss>>t.bgHue1>>t.bgHue2>>t.bgSat>>t.bgVal>>t.bgGradientAngle>>bg; t.bgGradient=bg; }
        for(int e=0;e<EFF_COUNT;++e){
            std::string le; if(!std::getline(f,le)) break;
            std::istringstream ss(le); int ov,vis; ss>>ov>>t.effHue[e]>>t.effSat[e]>>vis;
            t.effHueOverride[e]=ov; t.effVisible[e]=vis;
        }
    }
    std::string al; while(std::getline(f,al)){
        std::istringstream ss(al); std::string lb; int idx;
        if(ss>>lb>>idx&&lb=="ACTIVE") applyTheme(std::min(idx,NUM_PRESETS+g_numCustomThemes-1));
    }
}


static Mix_Music* g_music=nullptr;
static bool       g_paused=false;
static Uint32 g_lastTrackChange=0;
static float  g_titleBuildT=1.f;
// Photosensitivity warning shown on startup
static bool   g_warnActive  = true;  // shown until dismissed
static float  g_warnBuildT  = 0.f;
static float  g_warnAlpha   = 0.f;

void loadAndPlay(const std::string& p){
    if(g_music){Mix_HaltMusic();Mix_FreeMusic(g_music);g_music=nullptr;}
    g_music=Mix_LoadMUS(p.c_str());
    if(g_music){
        int r=Mix_PlayMusic(g_music,-1);
        g_paused=false;
        if(r<0) logErr("Mix_PlayMusic failed ["+p+"]: "+Mix_GetError());
    } else {
        logErr("Mix_LoadMUS failed ["+p+"]: "+Mix_GetError());
    }
    g_lastTrackChange=SDL_GetTicks();
    g_titleBuildT=0.f;
}

struct V2{float x,y;};
V2 operator+(V2 a,V2 b){return{a.x+b.x,a.y+b.y};}
V2 operator-(V2 a,V2 b){return{a.x-b.x,a.y-b.y};}
V2 operator*(V2 a,float s){return{a.x*s,a.y*s};}
V2 operator*(float s,V2 a){return{a.x*s,a.y*s};}
float vlen(V2 v){return sqrtf(v.x*v.x+v.y*v.y);}
V2 vnorm(V2 v){float l=vlen(v);return l>0?V2{v.x/l,v.y/l}:V2{0,0};}
V2 perp(V2 v){return{-v.y,v.x};}
V2 vrot(V2 v,float a){return{v.x*cosf(a)-v.y*sinf(a),v.x*sinf(a)+v.y*cosf(a)};}

// Runtime check for SDL_RenderGeometry (added in SDL 2.0.18)
static bool g_hasRenderGeometry = false;
typedef int (*RenderGeometryFn)(SDL_Renderer*,SDL_Texture*,const SDL_Vertex*,int,const int*,int);
static RenderGeometryFn g_renderGeometry = nullptr;

void initRenderGeometry(){
    // Try to load SDL_RenderGeometry dynamically
    void* fn = SDL_LoadFunction(SDL_LoadObject("SDL2.dll"), "SDL_RenderGeometry");
    if(!fn) fn = SDL_LoadFunction(SDL_LoadObject(nullptr), "SDL_RenderGeometry");
    if(fn){ g_renderGeometry=(RenderGeometryFn)fn; g_hasRenderGeometry=true; }
}

void triC(SDL_Renderer* r,V2 a,V2 b,V2 c,SDL_Color ca,SDL_Color cb,SDL_Color cc){
    if(g_hasRenderGeometry){
        SDL_Vertex v[3]={{{a.x,a.y},ca,{0,0}},{{b.x,b.y},cb,{0,0}},{{c.x,c.y},cc,{0,0}}};
        int i[]={0,1,2}; g_renderGeometry(r,nullptr,v,3,i,3);
        return;
    }
    // Fallback: average color scanline fill
    SDL_Color avg={(Uint8)((ca.r+cb.r+cc.r)/3),(Uint8)((ca.g+cb.g+cc.g)/3),
                   (Uint8)((ca.b+cb.b+cc.b)/3),(Uint8)((ca.a+cb.a+cc.a)/3)};
    SDL_SetRenderDrawColor(r,avg.r,avg.g,avg.b,avg.a);
    float minY=std::min({a.y,b.y,c.y}),maxY=std::max({a.y,b.y,c.y});
    for(int y=(int)minY;y<=(int)maxY;++y){
        float fy=(float)y; float xs[2]; int nx=0;
        auto edgeX=[&](V2 p0,V2 p1){
            if((p0.y<=fy&&p1.y>fy)||(p1.y<=fy&&p0.y>fy)){
                float t=(fy-p0.y)/(p1.y-p0.y);
                if(nx<2)xs[nx++]=p0.x+t*(p1.x-p0.x);
            }
        };
        edgeX(a,b);edgeX(b,c);edgeX(c,a);
        if(nx==2){
            int x0=(int)std::min(xs[0],xs[1]),x1=(int)std::max(xs[0],xs[1]);
            SDL_Rect line={x0,y,x1-x0+1,1}; SDL_RenderFillRect(r,&line);
        }
    }
}
void quadC(SDL_Renderer* r,V2 a,V2 b,V2 c,V2 d,SDL_Color ca,SDL_Color cb,SDL_Color cc,SDL_Color cd){
    if(g_hasRenderGeometry){
        SDL_Vertex v[4]={{{a.x,a.y},ca,{0,0}},{{b.x,b.y},cb,{0,0}},{{c.x,c.y},cc,{0,0}},{{d.x,d.y},cd,{0,0}}};
        int i[]={0,1,2,1,2,3}; g_renderGeometry(r,nullptr,v,4,i,6);
        return;
    }
    triC(r,a,b,c,ca,cb,cc); triC(r,a,c,d,ca,cc,cd);
}
void stroke(SDL_Renderer* r,V2 p0,V2 p1,float lw,SDL_Color c0,SDL_Color c1){
    if(vlen(p1-p0)<0.3f)return;
    V2 d=vnorm(p1-p0),s=perp(d);
    if(g_hasRenderGeometry){
        quadC(r,p0+s*lw,p0-s*lw,p1+s*lw,p1-s*lw,c0,c0,c1,c1);
        return;
    }
    SDL_Color avg={(Uint8)((c0.r+c1.r)/2),(Uint8)((c0.g+c1.g)/2),
                   (Uint8)((c0.b+c1.b)/2),(Uint8)((c0.a+c1.a)/2)};
    SDL_SetRenderDrawColor(r,avg.r,avg.g,avg.b,avg.a);
    SDL_RenderDrawLine(r,(int)p0.x,(int)p0.y,(int)p1.x,(int)p1.y);
    int extra=(int)(lw*0.5f);
    for(int i=1;i<=extra&&i<=3;++i){
        SDL_RenderDrawLine(r,(int)(p0.x+s.x*i),(int)(p0.y+s.y*i),(int)(p1.x+s.x*i),(int)(p1.y+s.y*i));
        SDL_RenderDrawLine(r,(int)(p0.x-s.x*i),(int)(p0.y-s.y*i),(int)(p1.x-s.x*i),(int)(p1.y-s.y*i));
    }
}
void softCircle(SDL_Renderer* r,V2 c,float rad,SDL_Color col,SDL_Color edge,int seg=16){
    if(g_hasRenderGeometry){
        for(int i=0;i<seg;++i){
            float a0=(float)i/seg*2*(float)M_PI,a1=(float)(i+1)/seg*2*(float)M_PI;
            triC(r,c,{c.x+cosf(a0)*rad,c.y+sinf(a0)*rad},{c.x+cosf(a1)*rad,c.y+sinf(a1)*rad},col,edge,edge);
        }
        return;
    }
    SDL_SetRenderDrawColor(r,col.r,col.g,col.b,col.a);
    for(int y=(int)(c.y-rad);y<=(int)(c.y+rad);++y){
        float dy=y-c.y; if(fabsf(dy)>rad)continue;
        float dx=sqrtf(rad*rad-dy*dy);
        SDL_Rect line={(int)(c.x-dx),(int)y,(int)(dx*2)+1,1};
        SDL_RenderFillRect(r,&line);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// PSYCHEDELIC FEEDBACK RENDERER
// ═══════════════════════════════════════════════════════════════════════════════
void blitFeedback(SDL_Renderer* r,SDL_Texture* tex,float cx,float cy,
                  float zoom,float rotation,float alpha,float hueShift,int ww,int wh){
    float hw=ww*0.5f*zoom,hh=wh*0.5f*zoom;
    SDL_Color tint=thsv(hueShift,0.12f,1.f);
    SDL_SetTextureColorMod(tex,tint.r,tint.g,tint.b);
    SDL_SetTextureAlphaMod(tex,(Uint8)(alpha*255));
    SDL_SetTextureBlendMode(tex,SDL_BLENDMODE_ADD);
    // Use SDL_RenderCopyEx for rotation+scale — works on all SDL2 versions
    SDL_Rect dst={(int)(cx-hw),(int)(cy-hh),(int)(hw*2),(int)(hh*2)};
    SDL_RenderCopyEx(r,tex,nullptr,&dst,rotation*180.0/M_PI,nullptr,SDL_FLIP_NONE);
}

// ═══════════════════════════════════════════════════════════════════════════════
// BAND-SPECIFIC EFFECTS (unchanged from original)
// ═══════════════════════════════════════════════════════════════════════════════

void drawSubBassMembrane(SDL_Renderer* r,float cx,float cy,float t,
                         float sub,float bass,float maxR,int ww,int wh){
    if(sub<0.03f&&bass<0.05f)return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    float intensity=sub*0.7f+bass*0.3f;
    int rings=20;
    for(int ri=0;ri<rings;++ri){
        float rt=(float)ri/rings;
        float phase=fmod(rt+t*0.25f*(1.f+sub*2.f),1.f);
        float rad=phase*maxR*1.5f;
        float thick=25.f+sub*80.f+bass*40.f;
        float alpha=(1.f-phase)*(1.f-phase)*intensity*85.f;
        float hue=fmod(t*6.f+ri*18.f,360.f);
        SDL_Color c=thsv(hue,1.f,1.f);c.a=(Uint8)std::min(alpha,255.f);
        SDL_Color ce=c;ce.a=0;
        int segs=40;
        for(int i=0;i<segs;++i){
            float a0=(float)i/segs*2*(float)M_PI;
            float a1=(float)(i+1)/segs*2*(float)M_PI;
            float d0=rad+sinf(a0*4+t*2.f)*sub*maxR*0.08f;
            float d1=rad+sinf(a1*4+t*2.f)*sub*maxR*0.08f;
            V2 p0i={cx+cosf(a0)*(d0-thick),cy+sinf(a0)*(d0-thick)};
            V2 p0o={cx+cosf(a0)*(d0+thick),cy+sinf(a0)*(d0+thick)};
            V2 p1i={cx+cosf(a1)*(d1-thick),cy+sinf(a1)*(d1-thick)};
            V2 p1o={cx+cosf(a1)*(d1+thick),cy+sinf(a1)*(d1+thick)};
            quadC(r,p0i,p0o,p1i,p1o,ce,c,ce,c);
        }
    }
}

static float g_lastBass=0;
static std::vector<std::pair<float,float>> g_shocks;
void triggerShockwave(float bass,float t){
    if(bass>0.55f&&g_lastBass<0.55f)
        g_shocks.push_back({0.f,fmod(t*60.f,360.f)});
    g_lastBass=bass;
}
void updateShockwaves(float dt){
    for(auto& s:g_shocks)s.first+=dt*600.f;
    g_shocks.erase(std::remove_if(g_shocks.begin(),g_shocks.end(),
        [](auto& s){return s.first>1200.f;}),g_shocks.end());
}
void drawShockwaves(SDL_Renderer* r,float cx,float cy,float bass){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    for(auto& s:g_shocks){
        float alpha=std::max(0.f,1.f-s.first/1200.f);alpha*=alpha;
        for(int layer=0;layer<5;++layer){
            float lrad=s.first*(1.f+layer*0.015f);
            float lw=5.f-layer*0.8f;
            SDL_Color c=thsv(s.second+layer*20.f,0.7f,1.f);
            c.a=(Uint8)(alpha*(1.f-layer*0.18f)*160);
            int segs=80;
            for(int i=0;i<segs;++i){
                float a0=(float)i/segs*2*(float)M_PI;
                float a1=(float)(i+1)/segs*2*(float)M_PI;
                stroke(r,{cx+cosf(a0)*lrad,cy+sinf(a0)*lrad},
                         {cx+cosf(a1)*lrad,cy+sinf(a1)*lrad},lw,c,c);
            }
        }
    }
}

void drawBeatCube(SDL_Renderer* r,float cx,float cy,float t,
                  float bass,float overall){
    if(bass<0.08f)return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    float size=(80.f+bass*200.f+overall*60.f);
    float rx=t*0.4f*(1.f+bass*0.8f);
    float ry=t*0.55f*(1.f+bass*0.6f);
    float rz=t*0.3f*(1.f+bass*0.4f);
    float pts[8][3]={
        {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1}
    };
    float px[8],py[8];
    for(int i=0;i<8;++i){
        float x=pts[i][0],y=pts[i][1],z=pts[i][2];
        float y2=y*cosf(rx)-z*sinf(rx);float z2=y*sinf(rx)+z*cosf(rx);y=y2;z=z2;
        float x2=x*cosf(ry)+z*sinf(ry);float z3=-x*sinf(ry)+z*cosf(ry);x=x2;z=z3;
        float x3=x*cosf(rz)-y*sinf(rz);float y3=x*sinf(rz)+y*cosf(rz);x=x3;y=y3;
        float fov=3.5f;
        float scale=size*fov/(fov+z*0.5f+1.5f);
        px[i]=cx+x*scale;py[i]=cy+y*scale;
    }
    int edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for(auto& e:edges){
        float hue=fmod(t*55.f+e[0]*45.f,360.f);
        SDL_Color c=thsv(hue,0.8f,1.f);c.a=(Uint8)((0.2f+bass*0.45f)*255);
        stroke(r,{px[e[0]],py[e[0]]},{px[e[1]],py[e[1]]},1.5f+bass*3.f,c,c);
    }
}

void drawSineWaves(SDL_Renderer* r,float cx,float cy,float t,
                   float lowMid,float bass,float overall,int ww,int wh){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    int waves=18;
    for(int w=0;w<waves;++w){
        float wt=(float)w/waves;
        float yBase=(float)wh*wt;
        float amp=(wh*0.04f)+(lowMid*wh*0.12f)+(bass*wh*0.06f*sinf(t*1.5f+w));
        float freq=2.f+wt*4.f+lowMid*3.f;
        float speed=t*(0.4f+wt*0.6f)*(w%2?1.f:-1.f);
        float hue=fmod(t*15.f+w*20.f,360.f);
        float alpha=(0.04f+lowMid*0.08f+overall*0.04f)*255;
        SDL_Color c=thsv(hue,0.9f,1.f);c.a=(Uint8)alpha;
        int pts=60;
        for(int p=0;p<pts-1;++p){
            float x0=(float)p/pts*(float)ww;
            float x1=(float)(p+1)/pts*(float)ww;
            float y0=yBase+sinf(x0/ww*(float)M_PI*2.f*freq+speed)*amp;
            float y1=yBase+sinf(x1/ww*(float)M_PI*2.f*freq+speed)*amp;
            stroke(r,{x0,y0},{x1,y1},1.2f+lowMid*2.5f,c,c);
        }
    }
}

void drawMegaLissajous(SDL_Renderer* r,float cx,float cy,float t,
                       float mid,float lowMid,float overall,float maxR){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    struct LParam{float fA,fB,phA,phB,scale,hueOff;};
    LParam lp[]={
        {3.f+mid*2.f,  2.f+mid,      t*0.4f,  t*0.3f,  0.85f, 0.f},
        {5.f+mid*1.5f, 4.f+mid*0.8f, t*0.35f, t*0.25f, 0.7f,  120.f},
        {2.f+mid*3.f,  3.f+mid*2.f,  t*0.5f,  t*0.4f,  0.6f,  240.f},
        {7.f+lowMid,   6.f+lowMid,   t*0.2f,  t*0.15f, 0.45f, 60.f},
    };
    int pts=300;
    for(auto& lpar:lp){
        for(int p=0;p<pts-1;++p){
            float t0=(float)p/pts*2*(float)M_PI;
            float t1=(float)(p+1)/pts*2*(float)M_PI;
            float x0=cx+sinf(lpar.fA*t0+lpar.phA)*maxR*lpar.scale;
            float y0=cy+sinf(lpar.fB*t0+lpar.phB)*maxR*lpar.scale;
            float x1=cx+sinf(lpar.fA*t1+lpar.phA)*maxR*lpar.scale;
            float y1=cy+sinf(lpar.fB*t1+lpar.phB)*maxR*lpar.scale;
            float hue=fmod(t*40.f+(float)p*1.2f+lpar.hueOff,360.f);
            SDL_Color c=thsv(hue,0.9f,1.f);
            c.a=(Uint8)((0.05f+mid*0.15f+overall*0.06f)*255);
            stroke(r,{x0,y0},{x1,y1},1.f+mid*2.5f,c,c);
        }
    }
}

void drawFractalBurst(SDL_Renderer* r,float cx,float cy,float t,
                      float highMid,float mid,float maxR){
    if(highMid<0.04f)return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    float spin=t*(0.3f+highMid*0.8f);
    for(int gen=0;gen<3;++gen){
        float gScale=1.f-gen*0.3f;
        float gSpin=spin*(gen+1)*(gen%2?1.f:-1.f);
        int arms=5+gen*3;
        float r0=30.f*gScale, r1=maxR*0.65f*gScale*(1.f+highMid*0.4f);
        for(int arm=0;arm<arms;++arm){
            float baseAng=(float)arm/arms*2*(float)M_PI+gSpin;
            for(int sub=0;sub<3;++sub){
                float subAng=baseAng+(sub-1)*0.15f*(1.f+mid*0.3f);
                float subR=sub==0?r1:r1*0.6f;
                V2 p0={cx+cosf(subAng)*r0,cy+sinf(subAng)*r0};
                V2 p1={cx+cosf(subAng)*subR,cy+sinf(subAng)*subR};
                float hue=fmod(t*60.f+gen*120.f+arm*37.f+sub*25.f,360.f);
                float alpha=(0.07f+highMid*0.25f+mid*0.1f)*255;
                SDL_Color tip=thsv(hue,0.85f,1.f);tip.a=(Uint8)alpha;
                SDL_Color root=tip;root.a=(Uint8)(alpha*0.1f);
                stroke(r,p0,p1,1.f+highMid*3.f-gen*0.5f,root,tip);
                if(sub==0&&arm<arms-1){
                    float nextAng=(float)(arm+1)/arms*2*(float)M_PI+gSpin;
                    float midAng=(baseAng+nextAng)*0.5f;
                    float midR=r1*0.5f;
                    V2 m0={cx+cosf(midAng)*r0*2,cy+sinf(midAng)*r0*2};
                    V2 m1={cx+cosf(midAng)*midR,cy+sinf(midAng)*midR};
                    SDL_Color mc=thsv(hue+30.f,0.7f,1.f);mc.a=(Uint8)(alpha*0.4f);
                    stroke(r,m0,m1,0.8f+highMid*1.5f,mc,{mc.r,mc.g,mc.b,0});
                }
            }
        }
    }
}

struct Neuron{float x,y,vx,vy,hue,phase;};
static std::vector<Neuron> g_neurons;
static bool g_neuronsInit=false;
void initNeurons(float cx,float cy,float maxR,std::mt19937& rng){
    std::uniform_real_distribution<float> U(0,1);
    g_neurons.resize(20);
    for(auto& n:g_neurons){
        float a=U(rng)*2*(float)M_PI;
        float r=U(rng)*maxR*0.7f;
        n.x=cx+cosf(a)*r;n.y=cy+sinf(a)*r;
        n.vx=(U(rng)-.5f)*40.f;n.vy=(U(rng)-.5f)*40.f;
        n.hue=U(rng)*360.f;n.phase=U(rng)*6.28f;
    }
    g_neuronsInit=true;
}
void updateNeurons(float dt,float cx,float cy,float high,float bass,float maxR,float t){
    for(auto& n:g_neurons){
        n.phase+=dt*(0.5f+high*2.f);
        n.vx+=(sinf(n.phase*1.3f)*20.f-n.vx*0.1f)*dt*(1.f+high);
        n.vy+=(cosf(n.phase*1.1f)*20.f-n.vy*0.1f)*dt*(1.f+high);
        n.vx+=((cx-n.x)*0.02f)*dt;n.vy+=((cy-n.y)*0.02f)*dt;
        n.vx*=0.98f;n.vy*=0.98f;
        n.x+=n.vx*dt*(1.f+bass);n.y+=n.vy*dt*(1.f+bass);
        n.hue=fmod(n.hue+50.f*dt,360.f);
        float dx=n.x-cx,dy=n.y-cy;
        if(sqrtf(dx*dx+dy*dy)>maxR*0.8f){n.vx*=-0.8f;n.vy*=-0.8f;}
    }
}
void drawNeuralNet(SDL_Renderer* r,float high,float highMid,float bass){
    if(high<0.03f&&highMid<0.04f)return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    float connDist=200.f+high*150.f+bass*80.f;
    for(int i=0;i<(int)g_neurons.size();++i){
        for(int j=i+1;j<(int)g_neurons.size();++j){
            float dx=g_neurons[i].x-g_neurons[j].x;
            float dy=g_neurons[i].y-g_neurons[j].y;
            float dist=sqrtf(dx*dx+dy*dy);
            if(dist>connDist)continue;
            float strength=1.f-dist/connDist;
            float hue=(g_neurons[i].hue+g_neurons[j].hue)*0.5f;
            SDL_Color c=thsv(hue,0.7f,1.f);
            c.a=(Uint8)(strength*strength*(0.08f+high*0.22f+highMid*0.12f)*255);
            stroke(r,{g_neurons[i].x,g_neurons[i].y},{g_neurons[j].x,g_neurons[j].y},
                   0.8f+strength*2.5f,c,c);
        }
    }
    for(auto& n:g_neurons){
        float r2=3.f+high*8.f+bass*6.f;
        SDL_Color c=thsv(n.hue,0.5f,1.f);c.a=(Uint8)((0.4f+high*0.6f)*255);
        softCircle(r,{n.x,n.y},r2,c,{c.r,c.g,c.b,0},12);
        SDL_Color wh={255,255,255,(Uint8)((0.5f+high*0.5f)*255)};
        softCircle(r,{n.x,n.y},r2*0.3f,wh,{255,255,255,0},8);
    }
}

void drawTunnel(SDL_Renderer* r,float cx,float cy,float t,
                float bass,float overall,float maxR){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    int layers=16;
    float globalSpin=t*0.12f*(1.f+overall*0.4f);
    for(int li=0;li<layers;++li){
        float phase=fmod((float)li/layers+t*0.4f*(1.f+bass*0.8f),1.f);
        float scale=phase*maxR*1.4f;
        float alpha=(1.f-phase)*(1.f-phase)*0.35f*(1.f+overall)*255;
        float hue=fmod(t*22.f+li*22.5f,360.f);
        SDL_Color c=thsv(hue,0.9f,1.f);c.a=(Uint8)std::min(alpha,255.f);
        SDL_Color ce=c;ce.a=0;
        SDL_Color cwedge=c;cwedge.a=(Uint8)std::min(alpha*0.04f,12.f);
        float spin=globalSpin+li*0.2f*(li%2?1.f:-1.f);
        int sides=3+(li%6);
        for(int i=0;i<sides;++i){
            float a0=(float)i/sides*2*(float)M_PI+spin;
            float a1=(float)(i+1)/sides*2*(float)M_PI+spin;
            float warp=scale*(1.f+overall*0.08f*sinf(a0*3+t*3.f));
            V2 p0={cx+cosf(a0)*warp,cy+sinf(a0)*warp};
            V2 p1={cx+cosf(a1)*warp*1.f,cy+sinf(a1)*warp};
            stroke(r,p0,p1,2.f+overall*3.f,c,c);
            triC(r,{cx,cy},p0,p1,ce,cwedge,cwedge);
        }
    }
}

void drawRibbonTornado(SDL_Renderer* r,float cx,float cy,float t,
                       float bass,float mid,float overall,float maxR){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    int ribbons=8;
    int pts=80;
    for(int ri=0;ri<ribbons;++ri){
        float rPhase=(float)ri/ribbons*2*(float)M_PI;
        float spinRate=0.5f+ri*0.12f;
        float hueBase=fmod(t*35.f+ri*45.f,360.f);
        V2 prev={0,0}; bool hasPrev=false;
        for(int p=0;p<pts;++p){
            float tf=(float)p/pts;
            float angle=rPhase+tf*4*(float)M_PI+t*spinRate*(ri%2?1.f:-1.f);
            float rad=tf*maxR*0.85f
                     +mid*maxR*0.15f*sinf(tf*8+t*1.5f+ri);
            float zOff=sinf(tf*3*(float)M_PI+t*0.8f+rPhase)*0.4f;
            float perspScale=1.f/(1.5f-zOff*0.4f);
            V2 cur={cx+cosf(angle)*rad*perspScale,
                    cy+sinf(angle)*rad*perspScale*0.6f+zOff*80.f};
            if(hasPrev){
                float hue=fmod(hueBase+tf*90.f,360.f);
                float alpha=(0.12f+overall*0.2f+bass*0.15f)*(1.f-tf*0.4f)*255;
                SDL_Color c=thsv(hue,0.85f,1.f);c.a=(Uint8)std::min(alpha,255.f);
                SDL_Color ce=c;ce.a=(Uint8)(c.a/5);
                float lw=(3.f+mid*6.f+bass*5.f)*(1.f-tf*0.5f);
                stroke(r,prev,cur,lw,ce,c);
            }
            prev=cur;hasPrev=true;
        }
    }
}

void drawAurora(SDL_Renderer* r,float cx,float cy,float t,
                float mid,float lowMid,float overall,int ww,int wh){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    int curtains=12;
    for(int ci=0;ci<curtains;++ci){
        float ct=(float)ci/curtains;
        float xBase=ct*(float)ww;
        float sway=sinf(t*0.3f+ci*0.7f)*ww*0.06f
                  +cosf(t*0.5f+ci*0.4f)*ww*0.03f*mid;
        float width=20.f+lowMid*60.f+mid*40.f+sinf(t*0.8f+ci)*20.f;
        float hue=fmod(t*12.f+ci*30.f,360.f);
        int strips=6;
        for(int si=0;si<strips;++si){
            float st=(float)si/strips;
            float x=xBase+sway+st*width;
            float alpha=(sinf(st*(float)M_PI))*(0.04f+mid*0.1f+lowMid*0.08f)*255;
            SDL_Color top=thsv(hue+si*8.f,0.75f,1.f);top.a=(Uint8)std::min(alpha,255.f);
            SDL_Color bot=top;bot.a=0;
            int segs=12;
            for(int seg=0;seg<segs;++seg){
                float y0=(float)seg/segs*(float)wh;
                float y1=(float)(seg+1)/segs*(float)wh;
                float wave0=sinf(y0/wh*4*(float)M_PI+t*0.6f+ci)*sway*0.3f;
                float wave1=sinf(y1/wh*4*(float)M_PI+t*0.6f+ci)*sway*0.3f;
                float aFrac=sinf((float)seg/segs*(float)M_PI);
                SDL_Color sc=top;sc.a=(Uint8)(top.a*aFrac);
                quadC(r,{x+wave0,y0},{x+wave0+width*0.15f,y0},
                         {x+wave1,y1},{x+wave1+width*0.15f,y1},
                         bot,sc,bot,sc);
            }
        }
    }
}

void drawHypnoSpiral(SDL_Renderer* r,float cx,float cy,float t,
                     float mid,float bass,float overall,float maxR){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    int arms=4;
    int pts=400;
    float spinRate=t*0.25f*(1.f+overall*0.5f);
    for(int arm=0;arm<arms;++arm){
        float armOff=(float)arm/arms*2*(float)M_PI;
        for(int p=0;p<pts-1;++p){
            float tf0=(float)p/pts,tf1=(float)(p+1)/pts;
            float theta0=tf0*6*(float)M_PI+spinRate+armOff;
            float theta1=tf1*6*(float)M_PI+spinRate+armOff;
            float srad0=tf0*maxR*0.9f;
            float srad1=tf1*maxR*0.9f;
            V2 p0={cx+cosf(theta0)*srad0,cy+sinf(theta0)*srad0};
            V2 p1={cx+cosf(theta1)*srad1,cy+sinf(theta1)*srad1};
            float hue=fmod(t*20.f+tf0*120.f+arm*90.f,360.f);
            float alpha=(0.05f+mid*0.12f+overall*0.06f)*(1.f-tf0*0.5f)*255;
            SDL_Color c=thsv(hue,0.9f,1.f);c.a=(Uint8)std::min(alpha,255.f);
            stroke(r,p0,p1,1.f+mid*2.5f+bass*2.f,c,c);
        }
    }
}

void drawWormhole(SDL_Renderer* r,float cx,float cy,float t,
                  float bass,float mid,float overall,float maxR){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    int rings=20;
    float tilt=sinf(t*0.2f)*(float)M_PI*0.35f+mid*0.3f;
    float tiltY=cosf(t*0.15f)*(float)M_PI*0.2f;
    float warpSpin=t*0.3f;
    for(int ri=0;ri<rings;++ri){
        float phase=fmod((float)ri/rings+t*0.5f*(1.f+bass*0.6f),1.f);
        float rad=phase*maxR*1.2f;
        float squash=0.45f+mid*0.2f+cosf(tilt)*0.35f;
        float alpha=(1.f-phase)*(1.f-phase)*(0.1f+overall*0.15f)*255;
        float hue=fmod(t*18.f+ri*18.f,360.f);
        SDL_Color c=thsv(hue,0.85f,1.f);c.a=(Uint8)std::min(alpha,255.f);
        int segs=48;
        float spinOff=warpSpin+ri*0.1f*(ri%2?1.f:-1.f);
        for(int i=0;i<segs;++i){
            float a0=(float)i/segs*2*(float)M_PI+spinOff;
            float a1=(float)(i+1)/segs*2*(float)M_PI+spinOff;
            float x0=cosf(a0)*rad,z0=sinf(a0)*rad;
            float x1=cosf(a1)*rad,z1=sinf(a1)*rad;
            float px0=cx+x0*cosf(tilt)-z0*sinf(tilt)*squash;
            float py0=cy+x0*sinf(tiltY)+z0*cosf(tilt)*squash;
            float px1=cx+x1*cosf(tilt)-z1*sinf(tilt)*squash;
            float py1=cy+x1*sinf(tiltY)+z1*cosf(tilt)*squash;
            stroke(r,{px0,py0},{px1,py1},1.5f+overall*2.f,c,c);
        }
    }
}

struct RainDrop{float x,y,speed,hue,brightness;int col;};
static std::vector<RainDrop> g_rain;
static bool g_rainInit=false;
void initRain(int ww,int wh,std::mt19937& rng){
    std::uniform_real_distribution<float> U(0,1);
    int cols=ww/14;g_rain.resize(cols);
    for(int i=0;i<cols;++i){
        g_rain[i]={14.f*i+(float)(rand()%14),U(rng)*(float)wh,
                   40.f+U(rng)*120.f,fmod(U(rng)*360.f,360.f),U(rng),i};
    }
    g_rainInit=true;
}
void updateRain(float dt,float high,float overall,int wh){
    for(auto& d:g_rain){
        d.y+=d.speed*(1.f+high*3.f+overall)*dt;
        if(d.y>(float)wh+20.f)d.y=-20.f;
        d.hue=fmod(d.hue+30.f*dt,360.f);
    }
}
void drawRain(SDL_Renderer* r,float high,float overall,int wh){
    if(high<0.03f&&overall<0.05f)return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    float baseAlpha=(0.03f+high*0.1f+overall*0.04f);
    for(auto& d:g_rain){
        int trailLen=8;
        for(int tr=0;tr<trailLen;++tr){
            float ty=d.y-(float)tr*14.f;
            float tf=1.f-(float)tr/trailLen;
            float a=tf*tf*baseAlpha*255;
            SDL_Color c=thsv(d.hue,0.7f,1.f);c.a=(Uint8)std::min(a,255.f);
            SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a);
            SDL_Rect dot={(int)d.x,(int)ty,5,8};
            SDL_RenderFillRect(r,&dot);
        }
    }
}

void drawGalaxy(SDL_Renderer* r,float cx,float cy,float t,
                float bass,float mid,float high,float maxR){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    int arms=4,pts=150;
    float spinRate=t*0.08f;
    for(int arm=0;arm<arms;++arm){
        float armBase=(float)arm/arms*2*(float)M_PI;
        for(int p=0;p<pts-1;++p){
            float tf0=(float)p/pts,tf1=(float)(p+1)/pts;
            float theta0=tf0*5*(float)M_PI+armBase+spinRate;
            float theta1=tf1*5*(float)M_PI+armBase+spinRate;
            float grad0=expf(tf0*1.8f)*(12.f+bass*8.f);
            float grad1=expf(tf1*1.8f)*(12.f+bass*8.f);
            if(grad0>maxR*0.9f||grad1>maxR*0.9f)break;
            float scatter=sinf(tf0*20+t*3+arm)*15.f*mid;
            float perpAng=theta0+(float)M_PI_2;
            V2 p0={cx+cosf(theta0)*grad0+cosf(perpAng)*scatter,
                   cy+sinf(theta0)*grad0+sinf(perpAng)*scatter};
            V2 p1={cx+cosf(theta1)*grad1,cy+sinf(theta1)*grad1};
            float hue=fmod(t*25.f+arm*90.f+tf0*60.f,360.f);
            float alpha=(0.06f+high*0.12f+mid*0.08f)*(1.f-tf0*0.3f)*255;
            SDL_Color c=thsv(hue,0.8f,1.f);c.a=(Uint8)std::min(alpha,255.f);
            float lw=1.f+high*2.f+(1.f-tf0)*bass*3.f;
            stroke(r,p0,p1,lw,c,c);
        }
    }
}

void drawSpectrumRing(SDL_Renderer* r,float cx,float cy,float t,
                      float bass,float mid,float high,float overall,
                      const std::vector<float>& spec,float maxR){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    int bars=256;
    float innerR=60.f+bass*25.f;
    for(int b=0;b<bars;++b){
        float bt=(float)b/bars;
        float ang=bt*2*(float)M_PI - (float)M_PI_2 + t*0.08f*(1.f+overall*0.3f);
        int fi=std::clamp((int)(bt*FREQ_BINS*0.55f),0,FREQ_BINS-1);
        float val=spec[fi];
        float r0=innerR;
        float r1=innerR+val*(160.f+bass*140.f+mid*60.f);
        V2 lp0={cx+cosf(ang)*r0,cy+sinf(ang)*r0};
        V2 lp1={cx+cosf(ang)*r1,cy+sinf(ang)*r1};
        float hue=fmod(t*35.f+bt*360.f,360.f);
        SDL_Color tip =thsv(hue,0.9f,1.f); tip.a =(Uint8)((0.18f+val*0.65f)*255);
        SDL_Color root=thsv(hue+40,0.6f,0.5f); root.a=(Uint8)(tip.a/8);
        stroke(r,lp0,lp1,1.2f+val*3.5f,root,tip);
    }
    for(int b=0;b<bars;++b){
        float bt=(float)b/bars;
        float ang=bt*2*(float)M_PI - (float)M_PI_2 + t*0.08f*(1.f+overall*0.3f);
        int fi=std::clamp((int)(bt*FREQ_BINS*0.55f),0,FREQ_BINS-1);
        float val=spec[fi]*0.6f;
        float r0=innerR;
        float r1=innerR-val*(50.f+bass*40.f);
        r1=std::max(r1,5.f);
        V2 lp0={cx+cosf(ang)*r0,cy+sinf(ang)*r0};
        V2 lp1={cx+cosf(ang)*r1,cy+sinf(ang)*r1};
        float hue=fmod(t*35.f+bt*360.f+180.f,360.f);
        SDL_Color c=thsv(hue,0.85f,1.f); c.a=(Uint8)((0.1f+val*0.4f)*255);
        stroke(r,lp0,lp1,0.8f+val*2.f,c,c);
    }
    int segs=128;
    for(int i=0;i<segs;++i){
        float a0=(float)i/segs*2*(float)M_PI+t*0.08f;
        float a1=(float)(i+1)/segs*2*(float)M_PI+t*0.08f;
        float hue=fmod(t*20.f+(float)i/segs*360.f,360.f);
        SDL_Color c=thsv(hue,0.8f,1.f); c.a=(Uint8)((0.15f+overall*0.2f)*255);
        stroke(r,{cx+cosf(a0)*innerR,cy+sinf(a0)*innerR},
                 {cx+cosf(a1)*innerR,cy+sinf(a1)*innerR},1.f+overall*1.5f,c,c);
    }
}

struct GeoShape{float x,y,angle,spinSpd,size,hue,orbitR,orbitAng,orbitSpd;int sides;};
static std::vector<GeoShape> g_geo;
static bool g_geoInit=false;
void initGeo(float cx,float cy,float maxR,std::mt19937& rng){
    std::uniform_real_distribution<float> U(0,1);
    g_geo.resize(40);
    for(int i=0;i<40;++i){
        auto& g=g_geo[i];
        g.orbitR  =maxR*(0.05f+U(rng)*0.88f);
        g.orbitAng=U(rng)*2*(float)M_PI;
        g.orbitSpd=(0.1f+U(rng)*0.5f)*(i%2?1.f:-1.f);
        g.angle   =U(rng)*6.28f;
        g.spinSpd =(0.3f+U(rng)*1.5f)*(i%3==0?1.f:-1.f);
        g.size    =8.f+U(rng)*28.f;
        g.hue     =U(rng)*360.f;
        g.sides   =3+i%6;
        g.x=cx+cosf(g.orbitAng)*g.orbitR;
        g.y=cy+sinf(g.orbitAng)*g.orbitR;
    }
    g_geoInit=true;
}
void updateGeo(float dt,float cx,float cy,float bass,float overall){
    for(auto& g:g_geo){
        g.orbitAng+=dt*g.orbitSpd*(1.f+overall*0.5f);
        g.angle   +=dt*g.spinSpd;
        g.hue=fmod(g.hue+25.f*dt,360.f);
        g.x=cx+cosf(g.orbitAng)*g.orbitR;
        g.y=cy+sinf(g.orbitAng)*g.orbitR;
    }
}
void drawGeoShapes(SDL_Renderer* r,float bass,float mid,float overall){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    for(auto& g:g_geo){
        float sz=g.size*(1.f+bass*0.4f+mid*0.2f);
        float alpha=(0.1f+overall*0.2f+mid*0.15f)*255;
        SDL_Color edge=thsv(g.hue,0.85f,1.f); edge.a=(Uint8)std::min(alpha,255.f);
        SDL_Color fill=edge; fill.a=(Uint8)(edge.a*0.08f);
        for(int p=0;p<g.sides;++p){
            float a0=(float)p/g.sides*2*(float)M_PI+g.angle;
            float a1=(float)(p+1)/g.sides*2*(float)M_PI+g.angle;
            V2 p0={g.x+cosf(a0)*sz,g.y+sinf(a0)*sz};
            V2 p1={g.x+cosf(a1)*sz,g.y+sinf(a1)*sz};
            triC(r,{g.x,g.y},p0,p1,fill,edge,edge);
            stroke(r,p0,p1,0.8f+mid*1.5f,edge,edge);
        }
        float innerSz=sz*0.45f;
        float innerRot=g.angle+(float)M_PI/g.sides;
        SDL_Color inner=thsv(fmod(g.hue+60,360.f),0.8f,1.f);
        inner.a=(Uint8)(edge.a*0.6f);
        for(int p=0;p<g.sides;++p){
            float a0=(float)p/g.sides*2*(float)M_PI+innerRot;
            float a1=(float)(p+1)/g.sides*2*(float)M_PI+innerRot;
            V2 p0={g.x+cosf(a0)*innerSz,g.y+sinf(a0)*innerSz};
            V2 p1={g.x+cosf(a1)*innerSz,g.y+sinf(a1)*innerSz};
            stroke(r,p0,p1,0.6f+overall,inner,inner);
        }
    }
}

// KALEIDOSCOPE CORE
static constexpr int KNODES=8;
struct KNode{
    V2    pos;
    float hue,hueSpd;
    float orbitRadius;
    float orbitAngle;
    float orbitSpeed;
    float orbitEcc;
    float wobbleAmp;
    float wobbleFreq;
    float wobblePhase;
    int   trailLen;
    std::deque<V2> trail;
};
static KNode g_kn[KNODES];
void initKNodes(float maxR,std::mt19937& rng){
    std::uniform_real_distribution<float> U(0,1);
    for(int i=0;i<KNODES;++i){
        auto& n=g_kn[i];
        float it=(float)i/KNODES;
        n.orbitRadius  = maxR*(0.08f+it*0.72f);
        n.orbitAngle   = U(rng)*2*(float)M_PI;
        n.orbitSpeed   = (0.25f+U(rng)*0.6f)*(i%2?1.f:-1.f);
        n.orbitEcc     = 0.05f+U(rng)*0.25f;
        n.wobbleAmp    = maxR*(0.02f+U(rng)*0.06f);
        n.wobbleFreq   = 2.f+U(rng)*4.f;
        n.wobblePhase  = U(rng)*6.28f;
        n.hue          = U(rng)*360.f;
        n.hueSpd       = 20.f+U(rng)*60.f;
        n.trailLen     = 80+(int)(U(rng)*60.f);
        float r=n.orbitRadius*(1.f+n.orbitEcc*cosf(n.orbitAngle));
        float a=n.orbitAngle/SANG;
        n.pos={r, clamp01(fmod(a,1.f))};
    }
}
void updateKNodes(float dt,float bass,float mid,float overall,float maxR){
    for(auto& n:g_kn){
        n.orbitAngle+=dt*n.orbitSpeed*(1.f+overall*0.6f);
        n.wobblePhase+=dt*n.wobbleFreq;
        n.hue=fmod(n.hue+n.hueSpd*dt,360.f);
        float r=n.orbitRadius*(1.f+n.orbitEcc*cosf(n.orbitAngle))
               +n.wobbleAmp*sinf(n.wobblePhase)*(1.f+mid*0.5f);
        r=std::max(r,5.f);
        float sliceAngle=fmod(n.orbitAngle/(2*(float)M_PI),1.f);
        if(sliceAngle<0)sliceAngle+=1.f;
        n.pos={r,sliceAngle};
        n.trail.push_front(n.pos);
        if((int)n.trail.size()>n.trailLen)n.trail.pop_back();
    }
}
V2 s2c(V2 p){float a=p.y*SANG;return{cosf(a)*p.x,sinf(a)*p.x};}

void drawKaleidoscope(SDL_Renderer* ren,float cx,float cy,
                      float bass,float mid,float high,float overall,
                      float t,const std::vector<float>& spec,float maxR){
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_ADD);
    // Always rotate clockwise — no direction reversal
    float gspin=t*0.06f;

    // Bass shockwave scale
    static float g_kBassShock=0.f;
    static float g_kLastBass=0.f;
    if(bass>0.55f&&g_kLastBass<=0.55f) g_kBassShock=1.f;
    g_kLastBass=bass;
    g_kBassShock=std::max(0.f,g_kBassShock-0.04f);
    float shockScale=1.f+g_kBassShock*0.18f;

    for(int s=0;s<KSLICES;++s){
        float base=s*SANG+gspin; bool mir=(s%2==1);
        auto W=[&](V2 p)->V2{
            p={p.x*shockScale,p.y*shockScale};
            if(mir)p={p.x,-p.y};
            return vrot(p,base)+V2{cx,cy};
        };

        // 1. SPECTRUM ARCS — segments along circular arcs at different radii
        // Each arc sits at a radius driven by that frequency bin
        // This makes the spokes form a CIRCLE not random lines
        {
            int nArcs=24; // reduced from 50 for perf
            for(int b=0;b<nArcs;++b){
                float bt=(float)b/nArcs;
                int fi=std::clamp((int)(bt*FREQ_BINS*0.5f),0,FREQ_BINS-1);
                float val=spec[fi];
                // Radius = base + spectrum value → arc at that radius
                float r=30.f+bass*20.f+val*(180.f+bass*140.f+mid*60.f);
                // Draw arc segment at this radius spanning the full slice angle
                int arcSegs=8; // segments per arc
                for(int as=0;as<arcSegs;++as){
                    float a0=bt*SANG+(float)as/arcSegs*(SANG/nArcs);
                    float a1=bt*SANG+(float)(as+1)/arcSegs*(SANG/nArcs);
                    V2 p0={cosf(a0)*r, sinf(a0)*r};
                    V2 p1={cosf(a1)*r, sinf(a1)*r};
                    float hue=fmod(t*38.f+bt*200.f+s*5.f,360.f);
                    SDL_Color c=thsv(hue,.9f,1.f);
                    c.a=(Uint8)((0.1f+val*0.5f)*255);
                    stroke(ren,W(p0),W(p1),1.2f+val*3.f,c,c);
                }
                // Connecting radial line between adjacent arcs — thin
                if(b>0){
                    int fiPrev=std::clamp((int)((float)(b-1)/nArcs*FREQ_BINS*0.5f),0,FREQ_BINS-1);
                    float valPrev=spec[fiPrev];
                    float rPrev=30.f+bass*20.f+valPrev*(180.f+bass*140.f+mid*60.f);
                    float ang=bt*SANG;
                    V2 p0={cosf(ang)*rPrev,sinf(ang)*rPrev};
                    V2 p1={cosf(ang)*r,    sinf(ang)*r};
                    SDL_Color lc=thsv(fmod(t*38.f+bt*200.f,360.f),.7f,.5f);
                    lc.a=(Uint8)((0.05f+(val+valPrev)*0.15f)*255);
                    stroke(ren,W(p0),W(p1),0.8f,lc,lc);
                }
            }
        }

        // 2. NODE TRAILS — reduced trail render frequency
        for(int ni=0;ni<KNODES;++ni){
            auto& n=g_kn[ni];
            int tlen=(int)n.trail.size();
            if(tlen<2) continue;
            // Only draw every other trail point for perf
            for(int tr=0;tr<tlen-2;tr+=2){
                float tf=1.f-(float)tr/tlen;
                SDL_Color c=thsv(fmod(n.hue+tr*2.5f,360.f),.85f,1.f);
                c.a=(Uint8)(tf*tf*(0.15f+overall*.25f)*255);
                stroke(ren,W(s2c(n.trail[tr])),W(s2c(n.trail[tr+2])),
                       (1.2f+overall*2.5f)*tf,c,c);
            }
            V2 head=W(s2c(n.trail[0]));
            SDL_Color dc=thsv(n.hue,.4f,1.f); dc.a=(Uint8)(180+overall*60);
            softCircle(ren,head,2.5f+overall*4.f,dc,{dc.r,dc.g,dc.b,0},8);
        }

        // 3. SPINNING PETALS — reduced to 2 petals
        for(int petal=0;petal<2;++petal){
            float pf=(float)petal/2,pr=55.f+pf*maxR*.4f;
            float spin=t*0.5f*(petal%2?1.f:-1.f);
            V2 center={cosf(pf*SANG)*pr,sinf(pf*SANG)*pr};
            float petalR=18.f+mid*25.f+bass*20.f;
            for(int pt=0;pt<6;++pt){
                float a0=(float)pt/6*2*(float)M_PI+spin;
                float a1=(float)(pt+1)/6*2*(float)M_PI+spin;
                V2 lp0=center+V2{cosf(a0)*petalR,sinf(a0)*petalR};
                V2 lp1=center+V2{cosf(a1)*petalR,sinf(a1)*petalR};
                float hue=fmod(t*50.f+petal*120.f+pt*60.f,360.f);
                SDL_Color edge=thsv(hue,.9f,1.f);
                edge.a=(Uint8)((0.15f+mid*.35f+overall*.15f)*255);
                SDL_Color fill=edge; fill.a=fill.a/6;
                triC(ren,W(center),W(lp0),W(lp1),fill,edge,edge);
            }
        }

        // 4. WARP RINGS — reduced to 5
        for(int ri=0;ri<4;++ri){
            float rt=(float)ri/4;
            float rr=35.f+rt*maxR*.75f+high*15.f*cosf(t*3.f+ri);
            SDL_Color c=thsv(fmod(t*28.f+ri*25.7f,360.f),.85f,1.f);
            c.a=(Uint8)((0.03f+overall*.06f)*255);
            for(int sg=0;sg<8;++sg){
                float a0=(float)sg/12*SANG, a1=(float)(sg+1)/12*SANG;
                if(mir){a0=SANG-a0; a1=SANG-a1;}
                stroke(ren,W({cosf(a0)*rr,sinf(a0)*rr}),
                           W({cosf(a1)*rr,sinf(a1)*rr}),
                           1.f+overall*1.5f,c,c);
            }
        }

        // 5. MANDALA PETALS — only when high energy
        if(high>0.06f){
            int petalCount=5;
            for(int p=0;p<petalCount;++p){
                float pa=(float)p/petalCount*SANG+t*0.3f*(s%2?1.f:-1.f);
                float innerR=20.f+mid*12.f;
                float outerR=innerR+(35.f+high*100.f+mid*50.f);
                float paMid=pa+0.03f+high*0.03f;
                V2 base0={cosf(pa)*innerR,      sinf(pa)*innerR};
                V2 base1={cosf(pa+0.06f)*innerR,sinf(pa+0.06f)*innerR};
                V2 tip2  ={cosf(paMid)*outerR,  sinf(paMid)*outerR};
                float hue=fmod(t*60.f+p*60.f+s*15.f,360.f);
                SDL_Color ec=thsv(hue,.9f,1.f);
                ec.a=(Uint8)((0.08f+high*0.3f)*255);
                SDL_Color fc=ec; fc.a=ec.a/6;
                triC(ren,W(base0),W(base1),W(tip2),fc,fc,ec);
            }
        }

        // 6. BASS SPIKES
        if(bass>0.2f){
            for(int sp=0;sp<8;++sp){
                float sa=(float)sp/8*SANG;
                float spikeLen=bass*maxR*0.28f*(0.5f+0.5f*sinf(t*8.f+sp*2.3f));
                V2 inner={cosf(sa)*30.f,sinf(sa)*30.f};
                V2 outer={cosf(sa)*(30.f+spikeLen),sinf(sa)*(30.f+spikeLen)};
                float hue=fmod(t*80.f+sp*45.f,360.f);
                SDL_Color c1=thsv(hue,1.f,1.f); c1.a=(Uint8)(bass*160.f);
                SDL_Color c0=c1; c0.a=0;
                stroke(ren,W(inner),W(outer),2.f+bass*3.f,c0,c1);
            }
        }
    }

    // CENTRAL ORB
    float orbR=14.f+overall*30.f+bass*40.f;
    for(int layer=4;layer>=0;--layer){
        float lt=(float)layer/4.f;
        SDL_Color c=thsv(fmod(t*65.f+layer*42.f,360.f),.85f,1.f);
        c.a=(Uint8)((0.03f+overall*.08f)*(1.f-lt)*255);
        softCircle(ren,{cx,cy},orbR*(3.5f-lt*2.5f),c,{c.r,c.g,c.b,0},24);
    }
    for(int ri=0;ri<3;++ri){
        float rr=orbR*(.55f+ri*.32f);
        float ang=t*(0.8f+ri*.4f); // all same direction
        SDL_Color c=thsv(fmod(t*75.f+ri*90.f,360.f),.9f,1.f);
        c.a=(Uint8)((0.25f+overall*.2f)*255);
        for(int sg=0;sg<20;++sg){
            float a0=(float)sg/36*2*(float)M_PI+ang;
            float a1=(float)(sg+1)/36*2*(float)M_PI+ang;
            stroke(ren,{cx+cosf(a0)*rr,cy+sinf(a0)*rr},
                       {cx+cosf(a1)*rr,cy+sinf(a1)*rr},1.2f+overall*1.2f,c,c);
        }
    }
    if(high>0.05f||mid>0.1f){
        int mPetals=8;
        for(int p=0;p<mPetals;++p){
            float pa=(float)p/mPetals*2*(float)M_PI+t*0.4f;
            float innerR2=orbR*1.2f;
            float outerR2=innerR2+(18.f+high*70.f+mid*35.f);
            float halfW=0.10f;
            V2 b0={cosf(pa-halfW)*innerR2,sinf(pa-halfW)*innerR2};
            V2 b1={cosf(pa+halfW)*innerR2,sinf(pa+halfW)*innerR2};
            V2 tp={cosf(pa)*outerR2,sinf(pa)*outerR2};
            float hue=fmod(t*55.f+p*45.f,360.f);
            SDL_Color ec=thsv(hue,0.95f,1.f);
            ec.a=(Uint8)((0.12f+high*0.35f+mid*0.12f)*255);
            SDL_Color fc=ec; fc.a=ec.a/5;
            triC(ren,{cx+b0.x,cy+b0.y},{cx+b1.x,cy+b1.y},{cx+tp.x,cy+tp.y},fc,fc,ec);
        }
    }
    SDL_Color wh={255,255,255,(Uint8)(std::min(1.f,.9f+bass*.1f)*255)};
    softCircle(ren,{cx,cy},orbR*.3f,wh,{255,255,255,0},24);
}

// ═══════════════════════════════════════════════════════════════════════════════
// REVOLUTIONARY NEW EFFECTS
// ═══════════════════════════════════════════════════════════════════════════════

// ── 1. CHROMATIC LIGHTNING STORM ─────────────────────────────────────────────
// Recursive branching lightning bolts that arc across the screen on bass hits
// Each bolt splits into sub-bolts, each sub-bolt splits again — fractal electricity
struct LightningBolt { V2 a, b; int depth; float hue; float alpha; };
static std::vector<LightningBolt> g_lightning;
static float g_lightningTimer = 0.f;

void spawnLightning(float cx, float cy, float t, float bass, float high, int ww, int wh, std::mt19937& rng) {
    std::uniform_real_distribution<float> U(0,1);
    g_lightningTimer -= 0.016f;
    if(bass > 0.5f && g_lightningTimer <= 0.f) {
        g_lightningTimer = 0.08f + U(rng)*0.12f;
        float hue = fmod(t*120.f + U(rng)*60.f, 360.f);
        // Iterative bolt builder using a stack instead of recursion
        struct BoltTask { V2 a, b; int depth; float h; };
        std::vector<BoltTask> stack;
        V2 start = {U(rng)*(float)ww, U(rng)*(float)wh};
        V2 end   = {U(rng)*(float)ww, U(rng)*(float)wh};
        int maxDepth = 2 + (int)(high*1.f);
        stack.push_back({start, end, maxDepth, hue});
        while(!stack.empty()){
            auto task = stack.back(); stack.pop_back();
            if(task.depth <= 0 || vlen(task.b-task.a) < 10.f){
                g_lightning.push_back({task.a, task.b, task.depth,
                                       task.h, bass*(0.3f+task.depth*0.2f)});
                continue;
            }
            V2 mid2 = (task.a+task.b)*0.5f;
            V2 perp2 = vnorm(perp(task.b-task.a));
            float offset = (U(rng)-0.5f) * vlen(task.b-task.a) * 0.4f;
            mid2 = mid2 + perp2*offset;
            stack.push_back({task.a, mid2, task.depth-1, task.h});
            stack.push_back({mid2, task.b, task.depth-1, task.h});
            if(task.depth >= 2 && U(rng) < 0.5f){
                V2 branchEnd = mid2 + V2{(U(rng)-0.5f)*200.f,(U(rng)-0.5f)*200.f};
                stack.push_back({mid2, branchEnd, task.depth-2, fmod(task.h+40.f,360.f)});
            }
        }
    }
    for(auto& b2 : g_lightning) b2.alpha -= 0.04f;
    g_lightning.erase(std::remove_if(g_lightning.begin(),g_lightning.end(),
        [](const LightningBolt& b2){return b2.alpha<=0.f;}),g_lightning.end());
}

void drawLightning(SDL_Renderer* r, float bass, float high) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    for(auto& b2 : g_lightning) {
        float a = clamp01(b2.alpha);
        // Multi-layer glow
        for(int g=0;g<4;++g){
            float gf=(float)g/4.f;
            SDL_Color c=thsv(b2.hue+g*10.f,0.7f,1.f);
            c.a=(Uint8)(a*(1.f-gf)*(1.f-gf)*220.f);
            stroke(r,b2.a,b2.b,(4.f-g)*1.5f,c,c);
        }
        // Bright core
        SDL_Color core={255,255,255,(Uint8)(a*200.f)};
        stroke(r,b2.a,b2.b,0.8f,core,core);
    }
}

// ── 2. SHATTERED MIRROR PLANES ───────────────────────────────────────────────
// The screen cracks into shards on bass hits — each shard shows a
// different time-offset of the visualizer (fake, using rotated/scaled draws)
// We simulate this with overlapping tilted rhombus panels that warp on beat
struct MirrorShard { float x,y,w,h,angle,phase,hue; };
static std::vector<MirrorShard> g_shards;
static bool g_shardsInit=false;

void initShards(int ww, int wh, std::mt19937& rng){
    std::uniform_real_distribution<float> U(0,1);
    g_shards.clear();
    int count=12;
    for(int i=0;i<count;++i){
        MirrorShard s;
        s.x=U(rng)*(float)ww;
        s.y=U(rng)*(float)wh;
        s.w=80.f+U(rng)*200.f;
        s.h=60.f+U(rng)*150.f;
        s.angle=U(rng)*2*(float)M_PI;
        s.phase=U(rng)*6.28f;
        s.hue=U(rng)*360.f;
        g_shards.push_back(s);
    }
    g_shardsInit=true;
}

void drawShatteredMirror(SDL_Renderer* r, float t, float bass, float mid, float overall){
    if(overall<0.05f) return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    for(auto& s:g_shards){
        // Shard pulses and spins on bass
        float pulse=sinf(t*1.5f+s.phase)*0.15f+0.85f;
        float extraRot=bass*0.3f*sinf(t*3.f+s.phase);
        float ang=s.angle+t*0.05f*(sinf(s.phase)>0?1.f:-1.f)+extraRot;
        float sw=s.w*pulse*(0.5f+overall*0.8f);
        float sh=s.h*pulse*(0.5f+overall*0.8f);
        // Four corners of the shard
        V2 corners[4]={
            {-sw*.5f,-sh*.5f},{sw*.5f,-sh*.5f},
            {sw*.5f, sh*.5f},{-sw*.5f, sh*.5f}
        };
        for(auto& c:corners) c=vrot(c,ang)+V2{s.x,s.y};
        float hue=fmod(s.hue+t*15.f,360.f);
        SDL_Color edge=thsv(hue,0.8f,1.f);
        edge.a=(Uint8)((0.04f+overall*0.06f+bass*0.08f)*255);
        SDL_Color inner=edge;inner.a=edge.a/8;
        // Draw shard edges
        for(int i=0;i<4;++i){
            int j=(i+1)%4;
            stroke(r,corners[i],corners[j],0.8f+mid*1.5f,edge,edge);
        }
        // Fill with very dim color
        triC(r,corners[0],corners[1],corners[2],inner,inner,inner);
        triC(r,corners[0],corners[2],corners[3],inner,inner,inner);
        // Crack lines from center
        V2 center={s.x,s.y};
        for(int c=0;c<4;++c){
            SDL_Color lc=edge;lc.a=edge.a/3;
            stroke(r,center,corners[c],0.5f,{0,0,0,0},lc);
        }
    }
}

// ── 3. SONIC DNA HELIX ───────────────────────────────────────────────────────
// A double helix that twists through 3D space, each base pair colored by
// the frequency at that point — looks like actual DNA built from sound
void drawDNAHelix(SDL_Renderer* r, float cx, float cy, float t,
                  float bass, float mid, float high, float overall,
                  const std::vector<float>& spec, float maxR){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    int steps=40;
    float helixR=80.f+mid*60.f;   // radius of helix
    float helixH=maxR*1.4f;       // total height span
    float twist=4.f*(float)M_PI;  // how many times it rotates
    float spinRate=t*0.4f;

    for(int strand=0;strand<2;++strand){
        float strandOff=strand*(float)M_PI; // opposite sides
        V2 prev1={0,0},prev2={0,0};bool hasPrev=false;

        for(int i=0;i<steps;++i){
            float tf=(float)i/steps;
            float tf1=(float)(i+1)/steps;

            // 3D helix: x=cos, z=sin, y=linear
            float ang0=tf*twist+spinRate+strandOff;
            float ang1=tf1*twist+spinRate+strandOff;

            // Z depth for perspective
            float z0=sinf(ang0),z1=sinf(ang1);
            float perspScale0=1.f/(2.f-z0*0.3f);
            float perspScale1=1.f/(2.f-z1*0.3f);

            float x0=cx+cosf(ang0)*helixR*perspScale0;
            float y0=cy+(tf-0.5f)*helixH;
            float x1=cx+cosf(ang1)*helixR*perspScale1;
            float y1=cy+(tf1-0.5f)*helixH;

            // Sample spectrum at this height
            int fi=std::clamp((int)(tf*FREQ_BINS*0.6f),0,FREQ_BINS-1);
            float val=spec[fi];
            float hue=fmod(tf*360.f+t*30.f+strand*180.f,360.f);
            SDL_Color c=thsv(hue,0.9f,1.f);
            c.a=(Uint8)((0.1f+val*0.4f+overall*0.15f)*(0.4f+z0*0.3f+0.3f)*255);

            if(hasPrev)
                stroke(r,prev1,{x0,y0},1.5f+val*3.f,{c.r,c.g,c.b,0},c);

            prev1={x0,y0};

            // Base pairs — horizontal rungs connecting the two strands
            if(i%4==0){
                float ang0b=tf*twist+spinRate; // other strand
                float z0b=sinf(ang0b+(float)M_PI);
                float perspB=1.f/(2.f-z0b*0.3f);
                float x0b=cx+cosf(ang0b+(float)M_PI)*helixR*perspB;
                SDL_Color rc=thsv(fmod(hue+90.f,360.f),0.7f,1.f);
                rc.a=(Uint8)((0.05f+val*0.15f+overall*0.08f)*255);
                stroke(r,{x0,y0},{x0b,y0},0.6f,rc,rc);
                // Glowing node at junction
                SDL_Color nc=rc;nc.a=(Uint8)(rc.a*2.f);
                softCircle(r,{x0,y0},2.f+val*4.f,nc,{nc.r,nc.g,nc.b,0},6);
            }

            hasPrev=true;
        }
    }
}

// ── 4. PARTICLE SUPERNOVA ─────────────────────────────────────────────────────
// Thousands of particles orbit the center in shells — on bass hit they
// explode outward then gravity pulls them back in — like a star breathing
struct NovaParticle {
    float angle, radius, radialVel, angularVel;
    float hue, alpha, size;
    int shell;
};
static std::vector<NovaParticle> g_nova;
static bool g_novaInit=false;
static float g_novaLastBass=0.f;

void initNova(float maxR, std::mt19937& rng){
    std::uniform_real_distribution<float> U(0,1);
    g_nova.resize(80);
    int shells=5;
    for(int i=0;i<(int)g_nova.size();++i){
        auto& p=g_nova[i];
        p.shell=i%shells;
        p.angle=U(rng)*2*(float)M_PI;
        p.radius=maxR*(0.05f+p.shell*0.12f+U(rng)*0.08f);
        p.radialVel=0.f;
        p.angularVel=(0.3f+U(rng)*0.8f)*(i%2?1.f:-1.f);
        p.hue=U(rng)*360.f;
        p.alpha=0.3f+U(rng)*0.7f;
        p.size=1.f+U(rng)*3.f;
    }
    g_novaInit=true;
}

void updateNova(float dt, float bass, float mid, float overall, float maxR){
    // Explode on bass hit
    if(bass>0.6f&&g_novaLastBass<=0.6f){
        for(auto& p:g_nova)
            p.radialVel+=80.f+p.shell*30.f;
    }
    g_novaLastBass=bass;
    for(auto& p:g_nova){
        p.angle+=dt*p.angularVel*(1.f+overall*0.5f+mid*0.3f);
        // Gravity pulls back to orbit radius
        float targetR=maxR*(0.05f+p.shell*0.12f);
        float gravity=(targetR-p.radius)*2.f;
        p.radialVel+=gravity*dt;
        p.radialVel*=0.92f; // damping
        p.radius+=p.radialVel*dt;
        p.radius=std::max(p.radius,5.f);
        p.hue=fmod(p.hue+40.f*dt,360.f);
    }
}

void drawNova(SDL_Renderer* r, float cx, float cy, float overall, float bass, float mid){
    if(overall<0.03f) return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    for(auto& p:g_nova){
        float x=cx+cosf(p.angle)*p.radius;
        float y=cy+sinf(p.angle)*p.radius;
        SDL_Color c=thsv(p.hue,0.8f,1.f);
        c.a=(Uint8)(p.alpha*(0.08f+overall*0.15f+bass*0.1f)*255);
        float sz=p.size*(1.f+mid*0.5f+bass*0.3f);
        softCircle(r,{x,y},sz,c,{c.r,c.g,c.b,0},6);
    }
}

// ── 5. VOID TENDRILS ─────────────────────────────────────────────────────────
// Dark tendrils snake outward from the center, bending toward bass energy
// They're drawn as SUBTRACTIVE (blend multiply) against the bright visuals
// creating dark negative-space ribbons that writhe like living darkness
struct Tendril {
    float angle, speed, phase, length, hue;
    std::vector<V2> pts;
};
static std::vector<Tendril> g_tendrils;
static bool g_tendrilsInit=false;

void initTendrils(std::mt19937& rng){
    std::uniform_real_distribution<float> U(0,1);
    int count=8;
    g_tendrils.resize(count);
    for(int i=0;i<count;++i){
        auto& t2=g_tendrils[i];
        t2.angle=(float)i/count*2*(float)M_PI;
        t2.speed=(0.4f+U(rng)*0.6f)*(i%2?1.f:-1.f);
        t2.phase=U(rng)*6.28f;
        t2.length=0.f;
        t2.hue=U(rng)*360.f;
        t2.pts.resize(40);
    }
    g_tendrilsInit=true;
}

void updateTendrils(float dt, float cx, float cy, float bass, float mid, float t2, float maxR){
    for(auto& td:g_tendrils){
        td.angle+=dt*td.speed*(1.f+mid*0.3f);
        td.phase+=dt*2.f;
        td.length=maxR*(0.3f+bass*0.5f+mid*0.2f);
        td.hue=fmod(td.hue+20.f*dt,360.f);
        int npts=(int)td.pts.size();
        for(int i=0;i<npts;++i){
            float tf=(float)i/npts;
            float r=td.length*tf;
            // Snake with multiple harmonics
            float wobble=
                sinf(tf*6.f+t2*2.f+td.phase)*0.12f+
                sinf(tf*11.f-t2*3.f+td.phase*1.7f)*0.06f+
                sinf(tf*3.f+t2*0.8f)*0.08f*bass;
            float ang=td.angle+wobble*(float)M_PI;
            td.pts[i]={cx+cosf(ang)*r, cy+sinf(ang)*r};
        }
    }
}

void drawTendrils(SDL_Renderer* r, float bass, float mid, float overall){
    if(overall<0.04f) return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    for(auto& td:g_tendrils){
        int npts=(int)td.pts.size();
        for(int i=0;i<npts-1;++i){
            float tf=(float)i/npts;
            float fade=(1.f-tf)*(1.f-tf);
            float hue=fmod(td.hue+tf*60.f,360.f);
            SDL_Color c=thsv(hue,0.6f,1.f);
            c.a=(Uint8)(fade*(0.06f+mid*0.12f+bass*0.08f)*255);
            float lw=(3.f+mid*6.f+bass*4.f)*(1.f-tf*0.7f);
            stroke(r,td.pts[i],td.pts[i+1],lw,{c.r,c.g,c.b,0},c);
        }
        // Glowing head
        if(!td.pts.empty()){
            SDL_Color hc=thsv(td.hue,0.5f,1.f);
            hc.a=(Uint8)((0.1f+bass*0.3f)*255);
            softCircle(r,td.pts[0],2.f+bass*6.f,hc,{hc.r,hc.g,hc.b,0},8);
        }
    }
}




// ═══════════════════════════════════════════════════════════════════════════════
// MORE EFFECTS — ROUND 2
// ═══════════════════════════════════════════════════════════════════════════════

// ── 6. CYMATICS PATTERN ───────────────────────────────────────────────────────
// Simulates Chladni figures — sand patterns that form on vibrating plates
// Concentric interference rings that morph based on frequency ratios
void drawCymatics(SDL_Renderer* r, float cx, float cy, float t,
                  float bass, float mid, float high, float overall, float maxR){
    if(overall < 0.03f) return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    int rings = 14;
    int spokes = 36;
    // Frequency ratios create different Chladni figures
    float freqA = 3.f + mid*4.f;   // radial frequency
    float freqB = 5.f + bass*3.f;  // angular frequency
    for(int ring=0; ring<rings; ++ring){
        float rf = (float)ring/rings;
        float baseR = maxR * rf * 0.9f;
        for(int sp=0; sp<spokes; ++sp){
            float ang0 = (float)sp/spokes * 2*(float)M_PI;
            float ang1 = (float)(sp+1)/spokes * 2*(float)M_PI;
            // Chladni equation: sin(freqA*r)*sin(freqB*theta)
            float chladni0 = sinf(freqA*rf*(float)M_PI + t*0.8f) *
                             sinf(freqB*ang0 + t*0.3f);
            float chladni1 = sinf(freqA*rf*(float)M_PI + t*0.8f) *
                             sinf(freqB*ang1 + t*0.3f);
            // Only draw near nodal lines (where chladni ≈ 0)
            float node0 = fabsf(chladni0);
            float node1 = fabsf(chladni1);
            float threshold = 0.15f + bass*0.1f;
            if(node0 > threshold && node1 > threshold) continue;
            float intensity = (1.f-node0)*(1.f-node1);
            float r0 = baseR + chladni0*(15.f + mid*20.f);
            float r1 = baseR + chladni1*(15.f + mid*20.f);
            V2 p0={cx+cosf(ang0)*r0, cy+sinf(ang0)*r0};
            V2 p1={cx+cosf(ang1)*r1, cy+sinf(ang1)*r1};
            float hue = fmod(t*20.f + rf*180.f + ang0*30.f, 360.f);
            SDL_Color c=thsv(hue,0.85f,1.f);
            c.a=(Uint8)(intensity*(0.08f+overall*0.15f)*255);
            stroke(r,p0,p1,1.f+intensity*2.f,c,c);
        }
    }
}

// ── 7. WORMHOLE VORTEX ────────────────────────────────────────────────────────
// A spinning vortex that sucks everything toward the center on bass hits
// Particles spiral inward trailing light like matter falling into a black hole
struct VortexParticle { float angle, r, speed, hue, life; };
static std::vector<VortexParticle> g_vortex;
static bool g_vortexInit=false;

void initVortex(std::mt19937& rng){
    std::uniform_real_distribution<float> U(0,1);
    g_vortex.resize(60);
    for(auto& p:g_vortex){
        p.angle=U(rng)*2*(float)M_PI;
        p.r=50.f+U(rng)*400.f;
        p.speed=0.5f+U(rng)*2.f;
        p.hue=U(rng)*360.f;
        p.life=U(rng);
    }
    g_vortexInit=true;
}

void updateVortex(float dt, float bass, float overall, float maxR, std::mt19937& rng){
    std::uniform_real_distribution<float> U(0,1);
    float suck=bass*300.f+overall*50.f;
    for(auto& p:g_vortex){
        // Spiral inward — angular speed increases as r decreases
        float angSpeed=p.speed*(1.f+20.f/(p.r+5.f))*(1.f+bass*2.f);
        p.angle+=dt*angSpeed;
        p.r-=dt*(suck*(1.f/(p.r*0.05f+1.f))+20.f);
        p.hue=fmod(p.hue+60.f*dt,360.f);
        p.life-=dt*0.3f;
        if(p.r<5.f||p.life<=0.f){
            p.r=maxR*0.5f+U(rng)*maxR*0.5f;
            p.angle=U(rng)*2*(float)M_PI;
            p.speed=0.5f+U(rng)*2.f;
            p.life=1.f;
        }
    }
}

void drawVortex(SDL_Renderer* r, float cx, float cy, float bass, float overall){
    if(overall<0.04f) return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    for(auto& p:g_vortex){
        float x=cx+cosf(p.angle)*p.r;
        float y=cy+sinf(p.angle)*p.r;
        // Trail in direction of motion
        float trailAng=p.angle-0.3f;
        float trailR=p.r+15.f;
        V2 tail={cx+cosf(trailAng)*trailR, cy+sinf(trailAng)*trailR};
        SDL_Color c=thsv(p.hue,0.8f,1.f);
        float fade=p.life*clamp01(1.f-p.r/500.f);
        c.a=(Uint8)(fade*(0.06f+overall*0.1f+bass*0.08f)*255);
        stroke(r,{x,y},tail,1.f+bass*2.f,{c.r,c.g,c.b,0},c);
        softCircle(r,{x,y},1.f+bass*2.f,c,{c.r,c.g,c.b,0},4);
    }
}

// ── 8. SONIC GEOMETRY — 3D PLATONIC SOLIDS ───────────────────────────────────
// A rotating icosahedron/dodecahedron that morphs between shapes driven by
// frequency — edges glow, vertices spark, faces fill on bass
struct Edge3D { int a,b; };
static const float ICO_VERTS[][3]={
    {0,1,1.618f},{0,-1,1.618f},{0,1,-1.618f},{0,-1,-1.618f},
    {1,1.618f,0},{-1,1.618f,0},{1,-1.618f,0},{-1,-1.618f,0},
    {1.618f,0,1},{-1.618f,0,1},{1.618f,0,-1},{-1.618f,0,-1}
};
static const Edge3D ICO_EDGES[]={
    {0,1},{0,4},{0,5},{0,8},{0,9},{1,6},{1,7},{1,8},{1,9},
    {2,3},{2,4},{2,5},{2,10},{2,11},{3,6},{3,7},{3,10},{3,11},
    {4,5},{4,8},{4,10},{5,9},{5,11},{6,7},{6,8},{6,10},{7,9},{7,11},{8,10},{9,11}
};

void drawSonicGeometry(SDL_Renderer* r, float cx, float cy, float t,
                       float bass, float mid, float high, float overall, float maxR){
    if(overall<0.05f) return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    float scale=(50.f+mid*60.f+bass*40.f)*(1.f+high*0.3f);
    // Triple rotation axes all driven by different freq bands
    float rx=t*0.4f+bass*0.5f;
    float ry=t*0.3f+mid*0.4f;
    float rz=t*0.2f+high*0.6f;
    // Project 3D verts to 2D
    auto rot3=[&](float x,float y,float z)->V2{
        // Rotate X
        float y2=y*cosf(rx)-z*sinf(rx);float z2=y*sinf(rx)+z*cosf(rx);
        // Rotate Y
        float x3=x*cosf(ry)+z2*sinf(ry);float z3=-x*sinf(ry)+z2*cosf(ry);
        // Rotate Z
        float x4=x3*cosf(rz)-y2*sinf(rz);float y4=x3*sinf(rz)+y2*cosf(rz);
        // Perspective project
        float fov=3.f;
        float pz=fov/(fov+z3*0.3f);
        return{cx+x4*scale*pz, cy+y4*scale*pz};
    };
    // Draw edges
    int nEdges=sizeof(ICO_EDGES)/sizeof(ICO_EDGES[0]);
    for(int e=0;e<nEdges;++e){
        auto& edge=ICO_EDGES[e];
        V2 p0=rot3(ICO_VERTS[edge.a][0],ICO_VERTS[edge.a][1],ICO_VERTS[edge.a][2]);
        V2 p1=rot3(ICO_VERTS[edge.b][0],ICO_VERTS[edge.b][1],ICO_VERTS[edge.b][2]);
        float hue=fmod(t*40.f+e*12.f,360.f);
        SDL_Color c=thsv(hue,0.85f,1.f);
        c.a=(Uint8)((0.12f+overall*0.2f+mid*0.15f)*255);
        stroke(r,p0,p1,1.f+mid*2.f+bass*1.f,c,c);
    }
    // Draw glowing vertices
    int nVerts=12;
    for(int v=0;v<nVerts;++v){
        V2 p=rot3(ICO_VERTS[v][0],ICO_VERTS[v][1],ICO_VERTS[v][2]);
        float hue=fmod(t*60.f+v*30.f,360.f);
        SDL_Color c=thsv(hue,0.6f,1.f);
        c.a=(Uint8)((0.2f+high*0.4f+bass*0.2f)*255);
        softCircle(r,p,3.f+high*8.f+bass*5.f,c,{c.r,c.g,c.b,0},8);
    }
}

// ── 9. AURORA FILAMENTS ───────────────────────────────────────────────────────
// Thin luminous filaments that drift upward like the northern lights
// but react to high-freq by branching and splitting
struct Filament {
    std::vector<V2> pts;
    float hue, drift, phase, life, maxLife;
};
static std::vector<Filament> g_filaments;

// Protects all particle vectors from main/render thread races
// Main thread holds this during update, render thread holds during draw
static std::mutex g_particleMtx;
static float g_filamentSpawn=0.f;

void updateFilaments(float dt, float t, float high, float mid, float overall,
                     int ww, int wh, std::mt19937& rng){
    std::uniform_real_distribution<float> U(0,1);
    g_filamentSpawn-=dt;
    int spawnRate=1+(int)(high*4.f)+(int)(overall*3.f);
    if(g_filamentSpawn<=0.f&&overall>0.04f){
        g_filamentSpawn=0.1f/(spawnRate);
        for(int s=0;s<spawnRate;++s){
            Filament f;
            f.hue=fmod(180.f+t*20.f+U(rng)*60.f,360.f); // cool blue/green/purple
            f.drift=(U(rng)-0.5f)*80.f;
            f.phase=U(rng)*6.28f;
            f.maxLife=1.5f+U(rng)*2.f;
            f.life=f.maxLife;
            // Build initial points from bottom
            float startX=U(rng)*(float)ww;
            float startY=(float)wh+10.f;
            int npts=20+U(rng)*30;
            f.pts.resize(npts);
            for(int i=0;i<npts;++i){
                float tf=(float)i/npts;
                float wobble=sinf(tf*8.f+f.phase)*30.f*tf;
                f.pts[i]={startX+wobble+f.drift*tf, startY-(float)wh*tf*1.2f};
            }
            g_filaments.push_back(f);
        }
    }
    for(auto& f:g_filaments){
        f.life-=dt;
        f.hue=fmod(f.hue+15.f*dt,360.f);
        // Drift horizontally
        for(auto& p:f.pts) p.x+=dt*f.drift*0.1f;
    }
    g_filaments.erase(std::remove_if(g_filaments.begin(),g_filaments.end(),
        [](const Filament& f){return f.life<=0.f;}),g_filaments.end());
    // Cap total
    if(g_filaments.size()>100) g_filaments.resize(400);
}

void drawFilaments(SDL_Renderer* r, float overall, float high, float mid){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    for(auto& f:g_filaments){
        float lifeFrac=f.life/f.maxLife;
        float fade=sinf(lifeFrac*(float)M_PI); // fade in and out
        int n=(int)f.pts.size();
        for(int i=0;i<n-1;++i){
            float tf=(float)i/n;
            SDL_Color c=thsv(fmod(f.hue+tf*40.f,360.f),0.7f,1.f);
            c.a=(Uint8)(fade*tf*(0.04f+high*0.08f+overall*0.06f)*255);
            float lw=0.6f+high*1.5f+tf*1.5f;
            stroke(r,f.pts[i],f.pts[i+1],lw,{c.r,c.g,c.b,0},c);
        }
    }
}

// ── 10. GRAVITY LENS ──────────────────────────────────────────────────────────
// Draws concentric distortion rings as if spacetime itself is warping
// — Einstein rings that pulse, stretch and twist with the music
void drawGravityLens(SDL_Renderer* r, float cx, float cy, float t,
                     float bass, float mid, float overall, float maxR){
    if(overall<0.04f) return;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    int nRings=6;
    int nPoints=128;
    for(int ring=0;ring<nRings;++ring){
        float rf=(float)(ring+1)/(nRings+1);
        float baseR=maxR*rf*0.85f;
        V2 prev={0,0};bool hasPrev=false;
        for(int pt=0;pt<=nPoints;++pt){
            float ang=(float)pt/nPoints*2*(float)M_PI;
            // Gravitational lens distortion — warps the ring into arcs
            float distort=
                sinf(ang*2.f+t*0.6f)*0.12f*bass+
                sinf(ang*3.f-t*0.9f)*0.08f*mid+
                sinf(ang*5.f+t*1.4f)*0.05f*overall+
                sinf(ang*ring+t*(0.2f+ring*0.1f))*0.06f;
            float lensR=baseR*(1.f+distort);
            // Secondary Einstein arc — mirrored ring inside
            float arcR=baseR*(0.7f-distort*0.5f);
            V2 p={cx+cosf(ang)*lensR, cy+sinf(ang)*lensR};
            if(hasPrev){
                float hue=fmod(t*12.f+ring*60.f+ang*20.f,360.f);
                SDL_Color c=thsv(hue,0.7f,1.f);
                c.a=(Uint8)((0.03f+overall*0.06f+bass*0.04f*(1.f-rf))*255);
                stroke(r,prev,p,0.8f+bass*1.5f*(1.f-rf),c,c);
            }
            prev=p; hasPrev=true;
        }
    }
    // Central Einstein ring — bright circle that flares on bass
    int n=256;
    for(int pt=0;pt<=n;++pt){
        float ang=(float)pt/n*2*(float)M_PI;
        float angP=(float)(pt-1)/n*2*(float)M_PI;
        float eR=maxR*0.08f*(1.f+bass*1.2f);
        float jitter=sinf(ang*8.f+t*5.f)*bass*8.f;
        V2 p0={cx+cosf(angP)*(eR+jitter), cy+sinf(angP)*(eR+jitter)};
        V2 p1={cx+cosf(ang)*(eR+jitter),  cy+sinf(ang)*(eR+jitter)};
        float hue=fmod(t*80.f+ang*60.f,360.f);
        SDL_Color c=thsv(hue,0.5f,1.f);
        c.a=(Uint8)((0.15f+bass*0.5f+overall*0.1f)*255);
        if(pt>0) stroke(r,p0,p1,1.5f+bass*3.f,c,c);
    }
}



struct Song {
    std::string path;
    std::string title;
};
struct Playlist {
    std::string name;
    std::vector<int> songIndices;
};

// ── WAVEFORM PREVIEW CACHE ───────────────────────────────────────────────────
static const int WF_COLS = 300;  // horizontal resolution of waveform thumbnail
struct WaveformCache {
    std::string        path;
    std::vector<float> peaks;
    bool               ready    = false;
    bool               building = false;
};
static WaveformCache g_wfCache;
static int           g_wfHoverSong = -1;
static bool          g_wfHovering  = false;
static int           g_wfMouseX    = 0;

// Instant deterministic waveform from filename hash — no file I/O, no freeze
void buildWaveform(const std::string& path){
    if(g_wfCache.building) return;
    if(g_wfCache.ready && g_wfCache.path==path) return; // already cached
    g_wfCache.ready   = false;
    g_wfCache.building= true;
    g_wfCache.path    = path;

    std::vector<float> peaks(WF_COLS, 0.f);
    size_t seed = std::hash<std::string>{}(path);
    // Two passes — coarse shape + fine detail layered on top
    for(int i=0;i<WF_COLS;++i){
        // Coarse shape — big slow waves
        size_t s1=seed^(size_t)(i*2654435761ULL);
        s1^=s1>>16; s1*=0x45d9f3b; s1^=s1>>16;
        float coarse=0.2f+0.5f*((s1&0xFF)/255.f);
        coarse*=0.5f+0.5f*fabsf(sinf((float)i*0.08f));
        // Fine detail — fast noise
        size_t s2=seed^(size_t)(i*2246822519ULL+1);
        s2^=s2>>16; s2*=0x85ebca6b; s2^=s2>>16;
        float fine=0.05f+0.2f*((s2&0xFF)/255.f);
        // Mix and add a musical-looking envelope
        float env=0.4f+0.6f*fabsf(sinf((float)i*0.04f+1.2f))
                       *fabsf(sinf((float)i*0.013f+0.3f));
        peaks[i]=std::min(1.f,(coarse+fine)*env);
    }

    g_wfCache.peaks   = std::move(peaks);
    g_wfCache.ready   = true;
    g_wfCache.building= false;
}

static std::vector<Song>     g_library;
static std::vector<Playlist> g_playlists;
static int                   g_currentSong    = -1;
static int                   g_currentPL      = -1;
static int                   g_currentPLTrack = -1;

static std::string getConfigPath() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr,buf,MAX_PATH);
    std::string exe(buf);
    auto slash=exe.find_last_of("\\/");
    return exe.substr(0,slash+1)+"library.dat";
}

std::string baseName(const std::string& p){
    auto s=p.find_last_of("\\/");
    std::string fn=(s==std::string::npos)?p:p.substr(s+1);
    auto dot=fn.rfind('.');
    return dot==std::string::npos?fn:fn.substr(0,dot);
}

void saveLibrary(){
    std::ofstream f(getConfigPath());
    if(!f)return;
    f<<"SONGS "<<g_library.size()<<"\n";
    for(auto& s:g_library) f<<s.path<<"\n";
    f<<"PLAYLISTS "<<g_playlists.size()<<"\n";
    for(auto& pl:g_playlists){
        f<<"PL "<<pl.name<<"\n";
        f<<"COUNT "<<pl.songIndices.size()<<"\n";
        for(int i:pl.songIndices) f<<i<<"\n";
    }
}


// ═══════════════════════════════════════════════════════════════════════════════
// CUSTOM EFFECT BUILDER
// ═══════════════════════════════════════════════════════════════════════════════
enum class EffShape  { Ring=0, Burst, Spiral, Wave, Grid, Star, Flower, Tunnel, Lissajous,
                       Web, Helix, Snowflake, Polygon, Comet, Matrix, Arc, COUNT };
enum class EffColor  { Spectrum=0, Solid, Pulse, Rainbow, Fire, Ice, Neon, Pastel, Duo, Glitch, COUNT };
enum class EffMotion { Spin=0, Breathe, Explode, Drift, Ripple, Orbit, Shake, Bounce, Converge, PulseM, Swirl, ScatterM, COUNT };
enum class EffTrigger{ Always=0, Bass, Mid, High, SubBass, COUNT };
enum class EffBlend  { Add=0, Normal, COUNT };

static const char* EFF_SHAPE_NAMES[]  ={"Ring","Burst","Spiral","Wave","Grid","Star","Flower","Tunnel","Lissajous","Web","Helix","Snowflake","Polygon","Comet","Matrix","Arc"};
static const char* EFF_COLOR_NAMES[]  ={"Spectrum","Solid","Pulse","Rainbow","Fire","Ice","Neon","Pastel","Duo","Glitch"};
static const char* EFF_MOTION_NAMES[] ={"Spin","Breathe","Explode","Drift","Ripple","Orbit","Shake","Bounce","Converge","Pulse","Swirl","Scatter"};
static const char* EFF_TRIGGER_NAMES[]={"Always","Bass","Mid","High","SubBass"};
static const char* EFF_BLEND_NAMES[]  ={"Add","Normal"};

static const int MAX_CUSTOM_EFFECTS = 64;

struct CustomEffect {
    std::string name;
    EffShape   shape      = EffShape::Ring;
    EffColor   color      = EffColor::Spectrum;
    EffMotion  motion     = EffMotion::Spin;
    EffTrigger trigger    = EffTrigger::Always;
    EffBlend   blend      = EffBlend::Add;
    float      hue        = 180.f;  // primary hue
    float      hue2       = 280.f;  // secondary hue (Duo, Pastel, etc)
    float      size       = 0.5f;
    float      speed      = 0.5f;
    float      density    = 0.5f;
    float      alpha      = 0.7f;
    float      trailLen   = 0.0f;   // 0=no trail, 1=long trail
    float      symmetry   = 1.0f;   // mirror copies (1-8)
    float      reactivity = 0.5f;   // how strongly audio moves it
    float      innerRadius= 0.0f;   // hollow center (0=solid, 1=thin ring)
    float      waveFreq   = 0.5f;   // wave/spiral frequency
    float      polySides  = 0.5f;   // 0=3 sides, 1=12 sides
    float      pulseRate  = 0.5f;   // independent pulse speed
    float      colorSpeed = 0.5f;   // color cycle speed independent of motion
    float      scatter    = 0.0f;   // randomness/chaos (0=clean, 1=wild)
    bool       enabled    = true;
    // Runtime state
    float      phase      = 0.f;
};

static CustomEffect g_customEffects[MAX_CUSTOM_EFFECTS] = {};
static int          g_numCustomEffects = 0;
static int          g_ceScrollOffset   = 0;  // scroll in custom effect list
static int          g_editingEffect    = -1;
static bool         g_ceFullscreen     = false; // fullscreen custom effect editor
static int          g_perfFrame        = 0;

// ── CUSTOM WINDOW DRAG — bypasses Windows modal loop entirely ─────────────────
// Windows' built-in drag enters a modal loop that blocks our thread.
// We intercept WM_NCLBUTTONDOWN on the title bar and implement drag ourselves
// using WM_MOUSEMOVE, so our render thread keeps running uninterrupted.
static HWND    g_hwnd        = nullptr;
static WNDPROC g_origWndProc = nullptr;
static std::atomic<bool> g_dragging{false};
static std::atomic<int>  g_dragOffX{0}, g_dragOffY{0};

LRESULT CALLBACK dragWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
        case WM_NCLBUTTONDOWN:
            if(wp == HTCAPTION){
                g_dragging = true;
                POINT pt; GetCursorPos(&pt);
                RECT wr; GetWindowRect(hwnd, &wr);
                g_dragOffX = pt.x - wr.left;
                g_dragOffY = pt.y - wr.top;
                SetCapture(hwnd);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if(g_dragging){
                POINT pt; GetCursorPos(&pt);
                int nx = pt.x - g_dragOffX;
                int ny = pt.y - g_dragOffY;
                // Check for Aero Snap zones
                HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{}; mi.cbSize=sizeof(mi);
                GetMonitorInfo(mon, &mi);
                RECT& wa = mi.rcWork;
                // Snap to top = maximize
                if(pt.y <= mi.rcMonitor.top + 4){
                    g_dragging = false;
                    ReleaseCapture();
                    SDL_Event e{}; e.type=SDL_USEREVENT; e.user.code=98;
                    e.user.data1=(void*)(intptr_t)1;
                    SDL_PushEvent(&e);
                    return 0;
                }
                if(pt.x <= mi.rcMonitor.left + 4){
                    g_dragging = false;
                    ReleaseCapture();
                    SDL_Event e{}; e.type=SDL_USEREVENT; e.user.code=97;
                    e.user.data1=(void*)(intptr_t)0; // left half
                    SDL_PushEvent(&e);
                    return 0;
                }
                if(pt.x >= mi.rcMonitor.right - 4){
                    g_dragging = false;
                    ReleaseCapture();
                    SDL_Event e{}; e.type=SDL_USEREVENT; e.user.code=97;
                    e.user.data1=(void*)(intptr_t)1; // right half
                    SDL_PushEvent(&e);
                    return 0;
                }
                SetWindowPos(hwnd,nullptr,nx,ny,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if(g_dragging){
                g_dragging = false;
                ReleaseCapture();
                return 0;
            }
            break;
        case WM_NCLBUTTONDBLCLK:
            if(wp == HTCAPTION){
                // Post SDL event to maximize safely from main thread
                SDL_Event e{}; e.type=SDL_USEREVENT; e.user.code=98;
                e.user.data1=(void*)(intptr_t)(IsZoomed(hwnd)?0:1);
                SDL_PushEvent(&e);
                return 0;
            }
            break;
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

void installDragHook(SDL_Window* win){
    SDL_SysWMinfo info{}; SDL_VERSION(&info.version);
    if(!SDL_GetWindowWMInfo(win, &info)) return;
    g_hwnd = info.info.win.window;
    g_origWndProc = (WNDPROC)GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)dragWndProc);
}

// ── RENDER THREAD ─────────────────────────────────────────────────────────────
static std::mutex        g_renderMtx;
static std::atomic<bool> g_renderRunning{false};
static std::atomic<bool> g_renderReady{false};
static std::mutex        g_resizeMtx;  // held during texture destroy/recreate
static std::condition_variable g_renderCv;
static std::mutex        g_renderCvMtx;

// Shared render parameters — main thread writes, render thread reads under mutex
struct RenderParams {
    float t=0,dt=0;
    float sBass=0,sMid=0,sHigh=0,sAll=0,sSub=0,sLM=0,sHM=0,rB=0;
    float cx=0,cy=0,maxR=0;
    int ww=1280,wh=720,frame=0;
    std::vector<float> spec;
    SDL_Texture* fbA=nullptr;
    SDL_Texture* fbB=nullptr;
    SDL_Texture* layerTex=nullptr;
};
static RenderParams g_rp;

void drawCustomEffect(SDL_Renderer* r, int idx, float cx, float cy,
                      float t, float bass, float mid, float high,
                      float overall, float maxR,
                      const std::vector<float>& spec){
    if(idx<0||idx>=g_numCustomEffects) return;
    auto& e=g_customEffects[idx];
    if(!e.enabled) return;

    // Trigger gate — reactivity scales response
    float react=0.3f+e.reactivity*1.4f;
    float trigVal=overall;
    switch(e.trigger){
        case EffTrigger::Bass:   trigVal=bass;    if(bass <0.1f) return; break;
        case EffTrigger::Mid:    trigVal=mid;     if(mid  <0.08f)return; break;
        case EffTrigger::High:   trigVal=high;    if(high <0.06f)return; break;
        case EffTrigger::SubBass:trigVal=bass*1.2f;if(bass<0.15f)return; break;
        default: trigVal=overall; break;
    }
    trigVal=clamp01(trigVal*react);

    // Advance phase
    float speedMult=0.2f+e.speed*2.f;
    g_customEffects[idx].phase=fmod(e.phase+0.016f*speedMult*(1.f+trigVal*0.5f),
                                    2.f*(float)M_PI);
    float ph=e.phase;

    float sz=maxR*(0.1f+e.size*0.9f);
    int   dens=2+(int)(e.density*22.f);
    float baseAlpha=e.alpha*(0.4f+trigVal*0.9f);
    int   symCopies=1+(int)(e.symmetry*7.f); // 1-8 symmetry

    // Blend mode
    SDL_SetRenderDrawBlendMode(r,
        e.blend==EffBlend::Normal ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_ADD);

    // Color picker — now supports Neon and Pastel + dual hue
    auto getColor=[&](float angle, float r2, int i)->SDL_Color{
        float h=0.f; float sat=0.9f, val2=1.f;
        switch(e.color){
            case EffColor::Spectrum:{
                int fi=std::clamp((int)((r2/maxR)*FREQ_BINS*0.5f),0,FREQ_BINS-1);
                h=fmod((float)fi/FREQ_BINS*360.f+t*20.f,360.f); break;}
            case EffColor::Solid:   h=e.hue; break;
            case EffColor::Pulse:   h=fmod(e.hue+sinf(t*3.f)*60.f,360.f); break;
            case EffColor::Rainbow: h=fmod(angle*57.3f+t*(20.f+e.colorSpeed*80.f),360.f); break;
            case EffColor::Fire:    h=fmod(e.hue+r2/maxR*60.f,60.f); break;
            case EffColor::Ice:     h=fmod(190.f+sinf(angle+t)*20.f,360.f); break;
            case EffColor::Neon:    h=fmod(e.hue+(float)i*137.5f,360.f);
                                    sat=1.f; val2=1.2f; break; // golden ratio hue steps
            case EffColor::Pastel:  h=fmod(e.hue2*(float)i/dens+t*15.f,360.f);
                                    sat=0.35f; val2=1.f; break;
            case EffColor::Duo:{    // blend between hue and hue2 based on radius
                float frac2=clamp01(r2/maxR);
                h=fmod(e.hue*(1.f-frac2)+e.hue2*frac2,360.f); break;}
            case EffColor::Glitch:{ // random hue jumps on beat
                float seed=(float)((i*1337+431)%360);
                h=fmod(seed+trigVal*180.f+t*(50.f+e.colorSpeed*200.f),360.f);
                sat=0.8f+trigVal*0.2f; break;}
            default: break;
        }
        SDL_Color col=thsv(h,std::min(1.f,sat),std::min(1.f,val2));
        col.a=(Uint8)(baseAlpha*255.f);
        return col;
    };

    // Helper: draw one instance of the shape, rotated by symAngle
    auto drawInstance=[&](float symAngle){
        // Save/restore center offset for symmetry rotation
        float rcx=cx,rcy=cy;
        // For symmetry we rotate coordinates — simulated by offsetting angle in all draws

        switch(e.shape){

        case EffShape::Ring:{
            float innerFrac=e.innerRadius; // 0=solid fill, 1=thin ring only
            for(int ri=0;ri<dens;++ri){
                float rf=(float)ri/dens;
                float r2=sz*(innerFrac*0.8f+rf*(1.f-innerFrac*0.8f));
                float motR=r2;
                switch(e.motion){
                    case EffMotion::Breathe: motR*=0.8f+0.2f*sinf(ph+rf*1.5f); break;
                    case EffMotion::Explode: motR*=1.f+trigVal*0.4f*sinf(ph+rf); break;
                    case EffMotion::Ripple:  motR*=1.f+0.15f*sinf(ph*3.f-rf*8.f); break;
                    case EffMotion::Bounce:  motR*=1.f+trigVal*0.5f*fabsf(sinf(ph*2.f+rf*2.f)); break;
                    case EffMotion::Converge:motR=sz*(1.f-rf*0.5f*(0.5f+0.5f*sinf(ph))); break;
                    case EffMotion::PulseM:  motR*=0.4f+0.6f*fabsf(sinf(ph*(1.f+e.pulseRate*5.f)+rf*2.f)); break;
                    case EffMotion::Swirl:   motR*=1.f+0.2f*sinf(ph*2.f+rf*4.f+symAngle*2.f); break;
                    case EffMotion::ScatterM:motR*=1.f+e.scatter*0.4f*sinf(ph*3.f+(float)ri*1.37f); break;
                    default: break;
                }
                float spinOff=(e.motion==EffMotion::Spin)?ph*(1.f+rf*0.5f):symAngle;
                int segs=24+dens*2;
                for(int s=0;s<segs;++s){
                    float a0=(float)s/segs*2*(float)M_PI+spinOff;
                    float a1=(float)(s+1)/segs*2*(float)M_PI+spinOff;
                    // Trail: fade alpha along trail length
                    float trailAlpha=e.trailLen>0.f?clamp01(1.f-(float)ri/(dens)*e.trailLen):1.f;
                    V2 p0={rcx+cosf(a0)*motR, rcy+sinf(a0)*motR};
                    V2 p1={rcx+cosf(a1)*motR, rcy+sinf(a1)*motR};
                    SDL_Color col=getColor(a0,motR,ri);
                    col.a=(Uint8)(col.a*trailAlpha);
                    stroke(r,p0,p1,1.f+trigVal*2.f,col,col);
                }
            }
            break;}

        case EffShape::Burst:{
            int spikes=dens*2;
            float spinOff=(e.motion==EffMotion::Spin)?ph:symAngle;
            for(int i=0;i<spikes;++i){
                float ang=(float)i/spikes*2*(float)M_PI+spinOff;
                float len=sz*0.85f;
                switch(e.motion){
                    case EffMotion::Breathe:  len*=0.7f+0.3f*sinf(ph+i); break;
                    case EffMotion::Explode:  len*=1.f+trigVal*0.7f; break;
                    case EffMotion::Ripple:   len*=0.8f+0.2f*sinf(ph*2.f+i*0.7f); break;
                    case EffMotion::Orbit:    len*=0.6f+0.4f*sinf(ph+i*2.f/spikes); break;
                    case EffMotion::Shake:    len*=1.f+0.15f*sinf(ph*10.f+i); break;
                    case EffMotion::Bounce:   len*=0.5f+0.5f*fabsf(sinf(ph+i*0.5f)); break;
                    case EffMotion::Converge: len*=(0.3f+0.7f*clamp01(1.f-trigVal)); break;
                    default: break;
                }
                V2 inner={rcx+cosf(ang)*sz*0.05f, rcy+sinf(ang)*sz*0.05f};
                V2 outer={rcx+cosf(ang)*len,       rcy+sinf(ang)*len};
                SDL_Color col=getColor(ang,len,i);
                SDL_Color c0=col; c0.a=0;
                // Trail effect on spikes
                if(e.trailLen>0.f){
                    int trailSteps=(int)(e.trailLen*8.f)+1;
                    for(int tr=0;tr<trailSteps;++tr){
                        float trf=1.f-(float)tr/trailSteps;
                        float tAng=ang-(float)tr*0.04f*(e.motion==EffMotion::Spin?1.f:0.f);
                        V2 tp={rcx+cosf(tAng)*len*trf*0.95f, rcy+sinf(tAng)*len*trf*0.95f};
                        SDL_Color tc=col; tc.a=(Uint8)(col.a*trf*0.3f);
                        stroke(r,inner,tp,0.8f,tc,tc);
                    }
                }
                stroke(r,inner,outer,1.f+trigVal*3.f,c0,col);
                softCircle(r,outer,2.f+trigVal*4.f,col,{col.r,col.g,col.b,0},6);
            }
            break;}

        case EffShape::Spiral:{
            int pts=60+dens*8;
            V2 prev2={rcx,rcy}; bool hasPrev=false;
            float trailFade=e.trailLen>0.f?0.5f+e.trailLen*0.5f:1.f;
            for(int i=0;i<pts;++i){
                float tf=(float)i/pts;
                float ang=tf*4.f*(float)M_PI*(1.f+e.speed)+ph+symAngle;
                float r2=sz*tf;
                switch(e.motion){
                    case EffMotion::Breathe: r2*=0.8f+0.2f*sinf(ph+tf*6.f); break;
                    case EffMotion::Drift:   ang+=sinf(ph+tf*3.f)*0.3f; break;
                    case EffMotion::Ripple:  r2*=1.f+0.1f*sinf(ph*4.f+tf*12.f); break;
                    case EffMotion::Bounce:  r2*=0.9f+0.1f*fabsf(sinf(ph*3.f+tf*6.f)); break;
                    case EffMotion::Orbit:   ang+=sinf(ph*0.5f)*0.8f; break;
                    default: break;
                }
                V2 p={rcx+cosf(ang)*r2, rcy+sinf(ang)*r2};
                if(hasPrev){
                    SDL_Color col=getColor(ang,r2,i);
                    col.a=(Uint8)(col.a*trailFade*(0.4f+tf*0.6f));
                    stroke(r,prev2,p,1.f+tf*2.f+trigVal*2.f,col,col);
                }
                prev2=p; hasPrev=true;
            }
            break;}

        case EffShape::Wave:{
            int pts=80+dens*10;
            float amp=sz*0.4f*(1.f+trigVal*0.5f);
            float freq=(1.f+e.waveFreq*10.f)*(1.f+trigVal*0.2f);
            int layers=1+(int)(e.trailLen*4.f);
            for(int layer=0;layer<layers;++layer){
                float lf=(float)layer/layers;
                float phOff=lf*(float)M_PI*0.5f;
                V2 prev2={0,0}; bool hasPrev=false;
                for(int i=0;i<pts;++i){
                    float xf=(float)i/pts;
                    float x=rcx-sz+xf*sz*2.f;
                    float waveY=amp*sinf(xf*freq*2.f*(float)M_PI+ph+phOff+symAngle);
                    switch(e.motion){
                        case EffMotion::Ripple:   waveY+=amp*0.3f*sinf(xf*freq*4.f*(float)M_PI-ph*2.f); break;
                        case EffMotion::Breathe:  waveY*=0.6f+0.4f*sinf(ph+lf); break;
                        case EffMotion::Bounce:   waveY*=0.5f+0.5f*fabsf(sinf(ph*1.5f)); break;
                        case EffMotion::Explode:  waveY*=1.f+trigVal*0.6f; break;
                        case EffMotion::Converge: waveY*=(1.f-xf)*2.f; break;
                        default: break;
                    }
                    V2 p={x, rcy+waveY};
                    if(hasPrev){
                        SDL_Color col=getColor(xf*360.f,fabsf(waveY),i);
                        col.a=(Uint8)(col.a*(1.f-lf*0.6f));
                        stroke(r,prev2,p,1.5f+trigVal*2.f,col,col);
                    }
                    prev2=p; hasPrev=true;
                }
            }
            break;}

        case EffShape::Grid:{
            int rows=2+dens/3;
            float spacing=sz*2.f/rows;
            for(int row=0;row<rows;++row){
                for(int col2=0;col2<rows;++col2){
                    float gx=rcx-sz+col2*spacing+spacing*0.5f;
                    float gy=rcy-sz+row*spacing+spacing*0.5f;
                    float dist=sqrtf((gx-rcx)*(gx-rcx)+(gy-rcy)*(gy-rcy));
                    float dotSize=4.f+trigVal*8.f;
                    switch(e.motion){
                        case EffMotion::Breathe:  dotSize*=0.5f+0.5f*sinf(ph+dist*0.05f); break;
                        case EffMotion::Ripple:   dotSize*=0.5f+0.5f*sinf(ph*2.f-dist*0.08f); break;
                        case EffMotion::Bounce:   dotSize*=0.3f+0.7f*fabsf(sinf(ph+dist*0.04f)); break;
                        case EffMotion::Spin:{
                            float ang2=atan2f(gy-rcy,gx-rcx)+ph+symAngle;
                            gx=rcx+cosf(ang2)*dist; gy=rcy+sinf(ang2)*dist; break;}
                        case EffMotion::Explode:  dotSize*=1.f+trigVal*0.5f; break;
                        case EffMotion::Converge:{
                            float ang2=atan2f(gy-rcy,gx-rcx);
                            float pullR=dist*(1.f-trigVal*0.4f);
                            gx=rcx+cosf(ang2)*pullR; gy=rcy+sinf(ang2)*pullR; break;}
                        default: break;
                    }
                    if(dotSize>1.f){
                        float ang2=atan2f(gy-rcy,gx-rcx);
                        SDL_Color col=getColor(ang2,dist,row*rows+col2);
                        // Trail: ghost dots
                        if(e.trailLen>0.f){
                            SDL_Color tc=col; tc.a=(Uint8)(col.a*e.trailLen*0.2f);
                            softCircle(r,{gx+cosf(ang2+symAngle)*dotSize*e.trailLen*2.f,
                                          gy+sinf(ang2+symAngle)*dotSize*e.trailLen*2.f},
                                       dotSize*0.6f,tc,{tc.r,tc.g,tc.b,0},4);
                        }
                        softCircle(r,{gx,gy},dotSize,col,{col.r,col.g,col.b,0},8);
                    }
                }
            }
            break;}

        case EffShape::Star:{
            int points=3+(int)(e.density*7.f);
            float spinOff=(e.motion==EffMotion::Spin)?ph:
                          (e.motion==EffMotion::Shake)?sinf(ph*8.f)*0.3f:0.f;
            spinOff+=symAngle;
            float outerR=sz*(0.8f+(e.motion==EffMotion::Breathe?0.2f*sinf(ph):0.f)
                              +(e.motion==EffMotion::Explode?trigVal*0.4f:0.f)
                              +(e.motion==EffMotion::Bounce?0.2f*fabsf(sinf(ph*2.f)):0.f));
            float innerR=outerR*(0.2f+e.density*0.3f);
            for(int p=0;p<points;++p){
                float a0=(float)p/points*2*(float)M_PI+spinOff;
                float a1=(float)(p+0.5f)/points*2*(float)M_PI+spinOff;
                float a2=(float)(p+1)/points*2*(float)M_PI+spinOff;
                V2 tip ={rcx+cosf(a0)*outerR, rcy+sinf(a0)*outerR};
                V2 in1 ={rcx+cosf(a1)*innerR, rcy+sinf(a1)*innerR};
                V2 tip2={rcx+cosf(a2)*outerR, rcy+sinf(a2)*outerR};
                SDL_Color col=getColor(a0,outerR,p);
                SDL_Color c2=col; c2.a=col.a/4;
                stroke(r,tip,in1,1.5f+trigVal*3.f,c2,col);
                stroke(r,in1,tip2,1.5f+trigVal*3.f,col,c2);
                V2 ctr={rcx,rcy};
                triC(r,ctr,in1,tip,c2,c2,col);
                triC(r,ctr,tip,in1,col,c2,c2);
            }
            break;}

        case EffShape::Flower:{
            int petals=3+(int)(e.density*9.f);
            float petalR=sz*0.45f;
            float spinOff=(e.motion==EffMotion::Spin)?ph:0.f;
            spinOff+=symAngle;
            float orbitR=sz*(0.3f+(e.motion==EffMotion::Breathe?0.1f*sinf(ph):0.f)
                              +(e.motion==EffMotion::Bounce?0.1f*fabsf(sinf(ph*2.f)):0.f));
            for(int p=0;p<petals;++p){
                float ang=(float)p/petals*2*(float)M_PI+spinOff;
                float pcx2=rcx+cosf(ang)*orbitR;
                float pcy2=rcy+sinf(ang)*orbitR;
                float pr=petalR*(0.7f+0.3f*(e.motion==EffMotion::Explode?trigVal:1.f));
                SDL_Color col=getColor(ang,orbitR,p);
                int segs=16;
                for(int s=0;s<segs;++s){
                    float a0=(float)s/segs*2*(float)M_PI;
                    float a1=(float)(s+1)/segs*2*(float)M_PI;
                    V2 p0={pcx2+cosf(a0)*pr,pcy2+sinf(a0)*pr};
                    V2 p1={pcx2+cosf(a1)*pr,pcy2+sinf(a1)*pr};
                    stroke(r,p0,p1,1.f+trigVal*2.f,col,col);
                }
                // Trail ghost petal
                if(e.trailLen>0.f){
                    SDL_Color tc=col; tc.a=(Uint8)(col.a*e.trailLen*0.15f);
                    float tAng=ang-(e.motion==EffMotion::Spin?ph*0.3f:0.f);
                    float tpcx=rcx+cosf(tAng)*orbitR; float tpcy=rcy+sinf(tAng)*orbitR;
                    for(int s=0;s<segs;++s){
                        float a0=(float)s/segs*2*(float)M_PI;
                        float a1=(float)(s+1)/segs*2*(float)M_PI;
                        stroke(r,{tpcx+cosf(a0)*pr,tpcy+sinf(a0)*pr},
                                 {tpcx+cosf(a1)*pr,tpcy+sinf(a1)*pr},0.8f,tc,tc);
                    }
                }
            }
            break;}

        case EffShape::Tunnel:{
            int rings2=6+(int)(e.density*8.f);
            int sides=3+(int)(e.density*5.f);
            float zoom=fmod(ph*0.5f,1.f);
            for(int ri=0;ri<rings2;++ri){
                float rf=((float)ri/rings2+zoom);
                if(rf>1.f) rf-=1.f;
                float r2=sz*rf*(0.8f+(e.motion==EffMotion::Breathe?0.2f*sinf(ph+ri):1.f)
                                    +(e.motion==EffMotion::Bounce?0.15f*fabsf(sinf(ph+ri)):0.f));
                float spinOff=(e.motion==EffMotion::Spin)?ph*(1.f-rf)*2.f:0.f;
                spinOff+=symAngle;
                SDL_Color col=getColor(rf*360.f,r2,ri);
                col.a=(Uint8)(col.a*(1.f-rf*0.7f));
                for(int s=0;s<sides;++s){
                    float a0=(float)s/sides*2*(float)M_PI+spinOff;
                    float a1=(float)(s+1)/sides*2*(float)M_PI+spinOff;
                    V2 p0={rcx+cosf(a0)*r2,rcy+sinf(a0)*r2};
                    V2 p1={rcx+cosf(a1)*r2,rcy+sinf(a1)*r2};
                    stroke(r,p0,p1,1.5f+trigVal*2.f*(1.f-rf),col,col);
                }
            }
            break;}

        case EffShape::Lissajous:{
            float freqA=1.f+(int)(e.density*4.f);
            float freqB=freqA+(e.motion==EffMotion::Drift?sinf(ph*0.3f)*2.f:1.f);
            float phaseOff=(e.motion==EffMotion::Spin)?ph:
                           (e.motion==EffMotion::Ripple)?sinf(ph)*1.5f:(float)M_PI/2.f;
            phaseOff+=symAngle;
            int pts=120+(int)(e.density*80.f);
            V2 prev2={0,0}; bool hasPrev=false;
            for(int i=0;i<pts+1;++i){
                float tf=(float)i/pts*2*(float)M_PI;
                float ex=sz*0.9f*sinf(freqA*tf+phaseOff);
                float ey2=sz*0.9f*sinf(freqB*tf);
                if(e.motion==EffMotion::Shake){ex+=sinf(ph*12.f)*sz*0.05f*trigVal;}
                if(e.motion==EffMotion::Breathe){float s2=0.7f+0.3f*sinf(ph);ex*=s2;ey2*=s2;}
                if(e.motion==EffMotion::Bounce){float s2=0.5f+0.5f*fabsf(sinf(ph));ex*=s2;ey2*=s2;}
                V2 p={rcx+ex,rcy+ey2};
                if(hasPrev){
                    float dist=sqrtf(ex*ex+ey2*ey2);
                    SDL_Color col=getColor(tf*57.3f,dist,i);
                    float trailAlpha=e.trailLen>0.f?clamp01(0.3f+0.7f*(1.f-(float)i/pts*e.trailLen)):1.f;
                    col.a=(Uint8)(col.a*trailAlpha);
                    stroke(r,prev2,p,1.f+trigVal*2.f,col,col);
                }
                prev2=p; hasPrev=true;
            }
            break;}

        case EffShape::Web:{
            // Spider web — concentric rings connected by radial threads
            int threads=4+(int)(e.density*8.f);
            int rings2=3+(int)(e.density*4.f);
            float spinOff=(e.motion==EffMotion::Spin)?ph:symAngle;
            for(int ri=0;ri<rings2;++ri){
                float rf=(float)(ri+1)/rings2;
                float r2=sz*rf*(0.8f+(e.motion==EffMotion::Breathe?0.2f*sinf(ph+ri):0.f)
                                    +(e.motion==EffMotion::Bounce?0.15f*fabsf(sinf(ph+ri)):0.f));
                for(int ti=0;ti<threads;++ti){
                    float a0=(float)ti/threads*2*(float)M_PI+spinOff;
                    float a1=(float)(ti+1)/threads*2*(float)M_PI+spinOff;
                    V2 p0={rcx+cosf(a0)*r2,rcy+sinf(a0)*r2};
                    V2 p1={rcx+cosf(a1)*r2,rcy+sinf(a1)*r2};
                    SDL_Color col=getColor(a0,r2,ri*threads+ti);
                    col.a=(Uint8)(col.a*(0.4f+rf*0.6f));
                    stroke(r,p0,p1,1.f+trigVal*1.5f,col,col);
                    if(ri==0){
                        V2 ctr={rcx,rcy};
                        stroke(r,ctr,p0,0.8f,{col.r,col.g,col.b,(Uint8)(col.a*0.5f)},col);
                    }
                }
            }
            break;}

        case EffShape::Helix:{
            // 3D helix with two interleaved strands
            int pts=80+dens*6;
            float helixR=sz*0.4f;
            float helixH=sz*0.8f;
            for(int strand=0;strand<2;++strand){
                float strandOff=strand*(float)M_PI;
                V2 prev2={0,0}; bool hasPrev=false;
                for(int i=0;i<pts;++i){
                    float tf=(float)i/pts;
                    float ang=tf*4.f*(float)M_PI+ph+strandOff+symAngle;
                    float hx=rcx+cosf(ang)*helixR;
                    float hy=rcy-helixH*0.5f+tf*helixH;
                    float z=sinf(ang); // depth
                    float scale=0.6f+0.4f*((z+1.f)*0.5f);
                    switch(e.motion){
                        case EffMotion::Breathe: helixR=sz*(0.3f+0.1f*sinf(ph+tf*2.f)); break;
                        case EffMotion::Ripple:  hx+=cosf(ph*3.f+tf*8.f)*sz*0.05f; break;
                        case EffMotion::Shake:   hx+=sinf(ph*8.f)*sz*0.04f*trigVal; break;
                        case EffMotion::Bounce:  hy+=sz*0.1f*fabsf(sinf(ph*2.f+tf*4.f)); break;
                        default: break;
                    }
                    V2 p={hx,hy};
                    if(hasPrev){
                        SDL_Color col=getColor(ang,helixR,i);
                        col.a=(Uint8)(col.a*scale);
                        float trailA=e.trailLen>0.f?clamp01(1.f-tf*e.trailLen*0.8f):1.f;
                        col.a=(Uint8)(col.a*trailA);
                        stroke(r,prev2,p,1.5f+scale*2.f+trigVal*2.f,col,col);
                    }
                    prev2=p; hasPrev=true;
                    // Base pair connector
                    if(strand==0&&i%4==0){
                        float a2=ang+(float)M_PI;
                        V2 pair={rcx+cosf(a2)*helixR,hy};
                        SDL_Color bc=getColor(a2,helixR,i+1000);
                        bc.a=(Uint8)(bc.a*0.4f*scale);
                        stroke(r,p,pair,0.8f,bc,bc);
                    }
                }
            }
            break;}

        case EffShape::Snowflake:{
            // 6-fold symmetry with recursive branches
            int arms=6;
            float spinOff=(e.motion==EffMotion::Spin)?ph:symAngle;
            float armLen=sz*(0.7f+(e.motion==EffMotion::Breathe?0.3f*sinf(ph):0.f)
                              +(e.motion==EffMotion::Bounce?0.2f*fabsf(sinf(ph*2.f)):0.f)
                              +(e.motion==EffMotion::Explode?trigVal*0.3f:0.f));
            int branchLevels=1+(int)(e.density*2.f);
            for(int arm=0;arm<arms;++arm){
                float baseAng=(float)arm/arms*2*(float)M_PI+spinOff;
                // Main arm
                V2 armTip={rcx+cosf(baseAng)*armLen, rcy+sinf(baseAng)*armLen};
                SDL_Color col=getColor(baseAng,armLen,arm);
                stroke(r,{rcx,rcy},armTip,1.5f+trigVal*2.f,{col.r,col.g,col.b,(Uint8)(col.a*0.4f)},col);
                // Side branches
                for(int lv=1;lv<=branchLevels;++lv){
                    float bf=(float)lv/(branchLevels+1);
                    V2 broot={rcx+cosf(baseAng)*armLen*bf, rcy+sinf(baseAng)*armLen*bf};
                    float bLen=armLen*(0.3f*(1.f-bf*0.5f));
                    for(int side=-1;side<=1;side+=2){
                        float bAng=baseAng+(float)side*(float)M_PI/3.f;
                        V2 btip={broot.x+cosf(bAng)*bLen, broot.y+sinf(bAng)*bLen};
                        SDL_Color bc=getColor(bAng,bLen,arm*10+lv*2+side);
                        bc.a=(Uint8)(bc.a*(1.f-bf*0.4f));
                        stroke(r,broot,btip,1.f+trigVal,{bc.r,bc.g,bc.b,(Uint8)(bc.a*0.3f)},bc);
                    }
                }
            }
            break;}

        case EffShape::Polygon:{
            // Regular polygon that morphs, fills, and layers
            int sides=3+(int)(e.polySides*9.f);
            int layers=1+(int)(e.trailLen*4.f);
            float spinOff=(e.motion==EffMotion::Spin)?ph:symAngle;
            for(int layer=0;layer<layers;++layer){
                float lf=(float)layer/layers;
                float r2=sz*(0.2f+lf*0.8f);
                switch(e.motion){
                    case EffMotion::Breathe:  r2*=0.7f+0.3f*sinf(ph+lf*2.f); break;
                    case EffMotion::Explode:  r2*=1.f+trigVal*0.5f*(1.f-lf); break;
                    case EffMotion::Bounce:   r2*=0.6f+0.4f*fabsf(sinf(ph+lf*2.f)); break;
                    case EffMotion::Ripple:   r2*=1.f+0.1f*sinf(ph*3.f-lf*6.f); break;
                    case EffMotion::Converge: r2*=(1.f-lf*trigVal*0.5f); break;
                    default: break;
                }
                // Morph between polygon and circle using density
                float morphT=e.motion==EffMotion::Drift?0.5f+0.5f*sinf(ph*0.5f):0.f;
                int segs=sides*4;
                V2 prev2={0,0}; bool hasPrev=false;
                for(int s=0;s<=segs;++s){
                    float ang=(float)s/segs*2*(float)M_PI+spinOff+lf*0.3f;
                    // Polygon radius at this angle
                    float polyAng=fmod(ang,2*(float)M_PI/sides);
                    float polyR=r2*cosf((float)M_PI/sides)/cosf(polyAng-(float)M_PI/sides);
                    float actualR=polyR*(1.f-morphT)+r2*morphT;
                    V2 p={rcx+cosf(ang)*actualR, rcy+sinf(ang)*actualR};
                    if(hasPrev){
                        SDL_Color col=getColor(ang,actualR,layer*segs+s);
                        col.a=(Uint8)(col.a*(0.3f+lf*0.7f));
                        stroke(r,prev2,p,1.f+trigVal*2.f*(1.f-lf*0.5f),col,col);
                    }
                    prev2=p; hasPrev=true;
                }
            }
            break;}

        case EffShape::Comet:{
            // Particles streaming in arcs with bright heads and fading tails
            int numComets=2+(int)(e.density*8.f);
            for(int ci=0;ci<numComets;++ci){
                float cf=(float)ci/numComets;
                float orbitR=sz*(0.3f+cf*0.7f);
                float startAng=ph+cf*2.f*(float)M_PI+symAngle;
                float tailLen=(0.3f+e.trailLen*1.2f)*(float)M_PI;
                // Apply scatter
                orbitR*=1.f+e.scatter*0.3f*sinf(ph*3.f+ci*1.7f);
                int tailSegs=16+(int)(e.trailLen*32.f);
                V2 prev2={0,0}; bool hp=false;
                for(int s=0;s<=tailSegs;++s){
                    float sf=(float)s/tailSegs;
                    float ang=startAng-tailLen*(1.f-sf);
                    switch(e.motion){
                        case EffMotion::Breathe: orbitR*=1.f+0.1f*sinf(ph+cf*2.f); break;
                        case EffMotion::Swirl:   ang+=sinf(ph+sf*3.f)*0.4f; break;
                        default: break;
                    }
                    V2 p={rcx+cosf(ang)*orbitR, rcy+sinf(ang)*orbitR};
                    if(hp){
                        SDL_Color col=getColor(ang,orbitR,ci*tailSegs+s);
                        col.a=(Uint8)(col.a*sf*(0.5f+trigVal*0.5f));
                        float w=sf*(1.5f+trigVal*3.f);
                        stroke(r,prev2,p,w,col,col);
                    }
                    prev2=p; hp=true;
                }
                // Bright head
                SDL_Color hc=getColor(startAng,orbitR,ci); hc.a=230;
                SDL_Rect dot={(int)(rcx+cosf(startAng)*orbitR)-3,(int)(rcy+sinf(startAng)*orbitR)-3,6,6};
                SDL_SetRenderDrawColor(r,hc.r,hc.g,hc.b,hc.a); SDL_RenderFillRect(r,&dot);
            }
            break;}

        case EffShape::Matrix:{
            // Falling columns of glowing characters (rects)
            int cols=4+(int)(e.density*12.f);
            float colW=sz*2.f/cols;
            for(int col=0;col<cols;++col){
                float cf=(float)col/cols;
                // Each column has a different phase offset and speed
                float colPhase=ph*(1.f+cf*0.3f)+cf*7.3f;
                float colSpeed=0.5f+cf*0.5f+e.speed*0.5f;
                int glyphs=3+(int)(e.density*6.f);
                for(int g=0;g<glyphs;++g){
                    float gf=(float)g/glyphs;
                    float yOff=fmod(colPhase*colSpeed+gf,1.f)*sz*2.f-sz;
                    // Apply scatter
                    float xOff=(e.scatter>0.01f)?e.scatter*8.f*sinf(colPhase+g*2.3f):0.f;
                    float bri=1.f-gf*0.7f; // head brightest
                    SDL_Color col2=getColor(cf*2*(float)M_PI,sz*gf,col*glyphs+g);
                    col2.a=(Uint8)(col2.a*bri*(0.3f+trigVal*0.7f));
                    int gx=(int)(rcx-sz+col*colW*2.f+xOff);
                    int gy=(int)(rcy+yOff);
                    int gw=(int)(colW*1.6f);
                    SDL_SetRenderDrawColor(r,col2.r,col2.g,col2.b,col2.a);
                    SDL_Rect gr={gx,gy,std::max(2,gw),std::max(2,(int)(colW*2.4f))};
                    SDL_RenderFillRect(r,&gr);
                }
            }
            break;}

        case EffShape::Arc:{
            // Sweeping arcs that open/close with the music
            int numArcs=1+(int)(e.density*5.f);
            for(int ai=0;ai<numArcs;++ai){
                float af=(float)ai/numArcs;
                float arcR=sz*(0.2f+af*0.8f)*(1.f+e.innerRadius*0.5f);
                float arcStart=ph+af*2.f*(float)M_PI/numArcs+symAngle;
                float sweep=(0.2f+e.waveFreq*1.6f)*(float)M_PI*(1.f+trigVal*0.5f);
                switch(e.motion){
                    case EffMotion::Breathe:  arcR*=0.7f+0.3f*sinf(ph+af*2.f); break;
                    case EffMotion::Explode:  sweep*=1.f+trigVal*0.8f; break;
                    case EffMotion::PulseM:   arcR*=0.5f+0.5f*fabsf(sinf(ph*(1.f+e.pulseRate*4.f))); break;
                    case EffMotion::Swirl:    arcStart+=sinf(ph*0.7f+af)*0.8f; break;
                    case EffMotion::ScatterM: arcStart+=e.scatter*sinf(ph*3.f+af*2.f); break;
                    default: break;
                }
                int segs=24+(int)(e.density*32.f);
                V2 prev2={0,0}; bool hp=false;
                for(int s=0;s<=segs;++s){
                    float ang=arcStart+(float)s/segs*sweep;
                    V2 p={rcx+cosf(ang)*arcR, rcy+sinf(ang)*arcR};
                    if(hp){
                        SDL_Color col=getColor(ang,arcR,ai*segs+s);
                        float edgeFade=sinf((float)s/segs*(float)M_PI); // fade at tips
                        col.a=(Uint8)(col.a*edgeFade);
                        stroke(r,prev2,p,1.f+trigVal*3.f*(1.f-af*0.5f),col,col);
                    }
                    prev2=p; hp=true;
                }
            }
            break;}

        default: break;
        } // end shape switch
    }; // end drawInstance lambda

    // Draw symmetry copies
    for(int sym=0;sym<symCopies;++sym){
        float symAngle=(float)sym/symCopies*2*(float)M_PI;
        drawInstance(symAngle);
    }
}


void saveCustomEffects(){
    std::ofstream f(g_exeDir+"custom_effects.dat");
    f<<"EFFECTS3 "<<g_numCustomEffects<<"\n";
    for(int i=0;i<g_numCustomEffects;++i){
        auto& e=g_customEffects[i];
        f<<e.name<<"\n";
        f<<(int)e.shape<<" "<<(int)e.color<<" "<<(int)e.motion<<" "<<(int)e.trigger<<" "<<(int)e.blend<<"\n";
        f<<e.hue<<" "<<e.hue2<<" "<<e.size<<" "<<e.speed<<" "<<e.density<<" "
         <<e.alpha<<" "<<e.trailLen<<" "<<e.symmetry<<" "<<e.reactivity<<" "<<(int)e.enabled<<"\n";
        f<<e.innerRadius<<" "<<e.waveFreq<<" "<<e.polySides<<" "<<e.pulseRate<<" "
         <<e.colorSpeed<<" "<<e.scatter<<"\n";
    }
}

void loadCustomEffects(){
    std::ifstream f(g_exeDir+"custom_effects.dat");
    if(!f) return;
    std::string line; int n=0;
    if(std::getline(f,line)){
        std::istringstream ss(line); std::string ver; ss>>ver>>n;
        bool isV2=(ver=="EFFECTS2"||ver=="EFFECTS3");
        bool isV3=(ver=="EFFECTS3");
        g_numCustomEffects=std::min(n,MAX_CUSTOM_EFFECTS);
        for(int i=0;i<g_numCustomEffects;++i){
            auto& e=g_customEffects[i];
            e=CustomEffect{};
            std::string nm,l1,l2,l3;
            std::getline(f,nm); e.name=nm;
            if(std::getline(f,l1)){
                std::istringstream ss2(l1); int sh,co,mo,tr,bl=0;
                ss2>>sh>>co>>mo>>tr; if(isV2) ss2>>bl;
                e.shape  =(EffShape)  std::min(sh,(int)EffShape::COUNT-1);
                e.color  =(EffColor)  std::min(co,(int)EffColor::COUNT-1);
                e.motion =(EffMotion) std::min(mo,(int)EffMotion::COUNT-1);
                e.trigger=(EffTrigger)std::min(tr,(int)EffTrigger::COUNT-1);
                e.blend  =(EffBlend)  std::min(bl,(int)EffBlend::COUNT-1);
            }
            if(std::getline(f,l2)){
                std::istringstream ss2(l2); int en;
                if(isV2)
                    ss2>>e.hue>>e.hue2>>e.size>>e.speed>>e.density>>e.alpha>>e.trailLen>>e.symmetry>>e.reactivity>>en;
                else
                    ss2>>e.hue>>e.size>>e.speed>>e.density>>e.alpha>>en;
                e.enabled=en;
            }
            if(isV3&&std::getline(f,l3)){
                std::istringstream ss2(l3);
                ss2>>e.innerRadius>>e.waveFreq>>e.polySides>>e.pulseRate>>e.colorSpeed>>e.scatter;
            }
        }
    }
}

void loadLibrary(){
    g_library.clear();g_playlists.clear();
    std::ifstream f(getConfigPath());
    if(!f)return;
    std::string line;
    int nSongs=0;
    std::getline(f,line);
    if(line.substr(0,5)=="SONGS") nSongs=std::stoi(line.substr(6));
    for(int i=0;i<nSongs;++i){
        std::getline(f,line);
        if(!line.empty()&&fs::exists(line))
            g_library.push_back({line,baseName(line)});
    }
    int nPL=0;
    if(std::getline(f,line)&&line.substr(0,9)=="PLAYLISTS")
        nPL=std::stoi(line.substr(10));
    for(int p=0;p<nPL;++p){
        Playlist pl;
        if(std::getline(f,line)&&line.substr(0,2)=="PL") pl.name=line.substr(3);
        int cnt=0;
        if(std::getline(f,line)&&line.substr(0,5)=="COUNT") cnt=std::stoi(line.substr(6));
        for(int i=0;i<cnt;++i){
            std::getline(f,line);
            int idx=std::stoi(line);
            if(idx>=0&&idx<(int)g_library.size()) pl.songIndices.push_back(idx);
        }
        g_playlists.push_back(pl);
    }
}

void addToLibrary(const std::string& path){
    for(auto& s:g_library) if(s.path==path) return;
    g_library.push_back({path,baseName(path)});
    saveLibrary();
}

void playLibrarySong(int idx){
    if(idx<0||idx>=(int)g_library.size())return;
    g_currentSong=idx;
    loadAndPlay(g_library[idx].path);
}

void playNext(){
    if(g_currentPL>=0&&g_currentPL<(int)g_playlists.size()){
        auto& pl=g_playlists[g_currentPL];
        if(pl.songIndices.empty())return;
        g_currentPLTrack=(g_currentPLTrack+1)%(int)pl.songIndices.size();
        playLibrarySong(pl.songIndices[g_currentPLTrack]);
    } else if(!g_library.empty()){
        g_currentSong=(g_currentSong+1)%(int)g_library.size();
        playLibrarySong(g_currentSong);
    }
}
void playPrev(){
    if(g_currentPL>=0&&g_currentPL<(int)g_playlists.size()){
        auto& pl=g_playlists[g_currentPL];
        if(pl.songIndices.empty())return;
        g_currentPLTrack=((g_currentPLTrack-1)+(int)pl.songIndices.size())%(int)pl.songIndices.size();
        playLibrarySong(pl.songIndices[g_currentPLTrack]);
    } else if(!g_library.empty()){
        g_currentSong=((g_currentSong-1)+(int)g_library.size())%(int)g_library.size();
        playLibrarySong(g_currentSong);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// RADIAL MENU SYSTEM
// ═══════════════════════════════════════════════════════════════════════════════

// Menu button lives at top-center, updated each frame to ww/2
static float          MENU_BTN_X  = 640.f;
static constexpr float MENU_BTN_Y  = 36.f;
static constexpr float MENU_BTN_R  = 22.f;
static constexpr float MENU_ITEM_R = 22.f;
static constexpr float MENU_DIST   = 180.f;
// Arc fans downward symmetrically centered on 90deg (straight down in SDL coords)
// 20deg = down-right, 160deg = down-left, passing through 90deg = straight down
static constexpr float MENU_ARC_START = 20.f * (float)M_PI / 180.f;
static constexpr float MENU_ARC_END   = 160.f * (float)M_PI / 180.f;

enum class MenuAction { None, Library, Playlists, AddSong, PlayPause, Prev, Next, NewPlaylist, Spotify, Theme };
static constexpr int MENU_ITEM_COUNT = 9;

struct MenuItem {
    MenuAction  action;
    const char* label;
    const char* icon;
    float       hue;
};

static const MenuItem MENU_ITEMS[MENU_ITEM_COUNT] = {
    { MenuAction::Prev,        "Prev",      "<<",  200.f },
    { MenuAction::PlayPause,   "Play/Pause","||>", 120.f },
    { MenuAction::Next,        "Next",      ">>",  200.f },
    { MenuAction::AddSong,     "Add Song",  "+",    80.f },
    { MenuAction::Library,     "Library",   "LIB", 180.f },
    { MenuAction::Playlists,   "Playlists", "PLS", 280.f },
    { MenuAction::NewPlaylist, "New List",  "+PL", 320.f },
    { MenuAction::Spotify,     "Spotify",   "SPT", 135.f },
    { MenuAction::Theme,       "Theme",     "THM",  60.f },
};

// Menu state
static float g_menuOpen      = 0.f;  // 0=closed, 1=open (animated)
static bool  g_menuTarget    = false; // true = open, false = closed
static float g_menuBtnPulse  = 0.f;  // time-driven pulse on the button
static int   g_menuHover     = -1;   // which item is hovered (-1 = none)
// Per-item stagger: each item has its own animation progress
static float g_itemAnim[MENU_ITEM_COUNT] = {};
// Ripple rings emitted on open/close
struct MenuRipple { float r, alpha, hue; };
static std::vector<MenuRipple> g_menuRipples;

// Compute world position of menu item i given current open progress
V2 menuItemPos(int i, float openProg) {
    float t = (float)i / (MENU_ITEM_COUNT - 1);
    float ang = MENU_ARC_START + t * (MENU_ARC_END - MENU_ARC_START);
    // Spring the distance out
    float dist = MENU_DIST * openProg;
    return { MENU_BTN_X + cosf(ang) * dist,
             MENU_BTN_Y + sinf(ang) * dist };
}

// Draw a glowing circle button (used for both trigger and items)
void drawGlowCircle(SDL_Renderer* r, V2 pos, float radius,
                    SDL_Color col, float glowStrength, float alpha,
                    const std::string& label, bool centered=true) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    // Outer glow layers
    int glowLayers = 3;
    for(int g=glowLayers;g>=0;--g){
        float gf = (float)g/glowLayers;
        float gr = radius * (1.f + gf * 1.6f * glowStrength);
        SDL_Color gc = col;
        gc.a = (Uint8)(alpha * (1.f - gf) * (1.f - gf) * glowStrength * 0.6f * 255.f);
        softCircle(r, pos, gr, gc, {gc.r, gc.g, gc.b, 0}, 20);
    }
    // Main body — slightly filled
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_Color body = col;
    body.a = (Uint8)(alpha * 0.18f * 255.f);
    softCircle(r, pos, radius, body, {body.r, body.g, body.b, 0}, 24);
    // Crisp ring
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    SDL_Color ring = col;
    ring.a = (Uint8)(alpha * 255.f);
    int segs = 32;
    for(int i=0;i<segs;++i){
        float a0=(float)i/segs*2*(float)M_PI;
        float a1=(float)(i+1)/segs*2*(float)M_PI;
        stroke(r,
            {pos.x+cosf(a0)*radius, pos.y+sinf(a0)*radius},
            {pos.x+cosf(a1)*radius, pos.y+sinf(a1)*radius},
            1.5f, ring, ring);
    }
    // Label
    if(!label.empty() && g_fontSm) {
        SDL_Color tc = col;
        tc.a = (Uint8)(alpha * 255.f);
        drawText(r, g_fontSm, label,
                 (int)pos.x, (int)(pos.y - 6),
                 tc, centered);
    }
}

// Draw three horizontal lines (hamburger) or an X, animating between them
void drawHamburgerIcon(SDL_Renderer* r, V2 pos, float openProg, float alpha, float hue) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    SDL_Color c = thsv(hue, 0.6f, 1.f);
    c.a = (Uint8)(alpha * 255.f);

    float sz = 10.f;
    float rot = openProg * (float)M_PI * 0.75f; // rotate toward X

    // Line 1 — top bar / diagonal
    float off1Y = -5.f * (1.f - openProg); // collapse toward center
    V2 a1 = vrot({-sz, off1Y}, rot) + pos;
    V2 b1 = vrot({ sz, off1Y}, rot) + pos;
    stroke(r, a1, b1, 1.8f, c, c);

    // Line 2 — middle bar / fades out
    SDL_Color c2 = c; c2.a = (Uint8)(c.a * (1.f - openProg));
    V2 a2 = vrot({-sz, 0.f}, 0.f) + pos;
    V2 b2 = vrot({ sz, 0.f}, 0.f) + pos;
    stroke(r, a2, b2, 1.8f, c2, c2);

    // Line 3 — bottom bar / opposite diagonal
    float off3Y = 5.f * (1.f - openProg);
    V2 a3 = vrot({-sz, off3Y}, -rot) + pos;
    V2 b3 = vrot({ sz, off3Y}, -rot) + pos;
    stroke(r, a3, b3, 1.8f, c, c);
}

void updateMenuRipples(float dt) {
    for(auto& rp : g_menuRipples){
        rp.r += dt * 180.f;
        rp.alpha -= dt * 2.2f;
    }
    g_menuRipples.erase(
        std::remove_if(g_menuRipples.begin(), g_menuRipples.end(),
            [](const MenuRipple& rp){ return rp.alpha <= 0.f; }),
        g_menuRipples.end());
}

void drawMenuRipples(SDL_Renderer* r) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    V2 orig = {MENU_BTN_X, MENU_BTN_Y};
    for(auto& rp : g_menuRipples){
        int segs = 32;
        SDL_Color c = thsv(rp.hue, 0.7f, 1.f);
        c.a = (Uint8)(std::clamp(rp.alpha, 0.f, 1.f) * 180.f);
        for(int i=0;i<segs;++i){
            float a0=(float)i/segs*2*(float)M_PI;
            float a1=(float)(i+1)/segs*2*(float)M_PI;
            stroke(r,
                {orig.x+cosf(a0)*rp.r, orig.y+sinf(a0)*rp.r},
                {orig.x+cosf(a1)*rp.r, orig.y+sinf(a1)*rp.r},
                1.5f, c, c);
        }
    }
}

// Draw arc "connector" lines from the trigger to each item so it looks like
// a proper fan structure
void drawMenuArcs(SDL_Renderer* r, float openProg, float t) {
    if(openProg < 0.01f) return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    V2 orig = {MENU_BTN_X, MENU_BTN_Y};
    for(int i = 0; i < MENU_ITEM_COUNT; ++i){
        float ip = clamp01(g_itemAnim[i]);
        if(ip < 0.01f) continue;
        V2 itemP = menuItemPos(i, openProg);
        float hue = fmod(MENU_ITEMS[i].hue + t * 20.f, 360.f);
        SDL_Color c = thsv(hue, 0.8f, 1.f);
        c.a = (Uint8)(ip * 0.35f * 255.f);
        SDL_Color ce = c; ce.a = 0;
        // Draw the connector spoke
        int steps = 12;
        for(int s = 0; s < steps; ++s){
            float tf0 = (float)s/steps, tf1 = (float)(s+1)/steps;
            V2 p0 = orig + (itemP - orig) * tf0;
            V2 p1 = orig + (itemP - orig) * tf1;
            SDL_Color sc = c; sc.a = (Uint8)(c.a * tf0);
            stroke(r, p0, p1, 0.8f, ce, sc);
        }
    }
}

// Draw the full radial menu
void drawRadialMenu(SDL_Renderer* r, float t, float uiAlpha, float bass, float overall) {
    float alpha = uiAlpha;
    if(alpha < 0.01f) return;

    // Trigger button pulse (always visible, subtly)
    g_menuBtnPulse = t;
    float pulse = sinf(g_menuBtnPulse * 2.f) * 0.15f + 0.85f;
    float btnHue = fmod(t * 30.f, 360.f);
    // The button glows stronger when open
    float btnGlow = 0.4f + g_menuOpen * 0.7f;
    float btnR = MENU_BTN_R * (1.f + bass * 0.12f + (g_menuOpen > 0.5f ? 0.05f : 0.f));

    // Draw ripples behind everything
    drawMenuRipples(r);

    // Arc connectors
    drawMenuArcs(r, g_menuOpen, t);

    // Draw each menu item
    for(int i = 0; i < MENU_ITEM_COUNT; ++i){
        float ip = clamp01(g_itemAnim[i]);
        if(ip < 0.01f) continue;
        V2 pos = menuItemPos(i, g_menuOpen);
        float hue = fmod(MENU_ITEMS[i].hue + t * 15.f, 360.f);
        // Extra glow if hovered
        bool hov = (g_menuHover == i);
        float glowStr = hov ? 1.2f : 0.5f;
        float itemAlpha = ip * alpha;
        SDL_Color col = thsv(hue, 0.9f, 1.f);
        drawGlowCircle(r, pos, MENU_ITEM_R * (hov ? 1.15f : 1.f),
                       col, glowStr, itemAlpha, "");
        // Label always below button
        if(g_fontSm && ip > 0.3f){
            SDL_Color tc = col; tc.a = (Uint8)(itemAlpha * ip * 255.f);
            drawText(r, g_fontSm, MENU_ITEMS[i].label,
                     (int)pos.x, (int)(pos.y + MENU_ITEM_R + 4.f), tc, true);
        }
        // Icon in center
        if(g_font && ip > 0.5f){
            SDL_Color ic = {255,255,255,(Uint8)(itemAlpha * ip * 255.f)};
            drawText(r, g_fontSm, MENU_ITEMS[i].icon,
                     (int)pos.x, (int)(pos.y - 7.f), ic, true);
        }
    }

    // Draw trigger button on top of everything
    SDL_Color trigCol = thsv(btnHue, g_menuOpen > 0.5f ? 0.5f : 0.8f, 1.f);
    drawGlowCircle(r, {MENU_BTN_X, MENU_BTN_Y}, btnR * pulse,
                   trigCol, btnGlow, alpha, "");
    drawHamburgerIcon(r, {MENU_BTN_X, MENU_BTN_Y}, g_menuOpen, alpha, btnHue);
}

// Hit test: is (mx,my) over the trigger button?
bool hitMenuBtn(int mx, int my){
    float dx = mx - MENU_BTN_X, dy = my - MENU_BTN_Y;
    return sqrtf(dx*dx+dy*dy) <= MENU_BTN_R + 6.f;
}

// Hit test: is (mx,my) over menu item i?
bool hitMenuItem(int mx, int my, int i, float openProg){
    if(openProg < 0.1f) return false;
    V2 pos = menuItemPos(i, openProg);
    float dx = mx - pos.x, dy = my - pos.y;
    return sqrtf(dx*dx+dy*dy) <= MENU_ITEM_R + 4.f;
}

// Returns hovered item index, or -1
int menuHoverTest(int mx, int my, float openProg){
    if(openProg < 0.2f) return -1;
    for(int i=0;i<MENU_ITEM_COUNT;++i)
        if(hitMenuItem(mx,my,i,openProg)) return i;
    return -1;
}

void toggleMenu(float t) {
    g_menuTarget = !g_menuTarget;
    // Emit ripple rings
    for(int i=0;i<3;++i)
        g_menuRipples.push_back({MENU_BTN_R * (1.f + i * 0.3f),
                                  0.9f - i * 0.2f,
                                  fmod(t * 60.f + i * 40.f, 360.f)});
}

void updateMenu(float dt) {
    float speed = 3.5f;  // slower open/close — more frames to enjoy
    float target = g_menuTarget ? 1.f : 0.f;
    g_menuOpen = lerp(g_menuOpen, target, dt * speed);
    if(g_menuOpen < 0.001f) g_menuOpen = 0.f;
    if(g_menuOpen > 0.999f) g_menuOpen = 1.f;

    // Stagger each item — wider window so items visibly cascade one by one
    for(int i=0;i<MENU_ITEM_COUNT;++i){
        // Opening: item i starts moving after a delay proportional to its index
        // Closing: reverse order (last item retracts first)
        float staggerOpen  = i * 0.10f;   // 100ms stagger between items
        float staggerClose = (MENU_ITEM_COUNT-1-i) * 0.08f;
        float localT, localClose;
        if(g_menuTarget){
            // Scale menuOpen (0→1) into per-item window
            localT = clamp01((g_menuOpen * (1.f + staggerOpen * MENU_ITEM_COUNT) - staggerOpen)
                             / 1.f);
            g_itemAnim[i] = springEase(localT);
        } else {
            localClose = clamp01(((1.f - g_menuOpen) * (1.f + staggerClose * MENU_ITEM_COUNT)
                                  - staggerClose));
            g_itemAnim[i] = clamp01(1.f - localClose * 1.6f);
        }
    }
}

// ── PANEL SYSTEM (library list / playlist list) ───────────────────────────────
enum class UIPanel { None, Library, Playlists };
static UIPanel  g_panel      = UIPanel::None;
static UIPanel  g_prevPanel  = UIPanel::None;
static int      g_libScroll  = 0;
static int      g_libScrollPrev = -1;
static bool     g_libJustOpened = false; // triggers full stagger animation
static int      g_plScroll   = 0;
static int      g_selectedPL = -1;
static char     g_newPLName[128] = "New Playlist";
static bool     g_namingPL   = false;
static int      g_hoverSong  = -1;
static bool     g_uiVisible  = true;
static float    g_idleTimer  = 0.f;
// Progress bar drag state
static bool     g_seekDragging = false;
static float    g_seekPreview  = 0.f; // 0→1 preview position while dragging

// Panel build animation
static float g_panelBuildT   = 0.f;
static float g_panelBuildSpd = 0.55f;  // full build takes ~1.8 seconds
static bool  g_panelBuilding = false;

// Naming overlay build animation
static float g_namingBuildT  = 0.f;
static bool  g_namingBuilding= false;

// Glitch character set for the type-on header effect
static const char* GLITCH_CHARS = "!@#$%^&*<>?/\\|[]{}~`01ABCDEFabcdef";
inline char glitchChar(float seed){
    int len = 34;
    return GLITCH_CHARS[(int)(fabsf(sinf(seed*7.3f))*len)%len];
}

// Type-on with heavy glitch leading edge — each char cycles through noise before resolving
std::string buildTypeOn(const std::string& target, float progress, float t){
    int total = (int)target.size();
    std::string out;
    for(int i=0;i<total;++i){
        float charT = progress * (total + 6) - i;   // leading edge moves left-to-right
        if(charT < 0.f){
            out += ' ';
        } else if(charT < 2.f){
            // Heavy glitch zone — cycles fast
            out += glitchChar(t * 18.f + i * 3.7f);
        } else if(charT < 3.5f){
            // Settling — slower glitch
            out += glitchChar(t * 6.f + i * 1.9f);
        } else {
            out += target[i];
        }
    }
    return out;
}

static constexpr int PANEL_W      = 320;
static constexpr int ROW_H        = 32;
static constexpr int VISIBLE_ROWS = 14;
static float    g_rowBuildT[VISIBLE_ROWS+2] = {}; // per-row build timers

bool inRect(int mx,int my,SDL_Rect r){
    return mx>=r.x&&mx<r.x+r.w&&my>=r.y&&my<r.y+r.h;
}

// Horizontal scanline that sweeps down the panel during build
void drawScanSweep(SDL_Renderer* r, int px, int py, int pw, int ph,
                   float progress, float t, float hue){
    if(progress <= 0.f || progress >= 1.f) return;
    float sweepY = py + progress * ph;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    // Thick glowing horizontal bar
    for(int layer=0;layer<3;++layer){
        float lf = (float)layer/6.f;
        float lh = (6.f - layer) * 3.f;
        SDL_Color c = thsv(fmod(hue + layer*12.f, 360.f), 0.9f, 1.f);
        c.a = (Uint8)((1.f-lf)*(1.f-lf) * 180.f);
        SDL_Rect sr={px, (int)(sweepY - lh/2.f), pw, (int)(lh+1.f)};
        SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a);
        SDL_RenderFillRect(r,&sr);
    }
    // Bright core line
    SDL_Color core = thsv(hue, 0.4f, 1.f); core.a = 220;
    SDL_SetRenderDrawColor(r,core.r,core.g,core.b,core.a);
    SDL_Rect coreLine={px, (int)(sweepY-1.f), pw, 2};
    SDL_RenderFillRect(r,&coreLine);
}

// Horizontal noise fill — random pixel static that resolves into solid color
// Used to make each row materialise from noise
void drawNoiseRow(SDL_Renderer* r, int x, int y, int w, int h,
                  float noiseAmt, float t, float hue, Uint8 alpha){
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    int cols = w / 4;
    for(int c=0;c<cols;++c){
        float chance = sinf(c * 2.3f + t * 28.f + hue) * 0.5f + 0.5f;
        if(chance > (1.f - noiseAmt)){
            float bright = chance * noiseAmt;
            SDL_Color col = thsv(fmod(hue + c*4.f, 360.f), 0.7f, 1.f);
            col.a = (Uint8)(bright * alpha * 0.6f);
            SDL_SetRenderDrawColor(r,col.r,col.g,col.b,col.a);
            SDL_Rect dot={x + c*4, y, 3, h};
            SDL_RenderFillRect(r,&dot);
        }
    }
}

// Draw an animated self-building panel border with double-trace and glow dot
void drawBuildingBorder(SDL_Renderer* r, int x, int y, int w, int h,
                        float progress, float t, SDL_Color col){
SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
    float perim = 2.f*(w+h);
    float drawn = progress * perim;

    SDL_Color ghost = col; ghost.a = (Uint8)(col.a * 0.12f);
    int gsegs=30;
for(int i=0;i<gsegs;++i){
        float a0=(float)i/gsegs, a1=(float)(i+1)/gsegs;
        // Map 0-1 around the rectangle perimeter
        auto perimPt=[&](float u)->V2{
            float d=u*perim;
            if(d<w) return{(float)(x+d),(float)y};
            d-=w; if(d<h) return{(float)(x+w),(float)(y+d)};
            d-=h; if(d<w) return{(float)(x+w-d),(float)(y+h)};
            d-=w; return{(float)x,(float)(y+h-(d))};
        };
        V2 p0=perimPt(a0), p1=perimPt(a1);
        stroke(r,p0,p1,0.8f,ghost,ghost);
    }

    // Main drawing trace
    auto drawSegment=[&](float startPerim, float endPerim){
        if(endPerim<=0||startPerim>=perim) return;
        startPerim=std::max(0.f,startPerim);
        endPerim=std::min(endPerim,perim);
        int steps=std::max(2,(int)((endPerim-startPerim)/3.f));
        for(int s=0;s<steps;++s){
            float d0=startPerim+(endPerim-startPerim)*(float)s/steps;
            float d1=startPerim+(endPerim-startPerim)*(float)(s+1)/steps;
            auto pt=[&](float d)->V2{
                if(d<w) return{(float)(x+d),(float)y};
                d-=w; if(d<h) return{(float)(x+w),(float)(y+d)};
                d-=h; if(d<w) return{(float)(x+w-d),(float)(y+h)};
                d-=w; return{(float)x,(float)(y+h-d)};
            };
            float tf=(float)s/steps;
            SDL_Color c=col; c.a=(Uint8)(col.a*(0.5f+tf*0.5f));
            stroke(r,pt(d0),pt(d1),2.f,c,c);
        }
    };
drawSegment(0, drawn);
// Bright glow dot at the drawing tip
    if(progress > 0.f && progress < 1.f){
        auto tipPt=[&](float d)->V2{
            if(d<w) return{(float)(x+d),(float)y};
            d-=w; if(d<h) return{(float)(x+w),(float)(y+d)};
            d-=h; if(d<w) return{(float)(x+w-d),(float)(y+h)};
            d-=w; return{(float)x,(float)(y+h-d)};
        };
        V2 tip=tipPt(std::min(drawn,perim-0.1f));
        for(int g=4;g>=0;--g){
            float gf=(float)g/4.f;
            SDL_Color gc=col; gc.a=(Uint8)((1.f-gf)*(1.f-gf)*220.f);
            softCircle(r,tip,4.f+gf*12.f,gc,{gc.r,gc.g,gc.b,0},12);
        }
    }
}

// Animated row — text glitches in from corruption, no rainbow noise
void drawAnimatedRow(SDL_Renderer* r, int x, int y, int w,
                     const std::string& label, bool selected, bool hovered,
                     Uint8 alpha, float rowProgress, float t, float hue){
    if(rowProgress <= 0.f) return;

    // Always draw the solid background immediately — no noise phase
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    Uint8 bgAlpha = (Uint8)(alpha * smoothstep(clamp01(rowProgress * 3.f)));
    if(selected)      SDL_SetRenderDrawColor(r,40,80,160,bgAlpha);
    else if(hovered)  SDL_SetRenderDrawColor(r,25,40,80,bgAlpha);
    else              SDL_SetRenderDrawColor(r,8,8,20,bgAlpha);
    SDL_Rect rc={x, y, w, ROW_H-2}; SDL_RenderFillRect(r,&rc);
    SDL_SetRenderDrawColor(r,40,60,120,(Uint8)(bgAlpha/2));
    SDL_RenderDrawRect(r,&rc);

    // Color strip on left edge
    SDL_Color tc = selected ? SDL_Color{100,180,255,bgAlpha}
                            : SDL_Color{180,200,220,bgAlpha};
    SDL_SetRenderDrawColor(r,tc.r,tc.g,tc.b,(Uint8)(bgAlpha*0.8f));
    SDL_Rect strip={x+2,y+2,3,ROW_H-6}; SDL_RenderFillRect(r,&strip);

    // Subtle scan glow on leading edge while animating
    if(rowProgress < 0.8f){
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
        SDL_Color scanC = thsv(hue, 0.8f, 1.f);
        scanC.a = (Uint8)(bgAlpha * (1.f - rowProgress/0.8f) * 0.6f);
        SDL_SetRenderDrawColor(r,scanC.r,scanC.g,scanC.b,scanC.a);
        SDL_Rect scanR={x, y, 2, ROW_H-2}; SDL_RenderFillRect(r,&scanR);
    }

    // Text — glitch corruption resolves into real name
    // Clamp text rendering to stay within panel bounds
    float typeP = clamp01((rowProgress - 0.15f) / 0.65f);
    std::string typed = buildTypeOn(label, typeP, t);
    Uint8 ta = (Uint8)(alpha * smoothstep(clamp01((rowProgress-0.1f)*3.f)));
    SDL_Color txtC = selected ? SDL_Color{100,180,255,ta} : SDL_Color{180,200,220,ta};

    // Truncate label if too long to fit in panel
    // Rough char width estimate: ~7px per char at small font
    int maxChars = (w - 20) / 7;
    if((int)typed.size() > maxChars)
        typed = typed.substr(0, maxChars-2) + "..";

    drawText(r, g_fontSm, typed, x+12, y+ROW_H/2-6, txtC);
}

// ── DRAMATIC PANEL DRAW ───────────────────────────────────────────────────────
// bp timeline (0→1 over ~1.8s):
//   0.00–0.15  ghost outline appears
//   0.05–0.40  border traces itself around the rectangle
//   0.20–0.55  scanline sweeps top-to-bottom revealing bg + header
//   0.35–0.55  header title glitches in
//   0.45–1.00  rows materialise with staggered noise→solid→text sequence
//   0.90–1.00  scrollbar fades in

void drawLibraryPanel(SDL_Renderer* r, Uint8 alpha, float buildT, float t) {
    float bp = clamp01(buildT);
    int px = 10, py = 80;
    int ph = VISIBLE_ROWS * ROW_H + 32;
    float hue = fmod(t * 20.f + g_theme.uiHue, 360.f);

    // Background — revealed progressively by scanline
    float scanProg = clamp01((bp - 0.20f) / 0.40f);
    float revealedH = scanProg * ph;
    if(revealedH > 0.f){
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r,3,5,14,(Uint8)(alpha*0.93f));
        SDL_Rect bg={px,py,PANEL_W,(int)revealedH}; SDL_RenderFillRect(r,&bg);
    }

    // Scanline sweep
    drawScanSweep(r, px, py, PANEL_W, ph, scanProg, t, hue);

    // Animated border (starts early, finishes mid-way)
    float borderProg = smoothstep(clamp01((bp - 0.05f) / 0.40f));
    SDL_Color borderCol = thsv(hue, 0.85f, 1.f);
    borderCol.a = (Uint8)(alpha * 0.95f);
    drawBuildingBorder(r, px, py, PANEL_W, ph, borderProg, t, borderCol);

    // Header bar — slides down from top after scanline reaches it
    if(bp > 0.20f){
        float hdrFade = smoothstep(clamp01((bp - 0.20f) / 0.20f));
        int hdrSlide = (int)((1.f-hdrFade)*20.f);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r,20,50,105,(Uint8)(alpha*hdrFade*0.97f));
        SDL_Rect hdr={px, py-hdrSlide, PANEL_W, 28}; SDL_RenderFillRect(r,&hdr);

        // Header separator glow line
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
        SDL_Color hline = thsv(hue, 1.f, 1.f);
        hline.a=(Uint8)(alpha*hdrFade);
        // Draw as multi-layered glow
        for(int g=0;g<4;++g){
            float gf=(float)g/4.f;
            SDL_Color gc=hline; gc.a=(Uint8)(hline.a*(1.f-gf*0.8f));
            SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
            SDL_Rect hl={px, py+27-hdrSlide-g, PANEL_W, 1+g};
            SDL_RenderFillRect(r,&hl);
        }

        // Type-on title — starts at 35%
        if(bp > 0.35f){
            float typeP = clamp01((bp - 0.35f) / 0.30f);
            std::string fullTitle = "LIBRARY  [" + std::to_string(g_library.size()) + " TRACKS]";
            std::string typed = buildTypeOn(fullTitle, typeP, t);
            // Glow behind text
            SDL_Color tglow = thsv(hue, 0.6f, 1.f);
            tglow.a = (Uint8)(alpha * hdrFade * 0.4f);
            for(int g=0;g<3;++g)
                drawText(r,g_font,typed,px+8+g,py+6-hdrSlide,tglow,false);
            SDL_Color tc = {160,220,255,(Uint8)(alpha*hdrFade)};
            drawText(r, g_font, typed, px+8, py+6-hdrSlide, tc);
        }
    }

    // Rows — each driven purely by g_rowBuildT, reset on open/scroll
    if(bp > 0.45f){
        int listY = py + 30;
        int visEnd = std::min(g_libScroll+VISIBLE_ROWS,(int)g_library.size());
        for(int i = g_libScroll; i < visEnd; ++i){
            int rowIdx = i - g_libScroll;
            float rowProg = clamp01(g_rowBuildT[rowIdx]);
            int ry = listY + rowIdx * ROW_H;
            float rowHue = fmod(hue + rowIdx * 18.f, 360.f);
            drawAnimatedRow(r, px+4, ry, PANEL_W-8,
                            g_library[i].title,
                            i==g_currentSong, i==g_hoverSong,
                            alpha, rowProg, t, rowHue);
        }
        // Scrollbar
        if((int)g_library.size()>VISIBLE_ROWS){
            float st=(float)g_libScroll/g_library.size();
            float sh=(float)VISIBLE_ROWS/g_library.size();
            int sbY=py+30+(int)(VISIBLE_ROWS*ROW_H*st);
            int sbH=std::max(20,(int)(VISIBLE_ROWS*ROW_H*sh));
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color sbc=thsv(hue,0.7f,1.f);
            sbc.a=(Uint8)(alpha*clamp01((bp-0.45f)*4.f)*0.8f);
            SDL_SetRenderDrawColor(r,sbc.r,sbc.g,sbc.b,sbc.a);
            SDL_Rect sb={px+PANEL_W-8,sbY,5,sbH};SDL_RenderFillRect(r,&sb);
        }
    }

    // Corner accent sparks — little decorative touches during build
    if(bp > 0.05f && bp < 0.6f){
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
        float sparkT = clamp01(bp/0.6f);
        for(int corner=0;corner<4;++corner){
            float cx2 = (corner%2==0) ? (float)px : (float)(px+PANEL_W);
            float cy2 = (corner<2)    ? (float)py : (float)(py+ph);
            float sparkAlpha = sinf(sparkT*(float)M_PI) * 0.9f;
            for(int sp=0;sp<5;++sp){
                float sa = (float)sp/5.f * 2*(float)M_PI + t*8.f + corner*1.57f;
                float sr = 4.f + sp*6.f*sparkT;
                SDL_Color sc = thsv(fmod(hue+corner*90.f+sp*20.f,360.f),0.9f,1.f);
                sc.a=(Uint8)(sparkAlpha*(1.f-(float)sp/5.f)*200.f);
                softCircle(r,{cx2+cosf(sa)*sr*0.3f,cy2+sinf(sa)*sr*0.3f},
                           2.f,sc,{sc.r,sc.g,sc.b,0},6);
            }
        }
    }
}

void drawPlaylistPanel(SDL_Renderer* r, Uint8 alpha, float buildT, float t) {
    float bp = clamp01(buildT);
    int px = 10, py = 80;
    int rowCount = (int)g_playlists.size() + 1;
    int ph = std::min(rowCount * ROW_H + 32, VISIBLE_ROWS * ROW_H + 32);
    float hue = fmod(t * 20.f + g_theme.uiHue, 360.f); // purple tone

    float scanProg = clamp01((bp - 0.20f) / 0.40f);
    float revealedH = scanProg * ph;
    if(revealedH > 0.f){
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r,5,2,14,(Uint8)(alpha*0.93f));
        SDL_Rect bg={px,py,PANEL_W,(int)revealedH}; SDL_RenderFillRect(r,&bg);
    }
    drawScanSweep(r, px, py, PANEL_W, ph, scanProg, t, hue);

    float borderProg = smoothstep(clamp01((bp-0.05f)/0.40f));
    SDL_Color borderCol = thsv(hue, 0.85f, 1.f);
    borderCol.a = (Uint8)(alpha*0.95f);
    drawBuildingBorder(r, px, py, PANEL_W, ph, borderProg, t, borderCol);

    if(bp > 0.20f){
        float hdrFade = smoothstep(clamp01((bp-0.20f)/0.20f));
        int hdrSlide = (int)((1.f-hdrFade)*20.f);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r,30,10,75,(Uint8)(alpha*hdrFade*0.97f));
        SDL_Rect hdr={px, py-hdrSlide, PANEL_W, 28}; SDL_RenderFillRect(r,&hdr);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
        for(int g=0;g<4;++g){
            SDL_Color gc=thsv(hue,1.f,1.f); gc.a=(Uint8)(alpha*hdrFade*(1.f-g*0.2f));
            SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
            SDL_Rect hl={px, py+27-hdrSlide-g, PANEL_W, 1+g};
            SDL_RenderFillRect(r,&hl);
        }
        if(bp > 0.35f){
            float typeP = clamp01((bp-0.35f)/0.30f);
            std::string typed = buildTypeOn("PLAYLISTS  [LOADED]", typeP, t);
            SDL_Color tglow=thsv(hue,0.5f,1.f); tglow.a=(Uint8)(alpha*hdrFade*0.4f);
            for(int g=0;g<3;++g)
                drawText(r,g_font,typed,px+8+g,py+6-hdrSlide,tglow,false);
            SDL_Color tc={210,170,255,(Uint8)(alpha*hdrFade)};
            drawText(r,g_font,typed,px+8,py+6-hdrSlide,tc);
        }
    }

    if(bp > 0.45f){
        int listY = py+30;
        float row0Prog = clamp01((bp-0.45f)/0.40f);
        drawAnimatedRow(r,px+4,listY,PANEL_W-8,"ALL TRACKS",
                        g_currentPL==-1,false,alpha,row0Prog,t,hue);
        for(int i=0;i<(int)g_playlists.size();++i){
            float rowStart=0.45f+(i+1)*0.042f;
            float rowProg=clamp01((bp-rowStart)/0.40f);
            int ry=listY+(i+1)*ROW_H;
            if(ry>py+ph-ROW_H) break;
            std::string label=g_playlists[i].name
                             +" ["+std::to_string(g_playlists[i].songIndices.size())+"]";
            drawAnimatedRow(r,px+4,ry,PANEL_W-8,label,
                            i==g_currentPL,false,alpha,
                            rowProg,t,fmod(hue+(i+1)*25.f,360.f));
        }
    }

    // Corner sparks
    if(bp > 0.05f && bp < 0.6f){
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
        float sparkT=clamp01(bp/0.6f);
        for(int corner=0;corner<4;++corner){
            float cx2=(corner%2==0)?(float)px:(float)(px+PANEL_W);
            float cy2=(corner<2)?(float)py:(float)(py+ph);
            float sparkAlpha=sinf(sparkT*(float)M_PI)*0.9f;
            for(int sp=0;sp<5;++sp){
                float sa=(float)sp/5.f*2*(float)M_PI+t*8.f+corner*1.57f;
                float sr=4.f+sp*6.f*sparkT;
                SDL_Color sc=thsv(fmod(hue+corner*90.f+sp*20.f,360.f),0.9f,1.f);
                sc.a=(Uint8)(sparkAlpha*(1.f-(float)sp/5.f)*200.f);
                softCircle(r,{cx2+cosf(sa)*sr*0.3f,cy2+sinf(sa)*sr*0.3f},
                           2.f,sc,{sc.r,sc.g,sc.b,0},6);
            }
        }
    }
}

int libraryClickRow(int mx,int my){
    if(g_panel!=UIPanel::Library)return -1;
    int px=10,py=80,listY=py+30;
    if(mx<px||mx>px+PANEL_W)return -1;
    int row=(my-listY)/ROW_H;
    int idx=g_libScroll+row;
    if(row<0||row>=VISIBLE_ROWS||idx>=(int)g_library.size())return -1;
    return idx;
}
int playlistClickRow(int mx,int my){
    if(g_panel!=UIPanel::Playlists)return -2;
    int px=10,py=80,listY=py+30;
    if(mx<px||mx>px+PANEL_W)return -2;
    int row=(my-listY)/ROW_H;
    if(row<0)return -2;
    if(row==0)return -1;
    return row-1;
}
bool onProgressBar(int mx,int my,int ww,int wh){
    // matches new layout: pbX=120, pbW=ww-260, barY=wh-26, barH=10
    int panelH=52, barH=10;
    int barY=wh-panelH/2-barH/2;
    return mx>=120&&mx<=ww-140&&my>=barY-4&&my<=barY+barH+4;
}

// ── BOTTOM NOW-PLAYING BAR — NEON HUD EDITION ───────────────────────────────
void drawNowPlayingBar(SDL_Renderer* r, int ww, int wh, Uint8 alpha,
                       float t, float bass, float overall,
                       const std::vector<float>& spec) {
    if(alpha==0) return;

    double pos=0,dur=1;
    if(g_music){pos=Mix_GetMusicPosition(g_music);dur=Mix_MusicDuration(g_music);}
    if(dur<=0)dur=1;
    float prog = g_seekDragging ? g_seekPreview : clamp01((float)(pos/dur));

    // Layout
    int barH  = 10;
    int panelH = 52;
    int barY  = wh - panelH/2 - barH/2;
    int pbX   = 120;
    int pbW   = ww - 260;
    int fillW = (int)(pbW * prog);

    // Time formatter
    auto fmtTime=[](double s)->std::string{
        int m=(int)s/60; int sec=(int)s%60;
        char buf[16]; snprintf(buf,16,"%d:%02d",m,sec); return buf;
    };
    std::string elapsed = fmtTime(pos);
    std::string total   = fmtTime(dur);

    float nHue      = g_theme.uiHue;
    float dragBoost = g_seekDragging ? 1.5f : 1.f;

    // ── Dark panel ────────────────────────────────────────────────────────────
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r,0,0,0,(Uint8)(alpha*0.88f));
    SDL_Rect panel={0,wh-panelH,ww,panelH};
    SDL_RenderFillRect(r,&panel);

    // Thin separator line
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    SDL_Color sepC=hsv(nHue,0.7f,0.4f); sepC.a=(Uint8)(alpha*0.5f);
    SDL_SetRenderDrawColor(r,sepC.r,sepC.g,sepC.b,sepC.a);
    SDL_Rect sep={0,wh-panelH,ww,1}; SDL_RenderFillRect(r,&sep);

    // ── Empty track ───────────────────────────────────────────────────────────
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r,6,8,12,(Uint8)(alpha));
    SDL_Rect track={pbX,barY,pbW,barH};
    SDL_RenderFillRect(r,&track);

    // ── White fill ────────────────────────────────────────────────────────────
    if(fillW>0){
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r,220,235,255,(Uint8)(alpha));
        SDL_Rect fill={pbX,barY,fillW,barH};
        SDL_RenderFillRect(r,&fill);

        // Bass brightness pulse
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
        SDL_SetRenderDrawColor(r,255,255,255,(Uint8)(alpha*bass*0.3f));
        SDL_RenderFillRect(r,&fill);

        // Right edge bloom
        for(int g=0;g<8;++g){
            float gf=(float)g/8.f;
            SDL_Color gc=hsv(nHue,0.6f,1.f);
            gc.a=(Uint8)(alpha*(1.f-gf)*(1.f-gf)*0.5f*dragBoost);
            SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
            SDL_Rect gl={pbX+fillW,barY-1,g+1,barH+2};
            SDL_RenderFillRect(r,&gl);
        }
    }

    // ── Neon border ───────────────────────────────────────────────────────────
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    for(int g=4;g>=1;--g){
        SDL_Color gc=hsv(nHue,0.8f,1.f);
        gc.a=(Uint8)(alpha*(1.f-(float)g/4.f*0.6f)*0.35f*(1.f+bass*0.3f)*dragBoost);
        SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
        SDL_Rect gr={pbX-g,barY-g,pbW+g*2,barH+g*2};
        SDL_RenderDrawRect(r,&gr);
    }
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    SDL_Color borderC=hsv(nHue,0.5f,1.f); borderC.a=(Uint8)(alpha*0.9f);
    SDL_SetRenderDrawColor(r,borderC.r,borderC.g,borderC.b,borderC.a);
    SDL_RenderDrawRect(r,&track);

    // ── Playhead ──────────────────────────────────────────────────────────────
    int thumbX=pbX+fillW;
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r,255,255,255,(Uint8)(alpha*dragBoost));
    SDL_Rect thumbLine={thumbX-1,barY-3,2,barH+6};
    SDL_RenderFillRect(r,&thumbLine);
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    for(int g=1;g<6;++g){
        float gf=(float)g/6.f;
        SDL_Color gc=hsv(nHue,0.5f,1.f);
        gc.a=(Uint8)(alpha*(1.f-gf)*(1.f-gf)*0.8f*(1.f+bass*0.3f)*dragBoost);
        SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
        SDL_Rect tr={thumbX-g,barY-2,g*2+1,barH+4};
        SDL_RenderFillRect(r,&tr);
    }

    // ── Time labels ───────────────────────────────────────────────────────────
    SDL_Color timeC=hsv(nHue,0.4f,1.f); timeC.a=(Uint8)(alpha*0.9f);
    drawText(r,g_fontSm,elapsed,pbX-4,barY+barH/2-5,timeC);
    drawText(r,g_fontSm,total,pbX+pbW+6,barY+barH/2-5,timeC);

}

// New playlist name input overlay — dramatic build
// ── THEME EDITOR — FULLSCREEN ─────────────────────────────────────────────────
// Helper: draw a horizontal slider, returns true if clicked
bool drawSlider(SDL_Renderer* r, int x, int y, int w, float val,
                float mn, float mx, const char* label, SDL_Color ac, Uint8 alpha){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    // Track
    SDL_SetRenderDrawColor(r,15,18,35,alpha);
    SDL_Rect tr={x,y+14,w,8}; SDL_RenderFillRect(r,&tr);
    // Fill
    float frac=clamp01((val-mn)/(mx-mn));
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    SDL_Color fc=ac; fc.a=(Uint8)(alpha*0.9f);
    SDL_SetRenderDrawColor(r,fc.r,fc.g,fc.b,fc.a);
    SDL_Rect fill={x,y+14,(int)(w*frac),8}; SDL_RenderFillRect(r,&fill);
    // Glow on fill
    for(int g2=0;g2<4;++g2){
        SDL_Color gc=ac; gc.a=(Uint8)(alpha*(1.f-g2*0.25f)*0.15f);
        SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
        SDL_Rect gr={x-g2,y+14-g2,(int)(w*frac)+g2*2,8+g2*2};
        SDL_RenderFillRect(r,&gr);
    }
    // Thumb
    int tx=x+(int)(w*frac);
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r,220,230,255,alpha);
    SDL_Rect thumb={tx-5,y+10,10,16}; SDL_RenderFillRect(r,&thumb);
    SDL_Color tc=ac; tc.a=(Uint8)(alpha*0.6f);
    SDL_SetRenderDrawColor(r,tc.r,tc.g,tc.b,tc.a);
    SDL_RenderDrawRect(r,&thumb);
    // Label on left, value on right — both above the track
    SDL_Color lc={160,180,220,alpha};
    drawText(r,g_fontSm,label,x,y,lc);
    char vbuf[16]; snprintf(vbuf,16,"%.2f",val);
    SDL_Color vc=ac; vc.a=alpha;
    drawText(r,g_fontSm,vbuf,x+w-28,y,vc); // right-aligned within track width
    return false;
}

// Draw a hue rainbow bar for picking hues
void drawHueBar(SDL_Renderer* r, int x, int y, int w, int h, float selHue, Uint8 alpha){
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
    for(int px=0;px<w;++px){
        float hf=(float)px/w*360.f;
        SDL_Color c=thsv(hf,1.f,1.f); c.a=alpha;
        SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a);
        SDL_Rect seg={x+px,y,1,h}; SDL_RenderFillRect(r,&seg);
    }
    // Selection marker
    int mx2=x+(int)(selHue/360.f*w);
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r,255,255,255,alpha);
    SDL_Rect marker={mx2-2,y-3,4,h+6}; SDL_RenderFillRect(r,&marker);
    SDL_SetRenderDrawColor(r,0,0,0,alpha);
    SDL_RenderDrawRect(r,&marker);
}

void drawThemePanel(SDL_Renderer* r, int ww, int wh, float buildT, float t){
    if(!g_themeOpen && buildT<=0.f) return;
    float bp=clamp01(buildT);
    if(bp<=0.f) return;

    float hue=g_themeEditing ? g_themeEditBuf.uiHue : g_theme.uiHue;

    if(!g_themeEditing){
        // ── PRESET PICKER PANEL ───────────────────────────────────────────────
        int ox=ww-340,oy=80,ow=330;
        int cardH=52,cardGap=6;
        int totalCards=NUM_PRESETS+g_numCustomThemes+1;
        int oh=totalCards*(cardH+cardGap)+48;
        oh=std::min(oh,wh-120);
        Uint8 alpha=(Uint8)(smoothstep(clamp01(bp*3.f))*220.f);

        float scanP=clamp01((bp-0.1f)/0.4f);
        float revH=scanP*oh;
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        if(revH>0.f){
            SDL_SetRenderDrawColor(r,3,4,10,(Uint8)(alpha*0.95f));
            SDL_Rect bg={ox,oy,ow,(int)revH}; SDL_RenderFillRect(r,&bg);
        }
        drawScanSweep(r,ox,oy,ow,oh,scanP,t,hue);
        SDL_Color bc=thsv(hue,0.85f,1.f); bc.a=alpha;
        drawBuildingBorder(r,ox,oy,ow,oh,smoothstep(clamp01(bp/0.3f)),t,bc);

        // Header
        if(bp>0.2f){
            float hf=smoothstep(clamp01((bp-0.2f)/0.2f));
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,15,20,50,(Uint8)(alpha*hf*0.97f));
            SDL_Rect hdr={ox,oy,ow,28}; SDL_RenderFillRect(r,&hdr);
            SDL_Color tc={160,200,255,(Uint8)(alpha*hf)};
            drawText(r,g_fontSm,"THEMES",ox+10,oy+7,tc);
            SDL_Color ac=thsv(hue,0.9f,1.f); ac.a=(Uint8)(alpha*hf*0.8f);
            drawText(r,g_fontSm,"[ "+g_theme.name+" ]",ox+ow-10,oy+7,ac);
        }

        // Theme cards
        if(bp>0.35f){
            int listY=oy+34;
            for(int i=0;i<totalCards;++i){
                float rowF=smoothstep(clamp01((bp-0.35f-i*0.04f)/0.3f));
                if(rowF<=0.f) continue;
                int ry=listY+i*(cardH+cardGap);
                if(ry+cardH>oy+oh-8) break;
                bool isNew=(i==NUM_PRESETS+g_numCustomThemes);
                bool isCustom=(i>=NUM_PRESETS&&!isNew);
                bool isActive=(i==g_themeIdx);
                bool isHover=(i==g_themeHover);
                std::string nm=isNew?"+ New Custom":
                               i<NUM_PRESETS?PRESET_THEMES[i].name:
                               g_customThemes[i-NUM_PRESETS].name;
                float cHue=isNew?120.f:i<NUM_PRESETS?
                           PRESET_THEMES[i].uiHue:g_customThemes[i-NUM_PRESETS].uiHue;
                Uint8 ca=(Uint8)(alpha*rowF);
                SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
                if(isActive)     SDL_SetRenderDrawColor(r,20,40,80,ca);
                else if(isHover) SDL_SetRenderDrawColor(r,15,25,50,ca);
                else             SDL_SetRenderDrawColor(r,8,10,22,ca);
                SDL_Rect card={ox+6,ry,ow-12,cardH}; SDL_RenderFillRect(r,&card);
                // Rainbow preview strip
                if(!isNew){
                    float tHue=i<NUM_PRESETS?PRESET_THEMES[i].hueOffset:
                               g_customThemes[i-NUM_PRESETS].hueOffset;
                    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
                    for(int px=0;px<ow-24;++px){
                        float hf2=(float)px/(ow-24)*360.f;
                        SDL_Color sc=thsv(fmod(tHue+hf2,360.f),0.9f,0.8f);
                        sc.a=(Uint8)(ca*0.6f);
                        SDL_SetRenderDrawColor(r,sc.r,sc.g,sc.b,sc.a);
                        SDL_Rect sp={ox+12+px,ry+cardH-7,1,5};
                        SDL_RenderFillRect(r,&sp);
                    }
                }
                if(isActive){
                    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
                    SDL_Color ac2=thsv(cHue,0.9f,1.f); ac2.a=ca;
                    SDL_SetRenderDrawColor(r,ac2.r,ac2.g,ac2.b,ac2.a);
                    SDL_Rect bar={ox+6,ry,3,cardH}; SDL_RenderFillRect(r,&bar);
                }
                SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
                SDL_Color bc2=thsv(cHue,0.7f,isActive?1.f:0.4f);
                bc2.a=(Uint8)(ca*(isActive?0.9f:isHover?0.5f:0.2f));
                SDL_SetRenderDrawColor(r,bc2.r,bc2.g,bc2.b,bc2.a);
                SDL_RenderDrawRect(r,&card);
                SDL_Color nc=isActive?SDL_Color{220,240,255,ca}:
                             isNew?SDL_Color{100,200,120,ca}:
                             SDL_Color{160,180,210,ca};
                drawText(r,g_font,nm,ox+16,ry+8,nc);
                if(isCustom){
                    SDL_Color ec={100,160,200,ca};
                    drawText(r,g_fontSm,"[edit]",ox+ow-52,ry+8,ec);
                }
                if(!isNew){
                    float tIn=i<NUM_PRESETS?PRESET_THEMES[i].intensity:
                              g_customThemes[i-NUM_PRESETS].intensity;
                    std::string st="intensity: "+std::to_string((int)(tIn*100.f))+"%";
                    SDL_Color sc2={80,100,140,ca};
                    drawText(r,g_fontSm,st,ox+16,ry+26,sc2);
                }
            }
        }
    } else {
        // ── FULLSCREEN THEME EDITOR ───────────────────────────────────────────
        float ef=smoothstep(clamp01(g_themeEditorT*2.f));
        Uint8 alpha=(Uint8)(ef*235.f);

        // Full-screen animated dark backdrop
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r,2,3,8,(Uint8)(ef*245.f));
        SDL_RenderFillRect(r,nullptr);

        // Scanline reveals top-to-bottom
        float scanP=clamp01((g_themeEditorT-0.1f)/0.35f);
        if(scanP<1.f){
            // Scan line
            int sy=(int)(scanP*wh);
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color sc=thsv(hue,0.9f,1.f); sc.a=200;
            SDL_SetRenderDrawColor(r,sc.r,sc.g,sc.b,sc.a);
            SDL_Rect sl={0,sy,ww,2}; SDL_RenderFillRect(r,&sl);
            // Glow below scan
            for(int g2=1;g2<12;++g2){
                SDL_Color gc=sc; gc.a=(Uint8)(200*(1.f-g2/12.f));
                SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
                SDL_Rect gl={0,sy+g2,ww,1}; SDL_RenderFillRect(r,&gl);
            }
        }

        // Header bar
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r,8,10,25,(Uint8)(alpha*0.97f));
        SDL_Rect hbar={0,0,ww,52}; SDL_RenderFillRect(r,&hbar);
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
        SDL_Color hline=thsv(hue,0.8f,1.f); hline.a=(Uint8)(alpha*0.5f);
        SDL_SetRenderDrawColor(r,hline.r,hline.g,hline.b,hline.a);
        SDL_Rect hl={0,51,ww,1}; SDL_RenderFillRect(r,&hl);

        // Title
        SDL_Color titleC={200,220,255,alpha};
        drawText(r,g_font,"THEME EDITOR  —  "+g_themeEditBuf.name,20,16,titleC);
        SDL_Color saveC={80,220,120,alpha};
        SDL_Color cancelC={200,80,80,alpha};
        drawText(r,g_font,"[ SAVE ]",ww-200,16,saveC);
        drawText(r,g_font,"[ CANCEL ]",ww-110,16,cancelC);

        // Tab bar
        const char* tabs[]={"COLORS","EFFECTS","BACKGROUND"};
        int tabW=160, tabY=58;
        for(int tb=0;tb<3;++tb){
            bool active=(tb==g_themeEditorTab);
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_Color tbc=thsv(hue,active?0.8f:0.3f,active?1.f:0.5f);
            tbc.a=(Uint8)(alpha*(active?1.f:0.6f));
            SDL_SetRenderDrawColor(r,10+(active?20:0),12+(active?30:0),30+(active?40:0),(Uint8)(alpha*(active?0.9f:0.5f)));
            SDL_Rect tr={20+tb*tabW,tabY,tabW-4,28}; SDL_RenderFillRect(r,&tr);
            SDL_SetRenderDrawColor(r,tbc.r,tbc.g,tbc.b,tbc.a);
            SDL_RenderDrawRect(r,&tr);
            if(active){
                SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
                SDL_SetRenderDrawColor(r,tbc.r,tbc.g,tbc.b,(Uint8)(alpha*0.6f));
                SDL_Rect ub={20+tb*tabW,tabY+26,tabW-4,2}; SDL_RenderFillRect(r,&ub);
            }
            drawText(r,g_fontSm,tabs[tb],20+tb*tabW+tabW/2,tabY+7,tbc,true);
        }

        int contentY=100, contentX=30, contentW=ww-60;
        SDL_Color ac=thsv(hue,0.9f,1.f); ac.a=alpha;

        if(g_themeEditorTab==0){
            // ── COLORS TAB ────────────────────────────────────────────────────
            int col1=contentX, col2=contentX+contentW/2+20;
            int colW=contentW/2-30;

            // Global hue offset with rainbow bar
            SDL_Color lc={140,160,200,alpha};
            drawText(r,g_fontSm,"GLOBAL HUE OFFSET",col1,contentY,lc);
            drawHueBar(r,col1,contentY+18,colW,12,g_themeEditBuf.hueOffset,alpha);

            // Other global sliders
            struct{ const char* label; float* val; float mn,mx2; } sliders[]={
                {"Saturation Multiplier", &g_themeEditBuf.satMult,  0.f,  1.5f},
                {"Brightness Multiplier",&g_themeEditBuf.valMult,   0.2f, 1.3f},
                {"Effect Intensity",      &g_themeEditBuf.intensity, 0.1f, 1.8f},
            };
            for(int s=0;s<3;++s){
                int sy=contentY+60+s*50;
                SDL_Color sac=thsv(fmod(hue+s*40.f,360.f),0.9f,1.f); sac.a=alpha;
                drawSlider(r,col1,sy,colW,*sliders[s].val,
                           sliders[s].mn,sliders[s].mx2,sliders[s].label,sac,alpha);
            }

            // UI accent hue
            drawText(r,g_fontSm,"UI ACCENT HUE",col1,contentY+220,lc);
            drawHueBar(r,col1,contentY+238,colW,12,g_themeEditBuf.uiHue,alpha);

            // Right column: preview
            drawText(r,g_fontSm,"PREVIEW",col2,contentY,lc);
            // Draw a mini preview box showing theme colors
            int pw=colW, ph=200;
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,5,6,14,alpha);
            SDL_Rect prev={col2,contentY+20,pw,ph}; SDL_RenderFillRect(r,&prev);
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            // Draw 8 hue-shifted rings as preview
            for(int ring=0;ring<8;++ring){
                float rf=(float)ring/8.f;
                float previewHue=fmod(g_themeEditBuf.hueOffset+rf*360.f,360.f);
                float sat=std::min(1.f,g_themeEditBuf.satMult);
                float val2=std::min(1.f,g_themeEditBuf.valMult);
                SDL_Color rc=thsv(previewHue,sat,val2);
                rc.a=(Uint8)(alpha*0.4f*(1.f-rf*0.5f));
                SDL_SetRenderDrawColor(r,rc.r,rc.g,rc.b,rc.a);
                int rr=(int)(rf*pw/2.f*0.9f)+10;
                int cx2=col2+pw/2, cy2=contentY+20+ph/2;
                for(int seg=0;seg<32;++seg){
                    float a0=(float)seg/64*2*(float)M_PI;
                    float a1=(float)(seg+1)/64*2*(float)M_PI;
                    SDL_Rect dot={(int)(cx2+cosf(a0)*rr)-1,(int)(cy2+sinf(a0)*rr)-1,2,2};
                    SDL_RenderFillRect(r,&dot);
                }
            }
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_Color pbc=thsv(g_themeEditBuf.uiHue,0.7f,0.8f); pbc.a=(Uint8)(alpha*0.5f);
            SDL_SetRenderDrawColor(r,pbc.r,pbc.g,pbc.b,pbc.a);
            SDL_RenderDrawRect(r,&prev);

        } else if(g_themeEditorTab==1){
            // ── EFFECTS TAB ───────────────────────────────────────────────────
            // Split: left = builtin toggles, right = custom effect builder
            int leftW = contentW/2-10;
            int rightX = contentX+leftW+20;
            int rightW = contentW/2-20;

            // LEFT — builtin toggles
            drawText(r,g_fontSm,"BUILT-IN EFFECTS",contentX,contentY,{100,120,160,alpha});
            int rowH=30, visRows=(wh-contentY-80)/rowH;
            g_effScrollOffset=std::max(0,std::min(g_effScrollOffset,EFF_COUNT-visRows));
            for(int i=0;i<visRows&&(i+g_effScrollOffset)<EFF_COUNT;++i){
                int ei=i+g_effScrollOffset;
                int ry=contentY+20+i*rowH;
                bool vis=g_themeEditBuf.effVisible[ei];
                bool ovr=g_themeEditBuf.effHueOverride[ei];
                SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(r,vis?10:4,vis?12:5,vis?25:10,(Uint8)(alpha*0.7f));
                SDL_Rect row={contentX,ry,leftW,rowH-2}; SDL_RenderFillRect(r,&row);
                SDL_Color vc=vis?SDL_Color{80,220,100,alpha}:SDL_Color{120,40,40,alpha};
                SDL_SetRenderDrawColor(r,vc.r/4,vc.g/4,vc.b/4,alpha);
                SDL_Rect vtog={contentX+2,ry+4,32,rowH-10}; SDL_RenderFillRect(r,&vtog);
                SDL_SetRenderDrawColor(r,vc.r,vc.g,vc.b,alpha);
                SDL_RenderDrawRect(r,&vtog);
                drawText(r,g_fontSm,vis?"ON":"OFF",contentX+4,ry+6,vc);
                SDL_Color nc={(Uint8)(vis?170:70),(Uint8)(vis?190:80),(Uint8)(vis?230:100),(Uint8)(alpha*(vis?1.f:0.45f))};
                drawText(r,g_fontSm,EFF_NAMES[ei],contentX+40,ry+7,nc);
                if(vis){
                    SDL_Color oc=ovr?hsv(g_themeEditBuf.effHue[ei],0.9f,1.f):SDL_Color{50,60,90,alpha};
                    oc.a=(Uint8)(alpha*0.8f);
                    SDL_SetRenderDrawColor(r,oc.r/5,oc.g/5,oc.b/5,alpha);
                    SDL_Rect otog={contentX+leftW-62,ry+4,60,rowH-10}; SDL_RenderFillRect(r,&otog);
                    SDL_SetRenderDrawColor(r,oc.r,oc.g,oc.b,oc.a);
                    SDL_RenderDrawRect(r,&otog);
                    drawText(r,g_fontSm,ovr?"CLR":"---",contentX+leftW-60,ry+6,oc);
                    if(ovr) drawHueBar(r,contentX+leftW-120,ry+6,55,rowH-14,g_themeEditBuf.effHue[ei],alpha);
                }
            }
            if(EFF_COUNT>visRows){
                float st=(float)g_effScrollOffset/EFF_COUNT;
                float sh=(float)visRows/EFF_COUNT;
                int sbX=contentX+leftW-6;
                int sbY=contentY+20+(int)(visRows*rowH*st);
                int sbH=std::max(16,(int)(visRows*rowH*sh));
                SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
                SDL_Color sbc=hsv(hue,0.7f,1.f); sbc.a=(Uint8)(alpha*0.5f);
                SDL_SetRenderDrawColor(r,sbc.r,sbc.g,sbc.b,sbc.a);
                SDL_Rect sb={sbX,sbY,4,sbH}; SDL_RenderFillRect(r,&sb);
            }

            // RIGHT — custom effect builder
            {
                // Divider
                SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
                SDL_Color div=hsv(hue,0.5f,0.4f); div.a=(Uint8)(alpha*0.5f);
                SDL_SetRenderDrawColor(r,div.r,div.g,div.b,div.a);
                SDL_Rect dvd={rightX-12,contentY,1,wh-contentY-80};
                SDL_RenderFillRect(r,&dvd);

                drawText(r,g_fontSm,"CUSTOM EFFECTS",rightX,contentY,{140,200,140,alpha});

                // Custom effect slots
                int ceRowH=32;
                for(int i=g_ceScrollOffset;i<=g_numCustomEffects&&i<MAX_CUSTOM_EFFECTS;++i){
                    int ry=contentY+20+(i-g_ceScrollOffset)*ceRowH;
                    int maxListH=std::min((g_numCustomEffects+1)*ceRowH+20,(wh-contentY)/3);
                    if(ry+ceRowH>contentY+maxListH) break;
                    bool isAdd=(i==g_numCustomEffects);
                    bool isEdit=(i==g_editingEffect);

                    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
                    SDL_Color bg3=isEdit?SDL_Color{20,40,20,alpha}:SDL_Color{8,14,8,alpha};
                    SDL_SetRenderDrawColor(r,bg3.r,bg3.g,bg3.b,bg3.a);
                    SDL_Rect row={rightX,ry,rightW,ceRowH-2}; SDL_RenderFillRect(r,&row);

                    if(isAdd){
                        SDL_Color ac2={60,180,80,alpha};
                        SDL_SetRenderDrawColor(r,ac2.r,ac2.g,ac2.b,ac2.a);
                        SDL_RenderDrawRect(r,&row);
                        drawText(r,g_fontSm,"+ NEW CUSTOM EFFECT",rightX+6,ry+8,ac2);
                    } else {
                        auto& ce=g_customEffects[i];
                        // ON/OFF toggle
                        SDL_Color vc2=ce.enabled?SDL_Color{60,200,80,alpha}:SDL_Color{100,30,30,alpha};
                        SDL_SetRenderDrawColor(r,vc2.r/4,vc2.g/4,vc2.b/4,alpha);
                        SDL_Rect vt={rightX+2,ry+5,28,ceRowH-12}; SDL_RenderFillRect(r,&vt);
                        SDL_SetRenderDrawColor(r,vc2.r,vc2.g,vc2.b,alpha);
                        SDL_RenderDrawRect(r,&vt);
                        drawText(r,g_fontSm,ce.enabled?"ON":"OFF",rightX+4,ry+7,vc2);
                        // Name + type info
                        SDL_Color nc2=isEdit?SDL_Color{120,255,140,alpha}:SDL_Color{120,180,130,alpha};
                        drawText(r,g_fontSm,ce.name,rightX+36,ry+4,nc2);
                        std::string info=std::string(EFF_SHAPE_NAMES[(int)ce.shape])+" / "+
                                         EFF_MOTION_NAMES[(int)ce.motion];
                        SDL_Color ic={60,100,70,alpha};
                        drawText(r,g_fontSm,info,rightX+36,ry+16,ic);
                        // Edit/Delete
                        SDL_Color ec2={80,140,100,alpha};
                        drawText(r,g_fontSm,"[edit]",rightX+rightW-80,ry+4,ec2);
                        SDL_Color dc={140,60,60,alpha};
                        drawText(r,g_fontSm,"[del]",rightX+rightW-44,ry+16,dc);
                    }
                }

                // Inline edit panel removed — editing is now fullscreen (see g_ceFullscreen draw below)
            } // end right panel block

        } else {
            // ── BACKGROUND TAB ────────────────────────────────────────────────
            SDL_Color lc={140,160,200,alpha};
            int col1=contentX, col2=contentX+contentW/2+20;
            int colW=contentW/2-30;

            // Gradient toggle
            bool bg=g_themeEditBuf.bgGradient;
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_Color tgc=bg?SDL_Color{80,180,255,alpha}:SDL_Color{100,110,140,alpha};
            SDL_SetRenderDrawColor(r,tgc.r/5,tgc.g/5,tgc.b/5,alpha);
            SDL_Rect tog={col1,contentY,120,28}; SDL_RenderFillRect(r,&tog);
            SDL_SetRenderDrawColor(r,tgc.r,tgc.g,tgc.b,tgc.a);
            SDL_RenderDrawRect(r,&tog);
            drawText(r,g_fontSm,bg?"GRADIENT: ON":"GRADIENT: OFF",col1+6,contentY+7,tgc);

            // Color 1
            drawText(r,g_fontSm,"COLOR 1 (HUE)",col1,contentY+48,lc);
            drawHueBar(r,col1,contentY+66,colW,14,g_themeEditBuf.bgHue1,alpha);

            // Color 2
            drawText(r,g_fontSm,"COLOR 2 (HUE)",col1,contentY+100,lc);
            drawHueBar(r,col1,contentY+118,colW,14,g_themeEditBuf.bgHue2,alpha);

            // Saturation slider
            SDL_Color sac=thsv(g_themeEditBuf.bgHue1,0.9f,1.f); sac.a=alpha;
            drawSlider(r,col1,contentY+150,colW,g_themeEditBuf.bgSat,0.f,1.f,"Saturation",sac,alpha);
            drawSlider(r,col1,contentY+200,colW,g_themeEditBuf.bgVal,0.f,0.2f,"Brightness",sac,alpha);
            drawSlider(r,col1,contentY+250,colW,g_themeEditBuf.bgGradientAngle,0.f,360.f,"Angle",sac,alpha);

            // Preview
            drawText(r,g_fontSm,"PREVIEW",col2,contentY,lc);
            int pw=colW, ph=260;
            SDL_Rect prev={col2,contentY+20,pw,ph};
            if(g_themeEditBuf.bgGradient){
                // Draw gradient preview
                float ang=g_themeEditBuf.bgGradientAngle*(float)M_PI/180.f;
                SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
                for(int px=0;px<pw;++px){
                    for(int py2=0;py2<ph;py2+=2){
                        float fx=(float)px/pw, fy=(float)py2/ph;
                        float proj=fx*cosf(ang)+fy*sinf(ang);
                        proj=clamp01(proj);
                        float h2=g_themeEditBuf.bgHue1*(1.f-proj)+g_themeEditBuf.bgHue2*proj;
                        SDL_Color c=thsv(h2,g_themeEditBuf.bgSat,g_themeEditBuf.bgVal*20.f);
                        c.a=(Uint8)(alpha*0.8f);
                        SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a);
                        SDL_Rect dot={col2+px,contentY+20+py2,1,2};
                        SDL_RenderFillRect(r,&dot);
                    }
                }
            } else {
                SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(r,3,4,8,alpha);
                SDL_RenderFillRect(r,&prev);
            }
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_Color pbc=thsv(hue,0.5f,0.7f); pbc.a=(Uint8)(alpha*0.4f);
            SDL_SetRenderDrawColor(r,pbc.r,pbc.g,pbc.b,pbc.a);
            SDL_RenderDrawRect(r,&prev);
        }

        // ── FULLSCREEN CUSTOM EFFECT EDITOR ─────────────────────────────────────
        if(g_ceFullscreen && g_editingEffect>=0 && g_editingEffect<g_numCustomEffects){
            auto& ce=g_customEffects[g_editingEffect];
            float cef=smoothstep(clamp01(g_themeEditorT*2.f));
            Uint8 cAlpha=(Uint8)(cef*240.f);

            // Full dark overlay
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,2,5,8,(Uint8)(cef*248.f));
            SDL_RenderFillRect(r,nullptr);

            // Scan line build-in
            float cScan=clamp01((g_themeEditorT-0.05f)/0.3f);
            if(cScan<1.f){
                int sy=(int)(cScan*wh);
                SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
                SDL_Color sc2=hsv(120.f,0.9f,1.f); sc2.a=200;
                SDL_SetRenderDrawColor(r,sc2.r,sc2.g,sc2.b,sc2.a);
                SDL_Rect sl={0,sy,ww,2}; SDL_RenderFillRect(r,&sl);
                for(int gi=1;gi<10;++gi){
                    SDL_Color gc=sc2; gc.a=(Uint8)(200*(1.f-gi/10.f));
                    SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
                    SDL_Rect gl={0,sy+gi,ww,1}; SDL_RenderFillRect(r,&gl);
                }
            }

            // Header bar
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,6,12,8,(Uint8)(cAlpha*0.97f));
            SDL_Rect hbar={0,0,ww,52}; SDL_RenderFillRect(r,&hbar);
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color hline=hsv(120.f,0.8f,1.f); hline.a=(Uint8)(cAlpha*0.4f);
            SDL_SetRenderDrawColor(r,hline.r,hline.g,hline.b,hline.a);
            SDL_Rect hl={0,51,ww,1}; SDL_RenderFillRect(r,&hl);

            // Title + buttons
            SDL_Color titleC={180,255,200,cAlpha};
            drawText(r,g_font,"EFFECT EDITOR  —  "+ce.name,20,16,titleC);
            SDL_Color doneC={80,220,120,cAlpha};
            SDL_Color discC={200,80,80,cAlpha};
            drawText(r,g_font,"[ DONE ]",ww-130,16,doneC);
            drawText(r,g_font,"[ DISCARD ]",ww-270,16,discC);

            // Layout: left=controls, right=fullscreen live preview
            int ctrlW=460;          // wider controls column
            int prevX=ctrlW+16;     // clear gap before preview
            int prevW=ww-prevX-10;
            int topY=62;
            int botY=wh-36;
            int avH=botY-topY;

            // Controls bg
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,4,10,6,(Uint8)(cAlpha*0.96f));
            SDL_Rect cbg={0,topY,ctrlW,avH}; SDL_RenderFillRect(r,&cbg);
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color cbdr=hsv(120.f,0.5f,0.5f); cbdr.a=(Uint8)(cAlpha*0.3f);
            SDL_SetRenderDrawColor(r,cbdr.r,cbdr.g,cbdr.b,cbdr.a);
            SDL_RenderDrawRect(r,&cbg);

            // Divider
            SDL_SetRenderDrawColor(r,cbdr.r,cbdr.g,cbdr.b,cbdr.a);
            SDL_Rect dvd={ctrlW+5,topY,1,avH}; SDL_RenderFillRect(r,&dvd);

            SDL_Color lc2={100,180,120,cAlpha};
            SDL_Color ac2=hsv(120.f,0.8f,1.f); ac2.a=cAlpha;
            int cx2=6; // control x
            int sw2=ctrlW-cx2*2-30; // slider width (leave room for value label)
            int cy2=topY+6;

            // Helper macro for button rows
            auto drawBtnRow=[&](const char** names, int count, int selIdx,
                                SDL_Color selC, SDL_Color offC, int y, int h)->void{
                int bw=(ctrlW-cx2*2)/count;
                for(int i=0;i<count;++i){
                    bool sel=(i==selIdx);
                    SDL_Color bc3=sel?selC:offC;
                    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(r,bc3.r/5,bc3.g/5,bc3.b/5,cAlpha);
                    SDL_Rect bt={cx2+i*bw,y,bw-2,h}; SDL_RenderFillRect(r,&bt);
                    SDL_SetRenderDrawColor(r,bc3.r,bc3.g,bc3.b,cAlpha);
                    SDL_RenderDrawRect(r,&bt);
                    drawText(r,g_fontSm,names[i],cx2+i*bw+2,y+3,bc3);
                }
            };

            SDL_Color shapeC={80,220,100,cAlpha}, motC={80,180,220,cAlpha},
                      colC={220,180,80,cAlpha},   trgC={220,120,220,cAlpha};
            SDL_Color dimC={50,80,60,cAlpha}, dimM={40,70,90,cAlpha},
                      dimCol={80,70,40,cAlpha}, dimTrg={80,50,80,cAlpha};

            drawText(r,g_fontSm,"SHAPE",cx2,cy2,lc2); cy2+=14;
            // Clip controls to left column — prevents text bleeding into preview
            SDL_RenderSetClipRect(r,&cbg);
            drawBtnRow(EFF_SHAPE_NAMES,(int)EffShape::COUNT,(int)ce.shape,shapeC,dimC,cy2,16); cy2+=20;
            drawText(r,g_fontSm,"MOTION",cx2,cy2,lc2); cy2+=14;
            drawBtnRow(EFF_MOTION_NAMES,(int)EffMotion::COUNT,(int)ce.motion,motC,dimM,cy2,16); cy2+=20;
            drawText(r,g_fontSm,"COLOR",cx2,cy2,lc2); cy2+=14;
            drawBtnRow(EFF_COLOR_NAMES,(int)EffColor::COUNT,(int)ce.color,colC,dimCol,cy2,16); cy2+=20;
            drawText(r,g_fontSm,"TRIGGER",cx2,cy2,lc2); cy2+=14;
            drawBtnRow(EFF_TRIGGER_NAMES,(int)EffTrigger::COUNT,(int)ce.trigger,trgC,dimTrg,cy2,16); cy2+=24;

            // Sliders — two columns to save vertical space
            struct Sl{ const char* label; float* val; float mn,mx2; };
            Sl sliders[]={
                {"Size",        &ce.size,        0.f,1.f},
                {"Speed",       &ce.speed,       0.f,1.f},
                {"Density",     &ce.density,     0.f,1.f},
                {"Opacity",     &ce.alpha,       0.f,1.f},
                {"Inner Radius",&ce.innerRadius, 0.f,1.f},
                {"Wave Freq",   &ce.waveFreq,    0.f,1.f},
                {"Poly Sides",  &ce.polySides,   0.f,1.f},
                {"Trail",       &ce.trailLen,    0.f,1.f},
                {"Symmetry",    &ce.symmetry,    0.f,1.f},
                {"Reactivity",  &ce.reactivity,  0.f,1.f},
                {"Scatter",     &ce.scatter,     0.f,1.f},
                {"Pulse Rate",  &ce.pulseRate,   0.f,1.f},
                {"Color Speed", &ce.colorSpeed,  0.f,1.f},
            };
            int numSl=13;
            int colsW=(ctrlW-cx2*2)/2-4;
            for(int si=0;si<numSl;++si){
                int col=(si%2), row=(si/2);
                int sx=cx2+col*(colsW+8);
                int sy2=cy2+row*34;
                drawSlider(r,sx,sy2,colsW,*sliders[si].val,
                           sliders[si].mn,sliders[si].mx2,sliders[si].label,ac2,cAlpha);
            }
            cy2+=((numSl+1)/2)*34+8;

            // Blend toggle
            {bool isAdd=(ce.blend==EffBlend::Add);
             SDL_Color bc3=isAdd?SDL_Color{80,200,255,cAlpha}:SDL_Color{80,160,80,cAlpha};
             SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
             SDL_SetRenderDrawColor(r,bc3.r/5,bc3.g/5,bc3.b/5,cAlpha);
             SDL_Rect bt={cx2,cy2,sw2+30,18}; SDL_RenderFillRect(r,&bt);
             SDL_SetRenderDrawColor(r,bc3.r,bc3.g,bc3.b,cAlpha);
             SDL_RenderDrawRect(r,&bt);
             drawText(r,g_fontSm,std::string("BLEND: ")+(isAdd?"ADDITIVE":"NORMAL"),cx2+4,cy2+3,bc3);}
            cy2+=24;

            // Hue bars (only when relevant)
            if(ce.color==EffColor::Solid||ce.color==EffColor::Pulse||
               ce.color==EffColor::Neon ||ce.color==EffColor::Pastel||
               ce.color==EffColor::Duo  ||ce.color==EffColor::Glitch){
                drawText(r,g_fontSm,"HUE 1",cx2,cy2,lc2);
                drawHueBar(r,cx2+46,cy2,sw2-16,12,ce.hue,cAlpha); cy2+=18;
                drawText(r,g_fontSm,"HUE 2",cx2,cy2,lc2);
                drawHueBar(r,cx2+46,cy2,sw2-16,12,ce.hue2,cAlpha); cy2+=18;
            }

            // Clear clip
            SDL_RenderSetClipRect(r,nullptr);

            // ── RIGHT: FULLSCREEN LIVE PREVIEW ────────────────────────────────
            // Solid dark fill over the whole preview region first — kills any text bleed
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,2,5,8,255);
            SDL_Rect prevBg={prevX,topY,prevW,avH};
            SDL_RenderFillRect(r,&prevBg);

            float pcx2=(float)(prevX+prevW/2);
            float pcy2=(float)(topY+avH/2);
            float pmaxR2=std::min(prevW,avH)*0.45f;

            // Fake audio
            float previewT=SDL_GetTicks()*0.001f;
            float fakeBass=0.5f+0.5f*sinf(previewT*2.3f);
            float fakeMid =0.5f+0.5f*sinf(previewT*1.7f+1.f);
            float fakeHigh=0.4f+0.4f*sinf(previewT*3.7f+2.f);
            float fakeAll =0.5f+0.5f*sinf(previewT*1.1f);

            // Advance preview phase for this effect
            float speedMult=0.2f+ce.speed*2.f;
            g_customEffects[g_editingEffect].phase=fmod(
                ce.phase+0.016f*speedMult,2.f*(float)M_PI);

            // Clip to preview region
            SDL_Rect clip2={prevX,topY,prevW,avH};
            SDL_RenderSetClipRect(r,&clip2);

            // Draw using the real drawCustomEffect — ALL shapes work automatically
            // Temporarily force trigger to Always and override position/size
            EffTrigger savedTrig=ce.trigger;
            ce.trigger=EffTrigger::Always;
            // We can't change cx/cy/maxR in drawCustomEffect so we use a fake index
            // pointing at a modified copy in a spare slot
            CustomEffect preview=ce;
            preview.enabled=true;
            preview.trigger=EffTrigger::Always;
            preview.phase=ce.phase;
            // Temporarily write to a scratch slot at MAX_CUSTOM_EFFECTS-1
            // (safe — we never save that slot)
            g_customEffects[MAX_CUSTOM_EFFECTS-1]=preview;
            int savedN=g_numCustomEffects;
            g_numCustomEffects=MAX_CUSTOM_EFFECTS; // let it draw slot MAX-1
            drawCustomEffect(r,MAX_CUSTOM_EFFECTS-1,pcx2,pcy2,
                             previewT,fakeBass,fakeMid,fakeHigh,fakeAll,pmaxR2,{});
            g_numCustomEffects=savedN;
            ce.trigger=savedTrig;

            SDL_RenderSetClipRect(r,nullptr);

            // Preview label + border
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color pbc2=hsv(120.f,0.5f,0.6f); pbc2.a=(Uint8)(cAlpha*0.3f);
            SDL_SetRenderDrawColor(r,pbc2.r,pbc2.g,pbc2.b,pbc2.a);
            SDL_RenderDrawRect(r,&clip2);
            SDL_Color plbl=hsv(120.f,0.7f,1.f); plbl.a=(Uint8)(cAlpha*0.5f);
            drawText(r,g_fontSm,"LIVE PREVIEW",prevX+prevW/2,topY+6,plbl,true);
        }

        // Hint bar at bottom
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r,6,8,18,(Uint8)(alpha*0.95f));
        SDL_Rect hint={0,wh-28,ww,28}; SDL_RenderFillRect(r,&hint);
        SDL_Color hc={80,100,140,alpha};
        drawText(r,g_fontSm,"CLICK sliders to adjust  |  SCROLL to browse effects  |  ESC = cancel  |  TAB = next tab",20,wh-18,hc);
    }
}


// New playlist name input overlay — dramatic build
void drawNamingOverlay(SDL_Renderer* r, int ww, int wh, Uint8 alpha, float buildT, float t) {
    if(!g_namingPL) return;
    float bp = clamp01(buildT);
    int ox=ww/2-180, oy=wh/2-60;
    int ow=360, oh=110;
    float hue = fmod(t*28.f + 80.f, 360.f);

    // Dim the screen behind it
    float dimFade = smoothstep(clamp01(bp*4.f));
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r,0,0,0,(Uint8)(dimFade*120.f));
    SDL_RenderFillRect(r,nullptr);

    // Scanline sweeps the overlay
    float scanProg = clamp01((bp-0.15f)/0.45f);
    float revH = scanProg * oh;
    if(revH>0.f){
        SDL_SetRenderDrawColor(r,4,4,14,(Uint8)(alpha*0.96f));
        SDL_Rect bg={ox,oy,ow,(int)revH}; SDL_RenderFillRect(r,&bg);
    }
    drawScanSweep(r,ox,oy,ow,oh,scanProg,t,hue);

    // Border traces
    float borderProg = smoothstep(clamp01((bp-0.05f)/0.45f));
    SDL_Color bc = thsv(hue,0.9f,1.f); bc.a=(Uint8)(alpha*0.95f);
    drawBuildingBorder(r,ox,oy,ow,oh,borderProg,t,bc);

    // Corner sparks
    if(bp > 0.05f && bp < 0.65f){
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
        float sparkT=clamp01(bp/0.65f);
        for(int corner=0;corner<4;++corner){
            float cx2=(corner%2==0)?(float)ox:(float)(ox+ow);
            float cy2=(corner<2)?(float)oy:(float)(oy+oh);
            float sa2=sinf(sparkT*(float)M_PI)*180.f;
            for(int sp=0;sp<4;++sp){
                float sa=(float)sp/4.f*2*(float)M_PI+t*10.f+corner*1.57f;
                SDL_Color sc=thsv(fmod(hue+corner*90.f+sp*30.f,360.f),1.f,1.f);
                sc.a=(Uint8)(sa2*(1.f-(float)sp/4.f));
                softCircle(r,{cx2+cosf(sa)*sp*3.f,cy2+sinf(sa)*sp*3.f},
                           2.f+sp,sc,{sc.r,sc.g,sc.b,0},6);
            }
        }
    }

    // Prompt type-on
    if(bp > 0.35f){
        float fade=smoothstep(clamp01((bp-0.35f)/0.25f));
        float typeP=clamp01((bp-0.35f)/0.25f);
        std::string typed=buildTypeOn("> ENTER PLAYLIST NAME:", typeP, t);
        SDL_Color tglow=thsv(hue,0.5f,1.f); tglow.a=(Uint8)(alpha*fade*0.35f);
        for(int g=0;g<3;++g) drawText(r,g_font,typed,ox+12+g,oy+8,tglow,false);
        drawText(r,g_font,typed,ox+12,oy+8,{180,220,255,(Uint8)(alpha*fade)});
    }

    // Input box assembles
    if(bp > 0.58f){
        float fade=smoothstep(clamp01((bp-0.58f)/0.25f));
        // Glowing input box
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r,12,20,45,(Uint8)(200*fade));
        SDL_Rect inp={ox+10,oy+34,340,34}; SDL_RenderFillRect(r,&inp);
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
        // Multi-layer border glow
        for(int g=0;g<4;++g){
            SDL_Color gc=thsv(hue,0.9f,1.f);
            gc.a=(Uint8)(255*fade*(1.f-g*0.22f));
            SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
            SDL_Rect inpG={ox+10-g,oy+34-g,340+g*2,34+g*2};
            SDL_RenderDrawRect(r,&inpG);
        }
        // Text with blinking cursor
        std::string display=std::string(g_newPLName);
        if(fmod(t,1.f)<0.55f) display+="_";
        // Glow under text
        SDL_Color tglow=thsv(hue,0.4f,1.f); tglow.a=(Uint8)(alpha*fade*0.3f);
        for(int g=0;g<3;++g) drawText(r,g_font,display,ox+16+g,oy+40,tglow,false);
        drawText(r,g_font,display,ox+16,oy+40,{220,240,255,(Uint8)(alpha*fade)});

        // Hint
        float hintFade=clamp01((bp-0.80f)/0.15f);
        if(hintFade>0.f)
            drawText(r,g_fontSm,"[ ENTER ] confirm    [ ESC ] cancel",
                     ox+10,oy+78,{100,130,170,(Uint8)(160*hintFade)});
    }
}


// ═══════════════════════════════════════════════════════════════════════════════
// SPOTIFY IMPORT SYSTEM
// Flow: credentials dialog → OAuth browser → token capture → fetch playlists
//       → pick playlist → yt-dlp download queue → auto-add to library
// ═══════════════════════════════════════════════════════════════════════════════

// ── CONFIG ────────────────────────────────────────────────────────────────────
static std::string g_spotClientId;
static std::string g_spotClientSecret;
static std::string g_spotAccessToken;

static std::string getExeDir(){
    char buf[MAX_PATH]; GetModuleFileNameA(nullptr,buf,MAX_PATH);
    std::string s(buf); auto sl=s.find_last_of("\\/");
    return s.substr(0,sl+1);
}
static std::string spotConfigPath(){ return getExeDir()+"spotify_config.txt"; }

void saveSpotifyConfig(){
    std::ofstream f(spotConfigPath());
    if(!f) return;
    f<<"CLIENT_ID "<<g_spotClientId<<"\n";
    f<<"CLIENT_SECRET "<<g_spotClientSecret<<"\n";
}
void loadSpotifyConfig(){
    std::ifstream f(spotConfigPath());
    if(!f) return;
    std::string line;
    while(std::getline(f,line)){
        if(line.substr(0,10)=="CLIENT_ID ") g_spotClientId=line.substr(10);
        if(line.substr(0,14)=="CLIENT_SECRET ") g_spotClientSecret=line.substr(14);
    }
}

// ── TINY HTTP HELPERS (WinHTTP, ships with Windows) ──────────────────────────

// Synchronous HTTPS GET — returns response body or "" on error
std::string httpsGet(const std::string& host, const std::string& path,
                     const std::string& authHeader=""){
    HINTERNET hSession=WinHttpOpen(L"AcidKaleido/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!hSession) return "";
    std::wstring whost(host.begin(),host.end());
    HINTERNET hConn=WinHttpConnect(hSession,whost.c_str(),INTERNET_DEFAULT_HTTPS_PORT,0);
    if(!hConn){WinHttpCloseHandle(hSession);return "";}
    std::wstring wpath(path.begin(),path.end());
    HINTERNET hReq=WinHttpOpenRequest(hConn,L"GET",wpath.c_str(),
        nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if(!hReq){WinHttpCloseHandle(hConn);WinHttpCloseHandle(hSession);return "";}
    if(!authHeader.empty()){
        std::wstring wauth(authHeader.begin(),authHeader.end());
        WinHttpAddRequestHeaders(hReq,wauth.c_str(),(ULONG)-1,
            WINHTTP_ADDREQ_FLAG_ADD);
    }
    if(!WinHttpSendRequest(hReq,WINHTTP_NO_ADDITIONAL_HEADERS,0,
        WINHTTP_NO_REQUEST_DATA,0,0,0)){
        WinHttpCloseHandle(hReq);WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);return "";
    }
    WinHttpReceiveResponse(hReq,nullptr);
    std::string body;
    DWORD avail=0;
    while(WinHttpQueryDataAvailable(hReq,&avail)&&avail>0){
        std::vector<char> buf(avail+1,0);
        DWORD read=0;
        WinHttpReadData(hReq,buf.data(),avail,&read);
        body.append(buf.data(),read);
    }
    WinHttpCloseHandle(hReq);WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return body;
}

// HTTPS POST with body
std::string httpsPost(const std::string& host,const std::string& path,
                      const std::string& body,const std::string& contentType,
                      const std::string& authHeader=""){
    HINTERNET hSession=WinHttpOpen(L"AcidKaleido/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!hSession) return "";
    std::wstring whost(host.begin(),host.end());
    HINTERNET hConn=WinHttpConnect(hSession,whost.c_str(),INTERNET_DEFAULT_HTTPS_PORT,0);
    if(!hConn){WinHttpCloseHandle(hSession);return "";}
    std::wstring wpath(path.begin(),path.end());
    HINTERNET hReq=WinHttpOpenRequest(hConn,L"POST",wpath.c_str(),
        nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if(!hReq){WinHttpCloseHandle(hConn);WinHttpCloseHandle(hSession);return "";}
    std::wstring wct(contentType.begin(),contentType.end());
    WinHttpAddRequestHeaders(hReq,
        (L"Content-Type: "+wct).c_str(),(ULONG)-1,WINHTTP_ADDREQ_FLAG_ADD);
    if(!authHeader.empty()){
        std::wstring wauth(authHeader.begin(),authHeader.end());
        WinHttpAddRequestHeaders(hReq,wauth.c_str(),(ULONG)-1,WINHTTP_ADDREQ_FLAG_ADD);
    }
    WinHttpSendRequest(hReq,WINHTTP_NO_ADDITIONAL_HEADERS,0,
        (LPVOID)body.c_str(),(DWORD)body.size(),(DWORD)body.size(),0);
    WinHttpReceiveResponse(hReq,nullptr);
    std::string resp;
    DWORD avail=0;
    while(WinHttpQueryDataAvailable(hReq,&avail)&&avail>0){
        std::vector<char> buf(avail+1,0);DWORD read=0;
        WinHttpReadData(hReq,buf.data(),avail,&read);
        resp.append(buf.data(),read);
    }
    WinHttpCloseHandle(hReq);WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return resp;
}

// ── TINY JSON FIELD EXTRACTOR (no dependency) ────────────────────────────────
// Extracts the value of "key":"value" or "key":value from a JSON string
// Handles both string values and array/object scanning.
std::string jsonField(const std::string& json, const std::string& key){
    std::string search="\""+key+"\"";
    size_t pos=json.find(search);
    if(pos==std::string::npos) return "";
    pos+=search.size();
    // skip whitespace and colon
    while(pos<json.size()&&(json[pos]==' '||json[pos]=='\t'||json[pos]==':')) ++pos;
    if(pos>=json.size()) return "";
    if(json[pos]=='"'){
        // string value
        ++pos; std::string val;
        while(pos<json.size()&&json[pos]!='"'){
            if(json[pos]=='\\'&&pos+1<json.size()){++pos;}
            val+=json[pos++];
        }
        return val;
    } else {
        // number/bool/null
        size_t end=json.find_first_of(",}\n]",pos);
        return json.substr(pos,end-pos);
    }
}

// Extract all values of a key across a JSON array of objects
std::vector<std::string> jsonArrayField(const std::string& json, const std::string& key){
    std::vector<std::string> results;
    size_t pos=0;
    std::string search="\""+key+"\"";
    while((pos=json.find(search,pos))!=std::string::npos){
        pos+=search.size();
        while(pos<json.size()&&(json[pos]==' '||json[pos]=='\t'||json[pos]==':'))++pos;
        if(pos>=json.size()) break;
        if(json[pos]=='"'){
            ++pos; std::string val;
            while(pos<json.size()&&json[pos]!='"'){
                if(json[pos]=='\\'&&pos+1<json.size()){++pos;}
                val+=json[pos++];
            }
            results.push_back(val);
        }
    }
    return results;
}

// URL-encode a string for query params
std::string urlEncode(const std::string& s){
    std::string out; char hex[4];
    for(unsigned char c:s){
        if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') out+=c;
        else{ snprintf(hex,sizeof(hex),"%%%02X",c); out+=hex; }
    }
    return out;
}

// Base64 encode (for client_id:client_secret Basic auth)
static const char B64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64(const std::string& s){
    std::string out; int val=0,bits=-6;
    for(unsigned char c:s){
        val=(val<<8)+c; bits+=8;
        while(bits>=0){ out+=B64[(val>>bits)&0x3F]; bits-=6; }
    }
    if(bits>-6) out+=B64[((val<<8)>>(bits+8))&0x3F];
    while(out.size()%4) out+='=';
    return out;
}

// ── OAUTH LOCAL SERVER ────────────────────────────────────────────────────────
// Spins a TCP server on localhost:8888 and waits for the Spotify redirect.
// Runs on a background thread; deposits the auth code into g_spotAuthCode.
static std::string   g_spotAuthCode;
static std::atomic<bool> g_spotCodeReady{false};
static std::mutex    g_spotMtx;

void oauthServerThread(){
    WSADATA wsd; WSAStartup(MAKEWORD(2,2),&wsd);
    SOCKET srv=socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in addr{}; addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    addr.sin_port=htons(8888);
    int yes=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,(char*)&yes,sizeof(yes));
    if(bind(srv,(sockaddr*)&addr,sizeof(addr))!=0||listen(srv,1)!=0){
        closesocket(srv); WSACleanup(); return;
    }
    // Wait up to 120s for Spotify to redirect
    fd_set fds; FD_ZERO(&fds); FD_SET(srv,&fds);
    timeval tv{120,0};
    if(select(0,&fds,nullptr,nullptr,&tv)<=0){closesocket(srv);WSACleanup();return;}
    SOCKET client=accept(srv,nullptr,nullptr);
    char buf[4096]={};
    recv(client,buf,sizeof(buf)-1,0);
    std::string req(buf);
    // Parse ?code=XXXX from GET line
    auto codePos=req.find("code=");
    if(codePos!=std::string::npos){
        size_t end=req.find_first_of(" &\r\n",codePos+5);
        std::string code=req.substr(codePos+5,end-codePos-5);
        std::lock_guard<std::mutex> lk(g_spotMtx);
        g_spotAuthCode=code;
        g_spotCodeReady=true;
    }
    // Send a sick animated HTML response back to the browser
    const char* html="HTTP/1.1 200 OK\r\nContent-Type:text/html\r\n\r\n"
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<title>ACID KALEIDO — Connected</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{background:#000;color:#fff;font-family:monospace;overflow:hidden;height:100vh;display:flex;align-items:center;justify-content:center}"
"canvas{position:fixed;top:0;left:0;width:100%;height:100%;z-index:0}"
".ui{position:relative;z-index:10;text-align:center;padding:40px}"
".title{font-size:2.8em;letter-spacing:0.3em;color:#00ffaa;text-shadow:0 0 30px #00ffaa,0 0 60px #00ff88;animation:pulse 2s ease-in-out infinite}"
".sub{font-size:1em;letter-spacing:0.2em;color:#88ffcc;margin:18px 0 32px;opacity:0.8}"
".bar-wrap{width:420px;margin:0 auto 18px;background:#0a1a10;border:1px solid #00ff88;border-radius:2px;overflow:hidden;box-shadow:0 0 20px #00ff4433}"
".bar{height:14px;width:0%;background:linear-gradient(90deg,#00ff88,#00ffcc,#00aaff);animation:fill 4s cubic-bezier(0.1,0,0.2,1) forwards;box-shadow:0 0 12px #00ffaa}"
".pct{font-size:0.85em;color:#00ff88;letter-spacing:0.15em;margin-bottom:24px}"
".msg{font-size:0.75em;color:#446655;letter-spacing:0.12em;line-height:2}"
".close{margin-top:28px;font-size:0.7em;color:#224433;letter-spacing:0.15em}"
"@keyframes pulse{0%,100%{text-shadow:0 0 30px #00ffaa,0 0 60px #00ff88}50%{text-shadow:0 0 60px #00ffaa,0 0 120px #00ff88,0 0 200px #00ffcc}}"
"@keyframes fill{0%{width:0%}60%{width:72%}80%{width:88%}92%{width:94%}100%{width:100%}}"
"@keyframes fadeIn{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:translateY(0)}}"
".line{opacity:0;animation:fadeIn 0.4s ease forwards}"
".l1{animation-delay:0.3s}.l2{animation-delay:0.7s}.l3{animation-delay:1.1s}.l4{animation-delay:1.5s}.l5{animation-delay:2.2s}"
"</style></head>"
"<body>"
"<canvas id='c'></canvas>"
"<div class='ui'>"
"  <div class='title'>ACID KALEIDO</div>"
"  <div class='sub'>SPOTIFY LINK ESTABLISHED</div>"
"  <div class='bar-wrap'><div class='bar'></div></div>"
"  <div class='pct' id='pct'>0%</div>"
"  <div class='msg'>"
"    <div class='line l1'>&#x2714; OAuth token received</div>"
"    <div class='line l2'>&#x2714; Credentials verified</div>"
"    <div class='line l3'>&#x2714; Secure handshake complete</div>"
"    <div class='line l4'>&#x2714; Returning to visualizer...</div>"
"    <div class='line l5' style='color:#00ffaa'>&#x2605; Authorization complete</div>"
"  </div>"
"  <div class='close'>You can close this tab</div>"
"</div>"
"<script>"
// Kaleidoscope canvas animation
"var c=document.getElementById('c'),x=c.getContext('2d'),t=0;"
"function rs(){c.width=window.innerWidth;c.height=window.innerHeight}"
"rs();window.onresize=rs;"
"function draw(){"
"  requestAnimationFrame(draw);t+=0.012;"
"  x.fillStyle='rgba(0,0,0,0.18)';x.fillRect(0,0,c.width,c.height);"
"  var cx=c.width/2,cy=c.height/2,slices=8;"
"  for(var s=0;s<slices;s++){"
"    x.save();x.translate(cx,cy);x.rotate(s/slices*Math.PI*2+t*0.07);"
"    if(s%2)x.scale(1,-1);"
"    for(var i=0;i<40;i++){"
"      var bt=i/40,ang=bt*Math.PI/slices;"
"      var r0=20+Math.sin(t*1.5)*8,r1=r0+(0.3+Math.sin(t*2+i)*0.15)*280;"
"      var hue=(t*60+bt*200+s*5)%360;"
"      x.beginPath();"
"      x.moveTo(Math.cos(ang)*r0,Math.sin(ang)*r0);"
"      x.lineTo(Math.cos(ang)*r1,Math.sin(ang)*r1);"
"      x.strokeStyle='hsla('+hue+',90%,60%,'+(0.08+Math.sin(t+i)*0.04)+')';"
"      x.lineWidth=1+Math.sin(t*3+i)*1.5;"
"      x.stroke();"
"    }"
"    x.restore();"
"  }"
"}"
"draw();"
// Percentage counter
"var start=Date.now(),dur=4000;"
"function tick(){"
"  var p=Math.min(100,Math.floor((Date.now()-start)/dur*100));"
"  document.getElementById('pct').textContent=p+'%';"
"  if(p<100)setTimeout(tick,50);"
"}"
"tick();"
"</script></body></html>";
    send(client,html,(int)strlen(html),0);
    closesocket(client); closesocket(srv); WSACleanup();
}

// ── SPOTIFY STATE MACHINE ─────────────────────────────────────────────────────
enum class SpotState {
    Idle,
    CredentialsDialog,  // entering client ID / secret
    WaitingAuth,        // browser opened, waiting for redirect
    FetchingPlaylists,  // API call in progress
    PlaylistPicker,     // user picks which playlist to import
    Downloading,        // yt-dlp queue running
    Done
};

struct SpotTrack { std::string title, artist, searchQuery, filePath, status; };
struct SpotPlaylistInfo { std::string id, name; int total=0; };

static SpotState  g_spotState     = SpotState::Idle;
static float      g_spotBuildT    = 0.f;
static bool       g_spotBuilding  = false;
// Per-state build timers so fast state transitions don't kill animations
static float      g_spotWaitBuildT = 0.f;

// Credentials dialog fields
static char g_spotIdBuf[256]     = {};
static char g_spotSecretBuf[256] = {};
static bool g_spotFocusId        = true; // which field has focus

// Playlist picker
static std::vector<SpotPlaylistInfo> g_spotPlaylists;
static int  g_spotPlScroll  = 0;
static int  g_spotPlHover   = -1;
static std::string g_spotStatusMsg;
// Fake loading bar timer — purely cosmetic drama
static float g_spotFakeLoadT   = 0.f;   // 0→1 over 4 seconds
static bool  g_spotFakeLoading = false;

// Download queue
static std::vector<SpotTrack>  g_spotTracks;
static std::atomic<int>        g_spotDlIdx{0};   // which track is downloading
static std::mutex              g_spotTrackMtx;
static std::thread             g_spotDlThread;
static std::string             g_spotDlPlaylistName;
static std::atomic<bool>       g_spotDlRunning{false};

// ── SPOTIFY API CALLS ─────────────────────────────────────────────────────────
bool spotExchangeCode(const std::string& code){
    std::string body="grant_type=authorization_code"
                     "&code="+urlEncode(code)+
                     "&redirect_uri="+urlEncode("http://127.0.0.1:8888/callback");
    std::string auth="Authorization: Basic "+
                     base64(g_spotClientId+":"+g_spotClientSecret);
    std::string resp=httpsPost("accounts.spotify.com","/api/token",
                               body,"application/x-www-form-urlencoded",auth);
    g_spotAccessToken=jsonField(resp,"access_token");
    return !g_spotAccessToken.empty();
}

// Fetch user's playlists (first 50)
bool spotFetchPlaylists(){
    std::string auth="Authorization: Bearer "+g_spotAccessToken;
    std::string resp=httpsGet("api.spotify.com",
                              "/v1/me/playlists?limit=50",auth);
    // Write response to debug file so we can see what went wrong
    {
        std::ofstream dbg(getExeDir()+"spotify_debug.txt");
        dbg<<"ACCESS TOKEN: "<<g_spotAccessToken<<"\n\n";
        dbg<<"RESPONSE:\n"<<resp<<"\n";
    }
    if(resp.empty()) return false;
    g_spotPlaylists.clear();
    // Parse items array — find each "id" and "name" pair
    // We scan for "items":[...] and iterate through objects
    size_t itemsPos=resp.find("\"items\"");
    if(itemsPos==std::string::npos) return false;
    size_t pos=itemsPos;
    while(true){
        size_t idPos=resp.find("\"id\"",pos);
        size_t namePos=resp.find("\"name\"",pos);
        size_t totalPos=resp.find("\"total\"",pos);
        if(idPos==std::string::npos||namePos==std::string::npos) break;
        // Make sure we're inside items array (before "next" key)
        size_t nextPos=resp.find("\"next\"",pos);
        if(nextPos!=std::string::npos&&idPos>nextPos) break;
        SpotPlaylistInfo pl;
        // Extract id
        size_t p=idPos+5;
        while(p<resp.size()&&(resp[p]==' '||resp[p]==':'))++p;
        if(resp[p]=='"'){++p;while(p<resp.size()&&resp[p]!='"')pl.id+=resp[p++];}
        // Extract name
        p=namePos+7;
        while(p<resp.size()&&(resp[p]==' '||resp[p]==':'))++p;
        if(resp[p]=='"'){++p;while(p<resp.size()&&resp[p]!='"'){
            if(resp[p]=='\\'&&p+1<resp.size()){++p;}
            pl.name+=resp[p++];
        }}
        // Extract total tracks
        if(totalPos!=std::string::npos&&totalPos<(idPos+2000)){
            p=totalPos+8;
            while(p<resp.size()&&(resp[p]==' '||resp[p]==':'))++p;
            std::string tot;
            while(p<resp.size()&&isdigit(resp[p]))tot+=resp[p++];
            if(!tot.empty())pl.total=std::stoi(tot);
        }
        if(!pl.id.empty()&&!pl.name.empty())
            g_spotPlaylists.push_back(pl);
        pos=std::max(idPos,namePos)+1;
    }
    return !g_spotPlaylists.empty();
}

// Fetch all tracks from a playlist
bool spotFetchTracks(const std::string& playlistId, int total){
    g_spotTracks.clear();
    std::string auth="Authorization: Bearer "+g_spotAccessToken;
    int offset=0;
    while(offset<total){
        std::string path="/v1/playlists/"+playlistId+
                         "/tracks?limit=100&offset="+std::to_string(offset)+
                         "&fields=items(track(name,artists(name)))";
        std::string resp=httpsGet("api.spotify.com",path,auth);
        if(resp.empty()) break;

        // JSON: {"track":{"artists":[{"name":"ARTIST"}],"name":"TITLE"}}
        // "track" can be null for local/deleted tracks — skip those
        auto extractStr=[&](const std::string& src, size_t from)->std::pair<std::string,size_t>{
            size_t q=src.find('"',from);
            if(q==std::string::npos) return {"",std::string::npos};
            ++q; std::string out;
            while(q<src.size()&&src[q]!='"'){
                if(src[q]=='\\' &&q+1<src.size()) ++q;
                out+=src[q++];
            }
            return {out,q+1};
        };

        size_t pos=0;
        while(true){
            size_t trackPos=resp.find("\"track\":",pos);
            if(trackPos==std::string::npos) break;
            pos=trackPos+8;
            // skip whitespace
            while(pos<resp.size()&&resp[pos]==' ') ++pos;
            // null track = deleted/local song — skip it
            if(pos+3<resp.size()&&resp.substr(pos,4)=="null"){ pos+=4; continue; }

            // Grab the track object by walking braces (cap at 2048 chars)
            size_t objOpen=resp.find('{',pos);
            if(objOpen==std::string::npos) break;
            int depth=0; size_t objClose=objOpen;
            for(size_t k=objOpen;k<resp.size()&&k<objOpen+2048;++k){
                if(resp[k]=='{') ++depth;
                else if(resp[k]=='}'){ --depth; if(!depth){objClose=k;break;} }
            }
            std::string obj=resp.substr(objOpen,objClose-objOpen+1);
            pos=objClose+1;

            // Artist — inside the artists:[...] array
            std::string artist;
            size_t aPos=obj.find("\"artists\":");
            if(aPos!=std::string::npos){
                size_t aOpen=obj.find('[',aPos);
                size_t aClose=obj.find(']',aOpen!=std::string::npos?aOpen:aPos);
                if(aOpen!=std::string::npos){
                    size_t nPos=obj.find("\"name\":",aOpen);
                    if(nPos!=std::string::npos&&(aClose==std::string::npos||nPos<aClose))
                        artist=extractStr(obj,nPos+8).first;
                }
            }

            // Track name — find "name": that comes AFTER the artists array closes
            std::string title;
            size_t searchFrom=obj.find(']', obj.find("\"artists\":"));
            if(searchFrom==std::string::npos) searchFrom=0;
            size_t tnPos=obj.find("\"name\":",searchFrom);
            if(tnPos!=std::string::npos)
                title=extractStr(obj,tnPos+8).first;

            if(!title.empty()){
                SpotTrack tr;
                tr.title=title; tr.artist=artist;
                tr.searchQuery=artist.empty()?title:artist+" - "+title;
                tr.status="waiting";
                g_spotTracks.push_back(tr);
            }
        }
        offset+=100;
    }
    return !g_spotTracks.empty();
}

// ── YT-DLP DOWNLOAD THREAD ────────────────────────────────────────────────────
void downloadThread(const std::string& outDir){
    g_spotDlRunning=true;
    int n=(int)g_spotTracks.size();
    std::string ytdlp=getExeDir()+"yt-dlp.exe";
    std::string ffmpeg=getExeDir()+"ffmpeg.exe";

    for(int i=0;i<n;++i){
        g_spotDlIdx=i;
        {
            std::lock_guard<std::mutex> lk(g_spotTrackMtx);
            g_spotTracks[i].status="downloading";
        }
        std::string safeQuery=g_spotTracks[i].searchQuery;
        std::string escaped;
        for(char c:safeQuery){if(c=='"')escaped+="\\\"";else escaped+=c;}
        std::string outTemplate=outDir+"\\%(title)s.%(ext)s";
        // Build command — use CreateProcess with CREATE_NO_WINDOW to suppress console popup
        std::string cmdLine="\""+ytdlp+"\" \"ytsearch1:"+escaped+"\" "
                            "-x --audio-format mp3 --audio-quality 0 "
                            "--no-playlist --quiet "
                            "--ffmpeg-location \""+ffmpeg+"\" "
                            "-o \""+outTemplate+"\"";
        int ret=-1;
        {
            STARTUPINFOA si={};si.cb=sizeof(si);
            PROCESS_INFORMATION pi={};
            si.dwFlags=STARTF_USESTDHANDLES;
            // Redirect stdout/stderr to NUL to suppress output
            SECURITY_ATTRIBUTES sa={sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
            HANDLE nul=CreateFileA("NUL",GENERIC_WRITE,FILE_SHARE_WRITE,&sa,
                                   OPEN_EXISTING,0,nullptr);
            si.hStdOutput=nul; si.hStdError=nul; si.hStdInput=nullptr;
            std::vector<char> cmdBuf(cmdLine.begin(),cmdLine.end());cmdBuf.push_back(0);
            if(CreateProcessA(nullptr,cmdBuf.data(),nullptr,nullptr,TRUE,
                              CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){
                WaitForSingleObject(pi.hProcess,INFINITE);
                DWORD exitCode=1;
                GetExitCodeProcess(pi.hProcess,&exitCode);
                ret=(int)exitCode;
                CloseHandle(pi.hProcess);CloseHandle(pi.hThread);
            }
            if(nul!=INVALID_HANDLE_VALUE) CloseHandle(nul);
        }
        {
            std::lock_guard<std::mutex> lk(g_spotTrackMtx);
            g_spotTracks[i].status=(ret==0)?"done":"failed";
        }
    }

    // All downloads done — scan folder and push results to main thread via event
    // We store the found paths in a shared vector then signal the main thread
    {
        std::lock_guard<std::mutex> lk(g_spotTrackMtx);
        // Reuse the filePath field of each track to store found mp3 paths
        // We'll just signal main thread to do the scan itself
    }

    g_spotDlRunning=false;
    g_spotDlIdx=n;

    // Signal main thread to do the library import (code=2)
    SDL_Event e; e.type=SDL_USEREVENT; e.user.code=2;
    // Pass outDir as a heap string via data1
    e.user.data1=new std::string(outDir);
    SDL_PushEvent(&e);
}

// ── SPOTIFY UI DRAW ───────────────────────────────────────────────────────────
void drawSpotifyUI(SDL_Renderer* r, int ww, int wh, Uint8 alpha, float buildT, float t){
    if(g_spotState==SpotState::Idle) return;
    // Each state uses its own build timer so fast transitions don't kill animations
    float bp;
    if(g_spotState==SpotState::WaitingAuth||g_spotState==SpotState::FetchingPlaylists)
        bp=clamp01(g_spotWaitBuildT);
    else
        bp=clamp01(buildT);
    float hue=fmod(t*22.f+120.f,360.f);

    // Full screen dim
    SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r,0,0,0,(Uint8)(smoothstep(clamp01(bp*3.f))*160.f));
    SDL_RenderFillRect(r,nullptr);

    // ── CREDENTIALS DIALOG ───────────────────────────────────────────────────
    if(g_spotState==SpotState::CredentialsDialog){
        // Left panel: input form
        int fx=ww/2-540,fy=wh/2-160,fw=380,fh=320;
        // Right panel: instructions
        int ix=ww/2-140,iy=wh/2-160,iw=420,ih=320;

        // Clamp to screen
        if(fx<10){int shift=10-fx;fx+=shift;ix+=shift;}

        // ── FORM PANEL ───────────────────────────────────────────────────────
        float scanP=clamp01((bp-0.10f)/0.45f);
        float revH=scanP*fh;
        if(revH>0.f){
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,3,8,6,(Uint8)(alpha*0.97f));
            SDL_Rect bg={fx,fy,fw,(int)revH};SDL_RenderFillRect(r,&bg);
        }
        drawScanSweep(r,fx,fy,fw,fh,scanP,t,hue);
        SDL_Color bc=thsv(hue,0.9f,1.f);bc.a=(Uint8)(alpha*0.95f);
        drawBuildingBorder(r,fx,fy,fw,fh,smoothstep(clamp01((bp-0.05f)/0.40f)),t,bc);

        if(bp>0.28f){
            float f=smoothstep(clamp01((bp-0.28f)/0.22f));
            float tp=clamp01((bp-0.28f)/0.20f);
            std::string title=buildTypeOn("> SPOTIFY CONNECT",tp,t);
            SDL_Color tg=thsv(hue,0.5f,1.f);tg.a=(Uint8)(alpha*f*0.35f);
            for(int g2=0;g2<3;++g2) drawText(r,g_font,title,fx+14+g2,fy+10,tg,false);
            drawText(r,g_font,title,fx+14,fy+10,{140,255,160,(Uint8)(alpha*f)});

            // Thin separator under title
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color sep=thsv(hue,0.7f,1.f);sep.a=(Uint8)(alpha*f*0.5f);
            SDL_SetRenderDrawColor(r,sep.r,sep.g,sep.b,sep.a);
            SDL_Rect sepR={fx+10,fy+28,fw-20,1};SDL_RenderFillRect(r,&sepR);
        }
        // CLIENT ID field
        if(bp>0.44f){
            float f=smoothstep(clamp01((bp-0.44f)/0.20f));
            drawText(r,g_fontSm,"CLIENT ID",fx+14,fy+42,{100,200,120,(Uint8)(alpha*f)});
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,8,18,12,(Uint8)(200*f));
            SDL_Rect inp={fx+12,fy+58,fw-24,30};SDL_RenderFillRect(r,&inp);
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color fc=thsv(hue+(g_spotFocusId?0.f:50.f),0.9f,1.f);
            fc.a=(Uint8)(255*f*(g_spotFocusId?1.f:0.35f));
            for(int g2=0;g2<3;++g2){
                SDL_SetRenderDrawColor(r,fc.r,fc.g,fc.b,(Uint8)(fc.a*(1.f-g2*0.3f)));
                SDL_Rect inpG={fx+12-g2,fy+58-g2,fw-24+g2*2,30+g2*2};
                SDL_RenderDrawRect(r,&inpG);
            }
            std::string idDisp=std::string(g_spotIdBuf);
            if(g_spotFocusId&&fmod(t,1.f)<0.55f) idDisp+="_";
            drawText(r,g_fontSm,idDisp,fx+18,fy+64,{180,255,180,(Uint8)(alpha*f)});
        }
        // CLIENT SECRET field
        if(bp>0.56f){
            float f=smoothstep(clamp01((bp-0.56f)/0.20f));
            drawText(r,g_fontSm,"CLIENT SECRET",fx+14,fy+106,{100,200,120,(Uint8)(alpha*f)});
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,8,18,12,(Uint8)(200*f));
            SDL_Rect inp={fx+12,fy+122,fw-24,30};SDL_RenderFillRect(r,&inp);
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color fc=thsv(hue+(!g_spotFocusId?0.f:50.f),0.9f,1.f);
            fc.a=(Uint8)(255*f*(!g_spotFocusId?1.f:0.35f));
            for(int g2=0;g2<3;++g2){
                SDL_SetRenderDrawColor(r,fc.r,fc.g,fc.b,(Uint8)(fc.a*(1.f-g2*0.3f)));
                SDL_Rect inpG={fx+12-g2,fy+122-g2,fw-24+g2*2,30+g2*2};
                SDL_RenderDrawRect(r,&inpG);
            }
            std::string secDisp(strlen(g_spotSecretBuf),'*');
            if(!g_spotFocusId&&fmod(t,1.f)<0.55f) secDisp+="_";
            drawText(r,g_fontSm,secDisp,fx+18,fy+128,{180,255,180,(Uint8)(alpha*f)});
        }
        // Redirect URI reminder + controls
        if(bp>0.70f){
            float f=smoothstep(clamp01((bp-0.70f)/0.18f));
            // Redirect URI box (read-only, user needs to add this to Spotify app)
            drawText(r,g_fontSm,"REDIRECT URI  (add this to your Spotify app)",
                     fx+14,fy+172,{80,160,90,(Uint8)(alpha*f*0.9f)});
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,6,14,10,(Uint8)(160*f));
            SDL_Rect uriBox={fx+12,fy+188,fw-24,24};SDL_RenderFillRect(r,&uriBox);
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color uc=thsv(hue+30.f,0.6f,0.8f);uc.a=(Uint8)(180*f);
            SDL_SetRenderDrawColor(r,uc.r,uc.g,uc.b,uc.a);
            SDL_RenderDrawRect(r,&uriBox);
            drawText(r,g_fontSm,"http://127.0.0.1:8888/callback",
                     fx+16,fy+193,{120,220,140,(Uint8)(alpha*f)});
        }
        if(bp>0.82f){
            float f=smoothstep(clamp01((bp-0.82f)/0.15f));
            drawText(r,g_fontSm,"[TAB] switch   [ENTER] connect   [ESC] cancel",
                     fx+14,fy+228,{70,130,80,(Uint8)(150*f)});
            // Premium warning
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color warnC=thsv(30.f,1.f,1.f); warnC.a=(Uint8)(200*f);
            drawText(r,g_fontSm,"! Spotify Premium required (thanks, greedy devs)",
                     fx+14,fy+248,warnC);
            if(!g_spotStatusMsg.empty())
                drawText(r,g_fontSm,g_spotStatusMsg,
                         fx+14,fy+265,{255,100,100,(Uint8)(220*f)});
        }

        // ── INSTRUCTIONS PANEL ────────────────────────────────────────────────
        float scanP2=clamp01((bp-0.20f)/0.45f);
        float revH2=scanP2*ih;
        if(revH2>0.f){
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,4,6,14,(Uint8)(alpha*0.97f));
            SDL_Rect bg2={ix,iy,iw,(int)revH2};SDL_RenderFillRect(r,&bg2);
        }
        drawScanSweep(r,ix,iy,iw,ih,scanP2,t,fmod(hue+60.f,360.f));
        SDL_Color bc2=thsv(fmod(hue+60.f,360.f),0.85f,1.f);bc2.a=(Uint8)(alpha*0.85f);
        drawBuildingBorder(r,ix,iy,iw,ih,smoothstep(clamp01((bp-0.15f)/0.40f)),t,bc2);

        if(bp>0.35f){
            float f=smoothstep(clamp01((bp-0.35f)/0.22f));
            drawText(r,g_font,"HOW TO GET CREDENTIALS",
                     ix+14,iy+10,{160,180,255,(Uint8)(alpha*f)});
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color sep2=thsv(fmod(hue+60.f,360.f),0.7f,1.f);sep2.a=(Uint8)(alpha*f*0.5f);
            SDL_SetRenderDrawColor(r,sep2.r,sep2.g,sep2.b,sep2.a);
            SDL_Rect sepR2={ix+10,iy+28,iw-20,1};SDL_RenderFillRect(r,&sepR2);
        }

        // Step-by-step instructions — stagger in one by one
        struct Step{ float startT; const char* num; const char* text; };
        static const Step steps[]={
            {0.42f, "1.", "Go to  developer.spotify.com"},
            {0.50f, "2.", "Log in and click  'Create App'"},
            {0.57f, "3.", "Name it anything  (e.g. AcidKaleido)"},
            {0.63f, "4.", "Set Redirect URI:"},
            {0.63f, "  ", "  127.0.0.1:8888/callback"},
            {0.70f, "5.", "Open app Settings -> copy"},
            {0.70f, "  ", "  Client ID  and  Client Secret"},
            {0.77f, "6.", "Paste them into the fields on the left"},
            {0.84f, "7.", "Press ENTER to connect"},
            {0.88f, "!", "Spotify Premium required to use the API"},
            {0.88f, " ", "(yes really. their rules, not ours)"},
        };
        int nSteps=sizeof(steps)/sizeof(steps[0]);
        for(int si=0;si<nSteps;++si){
            if(bp<=steps[si].startT) continue;
            float sf=smoothstep(clamp01((bp-steps[si].startT)/0.12f));
            int sy=iy+40+si*26;
            // Slide in from right
            int slideX=(int)(ix+14+(1.f-sf)*40.f);
            Uint8 sa=(Uint8)(alpha*sf);
            // Warning lines render in orange
            bool isWarning=(steps[si].num[0]=='!'||steps[si].num[0]==' '&&si>=9);
            SDL_Color nc=isWarning?
                thsv(30.f,1.f,1.f):
                thsv(fmod(hue+60.f+si*15.f,360.f),0.9f,1.f);
            nc.a=sa;
            SDL_Color tc=isWarning?
                SDL_Color{255,180,80,sa}:
                SDL_Color{200,210,240,sa};
            drawText(r,g_fontSm,steps[si].num,slideX,sy,nc);
            drawText(r,g_fontSm,steps[si].text,slideX+22,sy,tc);
        }
    }

    // ── WAITING FOR AUTH / FETCHING ──────────────────────────────────────────
    if(g_spotState==SpotState::WaitingAuth||g_spotState==SpotState::FetchingPlaylists){
        int ox=ww/2-280,oy=wh/2-120,ow=560,oh=240;
        if(ox<10) ox=10;

        // Background + scanline build-in
        float scanP=clamp01(bp/0.45f);
        float revH=scanP*oh;
        if(revH>0.f){
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,3,8,6,(Uint8)(alpha*0.97f));
            SDL_Rect bg={ox,oy,ow,(int)revH};SDL_RenderFillRect(r,&bg);
        }
        drawScanSweep(r,ox,oy,ow,oh,scanP,t,hue);
        SDL_Color bc=thsv(hue,0.9f,1.f);bc.a=(Uint8)(alpha*0.95f);
        drawBuildingBorder(r,ox,oy,ow,oh,smoothstep(clamp01(bp/0.38f)),t,bc);

        // Corner sparks during build
        if(bp>0.05f&&bp<0.55f){
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            float sparkT=clamp01(bp/0.55f);
            for(int corner=0;corner<4;++corner){
                float cx2=(corner%2==0)?(float)ox:(float)(ox+ow);
                float cy2=(corner<2)?(float)oy:(float)(oy+oh);
                float sa2=sinf(sparkT*(float)M_PI)*180.f;
                for(int sp=0;sp<5;++sp){
                    float sa3=(float)sp/5.f*2*(float)M_PI+t*9.f+corner*1.57f;
                    SDL_Color sc=thsv(fmod(hue+corner*90.f+sp*25.f,360.f),1.f,1.f);
                    sc.a=(Uint8)(sa2*(1.f-(float)sp/5.f));
                    softCircle(r,{cx2+cosf(sa3)*sp*4.f,cy2+sinf(sa3)*sp*4.f},
                               2.f+sp*0.5f,sc,{sc.r,sc.g,sc.b,0},6);
                }
            }
        }

        if(bp>0.28f){
            float f=smoothstep(clamp01((bp-0.28f)/0.22f));

            // Title with glow
            bool isFetching=g_spotState==SpotState::FetchingPlaylists;
            std::string titleStr=isFetching?
                "> ESTABLISHING CONNECTION...":"> AWAITING AUTHORIZATION...";
            float tp=clamp01((bp-0.28f)/0.20f);
            std::string title=buildTypeOn(titleStr,tp,t);
            SDL_Color tg=thsv(hue,0.5f,1.f);tg.a=(Uint8)(alpha*f*0.35f);
            for(int g2=0;g2<3;++g2) drawText(r,g_font,title,ox+16+g2,oy+12,tg,false);
            drawText(r,g_font,title,ox+16,oy+12,{140,255,160,(Uint8)(alpha*f)});

            // Separator
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            SDL_Color sep=thsv(hue,0.7f,1.f);sep.a=(Uint8)(alpha*f*0.4f);
            SDL_SetRenderDrawColor(r,sep.r,sep.g,sep.b,sep.a);
            SDL_Rect sepR={ox+10,oy+32,ow-20,1};SDL_RenderFillRect(r,&sepR);
        }

        // Status lines — type on one by one
        if(bp>0.40f){
            float f=smoothstep(clamp01((bp-0.40f)/0.20f));
            bool isFetching=g_spotState==SpotState::FetchingPlaylists;

            struct StatusLine{ float start; const char* text; };
            const StatusLine authLines[]={
                {0.40f,"  Initializing secure handshake..."},
                {0.48f,"  Opening browser window..."},
                {0.55f,"  Waiting for Spotify OAuth response..."},
                {0.62f,"  Listening on port 8888..."},
            };
            const StatusLine fetchLines[]={
                {0.40f,"  Token exchange complete"},
                {0.47f,"  Authenticating with Spotify API..."},
                {0.54f,"  Requesting playlist data..."},
                {0.61f,"  Parsing response..."},
            };
            const StatusLine* lines=isFetching?fetchLines:authLines;
            for(int li=0;li<4;++li){
                if(bp<=lines[li].start) continue;
                float lf=smoothstep(clamp01((bp-lines[li].start)/0.10f));
                int ly=oy+42+li*18;
                // Slide in from left
                int lx=(int)(ox+16+(1.f-lf)*30.f);
                // Color: done lines go dim green, active line is bright
                bool isActive=(li==3&&bp<0.90f);
                SDL_Color lc;
                if(isActive) lc=thsv(hue,0.9f,1.f);
                else         lc={70,140,80,(Uint8)(alpha*lf*0.8f)};
                lc.a=(Uint8)(alpha*lf*(isActive?1.f:0.7f));
                // Add animated dots to active line
                std::string txt=lines[li].text;
                if(isActive){
                    int dots=(int)(t*3.f)%4;
                    // Strip existing dots and re-add
                    while(!txt.empty()&&txt.back()=='.') txt.pop_back();
                    for(int d=0;d<dots;d++) txt+='.';
                }
                drawText(r,g_fontSm,txt,lx,ly,lc);
                // Checkmark for completed lines
                if(!isActive&&lf>0.8f){
                    SDL_Color ck=thsv(hue,0.8f,1.f);ck.a=(Uint8)(alpha*lf*0.9f);
                    drawText(r,g_fontSm,"OK",ox+ow-36,ly,ck);
                }
            }
        }

        // ── FAKE LOADING BAR ─────────────────────────────────────────────────
        if(bp>0.55f){
            float f=smoothstep(clamp01((bp-0.55f)/0.20f));
            int pbX=ox+16, pbY=oy+126, pbW=ow-32, pbH=10;

            // Label
            float fakeP=g_spotFakeLoadT; // 0→1 over 4s
            int pct=(int)(fakeP*100.f);
            // Stall at 94% until real auth comes through — looks legit
            if(pct>94&&g_spotState==SpotState::WaitingAuth) pct=94;
            std::string pctStr="CONNECTING...  "+std::to_string(pct)+"%";
            if(g_spotState==SpotState::FetchingPlaylists) pctStr="FETCHING PLAYLISTS...  "+std::to_string(std::min(pct,99))+"%";
            drawText(r,g_fontSm,pctStr,pbX,pbY-16,
                     {100,200,120,(Uint8)(alpha*f)});

            // Track background
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,10,25,14,(Uint8)(200*f));
            SDL_Rect pbBg={pbX,pbY,pbW,pbH};SDL_RenderFillRect(r,&pbBg);

            // Filled portion — hue-shifts as it fills
            float fillW=fakeP*pbW;
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            // Draw in horizontal strips for a gradient effect
            int strips=20;
            for(int s=0;s<strips;++s){
                float sf=(float)s/strips;
                if(sf*pbW>fillW) break;
                float sw=std::min(pbW/strips+1.f, fillW-sf*pbW);
                SDL_Color sc=thsv(fmod(hue+sf*60.f+t*20.f,360.f),0.9f,1.f);
                sc.a=(Uint8)(200*f);
                SDL_SetRenderDrawColor(r,sc.r,sc.g,sc.b,sc.a);
                SDL_Rect stripe={pbX+(int)(sf*pbW),pbY,(int)sw,pbH};
                SDL_RenderFillRect(r,&stripe);
            }

            // Shimmer scan moving across the filled portion
            if(fillW>4.f){
                float shimmerX=fmod(t*0.4f,1.f)*fillW;
                for(int g2=0;g2<6;++g2){
                    float gf=(float)g2/6.f;
                    SDL_Color gc={255,255,255,(Uint8)(f*(1.f-gf)*80.f)};
                    SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
                    SDL_Rect shim={pbX+(int)(shimmerX-g2*3),pbY,3,pbH};
                    SDL_RenderFillRect(r,&shim);
                }
            }

            // Border
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,40,100,50,(Uint8)(160*f));
            SDL_RenderDrawRect(r,&pbBg);

            // Sub-label below bar
            if(g_spotState==SpotState::WaitingAuth){
                drawText(r,g_fontSm,"Browser should have opened — log in and authorize",
                         pbX,pbY+16,{60,120,70,(Uint8)(alpha*f*0.8f)});
            } else {
                drawText(r,g_fontSm,"Retrieving your playlists from Spotify",
                         pbX,pbY+16,{60,120,70,(Uint8)(alpha*f*0.8f)});
            }
        }

        // Cancel hint
        if(bp>0.70f){
            float f=smoothstep(clamp01((bp-0.70f)/0.15f));
            drawText(r,g_fontSm,"[ ESC ] cancel",
                     ox+16,oy+oh-20,{50,100,60,(Uint8)(140*f)});
        }
    }

    // ── PLAYLIST PICKER ──────────────────────────────────────────────────────
    if(g_spotState==SpotState::PlaylistPicker){
        int ox=10,oy=80,ow=360;
        int ph=std::min((int)g_spotPlaylists.size(),VISIBLE_ROWS)*ROW_H+32;
        float scanP=clamp01((bp-0.10f)/0.45f);
        float revH=scanP*ph;
        if(revH>0.f){
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,3,8,6,(Uint8)(alpha*0.97f));
            SDL_Rect bg={ox,oy,ow,(int)revH};SDL_RenderFillRect(r,&bg);
        }
        drawScanSweep(r,ox,oy,ow,ph,scanP,t,hue);
        SDL_Color bc=thsv(hue,0.9f,1.f);bc.a=(Uint8)(alpha*0.95f);
        drawBuildingBorder(r,ox,oy,ow,ph,smoothstep(clamp01((bp-0.05f)/0.40f)),t,bc);
        if(bp>0.25f){
            float hf=smoothstep(clamp01((bp-0.25f)/0.20f));
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,10,30,18,(Uint8)(alpha*hf*0.97f));
            SDL_Rect hdr={ox,oy,ow,28};SDL_RenderFillRect(r,&hdr);
            float tp=clamp01((bp-0.25f)/0.25f);
            std::string title=buildTypeOn("SELECT PLAYLIST TO IMPORT",tp,t);
            SDL_Color tg=thsv(hue,0.5f,1.f);tg.a=(Uint8)(alpha*hf*0.35f);
            for(int g2=0;g2<3;++g2) drawText(r,g_font,title,ox+8+g2,oy+6,tg,false);
            drawText(r,g_font,title,ox+8,oy+6,{140,255,160,(Uint8)(alpha*hf)});
        }
        if(bp>0.45f){
            int listY=oy+30;
            int visEnd=std::min(g_spotPlScroll+VISIBLE_ROWS,(int)g_spotPlaylists.size());
            for(int i=g_spotPlScroll;i<visEnd;++i){
                float rowStart=0.45f+(i-g_spotPlScroll)*0.035f;
                float rowP=clamp01((bp-rowStart)/0.35f);
                int ry=listY+(i-g_spotPlScroll)*ROW_H;
                std::string label=g_spotPlaylists[i].name+
                                  "  ["+std::to_string(g_spotPlaylists[i].total)+" tracks]";
                drawAnimatedRow(r,ox+4,ry,ow-8,label,false,i==g_spotPlHover,
                                alpha,rowP,t,fmod(hue+(i*18.f),360.f));
            }
        }
    }

    // ── DOWNLOAD QUEUE ───────────────────────────────────────────────────────
    if(g_spotState==SpotState::Downloading||g_spotState==SpotState::Done){
        int ox=10,oy=80,ow=520;
        int visRows=std::min((int)g_spotTracks.size(),VISIBLE_ROWS);
        int rowH=ROW_H+10; // taller rows to fit progress bar
        int ph=visRows*rowH+64;
        float bgF=smoothstep(clamp01(bp*4.f));
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r,3,6,4,(Uint8)(alpha*bgF*0.97f));
        SDL_Rect bg={ox,oy,ow,ph};SDL_RenderFillRect(r,&bg);
        SDL_Color bc=thsv(hue,0.9f,1.f);bc.a=(Uint8)(alpha*0.85f);
        drawBuildingBorder(r,ox,oy,ow,ph,smoothstep(clamp01(bp/0.3f)),t,bc);

        // Header
        int done=0,fail=0,total=(int)g_spotTracks.size();
        {
            std::lock_guard<std::mutex> lk(g_spotTrackMtx);
            for(auto& tr:g_spotTracks){
                if(tr.status=="done")++done;
                if(tr.status=="failed")++fail;
            }
        }
        std::string hdr="DOWNLOADING  "+std::to_string(done)+"/"+
                        std::to_string(total)+" tracks";
        if(!g_spotDlRunning&&g_spotState==SpotState::Done)
            hdr="IMPORT COMPLETE  "+std::to_string(done)+" ok  "+
                std::to_string(fail)+" failed";
        drawText(r,g_font,hdr,ox+10,oy+6,{140,255,160,alpha},false);

        // Overall progress bar
        float prog=(total>0)?(float)done/total:0.f;
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
        SDL_SetRenderDrawColor(r,20,60,30,alpha);
        SDL_Rect pbBg={ox+10,oy+26,ow-20,6};SDL_RenderFillRect(r,&pbBg);
        SDL_Color pfc=thsv(hue,0.9f,1.f);pfc.a=alpha;
        SDL_SetRenderDrawColor(r,pfc.r,pfc.g,pfc.b,pfc.a);
        SDL_Rect pbFill={ox+10,oy+26,(int)((ow-20)*prog),6};SDL_RenderFillRect(r,&pbFill);

        // Track list
        int dlIdx=g_spotDlIdx.load();
        int scrollTo=std::max(0,dlIdx-2);
        int listY=oy+40;
        int visEnd=std::min(scrollTo+visRows,(int)g_spotTracks.size());
        for(int i=scrollTo;i<visEnd;++i){
            int ry=listY+(i-scrollTo)*rowH;
            std::string status,label;
            {
                std::lock_guard<std::mutex> lk(g_spotTrackMtx);
                status=g_spotTracks[i].status;
                label=g_spotTracks[i].searchQuery;
            }
            bool isActive=(status=="downloading");
            bool isDone  =(status=="done");
            bool isFailed=(status=="failed");

            // Row background
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_Color bg2={8,16,10,(Uint8)(alpha*(isActive?0.6f:0.3f))};
            SDL_SetRenderDrawColor(r,bg2.r,bg2.g,bg2.b,bg2.a);
            SDL_Rect row={ox+4,ry,ow-8,rowH-2};SDL_RenderFillRect(r,&row);

            // ── PER-TRACK PROGRESS BAR ────────────────────────────────────────
            int barX=ox+8, barY=ry+rowH-10, barW=ow-16, barH=4;
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,12,30,16,(Uint8)(alpha*0.8f));
            SDL_Rect barBg={barX,barY,barW,barH};SDL_RenderFillRect(r,&barBg);

            float trackProg=0.f;
            SDL_Color barCol={0,0,0,0};
            if(isDone){
                trackProg=1.f;
                barCol=thsv(hue,0.9f,1.f);barCol.a=(Uint8)(alpha*0.9f);
            } else if(isFailed){
                trackProg=1.f;
                barCol={255,60,60,alpha};
            } else if(isActive){
                // Animated fake progress — crawls to 90% then stalls
                // We use a sine-eased time so it looks organic
                float elapsed=fmod(t,30.f)/30.f; // resets every 30s
                trackProg=0.9f*(1.f-expf(-elapsed*4.f));
                barCol=thsv(fmod(hue+t*20.f,360.f),0.9f,1.f);barCol.a=(Uint8)(alpha*0.9f);
            } else if(status=="waiting"){
                trackProg=0.f;
                barCol={40,80,50,(Uint8)(alpha*0.5f)};
            }

            // Fill bar
            SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
            if(trackProg>0.f){
                // Gradient fill
                int fillW=(int)(barW*trackProg);
                int strips=std::max(1,fillW/4);
                for(int s=0;s<strips;++s){
                    float sf=(float)s/strips;
                    SDL_Color sc=barCol;
                    sc.a=(Uint8)(barCol.a*(0.6f+sf*0.4f));
                    SDL_SetRenderDrawColor(r,sc.r,sc.g,sc.b,sc.a);
                    SDL_Rect stripe={barX+(int)(sf*fillW),barY,fillW/strips+1,barH};
                    SDL_RenderFillRect(r,&stripe);
                }
                // Shimmer on active track
                if(isActive){
                    float shim=fmod(t*0.8f,1.f)*(float)fillW;
                    for(int g2=0;g2<4;++g2){
                        SDL_Color gc={255,255,255,(Uint8)(alpha*(1.f-g2*0.25f)*0.4f)};
                        SDL_SetRenderDrawColor(r,gc.r,gc.g,gc.b,gc.a);
                        SDL_Rect shimR={barX+(int)(shim)-g2*2,barY,3,barH};
                        SDL_RenderFillRect(r,&shimR);
                    }
                }
            }

            // Status icon + label
            std::string icon="  ";
            SDL_Color tc={120,140,130,alpha};
            if(isActive){
                const char* spin[]={"/ ","- ","\\ ","| "};
                icon=spin[(int)(t*6.f)%4];
                tc={140,255,160,alpha};
            } else if(isDone){
                icon="OK"; tc={80,220,100,alpha};
            } else if(isFailed){
                icon="XX"; tc={255,80,80,alpha};
            } else {
                icon=".."; tc={60,80,70,alpha};
            }
            drawText(r,g_fontSm,icon+" "+label,ox+10,ry+4,tc);
        }
        if(g_spotState==SpotState::Done)
            drawText(r,g_fontSm,"[ ESC ] close",ox+10,oy+ph-18,
                     {60,120,70,alpha});
    }
}

// ── SPOTIFY ACTIONS CALLED FROM EVENT LOOP ───────────────────────────────────
void spotOpenCredentials(){
    loadSpotifyConfig();
    strncpy(g_spotIdBuf,g_spotClientId.c_str(),sizeof(g_spotIdBuf)-1);
    strncpy(g_spotSecretBuf,g_spotClientSecret.c_str(),sizeof(g_spotSecretBuf)-1);
    g_spotStatusMsg="";
    g_spotFocusId=true;
    g_spotState=SpotState::CredentialsDialog;
    g_spotBuildT=0.f; g_spotBuilding=true;
    SDL_StartTextInput();
}

void spotStartAuth(){
    g_spotClientId=std::string(g_spotIdBuf);
    g_spotClientSecret=std::string(g_spotSecretBuf);
    if(g_spotClientId.empty()||g_spotClientSecret.empty()){
        g_spotStatusMsg="ERROR: Both fields required"; return;
    }
    saveSpotifyConfig();
    // Start local OAuth capture server
    g_spotCodeReady=false;
    g_spotAuthCode="";
    g_spotAccessToken="";
    std::thread(oauthServerThread).detach();
    // Open browser
    std::string url="https://accounts.spotify.com/authorize?"
        "client_id="+g_spotClientId+
        "&response_type=code"
        "&redirect_uri="+urlEncode("http://127.0.0.1:8888/callback")+
        "&scope="+urlEncode("playlist-read-private playlist-read-collaborative");
    ShellExecuteA(nullptr,"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    g_spotState=SpotState::WaitingAuth;
    g_spotBuildT=0.f; g_spotBuilding=true;
    g_spotWaitBuildT=0.f;
    g_spotFakeLoadT=0.f; g_spotFakeLoading=false;
    SDL_StopTextInput();
}

void spotSelectPlaylist(int idx){
    if(idx<0||idx>=(int)g_spotPlaylists.size()) return;
    auto& pl=g_spotPlaylists[idx];
    g_spotStatusMsg="Fetching tracks...";
    g_spotDlPlaylistName=pl.name;
    // Build clean folder name
    std::string cleanName;
    for(unsigned char c:pl.name){
        if(c>=32&&c<128&&c!='/'&&c!='*'&&c!='?'&&c!='"'&&
           c!='<'&&c!='>'&&c!='|'&&c!=':') cleanName+=c;
    }
    if(cleanName.empty()||cleanName.find_first_not_of(' ')==std::string::npos)
        cleanName="Spotify_Playlist";
    while(!cleanName.empty()&&cleanName.back()==' ') cleanName.pop_back();
    std::string outDir=getExeDir()+"downloads\\"+cleanName;
    std::filesystem::create_directories(outDir);
    // Move to downloading state immediately — fetch + download on background thread
    g_spotState=SpotState::Downloading;
    g_spotBuildT=0.f; g_spotBuilding=true;
    g_spotDlIdx=0;
    g_spotDlRunning=true;
    std::string plId=pl.id; int plTotal=pl.total;
    if(g_spotDlThread.joinable()) g_spotDlThread.join();
    g_spotDlThread=std::thread([plId,plTotal,outDir](){
        // Fetch tracks first, then download
        spotFetchTracks(plId,plTotal);
        if(g_spotTracks.empty()){
            g_spotStatusMsg="ERROR: No tracks found";
            g_spotDlRunning=false;
            g_spotDlIdx=0;
            return;
        }
        downloadThread(outDir);
    });
}

// ── PHOTOSENSITIVITY WARNING ──────────────────────────────────────────────────
void drawWarning(SDL_Renderer* r, int ww, int wh, float buildT, float alpha, float t){
    if(alpha <= 0.f) return;
float bp = clamp01(buildT);
    Uint8 a = (Uint8)(alpha * 255.f);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)(alpha * 200.f));
    SDL_RenderFillRect(r, nullptr);
int ox = ww/2-260, oy = wh/2-140, ow = 520, oh = 280;
    float scanP = clamp01((bp-0.05f)/0.40f);
    float revH  = scanP * oh;
    if(revH > 0.f){
        SDL_SetRenderDrawColor(r, 4, 3, 2, (Uint8)(alpha*0.97f*255.f));
        SDL_Rect bg={ox,oy,ow,(int)revH}; SDL_RenderFillRect(r,&bg);
    }
float hue = fmod(t*8.f + 20.f, 360.f);
    drawScanSweep(r, ox, oy, ow, oh, scanP, t, hue);
SDL_Color bc = thsv(30.f, 0.9f, 1.f); bc.a = (Uint8)(alpha*0.95f*255.f);
    drawBuildingBorder(r, ox, oy, ow, oh, smoothstep(clamp01(bp/0.35f)), t, bc);
if(bp > 0.30f){
        float f = smoothstep(clamp01((bp-0.30f)/0.20f));
        // Warning symbol
        float tp = clamp01((bp-0.30f)/0.18f);
        std::string hdr = buildTypeOn("⚠  PHOTOSENSITIVITY WARNING", tp, t);
        SDL_Color tg = thsv(30.f,0.6f,1.f); tg.a=(Uint8)(alpha*f*0.4f*255.f);
        for(int g=0;g<3;++g) drawText(r,g_font,hdr,ox+20+g,oy+16,tg,false);
        SDL_Color tc = {255,200,80,(Uint8)(alpha*f*255.f)};
        drawText(r, g_font, hdr, ox+20, oy+16, tc);

        // Separator
        SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_ADD);
        SDL_Color sep=thsv(30.f,0.8f,1.f); sep.a=(Uint8)(alpha*f*0.5f*255.f);
        SDL_SetRenderDrawColor(r,sep.r,sep.g,sep.b,sep.a);
        SDL_Rect sl={ox+12,oy+38,ow-24,1}; SDL_RenderFillRect(r,&sl);
    }

    // Warning text lines — stagger in
    struct WLine{ float start; const char* text; };
    static const WLine lines[]={
        {0.42f, "This application contains rapidly flashing lights,"},
        {0.50f, "strobing effects, and high-contrast patterns that"},
        {0.57f, "may trigger seizures in people with photosensitive"},
        {0.63f, "epilepsy or other photosensitive conditions."},
        {0.72f, "If you or anyone around you has a history of"},
        {0.78f, "epilepsy or seizures, please consult a doctor"},
        {0.83f, "before using this application."},
    };
    for(int i=0;i<7;++i){
        if(bp <= lines[i].start) continue;
        float lf = smoothstep(clamp01((bp-lines[i].start)/0.10f));
        int lx = (int)(ox+20+(1.f-lf)*20.f);
        Uint8 la = (Uint8)(alpha*lf*255.f);
        bool isBold = (i==3||i==6); // last lines slightly brighter
        SDL_Color lc = isBold ?
            SDL_Color{220,200,160,la} : SDL_Color{170,155,120,la};
        drawText(r, g_fontSm, lines[i].text, lx, oy+52+i*22, lc);
    }

    // Dismiss prompt — pulses
    if(bp > 0.88f){
        float f = smoothstep(clamp01((bp-0.88f)/0.12f));
        float pulse = sinf(t*3.f)*0.15f+0.85f;
        Uint8 pa = (Uint8)(alpha*f*pulse*255.f);
        SDL_Color pc = {255,180,60,pa};
        drawText(r, g_font, "[ PRESS ANY KEY OR CLICK TO CONTINUE ]",
                 ww/2, oy+oh-24, pc, true);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════════════
// ── RENDER FRAME FUNCTION — called from render thread ────────────────────────
void renderFrame(SDL_Renderer* ren, RenderParams& rp){
    // Safety: bail if renderer or textures are invalid
    if(!ren || !rp.fbA || !rp.fbB || !rp.layerTex) return;
    int ww=rp.ww, wh=rp.wh;
    float t=rp.t, cx=rp.cx, cy=rp.cy, maxR=rp.maxR;
    float sBass=rp.sBass, sMid=rp.sMid, sHigh=rp.sHigh, sAll=rp.sAll;
    float sSub=rp.sSub, sLM=rp.sLM, sHM=rp.sHM, rB=rp.rB;
    int frame=rp.frame;
    auto& spec=rp.spec;
    SDL_Texture* fbA=rp.fbA;
    SDL_Texture* fbB=rp.fbB;
    SDL_Texture* layerTex=rp.layerTex;

    SDL_Texture* prevFb=(frame%2==0)?fbA:fbB;
    SDL_Texture* curFb =(frame%2==0)?fbB:fbA;

    // Render layer effects — skip entirely while warning is showing
    if(!g_warnActive || g_warnAlpha < 0.01f){
    if(!layerTex||!fbA||!fbB){ /* skip render */ }
    SDL_SetRenderTarget(ren,layerTex);
    SDL_SetRenderDrawColor(ren,0,0,0,255);SDL_RenderClear(ren);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_ADD);

#define EV(id) if(g_theme.effVisible[id])
    { std::lock_guard<std::mutex> lk(g_particleMtx);
    EV(EFF_MEMBRANE)  drawSubBassMembrane(ren,cx,cy,t,sSub,sBass,maxR,ww,wh);
    EV(EFF_SINE)      drawSineWaves(ren,cx,cy,t,sLM,sBass,sAll,ww,wh);
    float playScale=(g_music&&!g_paused)?0.22f:1.0f;
    EV(EFF_TUNNEL)    drawTunnel(ren,cx,cy,t,sBass*playScale,sAll*playScale,maxR);
    EV(EFF_WORMHOLE)  drawWormhole(ren,cx,cy,t,sBass*playScale,sMid*playScale,sAll*playScale,maxR);
    EV(EFF_LISSAJOUS) drawMegaLissajous(ren,cx,cy,t,sMid,sLM,sAll,maxR);
    EV(EFF_FRACTAL)   drawFractalBurst(ren,cx,cy,t,sHM,sMid,maxR);
    EV(EFF_NEURAL)    drawNeuralNet(ren,sHigh,sHM,sBass);
    EV(EFF_SHOCKWAVE) drawShockwaves(ren,cx,cy,sBass);
    EV(EFF_CUBE)      drawBeatCube(ren,cx,cy,t,sBass,sAll);
    EV(EFF_SPECTRUM)  drawSpectrumRing(ren,cx,cy,t,sBass,sMid,sHigh,sAll,spec,maxR);
    EV(EFF_GEOSHAPES) drawGeoShapes(ren,sBass,sMid,sAll);
    EV(EFF_KALEIDOSCOPE) drawKaleidoscope(ren,cx,cy,sBass,sMid,sHigh,sAll,t,spec,maxR);
    EV(EFF_RIBBON)   drawRibbonTornado(ren,cx,cy,t,sBass,sMid,sAll,maxR);
    EV(EFF_AURORA)   drawAurora(ren,cx,cy,t,sMid,sLM,sAll,ww,wh);
    EV(EFF_HYPNO)    drawHypnoSpiral(ren,cx,cy,t,sMid,sBass,sAll,maxR);
    EV(EFF_RAIN)     drawRain(ren,sHigh,sAll,wh);
    EV(EFF_GALAXY)   drawGalaxy(ren,cx,cy,t,sBass,sMid,sHigh,maxR);
    EV(EFF_LIGHTNING) drawLightning(ren,sBass,sHigh);
    EV(EFF_DNA)      drawDNAHelix(ren,cx,cy,t,sBass,sMid,sHigh,sAll,spec,maxR);
    EV(EFF_NOVA)     drawNova(ren,cx,cy,sAll,sBass,sMid);
    EV(EFF_TENDRILS) drawTendrils(ren,sBass,sMid,sAll);
    EV(EFF_CYMATICS) drawCymatics(ren,cx,cy,t,sBass,sMid,sHigh,sAll,maxR);
    EV(EFF_VORTEX)   drawVortex(ren,cx,cy,sBass,sAll);
    EV(EFF_GEOMETRY) if(g_perfFrame%2==0) drawSonicGeometry(ren,cx,cy,t,sBass,sMid,sHigh,sAll,maxR);
    EV(EFF_FILAMENTS) drawFilaments(ren,sAll,sHigh,sMid);
    EV(EFF_GRAVITY)  if(g_perfFrame%2==0) drawGravityLens(ren,cx,cy,t,sBass,sMid,sAll,maxR);
    } // release particle lock
#undef EV
    // Custom user effects
    for(int i=0;i<g_numCustomEffects;++i)
        drawCustomEffect(ren,i,cx,cy,t,sBass,sMid,sHigh,sAll,maxR,spec);
    SDL_SetRenderTarget(ren,nullptr);

    // Feedback pass
    SDL_SetRenderTarget(ren,curFb);
    SDL_SetRenderDrawColor(ren,0,0,1,255);SDL_RenderClear(ren);
    float feedbackZoom =1.014f+sAll*0.005f+sBass*0.004f;
    float feedbackRot  =0.004f*(1.f+sAll*0.4f);
    float feedbackAlpha=(0.82f-sAll*0.08f)*std::min(1.f,g_theme.intensity*0.9f+0.1f);
    float hueShift = fmod(t*15.f + g_theme.hueOffset, 360.f);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_ADD);
    blitFeedback(ren,prevFb,cx,cy,feedbackZoom,feedbackRot,feedbackAlpha,hueShift,ww,wh);
    SDL_SetTextureBlendMode(layerTex,SDL_BLENDMODE_ADD);
    SDL_SetTextureAlphaMod(layerTex,(Uint8)(255.f*std::min(1.f,themeIntensity())));
    SDL_SetTextureColorMod(layerTex,255,255,255);
    SDL_RenderCopy(ren,layerTex,nullptr,nullptr);
    SDL_SetRenderTarget(ren,nullptr);
    } // end skip-if-warning
    

    // Composite to screen
    SDL_Color bg2=themeBg();
    SDL_SetRenderDrawColor(ren,bg2.r,bg2.g,bg2.b,255);SDL_RenderClear(ren);
    // Background gradient
    if(g_theme.bgGradient && g_theme.bgVal > 0.001f){
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_ADD);
        float ang=g_theme.bgGradientAngle*(float)M_PI/180.f;
        for(int y2=0;y2<wh;y2+=2){
            float fy=(float)y2/wh;
            float proj=fy*sinf(ang);
            float h2=fmod(g_theme.bgHue1*(1.f-proj)+g_theme.bgHue2*proj,360.f);
            SDL_Color c=thsv(h2,g_theme.bgSat,g_theme.bgVal*15.f);
            c.a=180;
            SDL_SetRenderDrawColor(ren,c.r,c.g,c.b,c.a);
            SDL_Rect row={0,y2,ww,2}; SDL_RenderFillRect(ren,&row);
        }
    }
    if(!g_warnActive){
        if(rB>0.6f){
            SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_ADD);
            SDL_SetRenderDrawColor(ren,6,0,18,(Uint8)std::min((rB-.6f)*50.f,30.f));
            SDL_RenderFillRect(ren,nullptr);
        }
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_ADD);
        SDL_SetTextureBlendMode(curFb,SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaMod(curFb,255);SDL_SetTextureColorMod(curFb,255,255,255);
        SDL_RenderCopy(ren,curFb,nullptr,nullptr);
    }

    // UI idle fade — computed from g_idleTimer updated by main thread
    float uiA=g_uiVisible?(g_idleTimer>5.f?std::max(0.f,1.f-(g_idleTimer-5.f)*0.5f):1.f):0.f;
    // Always keep the menu button barely visible even when faded
    float menuBtnMinAlpha = 0.25f;
    float menuUiAlpha = std::max(uiA, menuBtnMinAlpha);
    Uint8 uiAlpha=(Uint8)(uiA*220);
    Uint8 menuAlpha=(Uint8)(menuUiAlpha*220);

    // Draw panels
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    if(uiAlpha>0 && !g_warnActive){
        if(g_panel==UIPanel::Library)   drawLibraryPanel(ren,uiAlpha,g_panelBuildT,t);
        if(g_panel==UIPanel::Playlists) drawPlaylistPanel(ren,uiAlpha,g_panelBuildT,t);
        drawNowPlayingBar(ren,ww,wh,uiAlpha,t,sBass,sAll,spec);

        // ── WAVEFORM HOVER POPUP ─────────────────────────────────────────────
        if(g_wfHovering && (g_wfCache.ready||g_wfCache.building) && uiAlpha>0){
            int panelH=52, barH=10;
            int barY=wh-panelH/2-barH/2;
            int pbX=120, pbW=ww-260;

            // Popup dimensions
            int popW=320, popH=80;
            // While dragging, pin popup to seek position not raw mouse X
            int popAnchorX = g_seekDragging ? (pbX+(int)(g_seekPreview*pbW)) : g_wfMouseX;
            int popX=std::clamp(popAnchorX-popW/2, 4, ww-popW-4);
            int popY=barY-popH-12;

            // Dark bg with neon border
            float nHue=g_theme.uiHue;
            SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren,2,4,8,230);
            SDL_Rect popBg={popX,popY,popW,popH};
            SDL_RenderFillRect(ren,&popBg);

            // Border glow
            SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_ADD);
            for(int g=3;g>=1;--g){
                SDL_Color gc=hsv(nHue,0.8f,1.f);
                gc.a=(Uint8)(uiAlpha*(1.f-(float)g/3.f*0.5f)*0.4f);
                SDL_SetRenderDrawColor(ren,gc.r,gc.g,gc.b,gc.a);
                SDL_Rect gb={popX-g,popY-g,popW+g*2,popH+g*2};
                SDL_RenderDrawRect(ren,&gb);
            }
            SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
            SDL_Color bdrC=hsv(nHue,0.5f,1.f); bdrC.a=(Uint8)(uiAlpha*0.8f);
            SDL_SetRenderDrawColor(ren,bdrC.r,bdrC.g,bdrC.b,bdrC.a);
            SDL_RenderDrawRect(ren,&popBg);

            // Clip to popup interior
            SDL_Rect clip={popX+1,popY+1,popW-2,popH-2};
            SDL_RenderSetClipRect(ren,&clip);

            // Loading spinner while background thread runs
            if(g_wfCache.building && !g_wfCache.ready){
                SDL_Color lc=hsv(nHue,0.7f,1.f); lc.a=(Uint8)(uiAlpha*0.7f);
                drawText(ren,g_fontSm,"scanning...",popX+popW/2,popY+popH/2-6,lc,true);
                // Animated dots
                int dotCount=(int)(t*3.f)%4;
                for(int d=0;d<dotCount;++d){
                    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_ADD);
                    SDL_Color dc=hsv(nHue,0.8f,1.f); dc.a=(Uint8)(uiAlpha*0.6f);
                    SDL_SetRenderDrawColor(ren,dc.r,dc.g,dc.b,dc.a);
                    SDL_Rect dot={popX+popW/2-20+d*14,popY+popH/2+8,6,6};
                    SDL_RenderFillRect(ren,&dot);
                }
                SDL_RenderSetClipRect(ren,nullptr);
            }

            // Draw waveform — in a block so the loading goto skips cleanly
            if(g_wfCache.ready){
                int wfPad=6;
                int wfW=popW-wfPad*2;
                int wfMidY=popY+popH/2;
                int wfMaxH=(popH/2)-6;
                SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_ADD);
                for(int col=0;col<WF_COLS;++col){
                    float peak=g_wfCache.peaks[col];
                    int x=popX+wfPad+(int)((float)col/WF_COLS*wfW);
                    int h=std::max(1,(int)(peak*wfMaxH));
                    float hue=fmod((float)col/WF_COLS*300.f+nHue,360.f);
                    SDL_Color wc=hsv(hue,0.9f,1.f);
                    wc.a=(Uint8)(uiAlpha*0.85f);
                    SDL_SetRenderDrawColor(ren,wc.r,wc.g,wc.b,wc.a);
                    SDL_Rect upper={x,wfMidY-h,1,h};
                    SDL_Rect lower={x,wfMidY,1,h};
                    SDL_RenderFillRect(ren,&upper);
                    SDL_RenderFillRect(ren,&lower);
                }
                // Playhead line
                float prog2=clamp01((float)(Mix_GetMusicPosition(g_music))/(float)(Mix_MusicDuration(g_music)));
                int wfPad2=6, wfW2=popW-wfPad2*2;
                int phX=popX+wfPad2+(int)(prog2*wfW2);
                SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren,255,255,255,200);
                SDL_Rect ph={phX,popY+4,1,popH-8};
                SDL_RenderFillRect(ren,&ph);
                // Hover seek line + time label
                float hoverProg=clamp01((float)(g_wfMouseX-pbX)/pbW);
                int hpX=popX+wfPad2+(int)(hoverProg*wfW2);
                SDL_Color hpC=hsv(nHue,0.5f,1.f); hpC.a=160;
                SDL_SetRenderDrawColor(ren,hpC.r,hpC.g,hpC.b,hpC.a);
                SDL_Rect hp={hpX,popY+4,1,popH-8};
                SDL_RenderFillRect(ren,&hp);
                double hoverSecs=hoverProg*Mix_MusicDuration(g_music);
                int hm=(int)hoverSecs/60, hs2=(int)hoverSecs%60;
                char htbuf[16]; snprintf(htbuf,16,"%d:%02d",hm,hs2);
                SDL_Color htC=hsv(nHue,0.3f,1.f); htC.a=(Uint8)(uiAlpha*0.9f);
                drawText(ren,g_fontSm,htbuf,popX+popW/2,popY+4,htC,true);
            }

            SDL_RenderSetClipRect(ren,nullptr);

            // Arrow pointing down to bar
            SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren,bdrC.r,bdrC.g,bdrC.b,bdrC.a);
            for(int i=0;i<6;++i){
                SDL_Rect arrow={g_wfMouseX-i,popY+popH+i,i*2+1,1};
                SDL_RenderFillRect(ren,&arrow);
            }
        }
        drawNamingOverlay(ren,ww,wh,uiAlpha,g_namingBuildT,t);
    }
    if(g_spotState!=SpotState::Idle)
        drawSpotifyUI(ren,ww,wh,220,g_spotBuildT,t);
    if(g_themePanelT > 0.f)
        drawThemePanel(ren,ww,wh,g_themePanelT,t);
    if(!g_warnActive)
        drawRadialMenu(ren, t, menuUiAlpha, sBass, sAll);
    // File picker indicator
    if(g_filePickerOpen){
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        SDL_Color bc=thsv(fmod(t*60.f,360.f),0.8f,1.f); bc.a=200;
        drawText(ren,g_fontSm,"  BROWSING FOR FILE...",8,8,bc);
    }
    if(g_warnAlpha > 0.f)
        drawWarning(ren, ww, wh, g_warnBuildT, g_warnAlpha, t);

    SDL_RenderPresent(ren);
}

int main(int argc,char** argv){
    g_exeDir=getExeDir();
    SetUnhandledExceptionFilter(crashHandler);
    { std::ofstream f(g_exeDir+"crash.log",std::ios::app);
      auto now=std::chrono::system_clock::now();
      auto t=std::chrono::system_clock::to_time_t(now);
      f<<"=== SESSION START "<<std::ctime(&t); }
    // Write a startup log — if the exe crashes silently, check startup.log
initPresets();
g_theme    = PRESET_THEMES[0];
    g_themePrev= PRESET_THEMES[0];
    g_themeBlend=1.f;

if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO)<0){std::cerr<<"SDL_Init failed\n";return 1;}
    initRenderGeometry();
TTF_Init();
initFonts();
SDL_Window* win=SDL_CreateWindow(
        "ACID KALEIDO  |  Click top-left button for menu  |  Esc=Quit",
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        1280,720,SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
SDL_Renderer* ren=SDL_CreateRenderer(win,-1,
        SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    installDragHook(win);
int mixFlags=MIX_INIT_MP3|MIX_INIT_OGG|MIX_INIT_FLAC|MIX_INIT_MOD;
    int mixInited=Mix_Init(mixFlags);
    {
        std::ofstream f(getExeDir()+"audio_debug.txt");
        f<<"Mix_Init requested: "<<mixFlags<<"\n";
        f<<"Mix_Init got:       "<<mixInited<<"\n";
        f<<"MP3  supported: "<<((mixInited&MIX_INIT_MP3) ?"YES":"NO")<<"\n";
        f<<"OGG  supported: "<<((mixInited&MIX_INIT_OGG) ?"YES":"NO")<<"\n";
        f<<"FLAC supported: "<<((mixInited&MIX_INIT_FLAC)?"YES":"NO")<<"\n";
        if(!(mixInited&MIX_INIT_MP3))
            f<<"MP3 error: "<<Mix_GetError()<<"\n";
    }
if(Mix_OpenAudio(44100,AUDIO_F32SYS,2,1024)<0){
        std::ofstream f(getExeDir()+"audio_debug.txt",std::ios::app);
        f<<"Mix_OpenAudio failed: "<<Mix_GetError()<<"\n";
        return 1;
    }
Mix_AllocateChannels(2);g_pcmMutex=SDL_CreateMutex();
    Mix_SetPostMix(audioCaptureCallback,nullptr);
    loadLibrary();
loadCustomEffects();
loadThemes();
Mix_HookMusicFinished([](){
        SDL_Event e; e.type=SDL_USEREVENT; e.user.code=1;
        SDL_PushEvent(&e);
    });
std::vector<double> fftIn(FFT_N,0.0);
    fftw_complex* fftOut=fftw_alloc_complex(FREQ_BINS+1);
    fftw_plan plan=fftw_plan_dft_r2c_1d(FFT_N,fftIn.data(),fftOut,FFTW_ESTIMATE);
    std::vector<double> hannWin(FFT_N);
    for(int i=0;i<FFT_N;++i)hannWin[i]=0.5*(1.0-cos(2.0*M_PI*i/(FFT_N-1)));
    std::vector<float> spec(FREQ_BINS,0.f);
std::mt19937 rng(std::random_device{}());
    int ww,wh;SDL_GetRendererOutputSize(ren,&ww,&wh);
    float maxR=sqrtf((ww*.5f)*(ww*.5f)+(wh*.5f)*(wh*.5f));
    initKNodes(maxR,rng);
auto makeT=[&]()->SDL_Texture*{
        SDL_Texture* tx=SDL_CreateTexture(ren,SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_TARGET,ww,wh);
        SDL_SetTextureBlendMode(tx,SDL_BLENDMODE_BLEND);
        SDL_SetRenderTarget(ren,tx);SDL_SetRenderDrawColor(ren,0,0,0,255);
        SDL_RenderClear(ren);SDL_SetRenderTarget(ren,nullptr);return tx;
    };
    SDL_Texture* fbA=makeT(),*fbB=makeT();
    SDL_Texture* layerTex=makeT();
if(argc>=2)loadAndPlay(argv[1]);

    float t=0.f;bool running=true;
    Uint32 lastTick=SDL_GetTicks();
    float sBass=0,sMid=0,sHigh=0,sAll=0,sSub=0,sLM=0,sHM=0;
    int frame=0;

    // ── LAUNCH RENDER THREAD ─────────────────────────────────────────────────
    // Render thread owns all SDL drawing — main thread only does logic + events
    // This keeps the window responsive during drag (Windows modal loop blocks
    // main thread but render thread keeps drawing)
    g_renderRunning = true;
    g_rp.fbA=fbA; g_rp.fbB=fbB; g_rp.layerTex=layerTex;
    g_rp.spec.resize(FREQ_BINS, 0.f);
    std::thread renderThread([&](){
        try {
        while(g_renderRunning){
            {
                std::unique_lock<std::mutex> lk(g_renderCvMtx);
                g_renderCv.wait_for(lk, std::chrono::milliseconds(32),
                                    []{ return g_renderReady.load(); });
                g_renderReady = false;
            }
            if(!g_renderRunning) break;
            // Hold resize lock while rendering — main thread waits here during resize
            std::lock_guard<std::mutex> rzlk(g_resizeMtx);
            if(!g_renderRunning) break;
            RenderParams rp;
            { std::lock_guard<std::mutex> lk(g_renderMtx); rp = g_rp; }
            // Skip frame if textures are null
            if(!rp.fbA || !rp.fbB || !rp.layerTex){ logErr("WARN: null texture in render"); continue; }
            renderFrame(ren, rp);
            ++g_perfFrame;
        }
        } catch(const std::exception& e){
            logErr(std::string("RENDER THREAD EXCEPTION: ")+e.what());
        } catch(...){
            logErr("RENDER THREAD UNKNOWN EXCEPTION");
        }
    });

    while(running){
        Uint32 now=SDL_GetTicks();
        float dt=std::min((now-lastTick)/1000.f,0.033f);
        lastTick=now;t+=dt;

        SDL_GetRendererOutputSize(ren,&ww,&wh);

        SDL_Event ev;
        while(SDL_PollEvent(&ev)){
            if(ev.type==SDL_QUIT)running=false;
            // Dismiss photosensitivity warning on any input
            if(g_warnActive && g_warnBuildT > 0.88f){
                if(ev.type==SDL_KEYDOWN||
                   (ev.type==SDL_MOUSEBUTTONDOWN&&ev.button.button==SDL_BUTTON_LEFT))
                    g_warnActive=false;
            }
            if(ev.type==SDL_USEREVENT&&ev.user.code==1){
                // Guard against rapid-fire: ignore if track just changed < 2s ago
                if(SDL_GetTicks()-g_lastTrackChange > 2000)
                    playNext();
            }

            // Drag timer wake-up (code=99) — just keeps event loop alive during modal drag
            // No action needed; the main loop will run a frame naturally
            // Maximize/restore (code=98) and snap (code=97) — handled on main thread
            // so render thread isn't mid-frame when textures get resized
            if(ev.type==SDL_USEREVENT&&ev.user.code==98){
                bool doMax=(intptr_t)ev.user.data1;
                if(doMax) SDL_MaximizeWindow(win);
                else      SDL_RestoreWindow(win);
            }
            if(ev.type==SDL_USEREVENT&&ev.user.code==97){
                // Half-screen snap
                int side=(intptr_t)ev.user.data1;
                SDL_DisplayMode dm{}; SDL_GetCurrentDisplayMode(0,&dm);
                if(side==0) SDL_SetWindowPosition(win,0,0),SDL_SetWindowSize(win,dm.w/2,dm.h);
                else        SDL_SetWindowPosition(win,dm.w/2,0),SDL_SetWindowSize(win,dm.w/2,dm.h);
            }

            // File picker result (async) — code=3
            if(ev.type==SDL_USEREVENT&&ev.user.code==3){
                std::string* res=static_cast<std::string*>(ev.user.data1);
                if(res){
                    if(!res->empty()){
                        addToLibrary(*res);
                        g_currentSong=(int)g_library.size()-1;
                        loadAndPlay(*res);
                    }
                    delete res;
                }
            }
            // Download complete — import MP3s into library on main thread (thread safe)
            if(ev.type==SDL_USEREVENT&&ev.user.code==2){
                std::string* outDirPtr=static_cast<std::string*>(ev.user.data1);
                if(outDirPtr){
                    std::string outDir=*outDirPtr;
                    delete outDirPtr;
                    // scan folder for mp3s
                    Playlist newPL; newPL.name=g_spotDlPlaylistName;
                    if(fs::exists(outDir)){
                        std::error_code ec;
                        for(auto& entry:fs::directory_iterator(outDir,ec)){
                            if(ec){ec.clear();continue;}
                            auto ext=entry.path().extension().string();
                            std::string extLo=ext;
                            for(auto& c:extLo) c=(char)tolower((unsigned char)c);
                            if(extLo==".mp3"){
                                std::string p=entry.path().string();
                                addToLibrary(p);
                                for(int i=0;i<(int)g_library.size();++i){
                                    if(g_library[i].path==p){
                                        bool already=false;
                                        for(int idx:newPL.songIndices)
                                            if(idx==i){already=true;break;}
                                        if(!already) newPL.songIndices.push_back(i);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if(!newPL.songIndices.empty()){
                        bool replaced=false;
                        for(auto& pl:g_playlists){
                            if(pl.name==newPL.name){pl=newPL;replaced=true;break;}
                        }
                        if(!replaced) g_playlists.push_back(newPL);
                        saveLibrary();
                    }
                }
            }

            if(ev.type==SDL_KEYDOWN){
                // Block all keyboard input while warning is active
                if(g_warnActive) break;
                if(g_namingPL){
                    if(ev.key.keysym.sym==SDLK_RETURN){
                        if(strlen(g_newPLName)>0){
                            Playlist pl; pl.name=std::string(g_newPLName);
                            g_playlists.push_back(pl); saveLibrary();
                        }
                        g_namingPL=false; SDL_StopTextInput();
                    } else if(ev.key.keysym.sym==SDLK_ESCAPE){
                        g_namingPL=false; SDL_StopTextInput();
                    } else if(ev.key.keysym.sym==SDLK_BACKSPACE){
                        int l=strlen(g_newPLName);
                        if(l>0)g_newPLName[l-1]=0;
                    } else if(ev.key.keysym.sym==SDLK_v &&
                              (ev.key.keysym.mod & KMOD_CTRL)){
                        if(SDL_HasClipboardText()){
                            char* clip=SDL_GetClipboardText();
                            if(clip){
                                std::string pasted;
                                for(char c:std::string(clip))
                                    if(c!='\n'&&c!='\r'&&c!='\t') pasted+=c;
                                SDL_free(clip);
                                int l=strlen(g_newPLName);
                                strncat(g_newPLName,pasted.c_str(),
                                        std::max(0,(int)(sizeof(g_newPLName)-l-1)));
                            }
                        }
                    }
                } else if(g_spotState==SpotState::CredentialsDialog){
                    if(ev.key.keysym.sym==SDLK_RETURN){ spotStartAuth(); }
                    else if(ev.key.keysym.sym==SDLK_ESCAPE){
                        g_spotState=SpotState::Idle; SDL_StopTextInput();
                    } else if(ev.key.keysym.sym==SDLK_TAB){
                        g_spotFocusId=!g_spotFocusId;
                    } else if(ev.key.keysym.sym==SDLK_BACKSPACE){
                        if(g_spotFocusId){ int l=strlen(g_spotIdBuf);     if(l>0)g_spotIdBuf[l-1]=0; }
                        else             { int l=strlen(g_spotSecretBuf); if(l>0)g_spotSecretBuf[l-1]=0; }
                    } else if(ev.key.keysym.sym==SDLK_v &&
                              (ev.key.keysym.mod & KMOD_CTRL)){
                        // Ctrl+V — paste from clipboard
                        if(SDL_HasClipboardText()){
                            char* clip=SDL_GetClipboardText();
                            if(clip){
                                // Strip newlines/tabs from pasted text
                                std::string pasted;
                                for(char c:std::string(clip))
                                    if(c!='\n'&&c!='\r'&&c!='\t') pasted+=c;
                                SDL_free(clip);
                                if(g_spotFocusId){
                                    int l=strlen(g_spotIdBuf);
                                    strncat(g_spotIdBuf,pasted.c_str(),
                                            std::max(0,(int)(sizeof(g_spotIdBuf)-l-1)));
                                } else {
                                    int l=strlen(g_spotSecretBuf);
                                    strncat(g_spotSecretBuf,pasted.c_str(),
                                            std::max(0,(int)(sizeof(g_spotSecretBuf)-l-1)));
                                }
                            }
                        }
                    } else if(ev.key.keysym.sym==SDLK_a &&
                              (ev.key.keysym.mod & KMOD_CTRL)){
                        // Ctrl+A — clear the active field
                        if(g_spotFocusId) memset(g_spotIdBuf,0,sizeof(g_spotIdBuf));
                        else              memset(g_spotSecretBuf,0,sizeof(g_spotSecretBuf));
                    }
                } else if(g_spotState==SpotState::WaitingAuth){
                    if(ev.key.keysym.sym==SDLK_ESCAPE) g_spotState=SpotState::Idle;
                } else if(g_spotState==SpotState::PlaylistPicker){
                    if(ev.key.keysym.sym==SDLK_ESCAPE) g_spotState=SpotState::Idle;
                } else if(g_spotState==SpotState::Done){
                    if(ev.key.keysym.sym==SDLK_ESCAPE) g_spotState=SpotState::Idle;
                } else {
                    switch(ev.key.keysym.sym){
                        case SDLK_ESCAPE:
                            if(g_ceFullscreen){ g_ceFullscreen=false; g_editingEffect=-1; saveCustomEffects(); }
                            else if(g_themeEditing){ g_themeEditing=false; g_effScrollOffset=0; }
                            else if(g_themeOpen){ g_themeOpen=false; }
                            else if(g_menuTarget){ toggleMenu(t); }
                            else if(g_panel!=UIPanel::None){ g_panel=UIPanel::None; }
                            else running=false;
                            break;
                        case SDLK_TAB:
                            if(g_themeEditing)
                                g_themeEditorTab=(g_themeEditorTab+1)%3;
                            break;
                        case SDLK_SPACE:
                            if(g_music){g_paused=!g_paused;g_paused?Mix_PauseMusic():Mix_ResumeMusic();}
                            break;
                        case SDLK_RIGHT: playNext(); break;
                        case SDLK_LEFT:  playPrev(); break;
                        case SDLK_m:     toggleMenu(t); break;
                        case SDLK_o:{
                            openFilePickerAsync();
                            break;
                        }
                    }
                }
            }

            if(ev.type==SDL_TEXTINPUT){
                if(g_namingPL){
                    int l=strlen(g_newPLName);
                    if(l<120){ strncat(g_newPLName,ev.text.text,120-l); }
                } else if(g_spotState==SpotState::CredentialsDialog){
                    if(g_spotFocusId){
                        int l=strlen(g_spotIdBuf);
                        if(l<250) strncat(g_spotIdBuf,ev.text.text,250-l);
                    } else {
                        int l=strlen(g_spotSecretBuf);
                        if(l<250) strncat(g_spotSecretBuf,ev.text.text,250-l);
                    }
                }
            }

            if(ev.type==SDL_MOUSEWHEEL){
                if(g_themeEditing&&g_themeEditorTab==1){
                    int wh2=0,ww2=0; SDL_GetRendererOutputSize(ren,&ww2,&wh2);
                    int visRows=(wh2-100-80)/36;
                    g_effScrollOffset=std::clamp(g_effScrollOffset-ev.wheel.y,0,std::max(0,EFF_COUNT-visRows));
                } else if(g_panel==UIPanel::Library)
                    g_libScroll=std::clamp(g_libScroll-ev.wheel.y,0,std::max(0,(int)g_library.size()-VISIBLE_ROWS));
                else if(g_panel==UIPanel::Playlists)
                    g_plScroll=std::clamp(g_plScroll-ev.wheel.y,0,std::max(0,(int)g_playlists.size()-VISIBLE_ROWS+1));
                else if(g_spotState==SpotState::PlaylistPicker)
                    g_spotPlScroll=std::clamp(g_spotPlScroll-ev.wheel.y,0,
                        std::max(0,(int)g_spotPlaylists.size()-VISIBLE_ROWS));
            }

            if(ev.type==SDL_MOUSEMOTION){
                int mx=ev.motion.x,my=ev.motion.y;
                g_menuHover = menuHoverTest(mx, my, g_menuOpen);
                g_hoverSong = libraryClickRow(mx,my);
                // Waveform hover over progress bar
                // Keep waveform visible while seek-dragging even if mouse leaves bar
                g_wfHovering = onProgressBar(mx,my,ww,wh) || g_seekDragging;
                g_wfMouseX   = mx;
                if(g_wfHovering && g_currentSong>=0 && g_currentSong<(int)g_library.size()){
                    if(!g_wfCache.ready || g_wfHoverSong!=g_currentSong){
                        g_wfHoverSong=g_currentSong;
                        buildWaveform(g_library[g_currentSong].path);
                    }
                }
                // Update active slider drag
                if(g_themeDragSlider>=0 && g_themeDragVal){
                    *g_themeDragVal=g_themeDragMin+(g_themeDragMax-g_themeDragMin)*
                                   clamp01((float)(mx-g_themeDragX)/g_themeDragW);
                }
                // Theme panel hover
                if(g_themeOpen){
                    int ox=ww-340,oy=80,ow=330;
                    int cardH=52,cardGap=6,listY=oy+34;
                    int totalCards=NUM_PRESETS+g_numCustomThemes+1;
                    g_themeHover=-1;
                    for(int i=0;i<totalCards;++i){
                        int ry=listY+i*(cardH+cardGap);
                        if(mx>=ox+6&&mx<=ox+ow-6&&my>=ry&&my<=ry+cardH){
                            g_themeHover=i; break;
                        }
                    }
                }
                // Update seek preview while dragging
                if(g_seekDragging)
                    g_seekPreview=clamp01((float)(mx-120)/(ww-260));
                // Spotify playlist hover
                if(g_spotState==SpotState::PlaylistPicker){
                    int ox=10,oy=80,ow=360,listY=oy+30;
                    if(mx>=ox&&mx<=ox+ow){
                        int row=(my-listY)/ROW_H;
                        g_spotPlHover=(row>=0&&row<VISIBLE_ROWS&&
                                       row+g_spotPlScroll<(int)g_spotPlaylists.size())?
                                       row+g_spotPlScroll:-1;
                    } else g_spotPlHover=-1;
                }
                g_idleTimer=0.f; g_uiVisible=true;
            }

            if(ev.type==SDL_MOUSEBUTTONDOWN&&ev.button.button==SDL_BUTTON_LEFT){
                int mx=ev.button.x,my=ev.button.y;
                g_idleTimer=0.f; g_uiVisible=true;

                // Spotify playlist picker click
                if(g_spotState==SpotState::PlaylistPicker){
                    int ox=10,oy=80,ow=360,listY=oy+30;
                    if(mx>=ox&&mx<=ox+ow&&my>=listY){
                        int row=(my-listY)/ROW_H;
                        int idx=row+g_spotPlScroll;
                        if(row>=0&&row<VISIBLE_ROWS&&idx<(int)g_spotPlaylists.size())
                            spotSelectPlaylist(idx);
                    }
                }

                // Fullscreen custom effect editor clicks (highest priority — covers screen)
                if(g_ceFullscreen && g_editingEffect>=0 && g_editingEffect<g_numCustomEffects){
                    auto& ce=g_customEffects[g_editingEffect];
                    int ctrlW2=460, cx2=6, topY2=62;
                    int sw2=ctrlW2-cx2*2-30;
                    int cy2=topY2+6;

                    // Helper — start a drag on any float* using global drag state
                    auto ceDrag=[&](float* val,float mn,float mx2,int tx,int tw){
                        g_themeDragVal=val; g_themeDragMin=mn; g_themeDragMax=mx2;
                        g_themeDragX=tx;   g_themeDragW=tw;  g_themeDragSlider=1;
                        *val=mn+(mx2-mn)*clamp01((float)(mx-tx)/tw);
                    };

                    // Done / Discard buttons
                    if(my>=10&&my<=42&&mx>=ww-130&&mx<=ww-10){
                        g_ceFullscreen=false; g_editingEffect=-1; saveCustomEffects();
                    } else if(my>=10&&my<=42&&mx>=ww-270&&mx<=ww-140){
                        g_ceFullscreen=false; g_editingEffect=-1;
                    } else {
                        // Button rows
                        int bwShape=(ctrlW2-cx2*2)/(int)EffShape::COUNT;
                        int bwMotion=(ctrlW2-cx2*2)/(int)EffMotion::COUNT;
                        int bwColor=(ctrlW2-cx2*2)/(int)EffColor::COUNT;
                        int bwTrig=(ctrlW2-cx2*2)/(int)EffTrigger::COUNT;
                        for(int i=0;i<(int)EffShape::COUNT;++i)
                            if(mx>=cx2+i*bwShape&&mx<=cx2+i*bwShape+bwShape-2&&my>=cy2+14&&my<=cy2+30)
                                ce.shape=(EffShape)i;
                        cy2+=34;
                        for(int i=0;i<(int)EffMotion::COUNT;++i)
                            if(mx>=cx2+i*bwMotion&&mx<=cx2+i*bwMotion+bwMotion-2&&my>=cy2+14&&my<=cy2+30)
                                ce.motion=(EffMotion)i;
                        cy2+=34;
                        for(int i=0;i<(int)EffColor::COUNT;++i)
                            if(mx>=cx2+i*bwColor&&mx<=cx2+i*bwColor+bwColor-2&&my>=cy2+14&&my<=cy2+30)
                                ce.color=(EffColor)i;
                        cy2+=34;
                        for(int i=0;i<(int)EffTrigger::COUNT;++i)
                            if(mx>=cx2+i*bwTrig&&mx<=cx2+i*bwTrig+bwTrig-2&&my>=cy2+14&&my<=cy2+30)
                                ce.trigger=(EffTrigger)i;
                        cy2+=38;

                        // Sliders — two columns
                        float* sv[]={&ce.size,&ce.speed,&ce.density,&ce.alpha,
                                     &ce.innerRadius,&ce.waveFreq,&ce.polySides,
                                     &ce.trailLen,&ce.symmetry,&ce.reactivity,&ce.scatter,
                                     &ce.pulseRate,&ce.colorSpeed};
                        int colsW=(ctrlW2-cx2*2)/2-4;
                        for(int si=0;si<13;++si){
                            int col=si%2, row=si/2;
                            int sx=cx2+col*(colsW+8);
                            int sy2=cy2+row*34+14;
                            if(mx>=sx&&mx<=sx+colsW&&my>=sy2&&my<=sy2+16)
                                ceDrag(sv[si],0.f,1.f,sx,colsW);
                        }
                        cy2+=((13+1)/2)*34+8;

                        // Blend toggle
                        if(mx>=cx2&&mx<=cx2+sw2+30&&my>=cy2&&my<=cy2+18)
                            ce.blend=(ce.blend==EffBlend::Add)?EffBlend::Normal:EffBlend::Add;
                        cy2+=24;

                        // Hue bars
                        if(ce.color==EffColor::Solid||ce.color==EffColor::Pulse||
                           ce.color==EffColor::Neon ||ce.color==EffColor::Pastel||
                           ce.color==EffColor::Duo  ||ce.color==EffColor::Glitch){
                            if(my>=cy2&&my<=cy2+12&&mx>=cx2+46&&mx<=cx2+46+sw2-16)
                                ceDrag(&ce.hue,0.f,360.f,cx2+46,sw2-16);
                            if(my>=cy2+18&&my<=cy2+30&&mx>=cx2+46&&mx<=cx2+46+sw2-16)
                                ceDrag(&ce.hue2,0.f,360.f,cx2+46,sw2-16);
                        }
                        saveCustomEffects();
                    }
                    goto skip_all_other_clicks;
                }

                // Theme panel clicks
                if(g_themeOpen && g_themePanelT > 0.3f){
                    if(!g_themeEditing){
                        // Preset picker clicks
                        int ox=ww-340,oy=80,ow=330;
                        int cardH=52,cardGap=6,listY=oy+34;
                        int totalCards=NUM_PRESETS+g_numCustomThemes+1;
                        for(int i=0;i<totalCards;++i){
                            int ry=listY+i*(cardH+cardGap);
                            if(mx>=ox+6&&mx<=ox+ow-6&&my>=ry&&my<=ry+cardH){
                                bool isNew=(i==NUM_PRESETS+g_numCustomThemes);
                                bool isCustom=(i>=NUM_PRESETS&&!isNew);
                                if(isNew&&g_numCustomThemes<4){
                                    g_numCustomThemes++;
                                    g_customThemes[g_numCustomThemes-1]=makeDefaultTheme("Custom "+std::to_string(g_numCustomThemes));
                                    g_themeEditing=true;
                                    g_themeEditSlot=g_numCustomThemes-1;
                                    g_themeEditBuf=g_customThemes[g_themeEditSlot];
                                    g_themeEditorT=0.f; g_themeEditorTab=0;
                                } else if(isCustom&&mx>=ox+ow-52){
                                    g_themeEditing=true;
                                    g_themeEditSlot=i-NUM_PRESETS;
                                    g_themeEditBuf=g_customThemes[g_themeEditSlot];
                                    g_themeEditorT=0.f; g_themeEditorTab=0;
                                } else if(!isNew){
                                    applyTheme(i); saveThemes();
                                }
                                break;
                            }
                        }
                    } else {
                        // Fullscreen editor clicks
                        float ef=clamp01(g_themeEditorT*2.f);
                        if(ef>=0.3f){
                            int contentY=100,contentX=30,contentW=ww-60;
                            bool handled=false;
                            for(int tb=0;tb<3&&!handled;++tb){
                                if(mx>=20+tb*160&&mx<=20+tb*160+156&&my>=58&&my<=86){
                                    g_themeEditorTab=tb; handled=true;
                                }
                            }
                            if(!handled&&my>=10&&my<=38&&mx>=ww-200&&mx<=ww-130){
                                g_customThemes[g_themeEditSlot]=g_themeEditBuf;
                                g_customThemes[g_themeEditSlot].custom=true;
                                g_themeEditing=false;
                                applyTheme(NUM_PRESETS+g_themeEditSlot);
                                saveThemes(); handled=true;
                            }
                            if(!handled&&my>=10&&my<=38&&mx>=ww-110&&mx<=ww-10){
                                if(g_customThemes[g_themeEditSlot].name.find("Custom")==0&&
                                   g_customThemes[g_themeEditSlot].satMult==1.f)
                                    g_numCustomThemes=std::max(0,g_numCustomThemes-1);
                                g_themeEditing=false; handled=true;
                            }
                            if(!handled){
                                int col1=contentX,colW=contentW/2-30;
                                auto startDrag=[&](float* val,float mn,float mx2,int tx,int tw){
                                    g_themeDragVal=val; g_themeDragMin=mn; g_themeDragMax=mx2;
                                    g_themeDragX=tx;   g_themeDragW=tw;
                                    g_themeDragSlider=1;
                                    *val=mn+(mx2-mn)*clamp01((float)(mx-tx)/tw);
                                };
                                if(g_themeEditorTab==0){
                                    // Hue offset bar
                                    if(my>=contentY+18&&my<=contentY+32&&mx>=col1&&mx<=col1+colW)
                                        startDrag(&g_themeEditBuf.hueOffset,0.f,360.f,col1,colW);
                                    // 3 sliders
                                    float* sv[]={&g_themeEditBuf.satMult,&g_themeEditBuf.valMult,&g_themeEditBuf.intensity};
                                    float smn[]={0.f,0.2f,0.1f},smx2[]={1.5f,1.3f,1.8f};
                                    for(int s=0;s<3;++s){
                                        int sy=contentY+60+s*50+14;
                                        if(my>=sy-4&&my<=sy+12&&mx>=col1&&mx<=col1+colW)
                                            startDrag(sv[s],smn[s],smx2[s],col1,colW);
                                    }
                                    // UI hue bar
                                    if(my>=contentY+238&&my<=contentY+252&&mx>=col1&&mx<=col1+colW)
                                        startDrag(&g_themeEditBuf.uiHue,0.f,360.f,col1,colW);
                                } else if(g_themeEditorTab==1){
                                    int leftW=contentW/2-10;
                                    int rightX=contentX+leftW+20;
                                    int rightW=contentW/2-20;
                                    // LEFT — builtin toggles
                                    int rowH=30,visRows=(wh-contentY-80)/rowH;
                                    for(int i=0;i<visRows;++i){
                                        int ei=i+g_effScrollOffset;
                                        if(ei>=EFF_COUNT) break;
                                        int ry=contentY+20+i*rowH;
                                        // ON/OFF toggle
                                        if(mx>=contentX+2&&mx<=contentX+34&&my>=ry+4&&my<=ry+rowH-8)
                                            g_themeEditBuf.effVisible[ei]=!g_themeEditBuf.effVisible[ei];
                                        // Color override toggle
                                        if(g_themeEditBuf.effVisible[ei]){
                                            if(mx>=contentX+leftW-62&&mx<=contentX+leftW-2&&my>=ry+4&&my<=ry+rowH-8)
                                                g_themeEditBuf.effHueOverride[ei]=!g_themeEditBuf.effHueOverride[ei];
                                            if(g_themeEditBuf.effHueOverride[ei]){
                                                int hbX=contentX+leftW-122,hbW=55;
                                                if(mx>=hbX&&mx<=hbX+hbW&&my>=ry+6&&my<=ry+rowH-8)
                                                    startDrag(&g_themeEditBuf.effHue[ei],0.f,360.f,hbX,hbW);
                                            }
                                        }
                                    }
                                    // RIGHT — custom effect builder
                                    int ceRowH=32;
                                    for(int i=g_ceScrollOffset;i<=g_numCustomEffects&&i<MAX_CUSTOM_EFFECTS;++i){
                                        int ry=contentY+20+(i-g_ceScrollOffset)*ceRowH;
                                        int maxListH2=std::min((g_numCustomEffects+1)*ceRowH+20,(wh-contentY)/3);
                                        if(ry+ceRowH>contentY+maxListH2) break;
                                        bool isAdd=(i==g_numCustomEffects);
                                        if(mx>=rightX&&mx<=rightX+rightW&&my>=ry&&my<=ry+ceRowH-2){
                                            if(isAdd){
                                                if(g_numCustomEffects<MAX_CUSTOM_EFFECTS){
                                                    g_customEffects[g_numCustomEffects]={};
                                                    g_customEffects[g_numCustomEffects].name="Effect "+std::to_string(g_numCustomEffects+1);
                                                    g_customEffects[g_numCustomEffects].enabled=true;
                                                    g_customEffects[g_numCustomEffects].size=0.5f;
                                                    g_customEffects[g_numCustomEffects].speed=0.5f;
                                                    g_customEffects[g_numCustomEffects].density=0.5f;
                                                    g_customEffects[g_numCustomEffects].alpha=0.7f;
                                                    g_editingEffect=g_numCustomEffects;
                                                    g_numCustomEffects++;
                                                    g_ceFullscreen=true;
                                                }
                                            } else {
                                                // ON/OFF
                                                if(mx<=rightX+34){
                                                    g_customEffects[i].enabled=!g_customEffects[i].enabled;
                                                    saveCustomEffects();
                                                }
                                                // Edit
                                                else if(mx>=rightX+rightW-80&&my<=ry+ceRowH/2)
                                                    if(g_editingEffect==i){ g_editingEffect=-1; g_ceFullscreen=false; }
                                                    else { g_editingEffect=i; g_ceFullscreen=true; }
                                                // Delete
                                                else if(mx>=rightX+rightW-44&&my>ry+ceRowH/2){
                                                    for(int j=i;j<g_numCustomEffects-1;++j)
                                                        g_customEffects[j]=g_customEffects[j+1];
                                                    g_numCustomEffects--;
                                                    if(g_editingEffect>=g_numCustomEffects){ g_editingEffect=-1; g_ceFullscreen=false; }
                                                    saveCustomEffects();
                                                }
                                            }
                                        }
                                    }
                                    // Edit panel now fullscreen — clicks handled outside theme editor block
                                    (void)0;
                                } else {
                                    // Gradient toggle
                                    if(mx>=col1&&mx<=col1+120&&my>=contentY&&my<=contentY+28)
                                        g_themeEditBuf.bgGradient=!g_themeEditBuf.bgGradient;
                                    // Hue bars
                                    if(my>=contentY+66&&my<=contentY+82&&mx>=col1&&mx<=col1+colW)
                                        startDrag(&g_themeEditBuf.bgHue1,0.f,360.f,col1,colW);
                                    if(my>=contentY+118&&my<=contentY+134&&mx>=col1&&mx<=col1+colW)
                                        startDrag(&g_themeEditBuf.bgHue2,0.f,360.f,col1,colW);
                                    float* bv[]={&g_themeEditBuf.bgSat,&g_themeEditBuf.bgVal,&g_themeEditBuf.bgGradientAngle};
                                    float bmn[]={0.f,0.f,0.f},bmx2[]={1.f,0.2f,360.f};
                                    for(int s=0;s<3;++s){
                                        int sy=contentY+150+s*50+14;
                                        if(my>=sy-4&&my<=sy+12&&mx>=col1&&mx<=col1+colW)
                                            startDrag(bv[s],bmn[s],bmx2[s],col1,colW);
                                    }
                                }
                            }
                        }
                    }
                }

                skip_all_other_clicks:;
                // Progress bar — start drag (not while theme editor open)
                if(!g_themeEditing && onProgressBar(mx,my,ww,wh)&&g_music){
                    g_seekDragging=true;
                    g_seekPreview=clamp01((float)(mx-120)/(ww-260));
                }

                // Check menu trigger button
                if(hitMenuBtn(mx,my)){
                    toggleMenu(t);
                }
                // Check menu items (only when open)
                else if(g_menuOpen > 0.3f){
                    int hit = -1;
                    for(int i=0;i<MENU_ITEM_COUNT;++i)
                        if(hitMenuItem(mx,my,i,g_menuOpen)){ hit=i; break; }
                    if(hit >= 0){
                        // Close menu with flourish
                        toggleMenu(t);
                        switch(MENU_ITEMS[hit].action){
                        case MenuAction::Library:
                            if(g_panel!=UIPanel::Library){
                                g_panel=UIPanel::Library;
                                g_panelBuildT=0.f; g_panelBuilding=true;
                                g_libJustOpened=true;
                            } else { g_panel=UIPanel::None; }
                            break;
                        case MenuAction::Playlists:
                            if(g_panel!=UIPanel::Playlists){
                                g_panel=UIPanel::Playlists;
                                g_panelBuildT=0.f; g_panelBuilding=true;
                            } else { g_panel=UIPanel::None; }
                            break;
                        case MenuAction::AddSong:{
                            openFilePickerAsync();
                            break;
                        }
                        case MenuAction::PlayPause:
                            if(g_music){g_paused=!g_paused;g_paused?Mix_PauseMusic():Mix_ResumeMusic();}
                            else if(!g_library.empty()) playLibrarySong(0);
                            break;
                        case MenuAction::Prev: playPrev(); break;
                        case MenuAction::Next: playNext(); break;
                        case MenuAction::NewPlaylist:
                            memset(g_newPLName,0,sizeof(g_newPLName));
                            strcpy(g_newPLName,"New Playlist");
                            g_namingPL=true;
                            g_namingBuildT=0.f; g_namingBuilding=true;
                            SDL_StartTextInput();
                            break;
                        case MenuAction::Spotify:
                            spotOpenCredentials();
                            break;
                        case MenuAction::Theme:
                            g_themeOpen=!g_themeOpen;
                            g_themePanelT=0.f;
                            break;
                        default: break;
                    }
                    }
                }

                // Panel row clicks
                int songClicked=libraryClickRow(mx,my);
                if(songClicked>=0){
                    if(g_selectedPL>=0&&g_selectedPL<(int)g_playlists.size()){
                        auto& pl=g_playlists[g_selectedPL];
                        bool alreadyIn=false;
                        for(int i:pl.songIndices)if(i==songClicked)alreadyIn=true;
                        if(!alreadyIn){pl.songIndices.push_back(songClicked);saveLibrary();}
                    }
                    g_currentSong=songClicked;
                    playLibrarySong(songClicked);
                }
                int plClicked=playlistClickRow(mx,my);
                if(plClicked>=-1){
                    if(plClicked==-1){
                        g_currentPL=-1;g_selectedPL=-1;
                        g_panel=UIPanel::Library;
                    } else if(plClicked<(int)g_playlists.size()){
                        g_currentPL=plClicked;g_selectedPL=plClicked;
                        g_currentPLTrack=-1;
                        if(!g_playlists[plClicked].songIndices.empty()){
                            g_currentPLTrack=0;
                            playLibrarySong(g_playlists[plClicked].songIndices[0]);
                        }
                    }
                }
            }

            // Commit seek on mouse release
            if(ev.type==SDL_MOUSEBUTTONUP&&ev.button.button==SDL_BUTTON_LEFT){
                if(g_seekDragging&&g_music){
                    double dur=Mix_MusicDuration(g_music);
                    Mix_SetMusicPosition(dur*g_seekPreview);
                    g_lastTrackChange=SDL_GetTicks();
                }
                g_seekDragging=false;
                g_themeDragSlider=-1;
                g_themeDragVal=nullptr;
            }

            if(ev.type==SDL_DROPFILE){
                std::string f(ev.drop.file);SDL_free(ev.drop.file);
                addToLibrary(f);g_currentSong=(int)g_library.size()-1;loadAndPlay(f);
            }
            if(ev.type==SDL_WINDOWEVENT&&ev.window.event==SDL_WINDOWEVENT_SIZE_CHANGED){
                // Stop render thread, swap textures, restart
                g_renderRunning = false;
                g_renderCv.notify_all();
                if(renderThread.joinable()) renderThread.join();

                // Hold resize lock while destroying/recreating textures
                { std::lock_guard<std::mutex> lk(g_resizeMtx);
                  SDL_GetRendererOutputSize(ren,&ww,&wh);
                  maxR=sqrtf((ww*.5f)*(ww*.5f)+(wh*.5f)*(wh*.5f));
                  SDL_DestroyTexture(fbA);SDL_DestroyTexture(fbB);SDL_DestroyTexture(layerTex);
                  fbA=makeT();fbB=makeT();layerTex=makeT();
                }
                initKNodes(maxR,rng);g_neuronsInit=false;g_rainInit=false;g_geoInit=false;
                g_shardsInit=false;g_novaInit=false;g_tendrilsInit=false;g_vortexInit=false;

                g_rp.fbA=fbA; g_rp.fbB=fbB; g_rp.layerTex=layerTex;
                g_renderRunning = true;
                renderThread = std::thread([&](){
                    try {
                    while(g_renderRunning){
                        { std::unique_lock<std::mutex> lk(g_renderCvMtx);
                          g_renderCv.wait_for(lk,std::chrono::milliseconds(32),
                                              []{ return g_renderReady.load(); });
                          g_renderReady=false; }
                        if(!g_renderRunning) break;
                        std::lock_guard<std::mutex> rzlk(g_resizeMtx);
                        if(!g_renderRunning) break;
                        RenderParams rp;
                        { std::lock_guard<std::mutex> lk(g_renderMtx); rp=g_rp; }
                        if(!rp.fbA||!rp.fbB||!rp.layerTex){ logErr("WARN: null tex after resize"); continue; }
                        renderFrame(ren,rp);
                        ++g_perfFrame;
                    }
                    } catch(const std::exception& e){ logErr(std::string("RENDER THREAD: ")+e.what()); }
                      catch(...){ logErr("RENDER THREAD UNKNOWN EXCEPTION"); }
                });
            }
        }

        // Update menu animation
        updateMenu(dt);
        updateMenuRipples(dt);
        MENU_BTN_X = ww * 0.5f;

        // Photosensitivity warning
        if(g_warnActive){
            g_warnBuildT += dt * 0.55f;
            if(g_warnBuildT > 1.f) g_warnBuildT = 1.f;
            g_warnAlpha = 1.f;
        } else {
            g_warnAlpha -= dt * 2.5f;
            if(g_warnAlpha < 0.f) g_warnAlpha = 0.f;
        }
        // Theme panel timer
        if(g_themeOpen){
            g_themePanelT=std::min(1.f,g_themePanelT+dt*1.8f);
        } else {
            g_themePanelT=std::max(0.f,g_themePanelT-dt*3.f);
        }
        if(g_themeEditing) g_themeEditorT=std::min(1.f,g_themeEditorT+dt*2.2f);
        else g_themeEditorT=std::max(0.f,g_themeEditorT-dt*4.f);
        g_themeBlend=std::min(1.f,g_themeBlend+dt*2.5f);
        if(g_titleBuildT < 1.f)
            g_titleBuildT = std::min(1.f, g_titleBuildT + dt * 1.2f);

        // Advance per-row build timers
        if(g_libJustOpened){
            for(int i=0;i<VISIBLE_ROWS+2;++i)
                g_rowBuildT[i] = -(float)i * 0.08f;
            g_libScrollPrev = g_libScroll;
            g_libJustOpened = false;
        } else if(g_libScroll != g_libScrollPrev){
            int delta = g_libScroll - g_libScrollPrev;
            if(delta > 0){
                for(int i=0;i<VISIBLE_ROWS-delta;++i)
                    g_rowBuildT[i] = g_rowBuildT[i+delta];
                for(int i=VISIBLE_ROWS-delta;i<VISIBLE_ROWS;++i)
                    g_rowBuildT[i] = 0.f;
            } else {
                int absDelta = -delta;
                for(int i=VISIBLE_ROWS-1;i>=absDelta;--i)
                    g_rowBuildT[i] = g_rowBuildT[i-absDelta];
                for(int i=0;i<absDelta;++i)
                    g_rowBuildT[i] = 0.f;
            }
            g_libScrollPrev = g_libScroll;
        }
        // Only advance row timers once the panel is far enough built to show rows
        if(g_panel==UIPanel::Library && g_panelBuildT > 0.45f){
            for(int i=0;i<VISIBLE_ROWS;++i){
                g_rowBuildT[i] += dt * 1.8f;
                if(g_rowBuildT[i] > 1.f) g_rowBuildT[i] = 1.f;
            }
        }

        // Advance fake loading bar — crawls to 94% over 4s then stalls waiting for real auth
        if(g_spotState==SpotState::WaitingAuth||g_spotState==SpotState::FetchingPlaylists){
            // Advance the waiting screen's own build timer
            if(g_spotWaitBuildT < 1.f)
                g_spotWaitBuildT = std::min(1.f, g_spotWaitBuildT + dt * 0.55f);
            if(!g_spotFakeLoading){
                g_spotFakeLoadT=0.f;
                g_spotFakeLoading=true;
            }
            // Eased crawl — fast at start, slows near 94%, then holds
            float target = g_spotState==SpotState::FetchingPlaylists ? 0.99f : 0.94f;
            if(g_spotFakeLoadT < target){
                // Non-linear speed: fast early, very slow near target
                float gap = target - g_spotFakeLoadT;
                float speed = gap * 0.7f + 0.02f; // proportional + tiny minimum
                g_spotFakeLoadT += dt * speed * (1.f/4.f); // 4s to reach target
                g_spotFakeLoadT = std::min(g_spotFakeLoadT, target);
            }
        } else {
            g_spotFakeLoading=false;
            g_spotFakeLoadT=0.f;
        }

        // Spotify state machine tick
        if(g_spotState==SpotState::WaitingAuth&&g_spotCodeReady.load()){
            // Grab and immediately clear the code so it can't be reused
            std::string code;
            {
                std::lock_guard<std::mutex> lk(g_spotMtx);
                code=g_spotAuthCode;
                g_spotAuthCode="";
            }
            g_spotCodeReady=false;  // reset so next auth attempt works fresh
            g_spotState=SpotState::FetchingPlaylists;
            g_spotBuildT=0.f; g_spotBuilding=true;
            g_spotWaitBuildT=0.f;
            // Exchange code for token + fetch playlists on background thread
            std::thread([code]{
                if(!spotExchangeCode(code)){
                    g_spotStatusMsg="ERROR: Token exchange failed. Check Client ID/Secret.";
                    g_spotState=SpotState::CredentialsDialog;
                    g_spotBuildT=0.f; g_spotBuilding=true;
                    return;
                }
                if(!spotFetchPlaylists()){
                    g_spotStatusMsg="ERROR: Could not fetch playlists. Try again.";
                    g_spotState=SpotState::CredentialsDialog;
                    g_spotBuildT=0.f; g_spotBuilding=true;
                    return;
                }
                g_spotState=SpotState::PlaylistPicker;
                g_spotBuildT=0.f; g_spotBuilding=true;
            }).detach();
        }
        // Downloading → Done transition
        if(g_spotState==SpotState::Downloading&&!g_spotDlRunning.load())
            g_spotState=SpotState::Done;

        // Advance Spotify build timer
        if(g_spotBuilding){
            g_spotBuildT+=dt*0.55f;
            if(g_spotBuildT>=1.f){g_spotBuildT=1.f;g_spotBuilding=false;}
        }

        // Advance panel build timers
        if(g_panelBuilding){
            g_panelBuildT += dt * g_panelBuildSpd;
            if(g_panelBuildT >= 1.f){ g_panelBuildT=1.f; g_panelBuilding=false; }
        }
        if(g_namingBuilding){
            g_namingBuildT += dt * 2.8f;
            if(g_namingBuildT >= 1.f){ g_namingBuildT=1.f; g_namingBuilding=false; }
        }

        // FFT
        SDL_LockMutex(g_pcmMutex);
        int base=(g_pcmWrite-FFT_N*2+FFT_N*200)%(FFT_N*2);
        for(int i=0;i<FFT_N;++i){
            float l=g_pcmBuf[(base+i*2)%(FFT_N*2)],r2=g_pcmBuf[(base+i*2+1)%(FFT_N*2)];
            fftIn[i]=(l+r2)*.5*hannWin[i];
        }
        SDL_UnlockMutex(g_pcmMutex);
        fftw_execute(plan);
        for(int i=0;i<FREQ_BINS;++i){
            double re=fftOut[i][0],im=fftOut[i][1];
            float n=clamp01((20.f*log10f((float)(sqrt(re*re+im*im)/FFT_N)+1e-7f)+90.f)/90.f);
            spec[i]=lerp(spec[i],n,.3f);
        }
        float rSub=0,rB=0,rLM=0,rM=0,rHM=0,rH=0;
        for(int i=0; i<3;  ++i)rSub+=spec[i];rSub/=3.f;
        for(int i=3; i<11; ++i)rB  +=spec[i];rB  /=8.f;
        for(int i=11;i<24; ++i)rLM +=spec[i];rLM /=13.f;
        for(int i=24;i<93; ++i)rM  +=spec[i];rM  /=69.f;
        for(int i=93;i<180;++i)rHM +=spec[i];rHM /=87.f;
        for(int i=180;i<280;++i)rH+=spec[i];rH/=100.f;
        float rA=rSub*.1f+rB*.35f+rLM*.15f+rM*.25f+rHM*.1f+rH*.05f;
        sBass=lerp(sBass,rB,.2f);sMid=lerp(sMid,rM,.2f);sHigh=lerp(sHigh,rH,.2f);
        sAll=lerp(sAll,rA,.2f);sSub=lerp(sSub,rSub,.2f);
        sLM=lerp(sLM,rLM,.2f);sHM=lerp(sHM,rHM,.2f);

        SDL_GetRendererOutputSize(ren,&ww,&wh);
        float cx=ww*.5f,cy=wh*.5f;

        if(!g_neuronsInit){
            initNeurons(cx,cy,maxR,rng);
        }
        if(!g_rainInit)initRain(ww,wh,rng);
        if(!g_geoInit)initGeo(cx,cy,maxR,rng);
        if(!g_shardsInit)initShards(ww,wh,rng);
        if(!g_novaInit)initNova(maxR,rng);
        if(!g_tendrilsInit)initTendrils(rng);
        if(!g_vortexInit)initVortex(rng);

        { std::lock_guard<std::mutex> lk(g_particleMtx);
          updateKNodes(dt,sBass,sMid,sAll,maxR);
          updateNeurons(dt,cx,cy,sHigh,sBass,maxR,t);
          triggerShockwave(rB,t);
          updateShockwaves(dt);
          updateRain(dt,sHigh,sAll,wh);
          updateGeo(dt,cx,cy,sBass,sAll);
          spawnLightning(cx,cy,t,sBass,sHigh,ww,wh,rng);
          updateNova(dt,sBass,sMid,sAll,maxR);
          updateTendrils(dt,cx,cy,sBass,sMid,t,maxR);
          updateVortex(dt,sBass,sAll,maxR,rng);
          updateFilaments(dt,t,sHigh,sMid,sAll,ww,wh,rng);
        }

        // UI logic — must stay on main thread
        g_idleTimer += dt;

        // ── HAND OFF TO RENDER THREAD ───────────────────────────────────────
        {
            std::lock_guard<std::mutex> lk(g_renderMtx);
            g_rp.ww=ww; g_rp.wh=wh; g_rp.t=t; g_rp.cx=cx; g_rp.cy=cy;
            g_rp.maxR=maxR; g_rp.frame=frame;
            g_rp.sBass=sBass; g_rp.sMid=sMid; g_rp.sHigh=sHigh;
            g_rp.sAll=sAll; g_rp.sSub=sSub; g_rp.sLM=sLM; g_rp.sHM=sHM;
            g_rp.rB=rB; g_rp.spec=spec;
            g_rp.fbA=fbA; g_rp.fbB=fbB; g_rp.layerTex=layerTex;
        }
        { std::lock_guard<std::mutex> lk(g_renderCvMtx); g_renderReady=true; }
        g_renderCv.notify_one();
        SDL_Delay(1); // yield to render thread
        ++frame;
    }

    // Stop render thread cleanly before destroying resources
    g_renderRunning = false;
    g_renderCv.notify_all();
    if(renderThread.joinable()) renderThread.join();

    SDL_DestroyTexture(fbA);SDL_DestroyTexture(fbB);SDL_DestroyTexture(layerTex);
    if(g_spotDlThread.joinable()){ g_spotDlRunning=false; g_spotDlThread.join(); }
    if(g_music){Mix_HaltMusic();Mix_FreeMusic(g_music);}
    fftw_destroy_plan(plan);fftw_free(fftOut);SDL_DestroyMutex(g_pcmMutex);
    Mix_CloseAudio();Mix_Quit();SDL_DestroyRenderer(ren);SDL_DestroyWindow(win);
    SDL_Quit();return 0;
}
