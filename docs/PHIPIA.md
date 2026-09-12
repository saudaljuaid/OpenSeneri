<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Phipia

Phipia is Phipia's graphical environment. It starts after the kernel has
installed the framebuffer, input, timer, storage, and desktop services.

## Desktop and Dock

The desktop offers fourteen photographic scenes at 1024×768. Sources are
converted into a compact album during the build and validated before display.
Wallpaper restoration uses cached row copies so pointer and window movement do
not repaint the complete screen.

The 3D Dock contains Files, Terminal, Notes, Media Editor, Camera, Canvas, Store,
and Settings.
It uses fixed-point arithmetic for icon magnification, neighbor movement,
reflections, tooltips, press feedback, and launch bounce. Dark appearance
changes the shelf colour without changing its geometry or behavior.

## Windows

All eight Dock applications can remain open. Clicking a window raises it;
dragging the title bar moves it within the screen. The red control closes, the
violet control toggles maximized geometry, and the white/grey control minimizes
the window to its live Dock item.
Windows open from their Dock icon with a 16.16 fixed-point genie warp and
return along the same path when closed or minimized. The finished window is
captured once; each animation row is bounded, resampled, and driven by
monotonic time rather than frame count. Other open windows remain composed
behind it. The desktop repaints changed rectangles instead of the whole
framebuffer.

Interface text uses a build-time Inter atlas. The kernel reads a small
proportional bitmap format and does not include a TrueType engine.

## Applications

### Files

Files browses the writable FAT32 data volume. Folder traversal, refresh, file
creation, folder creation, opening documents, item counts, and free-space
reporting use the kernel filesystem interface.

### Notes

Notes reads and writes text files on the data volume. It supports printable
input, Enter, Backspace, search presentation, new-note creation, and `Ctrl-S`.
Saving replaces the target through a synchronized temporary file.

### Settings

Settings provides Appearance, Desktop, Dock, Displays, Keyboard, Pointer,
Performance, Network, Storage, Camera, Windows, and About pages. Desktop and
Appearance are interactive. Hardware pages report the current configuration
and mark unavailable facilities clearly.

### Camera

Camera has a preview, connection status, and shutter. A double-buffered RGB888
provider publishes complete 320×180 frames; capture writes the next available
`PHOTO00.BMP` through `PHOTO99.BMP` to the data volume.

The standard QEMU profile has no webcam or UVC transport, so Camera reports
`No camera connected`.

### Canvas

Canvas is a native ABI v1 process with an application-owned drawing surface,
Lucide tools, palette and brush sizing. It remains outside the kernel UI and
presents bounded damage rectangles through the public graphics contract.

### Store

Store is a white, Inter-based signed-catalog client with searchable navigation
and pinned Lucide icons. The built-in lifecycle catalog publishes SDL 2.32.10's
Chess Board proof as a real package. Its Install / Update action queues the
privileged Phip client, which authenticates the repository and payload before
the package service changes installed authority. Home, search, All
Applications, and Games expose that entry; empty categories, Installed,
Updates, Settings, and About report their bounded state directly.

### Terminal

Terminal exposes Phipia's shell, filesystem and networking commands, and the
measured BusyBox profiles. `fetch` displays the Phipia mark and basic system
information.

### Media Editor

Media Editor provides a source browser, viewer, inspector, timeline, tracks,
clips, and a playhead. It can import an uncompressed 24-bit BMP from the data
volume, trim and save a project, and export the selected frame as a 24-bit BMP
up to 320×180.

The vendored editor foundation contains the project model, timecode, render
graph, compositing, audio, LUT, EDL, mask, transition, and freestanding image.
Phipia's integrated workspace exposes the BMP workflow described above.

## Demo capture

`make capture-phipia` boots the production ISO with separate system and data
volumes. QMP sends pointer and keyboard input to the guest while the capture
opens applications, changes the Dock appearance and wallpaper, edits a note,
and uses Media Editor. Camera remains closed because the QEMU machine has no
camera source.

The same session saves the note and Media Editor project, exports a BMP, and
checks the retained data image after shutdown.

## Limits

Phipia has six integrated applications, six windows, one supported display
geometry, and printable-ASCII text. Its integrated Camera and Media Editor
workspaces use the bounded capture and BMP workflows described above. Native
applications use the separate window and input ABI.
