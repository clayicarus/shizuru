# Windows Build Instructions

## Prerequisites

- Flutter SDK 3.41+ (Dart 3.11+) installed and on `PATH`
- CMake 3.20+
- Visual Studio 2022 (MSVC 17+) with C++ Desktop workload

## Build Steps

### 1. Build the C++ bridge

From the repository root:

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --target shizuru_bridge
```

This produces `build\ui\bridge\Debug\shizuru_bridge.dll`.

### 2. Copy the DLL into the Flutter runner

```powershell
.\ui\windows\copy_bridge.ps1
```

Or manually:

```powershell
New-Item -ItemType Directory -Force ui\build\windows\x64\runner\Debug
Copy-Item build\ui\bridge\Debug\shizuru_bridge.dll ui\build\windows\x64\runner\Debug\
```

### 3. Install Flutter dependencies

```powershell
cd ui
flutter pub get
```

### 4. Run the Flutter app

```powershell
flutter run -d windows
```

---

## Release build

```powershell
cmake --build build --config Release --target shizuru_bridge
.\ui\windows\copy_bridge.ps1 -Config Release
cd ui && flutter build windows
```

---

## Troubleshooting

**`shizuru_bridge.dll` not found at runtime**
- Confirm step 2 was completed.
- The DLL must be in the same directory as `shizuru_ui.exe`.

**`cmake --build` fails**
- Dependencies (PortAudio, nlohmann/json, spdlog, httplib) are fetched
  automatically by CMake's FetchContent on first configure.
- Run `cmake -B build` again if CMake cache is stale.

**`flutter pub get` fails with SDK constraint error**
- Ensure Flutter 3.41+ / Dart 3.11+ is installed: `flutter --version`
