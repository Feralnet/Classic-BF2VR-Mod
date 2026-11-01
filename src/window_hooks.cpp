
#include "window_hooks.hpp"
#include "logger.hpp"
#include "user_config.hpp"
#include "vr/vr_system.hpp"

#include <atomic>

namespace sp {

namespace {

std::atomic_flag set_dpi_awareness{};
DWORD game_thread_id{};
std::atomic<HWND> game_window{};
std::atomic_flag logged_get_cursor_pos{};
std::atomic_flag logged_set_cursor_pos{};

}

void init_window_hook_game_thread_id(DWORD id) noexcept
{
   game_thread_id = id;
}

}

using namespace sp;

extern "C" HWND WINAPI CreateWindowExA_hook(DWORD dwExStyle, LPCSTR lpClassName,
                                            LPCSTR lpWindowName, DWORD dwStyle,
                                            int X, int Y, int nWidth, int nHeight,
                                            HWND hWndParent, HMENU hMenu,
                                            HINSTANCE hInstance, LPVOID lpParam)
{
   if (GetCurrentThreadId() == game_thread_id && user_config.enabled &&
       user_config.display.dpi_aware && !set_dpi_awareness.test_and_set()) {
      log(Log_level::info, "Setting game thread as Per Monitor Aware DPI aware."sv);

      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   }

   const HWND window = CreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle, X,
                                       Y, nWidth, nHeight, hWndParent, hMenu, hInstance,
                                       lpParam);

   if (window && !hWndParent && GetCurrentThreadId() == game_thread_id) {
      game_window = window;
   }

   return window;
}

extern "C" BOOL WINAPI GetCursorPos_hook(LPPOINT lpPoint)
{
   const HWND window = game_window.load();
   const auto pointer = vr::screen_pointer();

   if (!pointer || !window || !lpPoint) return GetCursorPos(lpPoint);

   RECT client{};
   GetClientRect(window, &client);

   POINT point{static_cast<LONG>(pointer->x * static_cast<float>(client.right - 1) + 0.5f),
               static_cast<LONG>(pointer->y * static_cast<float>(client.bottom - 1) + 0.5f)};
   ClientToScreen(window, &point);

   if (!logged_get_cursor_pos.test_and_set()) {
      log_fmt(Log_level::info, "VR: game reads the cursor via GetCursorPos ({}, {}).",
              point.x, point.y);
   }

   *lpPoint = point;

   return TRUE;
}

extern "C" BOOL WINAPI SetCursorPos_hook(int X, int Y)
{
   if (!logged_set_cursor_pos.test_and_set()) {
      log_fmt(Log_level::info, "VR: game calls SetCursorPos({}, {}).", X, Y);
   }

   return SetCursorPos(X, Y);
}