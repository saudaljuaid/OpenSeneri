<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native graphics and input

A process with `window` capability submits one bounded create record containing
an ASCII title, content width and height, and pixel format. Version 1 supports
xRGB8888 content up to 1280×720. Success returns a window handle, an event-queue
handle, and a process-local RW/NX surface address with explicit width, height,
and byte stride.

The application owns only content pixels. Phipia owns chrome, focus, stacking,
movement, close/maximize/minimize controls, Dock behavior, animation, and
composition. A maximized native surface is scaled into the larger content
area and pointer coordinates are mapped back into its original geometry. The
same compositor snapshots a completed native window for the bounded Dock genie
animation; the process surface remains unchanged and is never writable by the
animation path. The surface is never a mapping of physical framebuffer memory.
Presentation names
one to eight checked damage rectangles. The kernel copies only bounded damaged
rows into the compositor shadow and accounts presented pixels; there is no
per-pixel syscall and a small update does not trigger an application-sized
full copy.

The bounded 64-entry queue reports key press/release/repeat, pointer movement,
pointer buttons, focus, close, and overflow. When full, the newest ordinary
event is dropped and one overflow event is retained for later delivery. Input
events require `input`; focus and close remain available to every window owner.
Pointer capture is explicit and ends on release, close, fault, or process exit.

Applications use `WAIT_READABLE` on the queue. Waiting parks the native thread,
so another application continues rendering while input is absent. Closing the
window or queue invalidates that handle immediately; the surface and Phipia
slot disappear when both object references are gone or during process cleanup.
