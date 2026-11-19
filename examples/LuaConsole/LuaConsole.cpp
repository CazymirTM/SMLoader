// LuaConsole - an SMLoader mod that opens a console window and runs the Lua
// you type inside the game's live Lua states.
//
//   :help              list commands
//   :states            list the game's Lua states and what they contain
//   :use <n>           pick the state to run code in
//   :env game|raw      game = the shared env all game scripts run in
//                             (SurvivalGame, BasePlayer, ... ; inherits unsafe_env)
//                      raw  = the state's real globals (ffi, debug, os, ...)
//   :run <file>        run SMLoader\scripts\<file>
//   <lua>              run a line; expressions print their value
//   line ending in \   continues on the next line

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../../include/smloader.h"

static const SMLoaderAPI* api = nullptr;
static const SMLuaAPI* lua = nullptr;

static std::mutex g_mx;
static lua_State* g_current = nullptr;
static bool g_rawEnv = false;
static std::string g_gameClass = "SurvivalGame";   // updated when a game-mode script loads
enum Ctx { CTX_NONE, CTX_CLIENT, CTX_SERVER };
static Ctx g_ctx = CTX_CLIENT;
static const char* CtxName(Ctx c) { return c == CTX_CLIENT ? "client" : c == CTX_SERVER ? "server" : "none"; }

// Which script callback to run inside for a context ("" = between callbacks).
static std::string CtxTarget(Ctx c) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (c == CTX_CLIENT) return g_gameClass + ".client_onUpdate";
    if (c == CTX_SERVER) return g_gameClass + ".server_onFixedUpdate";
    return "";
}
static lua_State* g_syntax = nullptr;   // private state, used only to syntax-check input

static void Out(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void Prompt() {
    if (g_rawEnv) Out("\nraw> ");
    else Out("\ngame:%s> ", CtxName(g_ctx));
}

static void OnDone(int ok, const char* text, void*) {
    if (!ok) Out("\x1b[31m%s\x1b[0m", text);
    else if (text && *text) Out("%s", text);
    Prompt();
}

// The game creates one gameplay state on the main thread plus a pool of
// terrain-generation states on worker threads. Follow whichever state compiles
// the game-mode script: that's the one running the world.
static int OnScriptLoad(lua_State* L, const char* chunkname, const char**, size_t*, void*) {
    static const char* kModes[] = {"SurvivalGame", "CreativeGame", "ChallengeGame"};
    if (!chunkname || !strstr(chunkname, "/Scripts/game/")) return 0;
    for (const char* mode : kModes) {
        if (strstr(chunkname, (std::string("/") + mode + ".lua").c_str())) {
            std::lock_guard<std::mutex> lk(g_mx);
            g_current = L;
            g_gameClass = mode;
        }
    }
    return 0;
}
static void OnStateClosed(lua_State* L, void*) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_current == L) g_current = nullptr;
}

static lua_State* Current() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_current) return g_current;
    lua_State* states[64];
    int n = api->GetStates(states, 64);
    if (n > 0) g_current = states[n - 1];
    return g_current;
}

// "x + 1" -> run as "return x + 1" when that compiles, so expressions print.
static std::string MakeChunk(const std::string& line) {
    static std::mutex syntaxMx;   // g_syntax is shared by the console and pipe threads
    std::lock_guard<std::mutex> lk(syntaxMx);
    std::string ret = "return " + line;
    if (lua->luaL_loadbufferx(g_syntax, ret.data(), ret.size(), "=check", "t") == 0) {
        lua->lua_settop(g_syntax, 0);
        return ret;
    }
    lua->lua_settop(g_syntax, 0);
    return line;
}

// Route a chunk to the right environment/context. In a callback context the
// chunk sees the callback's own arguments; `self` is bound to the first one
// (the game-mode script instance: self.sv, self.cl, self.network, ...).
static void Submit(lua_State* L, const std::string& code, const char* chunkname, bool raw, Ctx ctx,
                   void (*done)(int, const char*, void*), void* user) {
    if (raw) { api->QueueLua(L, code.c_str(), chunkname, nullptr, done, user); return; }
    std::string target = CtxTarget(ctx);
    if (target.empty()) { api->QueueLua(L, code.c_str(), chunkname, "game", done, user); return; }
    std::string wrapped = "local self = ...\n" + code;
    api->QueueLuaIn(L, target.c_str(), wrapped.c_str(), chunkname, done, user);
}

static const char* kProbe =
    "local e = rawget(_G, 'unsafe_env')\n"
    "local kind = 'no sandbox'\n"
    "if type(e) == 'table' and type(e.sm) == 'table' then\n"
    "  kind = e.sm.game and 'gameplay' or (e.sm.terrainGeneration and 'terrain' or 'other')\n"
    "end\n"
    "local classes = {}\n"
    "if type(e) == 'table' then\n"
    "  for _, k in ipairs({'SurvivalGame','CreativeGame','ChallengeGame','MenuGame','SurvivalPlayer'}) do\n"
    "    if rawget(e, k) then classes[#classes+1] = k end\n"
    "  end\n"
    "end\n"
    "return kind .. (#classes > 0 and (' [' .. table.concat(classes, ', ') .. ']') or '')\n";

struct ProbeCtx { int index; lua_State* L; };
static void OnProbe(int ok, const char* text, void* user) {
    auto* c = (ProbeCtx*)user;
    Out("  %d: %p  %s%s\n", c->index, (void*)c->L, ok ? text : "(probe failed)",
        c->L == g_current ? "   <- current" : "");
    delete c;
}

static void ListStates() {
    lua_State* states[64];
    int n = api->GetStates(states, 64);
    if (n == 0) { Out("no Lua states yet\n"); return; }
    Out("%d state(s) (results arrive as each state runs its next game call):\n", n);
    for (int i = 0; i < n && i < 64; ++i)
        api->QueueLua(states[i], kProbe, "=probe", nullptr, OnProbe, new ProbeCtx{i, states[i]});
}

static bool ReadFileUtf8(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096]; size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static void Handle(const std::string& line) {
    if (line.empty()) { Prompt(); return; }
    if (line[0] == ':') {
        char arg[512] = {};
        if (line == ":help") {
            Out(":states | :use <n> | :env game|raw | :ctx client|server|none | :run <file in SMLoader\\scripts> | <lua>\n"
                "game env: what game scripts see (SurvivalGame, sm.*, ...).\n"
                "  :ctx client  runs inside <Game>.client_onUpdate      (client sm.* calls allowed)\n"
                "  :ctx server  runs inside <Game>.server_onFixedUpdate (server sm.* calls; host only)\n"
                "  :ctx none    runs between callbacks (most sm.* calls are rejected there)\n"
                "  In client/server ctx, `self` is the live game script instance.\n"
                "raw env: the real globals - unsafe_env, ffi, debug, os (note: `type` is the game's).\n");
        } else if (line == ":states") {
            ListStates();
            return;   // prompt follows the probe output
        } else if (int idx = -1; sscanf(line.c_str(), ":use %d", &idx) == 1) {
            lua_State* states[64];
            int n = api->GetStates(states, 64);
            if (idx >= 0 && idx < n && idx < 64) {
                std::lock_guard<std::mutex> lk(g_mx);
                g_current = states[idx];
                Out("using state %d (%p)\n", idx, (void*)g_current);
            } else Out("no state %d\n", idx);
        } else if (line == ":env raw") { g_rawEnv = true;
        } else if (line == ":env game") { g_rawEnv = false;
        } else if (sscanf(line.c_str(), ":run %511[^\n]", arg) == 1) {
            std::string path = std::string(api->loaderDir) + "\\scripts\\" + arg;
            std::string src;
            lua_State* L = Current();
            if (!ReadFileUtf8(path, src)) Out("cannot read %s\n", path.c_str());
            else if (!L) Out("no Lua state yet\n");
            else { Submit(L, src, ("@" + std::string(arg)).c_str(), g_rawEnv, g_ctx, OnDone, nullptr); return; }
        } else if (line == ":ctx client") { g_ctx = CTX_CLIENT;
        } else if (line == ":ctx server") { g_ctx = CTX_SERVER;
        } else if (line == ":ctx none")   { g_ctx = CTX_NONE;
        } else {
            Out("unknown command, try :help\n");
        }
        Prompt();
        return;
    }
    lua_State* L = Current();
    if (!L) { Out("no Lua state yet - load into a world first\n"); Prompt(); return; }
    Submit(L, MakeChunk(line), "=console", g_rawEnv, g_ctx, OnDone, nullptr);
}

// ---------------------------------------------------------------------------
// Named pipe \\.\pipe\SMLoaderConsole - lets external tools (tools\smexec.ps1)
// run Lua the same way the console does. Local clients only. Request is one
// message: optional header line "#game|#raw [stateIndex]" then Lua source.
// Reply: "OK\n<results>" or "ERR\n<message>".
// ---------------------------------------------------------------------------

struct PipeReq { HANDLE done; int ok = 0; std::string text; };
static void OnPipeDone(int ok, const char* text, void* user) {
    auto* r = (PipeReq*)user;
    r->ok = ok;
    r->text = text ? text : "";
    SetEvent(r->done);
}

static std::string RunFromPipe(const std::string& req) {
    std::string code = req;
    bool raw = g_rawEnv;
    Ctx ctx = g_ctx;
    int stateIdx = -1;
    if (!code.empty() && code[0] == '#') {
        size_t nl = code.find('\n');
        std::string hdr = code.substr(1, nl == std::string::npos ? std::string::npos : nl - 1);
        code = nl == std::string::npos ? "" : code.substr(nl + 1);
        char env[16] = {}, ctxs[16] = {};
        int got = sscanf(hdr.c_str(), "%15s %d %15s", env, &stateIdx, ctxs);
        if (got >= 1) raw = strcmp(env, "raw") == 0;
        if (got >= 3) ctx = !strcmp(ctxs, "server") ? CTX_SERVER : !strcmp(ctxs, "none") ? CTX_NONE : CTX_CLIENT;
    }
    lua_State* L = Current();
    if (stateIdx >= 0) {
        lua_State* states[64];
        int n = api->GetStates(states, 64);
        L = stateIdx < n && stateIdx < 64 ? states[stateIdx] : nullptr;
    }
    if (!L) return "ERR\nno such Lua state (load into a world first)";

    Out("\n\x1b[36m[pipe %s%s%s] %s\x1b[0m\n", raw ? "raw" : "game", raw ? "" : ":", raw ? "" : CtxName(ctx),
        code.size() > 300 ? (code.substr(0, 300) + " ...").c_str() : code.c_str());
    std::string chunk = code.find('\n') == std::string::npos ? MakeChunk(code) : code;
    auto* r = new PipeReq;
    r->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Submit(L, chunk, "=pipe", raw, ctx, OnPipeDone, r);
    if (WaitForSingleObject(r->done, 15000) != WAIT_OBJECT_0)
        return "ERR\ntimed out waiting for the game to run the chunk (is the world paused or loading?)";
    std::string reply = (r->ok ? "OK\n" : "ERR\n") + r->text;
    Out("%s", r->text.c_str());
    Prompt();
    CloseHandle(r->done);
    delete r;   // only freed when the callback has fired; leaked on timeout on purpose
    return reply;
}

static DWORD WINAPI PipeThread(LPVOID) {
    for (;;) {
        HANDLE p = CreateNamedPipeW(L"\\\\.\\pipe\\SMLoaderConsole", PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 1 << 20, 1 << 20, 0, nullptr);
        if (p == INVALID_HANDLE_VALUE) { api->Log("LuaConsole: CreateNamedPipe failed (%lu)", GetLastError()); return 1; }
        if (ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            std::string req;
            static char buf[1 << 16];
            for (;;) {
                DWORD n = 0;
                BOOL ok = ReadFile(p, buf, sizeof buf, &n, nullptr);
                req.append(buf, n);
                if (ok) break;
                if (GetLastError() != ERROR_MORE_DATA) { req.clear(); break; }
            }
            if (!req.empty()) {
                std::string reply = RunFromPipe(req);
                DWORD w;
                WriteFile(p, reply.data(), (DWORD)reply.size(), &w, nullptr);
                FlushFileBuffers(p);
            }
        }
        DisconnectNamedPipe(p);
        CloseHandle(p);
    }
}

static DWORD WINAPI ConsoleThread(LPVOID) {
    AllocConsole();
    SetConsoleTitleW(L"SMLoader Lua Console");
    // Closing a console window terminates its process - i.e. the game. Remove the X.
    if (HWND w = GetConsoleWindow()) DeleteMenu(GetSystemMenu(w, FALSE), SC_CLOSE, MF_BYCOMMAND);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONIN$", "r", stdin);
    DWORD mode = 0;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(hOut, &mode)) SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    Out("SMLoader Lua Console - type :help\n");
    Prompt();
    std::string pending;
    char buf[4096];
    while (fgets(buf, sizeof buf, stdin)) {
        std::string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (!line.empty() && line.back() == '\\') { line.pop_back(); pending += line + "\n"; Out(">> "); continue; }
        pending += line;
        Handle(pending);
        pending.clear();
    }
    return 0;
}

extern "C" __declspec(dllexport) void SMMod_Init(const SMLoaderAPI* a) {
    if (a->version < SMLOADER_API_VERSION) return;
    api = a;
    lua = a->lua;
    g_syntax = lua->luaL_newstate();
    CreateDirectoryA((std::string(api->loaderDir) + "\\scripts").c_str(), nullptr);

    static SMModCallbacks cb{};
    cb.OnScriptLoad = OnScriptLoad;
    cb.OnStateClosed = OnStateClosed;
    api->RegisterCallbacks(&cb);

    CreateThread(nullptr, 0, ConsoleThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    api->Log("LuaConsole ready (pipe \\\\.\\pipe\\SMLoaderConsole)");
}
