# Folder `resources/`

Application icons and operating-system integration files. Installed to `bin/resources/` (the Linux
script to `bin/`).

| File | Purpose |
|---|---|
| `ungine.png` | 256 × 256 application icon (Linux menu entry and file-type icon) |
| `ungine.ico` | Windows icon (executables, installer, `.ungineproj` files) |
| `windows/ungine.rc` | Resource script that embeds `ungine.ico` into UngineEditor, UnginePlayer and Sandbox |
| `linux/ungine-editor.desktop.in` | Desktop entry template; `@BIN@` is replaced with the install's `bin/` path |
| `linux/ungine-mime.xml` | MIME type `application/x-ungine-project` for `*.ungineproj` |
| `linux/install-desktop-integration.sh` | Registers the menu entry, icons and file type for the current user (XDG data directory) and makes UngineEditor the default handler |

- The window icon is drawn procedurally at runtime (`EngineIcon` in `Core/Window`) with the same
  design as these files; change both together.
- On Windows the NSIS installer registers the `.ungineproj` association itself
  (`cmake/Package.cmake`); the Linux script is the counterpart for the TGZ package. Run it from the
  unpacked `bin/` folder:

  ```
  ./install-desktop-integration.sh
  ```
