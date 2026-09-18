# Relative mouse capture

First-person and free-look projects should not call `DisableCursor` and `GetMouseDelta` separately.
Window focus and display-mode changes can rebase raylib's cursor coordinates between those calls,
turning a desktop-position jump into camera motion.

Keep one zero-initialized `CoreMouseCapture` in application state and update it once per rendered
frame:

```c
CoreMouseCapture mouse = {0};

Vector2 look = CoreMouseCaptureUpdate(&mouse, playing && !paused);
camera.yaw -= look.x * sensitivity;
camera.pitch -= look.y * sensitivity;
```

The helper uses raylib's locked relative-pointer mode while the window is focused. It releases the
cursor on focus loss or when capture is not requested, then reacquires it and discards cursor-rebase
frames after focus, fullscreen, and window-size transitions. It does not clamp legitimate fast mouse
motion. Call `CoreMouseCaptureRelease(&mouse)` before closing the window.

Only one subsystem may own the window cursor. Menus and gameplay should express their desired mode
through the `requested` argument rather than calling raylib cursor functions independently.
