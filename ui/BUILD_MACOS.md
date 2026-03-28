# macOS Build Instructions

## Prerequisites

- Flutter SDK installed and on `PATH`
- CMake 3.20+
- Xcode 14+
- CocoaPods (`gem install cocoapods`)

## Build Steps

### 1. Build the C++ bridge

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target shizuru_bridge
```

This produces `build/ui/bridge/libshizuru_bridge.dylib`.

### 2. Copy the dylib into the Flutter runner

Before running `flutter build macos`, copy the dylib into the Runner's
Frameworks directory so Xcode can embed it:

```bash
chmod +x ui/macos/copy_bridge.sh
ui/macos/copy_bridge.sh
```

Or manually:

```bash
mkdir -p ui/macos/Runner/Frameworks
cp build/ui/bridge/libshizuru_bridge.dylib ui/macos/Runner/Frameworks/
```

### 3. Build the Flutter app

```bash
cd ui
flutter build macos
```

The app will be at `ui/build/macos/Build/Products/Release/shizuru_ui.app`.

### 4. Run

```bash
open ui/build/macos/Build/Products/Release/shizuru_ui.app
```

---

## Automating the dylib copy in Xcode (one-time setup)

To have Xcode copy the dylib automatically on every build:

1. Open `ui/macos/Runner.xcworkspace` in Xcode
2. Select the **Runner** target → **Build Phases**
3. Click **+** → **New Run Script Phase**
4. Name it **Copy Shizuru Bridge**
5. Set the script body to:
   ```
   "${SRCROOT}/copy_bridge.sh"
   ```
6. Drag the phase to run **before** the **Embed Frameworks** phase

After this one-time setup, `flutter build macos` will invoke the script
automatically and the dylib will always be up to date in the bundle.

---

## Troubleshooting

**`libshizuru_bridge.dylib` not found at runtime**
- Ensure step 2 was completed before building the Flutter app.
- Check that the dylib is present in the `.app` bundle:
  ```bash
  ls ui/build/macos/Build/Products/Release/shizuru_ui.app/Contents/Frameworks/
  ```

**`cmake --build` fails**
- Make sure all dependencies (PortAudio, nlohmann/json, etc.) are available.
  They are fetched automatically by CMake's FetchContent on first configure.

**`flutter build macos` fails with signing errors**
- Open `ui/macos/Runner.xcworkspace` and set a valid development team in
  **Runner → Signing & Capabilities**.
