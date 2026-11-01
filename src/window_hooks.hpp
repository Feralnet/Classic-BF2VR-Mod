#pragma once

#include <Windows.h>

namespace sp {

void init_window_hook_game_thread_id(DWORD id) noexcept;

}

extern "C" HWND WINAPI CreateWindowExA_hook(DWORD dwExStyle, LPCSTR lpClassName,
                                            LPCSTR lpWindowName, DWORD dwStyle,
                                            int X, int Y, int nWidth, int nHeight,
                                            HWND hWndParent, HMENU hMenu,
                                            HINSTANCE hInstance, LPVOID lpParam);

// In VR menus the cursor is where the right controller points on the virtual screen.
extern "C" BOOL WINAPI GetCursorPos_hook(LPPOINT lpPoint);

extern "C" BOOL WINAPI SetCursorPos_hook(int X, int Y);