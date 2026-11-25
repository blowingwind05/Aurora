# Qt native desktop assembly

Use this entry point to build the Qt launcher and assemble it with existing runtime deployments. The launcher is named Aurora and the assembler scans the finished tree with tests/Check-NativeDesktopPackage.py before publication.

## Windows x64

Run the launcher build in a Visual Studio x64 developer PowerShell. Select the same Qt 5.15 or Qt 6 installation used for the deployed streaming runtime.

~~~powershell
cmake -S desktop/native-package -B build/native-win -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$qtPrefix"
cmake --build build/native-win --target AuroraQtShell
python desktop/Build-NativeDesktop.py windows --launcher build/native-win/bin/Aurora.exe --streamer-directory $streamer --sunshine-directory $sunshine --tailscale-directory $tailnet --output dist/Aurora-Windows-native
~~~

The three input directories are the deployed outputs of Build-Windows-Streamer.ps1, Build-Windows-Sunshine.ps1 and Build-Tailnet.ps1. The launcher is a GUI executable with the existing Aurora.manifest and icon embedded. The output requires a filesystem that supports hard links. The root launcher and Components/Moonlight streamer use the same Qt DLLs through app-local names. Root qt.conf points to the streamer's Qt plugins and QML modules. The native shell's QML and visual assets are staged from this checkout under QtPreview.

The output is a portable directory with Aurora.exe at its root. Installer and driver packaging belong to the existing release workflow.

## macOS

Select an existing integrated Aurora.app for the same architecture and Qt 6 SDK. The donor supplies the deployed streaming executable, Qt frameworks and plugins, bundled AuroraTailnet, and the AuroraHost helper with its existing bundle identity. Build the launcher with the matching architecture and deployment target from the existing Info.plist.

~~~sh
qt_prefix="/path/to/Qt/6.6.3/macos"
min_os="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' desktop/Info.plist)"
cmake -S desktop/native-package -B build/native-mac -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=$qt_prefix" -DCMAKE_OSX_ARCHITECTURES=arm64 "-DCMAKE_OSX_DEPLOYMENT_TARGET=$min_os"
cmake --build build/native-mac --config Release --target AuroraQtShell
python3 desktop/Build-NativeDesktop.py macos --launcher build/native-mac/bin/Aurora \
  --runtime-app "$runtime_app" --qt-bin "$qt_prefix/bin" --architecture arm64 \
  --output dist/macos-arm64-native/Aurora.app
~~~

For Intel builds, use x86_64 for CMAKE_OSX_ARCHITECTURES and x64 for the assembler. The new app uses the existing Info.plist and Aurora.icns. Qt is deployed with ad-hoc signing against Contents/MacOS/Aurora before the prebuilt signed host app is copied into Contents/Helpers. QtPreview lives in Contents/Resources. Apply the final outer bundle signature after assembly.

## Package checks

Each command requires a new output path. The assembler stages alongside that path, checks for managed binaries, and then publishes the completed directory.

~~~powershell
python tests/Test-NativeDesktopAssembly.py
python tests/Check-NativeDesktopPackage.py dist/Aurora-Windows-native
~~~

Test-NativeDesktopAssembly.py uses synthetic executable headers and a mock macOS deployment. For a built package, Aurora.exe --check-native or Aurora.app/Contents/MacOS/Aurora --check-native loads the bundled QML and exercises the in-process bridge in the existing check mode.
