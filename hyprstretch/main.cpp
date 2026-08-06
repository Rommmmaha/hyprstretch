#define WLR_USE_UNSTABLE
#include <array>
#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <optional>
#include <regex>
#include <unordered_set>
#include <vector>

#include "globals.hpp"
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}
// ============================================================================
// Function Signatures & Hook Handles
// ============================================================================
using FnPointerMotion = void (*)(CSeatManager*, uint32_t, const Vector2D&);
using FnXWaylandConfigure = void (*)(CXWaylandSurface*, const CBox&);
using FnXDGToplevelSetSize = uint32_t (*)(CXDGToplevelResource*, const Vector2D&);
using FnCalculateUVForSurface = void (*)(Render::IElementRenderer*, PHLWINDOW, SP<CWLSurfaceResource>, PHLMONITOR, bool, const Vector2D&, const Vector2D&, bool);
using FnWLSurfaceComputeDamage = CRegion (*)(Desktop::View::CWLSurface*);
inline CFunctionHook* g_pPointerMotionHook = nullptr;
inline CFunctionHook* g_pXWaylandConfigureHook = nullptr;
inline CFunctionHook* g_pXDGToplevelSetSizeHook = nullptr;
inline CFunctionHook* g_pCalculateUVHook = nullptr;
inline CFunctionHook* g_pWLSurfaceComputeDamageHook = nullptr;
struct SAppConfig {
  std::regex regex;
  std::optional<Vector2D> res;
};
static std::vector<SAppConfig> g_appConfigs;
static std::unordered_set<Desktop::View::CWindow*> g_initializedWindows;
static std::unordered_set<Desktop::View::CWindow*> g_enabledWindows;
static std::unordered_set<Desktop::View::CWindow*> g_disabledWindows;
// ============================================================================
// Helpers
// ============================================================================
APICALL EXPORT std::string PLUGIN_API_VERSION() {
  return HYPRLAND_API_VERSION;
}
static const SAppConfig* findConfig(const std::string& appClass) {
  for (const auto& ac : g_appConfigs)
    if (std::regex_match(appClass, ac.regex))
      return &ac;
  return nullptr;
}
static bool isActive(Desktop::View::CWindow* window, const std::string& appClass) {
  const bool configMatch = findConfig(appClass) != nullptr;
  return configMatch ? !g_disabledWindows.contains(window) : g_enabledWindows.contains(window);
}
template <typename Fn, typename... Args>
inline decltype(auto) callOriginal(CFunctionHook* hook, Args&&... args) {
  return reinterpret_cast<Fn>(hook->m_original)(std::forward<Args>(args)...);
}
// ============================================================================
// Hook Handlers
// ============================================================================
void hkSendPointerMotion(CSeatManager* seatManager, uint32_t timeMsec, const Vector2D& localCoords) {
  Vector2D newCoords = localCoords;
  const auto focusState = Desktop::focusState();
  const auto window = focusState ? focusState->window() : nullptr;
  if (window && isActive(window.get(), window->m_initialClass)) {
    const Vector2D windowSize = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto wlSurface = window->wlSurface();
    const auto surfaceResource = wlSurface ? wlSurface->resource() : nullptr;
    if (surfaceResource && surfaceResource->m_current.bufferSize.x > 0 && surfaceResource->m_current.bufferSize.y > 0) {
      newCoords.x *= (surfaceResource->m_current.bufferSize.x / windowSize.x) / window->m_X11SurfaceScaledBy;
      newCoords.y *= (surfaceResource->m_current.bufferSize.y / windowSize.y) / window->m_X11SurfaceScaledBy;
    }
  }
  callOriginal<FnPointerMotion>(g_pPointerMotionHook, seatManager, timeMsec, newCoords);
}
void hkXWaylandConfigure(CXWaylandSurface* surface, const CBox& box) {
  if (surface) {
    if (const auto surfaceResource = surface->m_surface.lock()) {
      if (const auto wlSurface = Desktop::View::CWLSurface::fromResource(surfaceResource)) {
        if (const auto window = Desktop::View::CWindow::fromView(wlSurface->view())) {
          if (isActive(window.get(), window->m_initialClass)) {
            wlSurface->m_fillIgnoreSmall = true;
            if (const auto CONFIG = findConfig(window->m_initialClass); CONFIG && CONFIG->res && g_initializedWindows.emplace(window.get()).second) {
              CBox newBox = box;
              newBox.w = CONFIG->res->x;
              newBox.h = CONFIG->res->y;
              callOriginal<FnXWaylandConfigure>(g_pXWaylandConfigureHook, surface, newBox);
              return;
            }
          }
        }
      }
    }
  }
  callOriginal<FnXWaylandConfigure>(g_pXWaylandConfigureHook, surface, box);
}
uint32_t hkXDGToplevelSetSize(CXDGToplevelResource* toplevel, const Vector2D& size) {
  Vector2D newSize = size;
  if (toplevel) {
    if (const auto window = toplevel->m_window.get()) {
      if (isActive(window, window->m_initialClass)) {
        if (window->wlSurface())
          window->wlSurface()->m_fillIgnoreSmall = true;
        if (const auto CONFIG = findConfig(window->m_initialClass); CONFIG && CONFIG->res && g_initializedWindows.emplace(window).second)
          return callOriginal<FnXDGToplevelSetSize>(g_pXDGToplevelSetSizeHook, toplevel, *CONFIG->res);
        const auto surfaceResource = window->wlSurface() ? window->wlSurface()->resource() : nullptr;
        if (surfaceResource && surfaceResource->m_current.size.x > 0 && surfaceResource->m_current.size.y > 0)
          newSize = surfaceResource->m_current.size;
      }
    }
  }
  return callOriginal<FnXDGToplevelSetSize>(g_pXDGToplevelSetSizeHook, toplevel, newSize);
}
void hkCalculateUVForSurface(Render::IElementRenderer* renderer, PHLWINDOW window, SP<CWLSurfaceResource> surface, PHLMONITOR monitor, bool main, const Vector2D& projSize, const Vector2D& projSizeUnscaled, bool fixMisalignedFSV1) {
  if (window && isActive(window.get(), window->m_initialClass)) {
    g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft = Vector2D(-1, -1);
    g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
    return;
  }
  callOriginal<FnCalculateUVForSurface>(g_pCalculateUVHook, renderer, window, surface, monitor, main, projSize, projSizeUnscaled, fixMisalignedFSV1);
}
CRegion hkWLSurfaceComputeDamage(Desktop::View::CWLSurface* wlSurface) {
  const auto damageRegion = callOriginal<FnWLSurfaceComputeDamage>(g_pWLSurfaceComputeDamageHook, wlSurface);
  if (wlSurface && wlSurface->exists()) {
    if (const auto window = Desktop::View::CWindow::fromView(wlSurface->view())) {
      if (isActive(window.get(), window->m_initialClass)) {
        if (const auto monitor = window->m_monitor.lock()) {
          g_pHyprRenderer->damageMonitor(monitor);
        } else {
          g_pHyprRenderer->damageWindow(window);
        }
      }
    }
  }
  return damageRegion;
}
static void refreshWindowSize(Desktop::View::CWindow* window) {
  if (const auto XDG = window->m_xdgSurface.lock()) {
    if (const auto TOPLEVEL = XDG->m_toplevel.lock())
      TOPLEVEL->setSize(window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT));
  } else if (const auto XWAY = window->m_xwaylandSurface.lock()) {
    XWAY->configure(window->getWindowMainSurfaceBox());
  }
}
static bool toggleFocusedWindow() {
  const auto focusState = Desktop::focusState();
  const auto window = focusState ? focusState->window() : nullptr;
  if (!window)
    return false;
  const std::string appClass = window->m_initialClass;
  const bool isOn = isActive(window.get(), appClass);
  if (isOn) {
    if (!g_enabledWindows.erase(window.get()))
      g_disabledWindows.insert(window.get());
    refreshWindowSize(window.get());
  } else {
    if (!g_disabledWindows.erase(window.get()))
      g_enabledWindows.insert(window.get());
  }
  if (const auto monitor = window->m_monitor.lock())
    g_pHyprRenderer->damageMonitor(monitor);
  else
    g_pHyprRenderer->damageWindow(window);
  return true;
}
SDispatchResult toggleDispatcher(std::string) {
  SDispatchResult result;
  if (!toggleFocusedWindow()) {
    result.success = false;
    result.error = "no focused window";
  }
  return result;
}
int toggleLua(lua_State* L) {
  if (!toggleFocusedWindow())
    return Config::Lua::Bindings::Internal::configError(L, "[hyprstretch] no focused window");
  return 0;
}
// ============================================================================
// Lua Binding & Plugin Lifecycle
// ============================================================================
int appLua(lua_State* L) {
  if (!lua_istable(L, 1))
    return Config::Lua::Bindings::Internal::configError(L, "[hyprstretch] Config: expected a table { class [, w, h] }");
  SAppConfig config;
  {
    Hyprutils::Utils::CScopeGuard scopeGuard([L] { lua_pop(L, 1); });
    lua_getfield(L, 1, "class");
    if (!lua_isstring(L, -1))
      return Config::Lua::Bindings::Internal::configError(L, "[hyprstretch] Config: class must be a regex string");
    try {
      config.regex = std::regex(lua_tostring(L, -1));
    } catch (const std::regex_error&) {
      return Config::Lua::Bindings::Internal::configError(L, "[hyprstretch] Config: invalid regex");
    }
  }
  Vector2D res;
  bool hasW = false;
  bool hasH = false;
  {
    Hyprutils::Utils::CScopeGuard scopeGuard([L] { lua_pop(L, 1); });
    lua_getfield(L, 1, "w");
    if (lua_isinteger(L, -1)) {
      res.x = lua_tointeger(L, -1);
      hasW = true;
    }
  }
  {
    Hyprutils::Utils::CScopeGuard scopeGuard([L] { lua_pop(L, 1); });
    lua_getfield(L, 1, "h");
    if (lua_isinteger(L, -1)) {
      res.y = lua_tointeger(L, -1);
      hasH = true;
    }
  }
  if (hasW != hasH)
    return Config::Lua::Bindings::Internal::configError(L, "[hyprstretch] Config: w and h must be provided together");
  if (hasW)
    config.res = res;
  g_appConfigs.emplace_back(std::move(config));
  return 0;
}
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  PHANDLE = handle;
  if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash()) {
    throw std::runtime_error("[hyprstretch] Version mismatch");
  }
  static auto preReloadListener = Event::bus()->m_events.config.preReload.listen([] {
    g_appConfigs.clear();
  });
  static auto windowCloseListener = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
    g_initializedWindows.erase(w.get());
    g_enabledWindows.erase(w.get());
    g_disabledWindows.erase(w.get());
  });
  HyprlandAPI::addDispatcherV2(PHANDLE, "toggle", ::toggleDispatcher);
  HyprlandAPI::addLuaFunction(PHANDLE, "hyprstretch", "toggle", ::toggleLua);
  HyprlandAPI::addLuaFunction(PHANDLE, "hyprstretch", "app", ::appLua);
  auto createHook = [](const std::string& name, const std::string& demangledContains, void* hookFn) -> CFunctionHook* {
    for (const auto& fn : HyprlandAPI::findFunctionsByName(PHANDLE, name)) {
      if (fn.demangled.contains(demangledContains)) {
        return HyprlandAPI::createFunctionHook(PHANDLE, fn.address, hookFn);
      }
    }
    return nullptr;
  };
  g_pPointerMotionHook = createHook("sendPointerMotion", "CSeatManager", (void*)::hkSendPointerMotion);
  g_pXWaylandConfigureHook = createHook("configure", "XWaylandSurface", (void*)::hkXWaylandConfigure);
  g_pXDGToplevelSetSizeHook = createHook("setSize", "CXDGToplevelResource", (void*)::hkXDGToplevelSetSize);
  g_pCalculateUVHook = createHook("calculateUVForSurface", "IElementRenderer", (void*)::hkCalculateUVForSurface);
  g_pWLSurfaceComputeDamageHook = createHook("computeDamage", "CWLSurface", (void*)::hkWLSurfaceComputeDamage);
  const std::array<CFunctionHook*, 5> hooks = {
      g_pPointerMotionHook,
      g_pXWaylandConfigureHook,
      g_pXDGToplevelSetSizeHook,
      g_pCalculateUVHook,
      g_pWLSurfaceComputeDamageHook};
  for (auto hook : hooks) {
    if (!hook || !hook->hook())
      throw std::runtime_error("[hyprstretch] Failed to initialize hooks");
  }
  return {"hyprstretch", "Resize windows without changing their resolution", "Rommmmaha", "1.0"};
}
APICALL EXPORT void PLUGIN_EXIT() {}
