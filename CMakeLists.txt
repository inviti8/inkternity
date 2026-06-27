cmake_minimum_required(VERSION 3.22.0)  # Android SDK Manager's stable CMake (3.22.1) needs to satisfy this; desktop's Conan cmake/3.27.0 still works.
set(CMAKE_CXX_STANDARD 23)
project(infinipaint)

# Single source of truth for the build's version triple. Defaults match
# src/VersionConstants.hpp::CURRENT_VERSION_STRING; the release workflow
# overrides these via -DCPACK_PACKAGE_VERSION_* at configure time
# (parsed from the git tag, e.g. v0.11.0-rc15 → MAJ=0 MIN=11 PATCH=0).
# Used downstream by both CPack (Windows NSIS, macOS DragNDrop) and the
# macOS .app Info.plist (MACOSX_BUNDLE_*).
if(NOT DEFINED CPACK_PACKAGE_VERSION_MAJOR)
    set(CPACK_PACKAGE_VERSION_MAJOR "0")
endif()
if(NOT DEFINED CPACK_PACKAGE_VERSION_MINOR)
    set(CPACK_PACKAGE_VERSION_MINOR "11")
endif()
if(NOT DEFINED CPACK_PACKAGE_VERSION_PATCH)
    set(CPACK_PACKAGE_VERSION_PATCH "0")
endif()
set(INKTERNITY_VERSION_STRING "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
# Force CPACK_PACKAGE_VERSION explicitly so CPack doesn't auto-compose
# something like "0.11.0.0.0" (which we saw in v0.11.0-rc15 — CPack
# appended phantom TWEAK and PROJECT_VERSION components when only
# MAJ/MIN/PATCH were passed). Also feeds Apple's CFBundleVersion via
# the configured Info.plist; Apple's notary service rejects malformed
# version strings.
set(CPACK_PACKAGE_VERSION "${INKTERNITY_VERSION_STRING}")

# Android ships only Release-built native deps (Skia etc), even for
# Debug app builds — see android-project/app/build.gradle's conanInstall
# task. Conan-generated imported targets gate their include dirs and
# link libs on $<CONFIG:Release>, so a Debug consumer sees empty
# INTERFACE_INCLUDE_DIRECTORIES from those targets. Mapping Debug
# imports to Release fixes propagation without rebuilding all natives
# for Debug.
if(ANDROID)
    set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options to set
set(availableGraphicsBackends OpenGL3.3 OpenGL2.1 OpenGLES3.0 Vulkan)
set(GRAPHICS_BACKEND OpenGL3.3 CACHE STRING "Graphics backend to use")

set_property(CACHE GRAPHICS_BACKEND PROPERTY STRINGS ${availableGraphicsBackends})
list(FIND availableGraphicsBackends ${GRAPHICS_BACKEND} graphicsBackendIndex)
if(graphicsBackendIndex EQUAL -1)
    message(FATAL_ERROR "Graphics backend must be one of: ${availableGraphicsBackends}")
endif()

option(USE_GRAPHITE "Use Skia Graphite Backend (only works for Vulkan graphics backend)" OFF)

option(USE_ASAN "Enable Address Sanitizer" OFF)
option(USE_UBSAN "Enable Undefined Behaviour Sanitizer" OFF)
option(USE_LEAKSAN "Enable Leak Sanitizer" OFF)
option(USE_THREADSAN "Enable Thread Sanitizer" OFF)
option(USE_MEMORYSAN "Enable Memory Sanitizer" OFF)

option(NO_CONAN_BUILD "Not using conan to get packages" OFF)
option(CONFIG_NEXT_TO_EXECUTABLE "Place configuration folder next to executable (portable executable)" OFF)
option(MACOS_MAKE_BUNDLE "Package the application as a bundle" ON)

# Setting sources
set(sources "src/main.cpp"
            "src/SwitchCWD.cpp"
            "src/MainProgram.cpp"
            "src/AndroidJNICalls.cpp"
            "src/DrawData.cpp"
            "src/FontData.cpp"
            "src/ClientData.cpp"
            "src/GridManager.cpp"
            "src/WorldGrid.cpp"
            "src/TimePoint.cpp"
            "src/InputManager.cpp"
            "src/UndoManager.cpp"
            "src/CanvasTheme.cpp"
            "src/DrawCamera.cpp"
            "src/DrawCollision.cpp"
            "src/CoordSpaceHelper.cpp"
            "src/CoordSpaceHelperTransform.cpp"
            "src/ResourceManager.cpp"
            "src/Toolbar.cpp"
            "src/World.cpp"
            "src/WorldScreenshot.cpp"
            "src/FileHelpers.cpp"
            "src/VersionConstants.cpp"
            "src/ScaleUpCanvas.cpp"
            "src/WorldUndoManager.cpp"
            "src/GUIHolder.cpp"
            "src/GlobalConfig.cpp"
            "src/DevKeys.cpp"
            "src/PublishedCanvases.cpp"
            "src/Distribution/ProcessTests.cpp"
            "src/Distribution/HostOnly.cpp"
            "src/Distribution/SideInstances.cpp"
            "src/Subscription/TokenVerifier.cpp"
            "src/C2PA/AppCa.cpp"
            "src/C2PA/Bundle.cpp"
            "src/C2PA/KeyStore.cpp"
            "src/C2PA/LeafIssuer.cpp"
            "src/C2PA/Manifest.cpp"
            "src/C2PA/PublishHook.cpp"
            "src/C2PA/QrCodeRender.cpp"
            "src/C2PA/RegistrationFlow.cpp"
            "src/C2PA/Registry.cpp"
            "src/C2PA/SorobanSubmit.cpp"
            "src/C2PA/StellarCli.cpp"
            "src/C2PA/Verifier.cpp"
            "src/C2PA/WalletPanel.cpp"
            "src/C2PA/WireToken.cpp"
            "deps/qrcodegen/qrcodegen.cpp"
            "deps/tweetnacl/tweetnacl.c"
            "deps/tweetnacl/randombytes.cpp"
            "src/crypto/stellar/StellarStrkey.cpp"
            "src/crypto/stellar/StellarMnemonic.cpp"
            "src/crypto/stellar/trezor/bip39.c"
            "src/crypto/stellar/trezor/bip39_english.c"
            "src/crypto/stellar/trezor/hmac.c"
            "src/crypto/stellar/trezor/pbkdf2.c"
            "src/crypto/stellar/trezor/sha2.c"
            "src/crypto/stellar/trezor/memzero.c"
            "src/RichTextUserInput.cpp"
            "src/CustomEvents.cpp"
            "src/Screens/Screen.cpp"
            "src/Screens/DrawingProgramScreen.cpp"
            "src/Screens/DesktopDrawingProgramScreen.cpp"
            "src/Screens/PhoneDrawingProgramScreen.cpp"
            "src/Screens/FileSelectScreen.cpp"
            "src/Bookmarks/BookmarkListItem.cpp"
            "src/Bookmarks/BookmarkManager.cpp"
            "src/ResourceDisplay/ResourceDisplay.cpp"
            "src/ResourceDisplay/SvgResourceDisplay.cpp"
            "src/ResourceDisplay/ImageResourceDisplay.cpp"
            "src/ResourceDisplay/FileResourceDisplay.cpp"
            "src/RichText/TextBox.cpp"
            "src/RichText/TextStyleModifier.cpp"
            "src/RichText/ParagraphStyleData.cpp"
            "src/DrawingProgram/ToolConfiguration.cpp"
            "src/DrawingProgram/DrawingProgramSelection.cpp"
            "src/DrawingProgram/DrawingProgramCache.cpp"
            "src/DrawingProgram/DrawingProgram.cpp"
            "src/DrawingProgram/Tools/BrushTool.cpp"
            "src/DrawingProgram/Tools/ParticleBrushTool.cpp"
            "src/DrawingProgram/Tools/LineDrawTool.cpp"
            "src/DrawingProgram/Tools/TextBoxTool.cpp"
            "src/DrawingProgram/Tools/EraserTool.cpp"
            "src/DrawingProgram/Tools/RectSelectTool.cpp"
            "src/DrawingProgram/Tools/LassoSelectTool.cpp"
            "src/DrawingProgram/Tools/RectDrawTool.cpp"
            "src/DrawingProgram/Tools/EllipseDrawTool.cpp"
            "src/DrawingProgram/Tools/EyeDropperTool.cpp"
            "src/DrawingProgram/Tools/ScreenshotTool.cpp"
            "src/DrawingProgram/Tools/EditTool.cpp"
            "src/DrawingProgram/Tools/ZoomCanvasTool.cpp"
            "src/DrawingProgram/Tools/PanCanvasTool.cpp"
            "src/DrawingProgram/Tools/GridModifyTool.cpp"
            "src/DrawingProgram/Tools/DrawingProgramToolBase.cpp"
            "src/DrawingProgram/Tools/EditTools/DrawingProgramEditToolBase.cpp"
            "src/DrawingProgram/Tools/EditTools/ImageEditTool.cpp"
            "src/DrawingProgram/Tools/EditTools/EllipseDrawEditTool.cpp"
            "src/DrawingProgram/Tools/EditTools/RectDrawEditTool.cpp"
            "src/DrawingProgram/Tools/EditTools/TextBoxEditTool.cpp"
            "src/DrawingProgram/Tools/EditTools/BrushEditTool.cpp"
            "src/DrawingProgram/Layers/DrawingProgramLayerManagerGUI.cpp"
            "src/DrawingProgram/Layers/DrawingProgramLayerManager.cpp"
            "src/DrawingProgram/Layers/DrawingProgramLayerListItem.cpp"
            "src/DrawingProgram/Layers/DrawingProgramLayerFolder.cpp"
            "src/DrawingProgram/Layers/DrawingProgramLayer.cpp"
            "src/DrawingProgram/Layers/SerializedBlendMode.cpp"
            "src/CanvasComponents/CanvasComponentContainer.cpp"
            "src/CanvasComponents/CanvasComponentAllocator.cpp"
            "src/CanvasComponents/BrushStrokeCanvasComponent.cpp"
            "src/CanvasComponents/RectangleCanvasComponent.cpp"
            "src/CanvasComponents/TextBoxCanvasComponent.cpp"
            "src/CanvasComponents/EllipseCanvasComponent.cpp"
            "src/CanvasComponents/ImageCanvasComponent.cpp"
            "src/CanvasComponents/ParticleCanvasComponent.cpp"
            "src/CanvasComponents/Particles/LegacyFxLibrary.cpp"
            "src/CanvasComponents/Particles/LegacyFxRenderer.cpp"
            "src/CanvasComponents/Particles/FxLibraryStore.cpp"
            "src/CanvasComponents/CanvasComponent.cpp"
            "src/Brushes/BrushPresets.cpp"
            "src/Brushes/LibMyPaintBridgeTest.cpp"
            "src/Brushes/LibMyPaintSkiaSurface.cpp"
            "src/Brushes/LibMyPaintStrokeTest.cpp"
            "src/Brushes/MyPaintBrushParams.cpp"
            "src/Brushes/UserBrushPresets.cpp"
            "src/BrushCustomizationDrawer.cpp"
            "src/SavedPresetsDrawer.cpp"
            "src/AvatarStore.cpp"
            "src/StrokeVectorize/SchneiderFit.cpp"
            "src/Waypoints/Waypoint.cpp"
            "src/Waypoints/Edge.cpp"
            "src/Waypoints/WaypointGraph.cpp"
            "src/Waypoints/TreeView.cpp"
            "src/ReaderMode/ReaderMode.cpp"
            "src/Audio/AudioEngine.cpp"
            "deps/miniaudio/miniaudio.c"
            "src/CanvasComponents/MyPaintLayerCanvasComponent.cpp"
            "src/CanvasComponents/WaypointCanvasComponent.cpp"
            "src/DrawingProgram/Tools/MyPaintBrushTool.cpp"
            "src/DrawingProgram/Tools/WaypointTool.cpp"
            "src/DrawingProgram/Tools/ButtonSelectTool.cpp"
            "src/DrawingProgram/Tools/StrokeVectorizeTool.cpp"
            "src/DrawingProgram/RasterFlatten.cpp"
            "src/DrawingProgram/RasterResolution.cpp"
            "src/Armature/ArmatureSpike.cpp"
            "src/Armature/ArmatureModel.cpp"
            "src/Armature/ArmatureModalScreen.cpp"
            "src/DrawingProgram/Tools/SquareCanvasCaptureTool.cpp"
            "src/GUIStuff/GUIManager.cpp"
            "src/GUIStuff/GUIFloatAnimation.cpp"
            "src/GUIStuff/GUIManagerID.cpp"
            "src/GUIStuff/ClayImplement.cpp"
            "src/GUIStuff/Elements/GUIStuffHelpers.cpp"
            "src/GUIStuff/Elements/Element.cpp"
            "src/GUIStuff/Elements/AlphaSlider.cpp"
            "src/GUIStuff/Elements/HueVerticalSlider.cpp"
            "src/GUIStuff/Elements/SaturationValueSquare.cpp"
            "src/GUIStuff/Elements/ImageDisplay.cpp"
            "src/GUIStuff/Elements/MemoryImageDisplay.cpp"
            "src/GUIStuff/Elements/MutableTextLabel.cpp"
            "src/GUIStuff/Elements/SVGIcon.cpp"
            "src/GUIStuff/Elements/RadioButton.cpp"
            "src/GUIStuff/Elements/CheckBox.cpp"
            "src/GUIStuff/Elements/SelectableButton.cpp"
            "src/GUIStuff/Elements/MovableTabList.cpp"
            "src/GUIStuff/Elements/PaintCircleMenu.cpp"
            "src/GUIStuff/Elements/RotateWheel.cpp"
            "src/GUIStuff/Elements/TextParagraph.cpp"
            "src/GUIStuff/Elements/FontPicker.cpp"
            "src/GUIStuff/Elements/TreeListing.cpp"
            "src/GUIStuff/Elements/LayoutElement.cpp"
            "src/GUIStuff/Elements/ColorRectangleDisplay.cpp"
            "src/GUIStuff/Elements/ScrollArea.cpp"
            "src/GUIStuff/Elements/ManyElementScrollArea.cpp"
            "src/GUIStuff/Elements/PositionAdjustingPopupMenu.cpp"
            "src/GUIStuff/Elements/ColorPickerShaders.cpp"
            "src/GUIStuff/Elements/GridScrollArea.cpp"
            "src/GUIStuff/ElementHelpers/PopupHelpers.cpp"
            "src/GUIStuff/ElementHelpers/ButtonHelpers.cpp"
            "src/GUIStuff/ElementHelpers/LayoutHelpers.cpp"
            "src/GUIStuff/ElementHelpers/TextBoxHelpers.cpp"
            "src/GUIStuff/ElementHelpers/TextLabelHelpers.cpp"
            "src/GUIStuff/ElementHelpers/RadioButtonHelpers.cpp"
            "src/GUIStuff/ElementHelpers/CheckBoxHelpers.cpp"
            "include/Helpers/Networking/NetLibrary.cpp"
            "include/Helpers/Networking/NetServer.cpp"
            "include/Helpers/Networking/NetClient.cpp"
            "include/Helpers/Networking/ByteStream.cpp"
            "include/Helpers/NetworkingObjects/DelayUpdateSerializedClassManager.cpp"
            "include/Helpers/NetworkingObjects/NetObjManagerTypeList.cpp"
            "include/Helpers/NetworkingObjects/NetObjManager.cpp"
            "include/Helpers/NetworkingObjects/NetObjID.cpp"
            "include/Helpers/Hashes.cpp"
            "include/Helpers/CanvasShareId.cpp"
            "include/Helpers/BezierEasing.cpp"
            "include/Helpers/Random.cpp"
            "include/Helpers/Logger.cpp"
            "include/Helpers/StringHelpers.cpp"
            "include/Helpers/Serializers.cpp"
            "include/Helpers/FileDownloader.cpp"
            "include/Helpers/FileDownloaderEmscripten.cpp"
            "include/Helpers/VersionNumber.cpp"
        )

if((NOT EMSCRIPTEN) AND (GRAPHICS_BACKEND STREQUAL "Vulkan"))
    list(APPEND sources
        "src/VulkanContext/VkTestMemoryAllocator.cpp"
        "src/VulkanContext/VkTestUtils.cpp"
        "src/VulkanContext/WindowContext.cpp"
        "src/VulkanContext/VulkanWindowContext.cpp"
    )
    if(USE_GRAPHITE)
        list(APPEND sources
            "src/VulkanContext/GraphiteNativeVulkanWindowContext.cpp"
            "src/VulkanContext/GraphiteToolUtils.cpp"
        )
    endif()
endif()

if(WIN32)
    list(APPEND sources 
        "src/WindowsFontData/CustomFontSetManager.cpp"
        "windowsinstall/inkternity.manifest"
        "windowsinstall/app.rc")
endif()

# Get libraries
find_package(ICU 77.1 REQUIRED COMPONENTS uc)

if(NO_CONAN_BUILD)
    set(SKIA_INCLUDE "modules/skia" CACHE PATH "Path to Skia root directory")
    set(SKIA_LIB_PATH "modules/skia/out/Static" CACHE PATH "Path to Skia libs")
    find_library(SKIA_LIB skia PATHS ${SKIA_LIB_PATH})
    find_library(SKIA_SVG_LIB svg PATHS ${SKIA_LIB_PATH})
    find_library(SKIA_RESOURCES_LIB skresources PATHS ${SKIA_LIB_PATH})
    find_library(SKIA_PARAGRAPH_LIB skparagraph PATHS ${SKIA_LIB_PATH})
    find_library(SKIA_SHAPER_LIB skshaper PATHS ${SKIA_LIB_PATH})
    find_library(SKIA_UNICODE_ICU_LIB skunicode_icu PATHS ${SKIA_LIB_PATH})
    find_library(SKIA_UNICODE_CORE_LIB skunicode_core PATHS ${SKIA_LIB_PATH})

    find_package(Freetype REQUIRED)
    find_package(Fontconfig REQUIRED)
    find_package(PNG REQUIRED)
    find_package(ZLIB REQUIRED)
else()
    find_package(skia-infinipaint REQUIRED)
endif()

if(NOT EMSCRIPTEN)
    if(NOT ANDROID)
        find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3-shared)
    endif()
    find_package(LibDataChannel REQUIRED)
    find_package(CURL REQUIRED)
    find_package(harfbuzz REQUIRED)
    # C2PA app-CA + leaf-cert X.509 build/sign (docs/design/C2PA.md §8.1).
    # OpenSSL is also pulled transitively by libcurl on every native target;
    # the explicit find_package gives us OpenSSL::Crypto for the new TU.
    find_package(OpenSSL REQUIRED)
    # Conan's openssl/3.6.2 recipe ships headers at
    # <package_folder>/include but leaves cpp_info.components[*].includedirs
    # empty, so OpenSSL::Crypto's INTERFACE_INCLUDE_DIRECTORIES is empty
    # too and #include <openssl/*.h> fails to resolve. Attach the package
    # include dir manually — surgical workaround, applies only on the
    # native-target branch where find_package(OpenSSL) ran.
    if(TARGET OpenSSL::Crypto AND DEFINED openssl_PACKAGE_FOLDER_RELEASE)
        set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
            INTERFACE_INCLUDE_DIRECTORIES "${openssl_PACKAGE_FOLDER_RELEASE}/include")
    endif()
else()
    find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3-static)
endif()

if((NOT EMSCRIPTEN) AND (NOT APPLE) AND (NOT ANDROID))
    find_package(TBB REQUIRED)
endif()

if((NOT (GRAPHICS_BACKEND STREQUAL "Vulkan")) AND (NOT EMSCRIPTEN) AND LINUX AND (NOT NO_CONAN_BUILD))
    find_package(egl)
endif()

find_package(zstd REQUIRED)

message("Skia root: " ${SKIA_INCLUDE})
message("Build type: " ${CMAKE_BUILD_TYPE})

if(ANDROID)
    add_library(main SHARED ${sources})
else()
    add_executable(main ${sources})
endif()

if(NOT ANDROID)
    set_target_properties(main PROPERTIES OUTPUT_NAME "inkternity")
endif()

if(WIN32)
    set_target_properties(main PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    if(MACOS_MAKE_BUNDLE)
        target_compile_definitions(main PRIVATE MACOS_MAKE_BUNDLE)
        set_target_properties(main PROPERTIES MACOSX_BUNDLE TRUE)
        # Generate Info.plist from the template, substituting
        # @INKTERNITY_VERSION_STRING@ with the version triple. CMake copies
        # MACOSX_BUNDLE_INFO_PLIST verbatim into the .app, so we point it
        # at the configured copy rather than the source template.
        configure_file(
            "${CMAKE_SOURCE_DIR}/macosinstall/Info.plist"
            "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
            @ONLY
        )
        set_target_properties(main PROPERTIES
            BUNDLE True
            MACOSX_BUNDLE_GUI_IDENTIFIER com.inkternity.inkternity
            MACOSX_BUNDLE_BUNDLE_NAME inkternity
            MACOSX_BUNDLE_BUNDLE_VERSION "${INKTERNITY_VERSION_STRING}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${INKTERNITY_VERSION_STRING}"
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
        )
        add_custom_command(TARGET main POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/data"
            "$<TARGET_FILE_DIR:main>/../Resources/data"
        )
        add_custom_command(TARGET main POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/macosinstall/appicon.icns"
            "$<TARGET_FILE_DIR:main>/../Resources/appicon.icns"
        )
    endif()
endif()

if(EMSCRIPTEN)
    set(SDL_EMSCRIPTEN_PERSISTENT_PATH "/inkternity" CACHE STRING "SDL emscripten persistent path")

    if(CMAKE_BUILD_TYPE STREQUAL Debug)
        target_compile_options(main PRIVATE -g3)
        target_link_options(main PRIVATE -g3)
    else()
        target_compile_options(main PRIVATE -O3)
        target_link_options(main PRIVATE -O3)
    endif()

    target_compile_options(main PRIVATE -Wall -sUSE_HARFBUZZ=1 -fdiagnostics-color -sUSE_SDL=0 -include functional -fexceptions -pthread)
    target_link_options(main PRIVATE --emrun -sUSE_HARFBUZZ=1 -sUSE_SDL=0 -sFULL_ES2 -sUSE_WEBGL2=1 -sFETCH --embed-file data -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_RUNTIME_METHODS=ccall,HEAPU8,HEAPU32 -sEXPORTED_FUNCTIONS=[_main,_malloc,_free] -sMAXIMUM_MEMORY=4GB -sSINGLE_FILE -lidbfs.js -fexceptions -pthread) # -sINITIAL_MEMORY=67108864 
    set(CMAKE_EXECUTABLE_SUFFIX ".js")
    message("In Emscripten Mode")
endif()

if(USE_ASAN)
    target_compile_options(main PRIVATE -fsanitize=address)
    target_link_options(main PRIVATE -fsanitize=address)
    message("Using ASAN")
endif()
if(USE_UBSAN)
    target_compile_options(main PRIVATE -fsanitize=undefined)
    target_link_options(main PRIVATE -fsanitize=undefined)
    message("Using UBSan")
endif()
if(USE_LEAKSAN)
    target_compile_options(main PRIVATE -fsanitize=leak)
    target_link_options(main PRIVATE -fsanitize=leak)
    message("Using LeakSan")
endif()
if(USE_THREADSAN)
    target_compile_options(main PRIVATE -fsanitize=thread)
    target_link_options(main PRIVATE -fsanitize=thread)
    message("Using ThreadSan")
endif()
if(USE_MEMORYSAN)
    target_compile_options(main PRIVATE -fsanitize=memory)
    target_link_options(main PRIVATE -fsanitize=memory)
    message("Using MemSan")
endif()

if(NOT WIN32)
    if(CMAKE_BUILD_TYPE STREQUAL Debug)
        target_compile_options(main PRIVATE -g -Wall -fdiagnostics-color)
        message("In Debug Mode")
    else()
        target_compile_options(main PRIVATE -O3 -Wall -fdiagnostics-color)
        target_link_options(main PRIVATE -O3) # Other things to consider: -ffast-math and -march=native
        message("In Release Mode")
    endif()
endif()

if(MSVC)
    # Emit PDB symbols for all configs so post-mortem .dmp files decode to
    # named functions + line numbers. /OPT:REF and /OPT:ICF preserve the
    # usual Release size/speed savings; /DEBUG just adds a sibling .pdb.
    target_compile_options(main PRIVATE /Zi)
    target_link_options(main PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

target_include_directories(main PRIVATE "include")
target_include_directories(main PRIVATE "deps/c2pa")
target_include_directories(main PRIVATE "deps/miniaudio")
target_include_directories(main PRIVATE "deps/qrcodegen")
target_include_directories(main PRIVATE "deps/cgltf")  # PHASE9: glTF loader (header-only)

# c2pa-rs prebuilt C ABI (docs/design/C2PA.md §8.3 revised). ContentAuth
# publishes per-platform prebuilt zips on github.com/contentauth/c2pa-rs/
# releases; we download once on first configure into build/c2pa-runtime/
# and link against the platform-specific import lib + shared lib. Header
# is vendored under deps/c2pa/ so a clean checkout builds without
# requiring the download to succeed at the moment cmake runs.
set(C2PA_RS_VERSION "0.84.1")
set(C2PA_RUNTIME_DIR "${CMAKE_BINARY_DIR}/c2pa-runtime")
if(NOT EMSCRIPTEN)
    # Map CMake target to the Rust target triple ContentAuth ships
    # release assets under.
    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
            set(C2PA_RS_TRIPLE "aarch64-pc-windows-msvc")
        else()
            set(C2PA_RS_TRIPLE "x86_64-pc-windows-msvc")
        endif()
        set(C2PA_RS_SHLIB    "c2pa_c.dll")
        set(C2PA_RS_IMPLIB   "c2pa_c.dll.lib")
    elseif(APPLE)
        # Universal binary covers Intel + Apple Silicon in one zip.
        set(C2PA_RS_TRIPLE "universal-apple-darwin")
        set(C2PA_RS_SHLIB  "libc2pa_c.dylib")
        set(C2PA_RS_IMPLIB "libc2pa_c.dylib")
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(C2PA_RS_TRIPLE "aarch64-unknown-linux-gnu")
        else()
            set(C2PA_RS_TRIPLE "x86_64-unknown-linux-gnu")
        endif()
        set(C2PA_RS_SHLIB  "libc2pa_c.so")
        set(C2PA_RS_IMPLIB "libc2pa_c.so")
    endif()
    set(C2PA_RS_SHLIB_PATH  "${C2PA_RUNTIME_DIR}/lib/${C2PA_RS_SHLIB}")
    set(C2PA_RS_IMPLIB_PATH "${C2PA_RUNTIME_DIR}/lib/${C2PA_RS_IMPLIB}")
    if(NOT EXISTS "${C2PA_RS_SHLIB_PATH}")
        set(C2PA_RS_URL
            "https://github.com/contentauth/c2pa-rs/releases/download/c2pa-v${C2PA_RS_VERSION}/c2pa-v${C2PA_RS_VERSION}-${C2PA_RS_TRIPLE}.zip")
        message(STATUS "Fetching c2pa-rs prebuilt for ${C2PA_RS_TRIPLE} from ${C2PA_RS_URL}")
        set(C2PA_RS_ZIP "${CMAKE_BINARY_DIR}/c2pa-rs-${C2PA_RS_VERSION}.zip")
        file(DOWNLOAD "${C2PA_RS_URL}" "${C2PA_RS_ZIP}"
             SHOW_PROGRESS STATUS C2PA_RS_DL_STATUS)
        list(GET C2PA_RS_DL_STATUS 0 C2PA_RS_DL_CODE)
        if(NOT C2PA_RS_DL_CODE EQUAL 0)
            list(GET C2PA_RS_DL_STATUS 1 C2PA_RS_DL_MSG)
            message(FATAL_ERROR "c2pa-rs download failed: ${C2PA_RS_DL_MSG}")
        endif()
        file(MAKE_DIRECTORY "${C2PA_RUNTIME_DIR}")
        file(ARCHIVE_EXTRACT INPUT "${C2PA_RS_ZIP}"
             DESTINATION "${C2PA_RUNTIME_DIR}")
    endif()
    target_link_libraries(main "${C2PA_RS_IMPLIB_PATH}")
    # Tell c2pa.h's cbindgen-generated decls to use dllimport linkage
    # on Windows so the linker resolves through the import lib.
    if(WIN32)
        target_compile_definitions(main PRIVATE C2PA_DLL=0)
    endif()
    # Copy the shared lib next to inkternity.exe so it's on the
    # default DLL search path at run time. POSIX equivalents land
    # in $<TARGET_FILE_DIR:main>; macOS bundling step (DragNDrop)
    # picks them up via the same path. add_custom_command runs every
    # build (cheap copy_if_different).
    add_custom_command(TARGET main POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${C2PA_RS_SHLIB_PATH}"
                "$<TARGET_FILE_DIR:main>/${C2PA_RS_SHLIB}")

    # Dev-only repro harness for C2PA embed failures (tools/c2pa_repro.cpp).
    # Excluded from ALL — build explicitly with `--target c2pa_repro`.
    add_executable(c2pa_repro EXCLUDE_FROM_ALL
        "tools/c2pa_repro.cpp"
        "src/C2PA/AppCa.cpp"
        "src/C2PA/LeafIssuer.cpp"
        "src/C2PA/Manifest.cpp"
        "deps/tweetnacl/tweetnacl.c"
        "deps/tweetnacl/randombytes.cpp"
        "include/Helpers/Logger.cpp")
    target_include_directories(c2pa_repro PRIVATE "include" "deps/c2pa")
    target_link_libraries(c2pa_repro OpenSSL::Crypto "${C2PA_RS_IMPLIB_PATH}")
    if(WIN32)
        target_compile_definitions(c2pa_repro PRIVATE C2PA_DLL=0)
    endif()
    add_custom_command(TARGET c2pa_repro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${C2PA_RS_SHLIB_PATH}"
                "$<TARGET_FILE_DIR:c2pa_repro>/${C2PA_RS_SHLIB}")

    # PHASE5 (docs/design/PHASE5.md). Vendored TimelineFX particle runtime
    # (deps/timelinefx, pinned commit a5f323d826fa0d6e7ccb481961c518c56aa59285
    # — see deps/timelinefx/VENDORING.md). Self-contained: own allocator +
    # SIMD + threads, no deps.
    #
    # Compiled as an isolated C++17 static library. The upstream (alpha) TU
    # trips MSVC's C++20 rewritten-comparison-operator ambiguity
    # (tfx_attribute_node_s::operator== gains a synthesized reversed
    # candidate) at our project-wide C++23. Building the vendored TU at
    # C++17 sidesteps that rule and keeps the pinned source pristine — no
    # upstream fork. Consumers (main at M1, tfx_spike now) include the
    # header only for the extern "C" tfxAPI surface, which does not
    # instantiate the offending internal templates, so they stay at C++23.
    # EXCLUDE_FROM_ALL so it is built only when something links it (today
    # just tfx_spike); the shipping `main` binary picks it up at M1.
    add_library(timelinefx STATIC EXCLUDE_FROM_ALL "deps/timelinefx/timelinefx.cpp")
    target_include_directories(timelinefx PUBLIC "deps/timelinefx")
    set_target_properties(timelinefx PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    if(WIN32)
        target_compile_definitions(timelinefx PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    # TimelineFX's default (non-AVX) SIMD path is SSE4.1: tfxWideMuli expands to
    # _mm_mullo_epi32, plus _mm_blend_ps / _mm_blendv_ps / _mm_min_epi32 /
    # _mm_packus_epi32. MSVC exposes every intrinsic regardless of /arch, so
    # Windows builds clean; GCC/Clang refuse to inline an SSE4.1 always_inline
    # intrinsic unless the TU is compiled with -msse4.1 ("target specific option
    # mismatch"). Raise the ISA baseline for THIS isolated x86 target only
    # (SSE4.1 is x86-64-v2, universal since ~2008 — matches TimelineFX's own
    # default assumption). SSE41 also switches tfxWideFloor to the hardware
    # _mm_floor_ps. ARM (NEON) and Emscripten don't link this lib.
    if(NOT MSVC AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|i[3-6]86" OR NOT CMAKE_SYSTEM_PROCESSOR))
        target_compile_options(timelinefx PRIVATE -msse4.1)
        target_compile_definitions(timelinefx PRIVATE SSE41)
    endif()

    # M0 spike: compile+link proof for the vendored runtime, decoupled from
    # the main render path. Build explicitly with `--target tfx_spike`.
    add_executable(tfx_spike EXCLUDE_FROM_ALL "tools/tfx_spike.cpp")
    target_link_libraries(tfx_spike timelinefx)

    # M0 render proof (docs/design/PHASE5.md). Loads a .tfx package, runs the
    # CPU sim, and renders the particle instance buffer with a faithful
    # SkRuntimeEffect port of timelinefx.frag (per-texel color ramp / gradient
    # mapping) — the fidelity path chosen over plain drawAtlas. Links Skia +
    # the C++17 timelinefx lib; writes PNGs. EXCLUDE_FROM_ALL; build with
    # `--target tfx_render_spike`. Conan builds only (needs skia-infinipaint).
    if(NOT NO_CONAN_BUILD)
        add_executable(tfx_render_spike EXCLUDE_FROM_ALL "tools/tfx_render_spike.cpp")
        target_link_libraries(tfx_render_spike timelinefx skia-infinipaint::skia-infinipaint)
        target_compile_definitions(tfx_render_spike PRIVATE USE_SKIA_BACKEND_GANESH)
        if(WIN32)
            target_compile_definitions(tfx_render_spike PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        endif()
    endif()

    # PHASE5 legacy path spike: the classic, render-agnostic TimelineFX C++
    # runtime (deps/timelinefx_legacy = peterigz/timelinefx) reads the STABLE
    # legacy .eff/data.xml format. Isolated C++17 static lib (+ vendored pugixml).
    # We drive it with Skia in tools/tfx_legacy_spike.cpp. EXCLUDE_FROM_ALL.
    file(GLOB TFX_LEGACY_SRC "deps/timelinefx_legacy/source/*.cpp")
    add_library(timelinefx_legacy STATIC EXCLUDE_FROM_ALL
        ${TFX_LEGACY_SRC} "deps/timelinefx_legacy/pugixml/pugixml.cpp")
    target_include_directories(timelinefx_legacy PUBLIC
        "deps/timelinefx_legacy/source" "deps/timelinefx_legacy/pugixml")
    set_target_properties(timelinefx_legacy PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(timelinefx_legacy PRIVATE _USE_MATH_DEFINES)  # MSVC: M_PI
    if(WIN32)
        target_compile_definitions(timelinefx_legacy PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    endif()

    # miniz (Unlicense / public-domain) — in-memory .eff (zip) reader.
    add_library(miniz STATIC EXCLUDE_FROM_ALL "deps/miniz/miniz.c")
    target_include_directories(miniz PUBLIC "deps/miniz")

    if(NOT NO_CONAN_BUILD)
        add_executable(tfx_legacy_spike EXCLUDE_FROM_ALL "tools/tfx_legacy_spike.cpp")
        target_link_libraries(tfx_legacy_spike timelinefx_legacy miniz skia-infinipaint::skia-infinipaint)
        target_compile_definitions(tfx_legacy_spike PRIVATE USE_SKIA_BACKEND_GANESH)
        if(WIN32)
            target_compile_definitions(tfx_legacy_spike PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        endif()
    endif()
endif()
if(ANDROID)
    target_include_directories(main PRIVATE "android-project/SDL/include")
endif()

target_compile_definitions(main PRIVATE IS_CLIENT)

# Trezor BIP-39 sources vendored under src/crypto/stellar/trezor/ — disable
# the in-memory mnemonic cache (default-on in Trezor's options.h) so the
# artist's plaintext seed words don't linger in the process address space
# across their session. Inkternity calls mnemonic_to_seed at most a handful
# of times per launch; the cache buys nothing for our use case.
target_compile_definitions(main PRIVATE USE_BIP39_CACHE=0)
if(WIN32)
    target_compile_definitions(main PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(CONFIG_NEXT_TO_EXECUTABLE)
    target_compile_definitions(main PRIVATE CONFIG_NEXT_TO_EXECUTABLE)
    message("Place config next to executable")
else()
    message("Place config in global config folder")
endif()

if(NOT EMSCRIPTEN AND ((GRAPHICS_BACKEND STREQUAL "OpenGL3.3") OR (GRAPHICS_BACKEND STREQUAL "OpenGLES3.0") OR (GRAPHICS_BACKEND STREQUAL "OpenGL2.1")))
    if(ANDROID)
        # Android: GLES + EGL come from the NDK directly. CMake's
        # FindOpenGL hunts for desktop libGL / opengl32 / GLX and fails
        # against the NDK sysroot — link the NDK libraries by name.
        target_link_libraries(main GLESv3 EGL)
    else()
        find_package(OpenGL REQUIRED)
        if(GRAPHICS_BACKEND STREQUAL "OpenGLES3.0")
            target_link_libraries(main OpenGL::GLES3)
        else()
            target_link_libraries(main OpenGL::GL)
        endif()
    endif()
endif()

if((NOT EMSCRIPTEN) AND (NOT APPLE) AND (NOT ANDROID))
    if(NO_CONAN_BUILD)
        target_link_libraries(main TBB::tbb)
    else()
        target_link_libraries(main onetbb::onetbb)
    endif()
endif()

target_link_libraries(main zstd::libzstd_static)

if((NOT EMSCRIPTEN) AND WIN32)
    target_link_libraries(main dwrite)
    # Clip library for retrieving images from windows clipboard
    set(CLIP_EXAMPLES OFF CACHE BOOL "Compile clip examples")
    set(CLIP_TESTS OFF CACHE BOOL "Compile clip tests")
    set(CLIP_ENABLE_IMAGE ON CACHE BOOL "Compile with image clipboard support")
    add_subdirectory(deps/clip)
    target_link_libraries(main clip)
endif()

# libmypaint — natural-media brush engine. Disabled on Emscripten only;
# Android cross-compiles cleanly via Conan's json-c + the in-tree CMake
# wrapper (pure C99, no glib/autotools dependency).
if(NOT EMSCRIPTEN)
    add_subdirectory(cmake/libmypaint)
    target_link_libraries(main mypaint)
    target_compile_definitions(main PRIVATE HVYM_HAS_LIBMYPAINT)
endif()

# PHASE5 (docs/design/PHASE5.md): link the TimelineFX particle runtime into
# the app on desktop. timelinefx is the C++17-isolated static lib defined
# above (deps/timelinefx); linking it propagates its PUBLIC include dir so
# ParticleCanvasComponent.cpp can include timelinefx.h. HVYM_HAS_TIMELINEFX
# gates that component's timelinefx usage (web builds get a no-op stub),
# mirroring HVYM_HAS_LIBMYPAINT.
if(NOT EMSCRIPTEN)
    target_link_libraries(main timelinefx)
    target_compile_definitions(main PRIVATE HVYM_HAS_TIMELINEFX)
endif()

# PHASE5.1: legacy .eff particle path (deps/timelinefx_legacy = damucz/timelinefx,
# MIT + miniz for in-memory .eff zip reading). Desktop-only; HVYM_HAS_TIMELINEFX_LEGACY
# gates src/CanvasComponents/Particles/* (web gets the no-op paths). Linking the
# libs propagates their PUBLIC include dirs (TLFX* headers + miniz.h).
if(NOT EMSCRIPTEN)
    target_link_libraries(main timelinefx_legacy miniz)
    target_compile_definitions(main PRIVATE HVYM_HAS_TIMELINEFX_LEGACY)
endif()

if(EMSCRIPTEN)
    add_subdirectory(deps/datachannel-wasm EXCLUDE_FROM_ALL)
    target_compile_options(datachannel-wasm PRIVATE -pthread)
    target_link_libraries(main datachannel-wasm)
else()
    # OpenSSL::SSL + OpenSSL::Crypto are the standard CMake target names
    # exported by both system find_package(OpenSSL) (Linux Flatpak / macOS
    # system) AND Conan's generated FindOpenSSL.cmake (Windows). The
    # lowercase `openssl::openssl` meta-target is Conan-only and breaks
    # the no-Conan branch with "target was not found."
    target_link_libraries(main LibDataChannel::LibDataChannel CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
endif()

if(ANDROID)
    add_subdirectory(deps/SDL EXCLUDE_FROM_ALL)
endif()

if(NO_CONAN_BUILD)
    target_link_libraries(main ${SKIA_SVG_LIB} ${SKIA_RESOURCES_LIB} ${SKIA_LIB} ${SKIA_UNICODE_CORE_LIB} ${SKIA_UNICODE_ICU_LIB} ${SKIA_UNICODE_CORE_LIB} ${SKIA_PARAGRAPH_LIB} ${SKIA_SHAPER_LIB})
    target_include_directories(main PRIVATE ${SKIA_INCLUDE})
    target_link_libraries(main SDL3::SDL3 ICU::uc Fontconfig::Fontconfig Freetype::Freetype PNG::PNG ZLIB::ZLIB)
else()
    target_link_libraries(main skia-infinipaint::skia-infinipaint)
    if(ANDROID)
        target_link_libraries(main SDL3::SDL3)
    else()
        target_link_libraries(main sdl-infinipaint::sdl-infinipaint)
    endif()
    target_link_libraries(main icu-infinipaint::icu-infinipaint)
endif()

if(NOT EMSCRIPTEN)
    target_link_libraries(main harfbuzz::harfbuzz)
endif()

if((NOT (GRAPHICS_BACKEND STREQUAL "Vulkan")) AND (NOT EMSCRIPTEN) AND LINUX)
    target_link_libraries(main OpenGL::EGL)
endif()

if((NOT (GRAPHICS_BACKEND STREQUAL "Vulkan")) AND (NOT EMSCRIPTEN) AND LINUX AND (NOT NO_CONAN_BUILD))
    target_link_libraries(main egl::egl)
endif()

if(ANDROID)
    target_link_libraries(main EGL)
endif()

if(GRAPHICS_BACKEND STREQUAL "Vulkan")
    target_compile_definitions(main PRIVATE USE_BACKEND_VULKAN SK_VULKAN)
    if(USE_GRAPHITE)
        target_compile_definitions(main PRIVATE SK_GRAPHITE USE_SKIA_BACKEND_GRAPHITE)
        message("Using Vulkan Graphite Backend")
    else()
        target_compile_definitions(main PRIVATE USE_SKIA_BACKEND_GANESH)
        message("Using Vulkan Ganesh Backend")
    endif()
else()
    if(GRAPHICS_BACKEND STREQUAL "OpenGLES3.0")
        target_compile_definitions(main PRIVATE USE_BACKEND_OPENGLES_3_0)
        message("Using OpenGL ES 3.0")
    elseif(GRAPHICS_BACKEND STREQUAL "OpenGL2.1")
        target_compile_definitions(main PRIVATE USE_BACKEND_OPENGL_2_1)
        message("Using OpenGL 2.1")
    else()
        target_compile_definitions(main PRIVATE USE_BACKEND_OPENGL_3_3)
        message("Using OpenGL 3.3")
    endif()
    target_compile_definitions(main PRIVATE USE_BACKEND_OPENGL USE_SKIA_BACKEND_GANESH)
endif()

target_compile_definitions(main PRIVATE BOOST_MP_STANDALONE EIGEN_MPL2_ONLY)

include(CheckCCompilerFlag)
check_c_compiler_flag(-Wdeprecated-declarations HAS_DEPRECATED_DECLARATIONS)

if(HAS_DEPRECATED_DECLARATIONS)
    target_compile_options(main PRIVATE -Wno-deprecated-declarations)
endif()

if(APPLE OR WIN32)
    # Apple: build the binary with its install-time RPATHs already baked
    # in so `cmake --install` doesn't relink (which would rewrite the
    # binary content and invalidate any codesign signature we apply to
    # the build/Release/inkternity.app copy). This is the root cause of
    # the rc17 notarization rejection — the build-tree signature was
    # valid on disk, but cpack's internal install step relinked the
    # binary before DMG'ing it.
    if(APPLE)
        set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    endif()

    install(TARGETS main DESTINATION .)

    # Stage the c2pa-rs shared lib alongside the binary so CPack
    # (NSIS / DragNDrop) bundles it into the installer. The
    # add_custom_command(POST_BUILD copy_if_different) above only
    # helps local dev runs out of the build tree — without an
    # install rule the shipped installer has no c2pa_c.dll /
    # libc2pa_c.dylib and the loader bails at first launch
    # ("code execution cannot proceed because c2pa_c.dll was not
    # found" — rc2 Windows postmortem). Must be declared BEFORE
    # the macOS codesign install(CODE) below so the dylib is on
    # disk when codesign --deep walks the bundle.
    if(APPLE)
        install(FILES "${C2PA_RS_SHLIB_PATH}"
                DESTINATION "inkternity.app/Contents/MacOS")
    else()
        install(FILES "${C2PA_RS_SHLIB_PATH}" DESTINATION .)
    endif()

    # Apple: codesign the *staged* .app from inside the install step so
    # signing is the last thing that touches the binary before CPack
    # wraps it in a DMG. Workflow passes the Developer ID Application
    # SHA-1 hash as INKTERNITY_CODESIGN_IDENTITY at configure time;
    # when that's empty (local dev builds, missing-cert CI runs) the
    # install just skips signing without erroring.
    if(APPLE)
        install(CODE "
            set(_id \"${INKTERNITY_CODESIGN_IDENTITY}\")
            if(NOT _id STREQUAL \"\")
                set(_app \"\${CMAKE_INSTALL_PREFIX}/inkternity.app\")
                message(STATUS \"Codesigning \${_app} with \${_id}\")
                execute_process(
                    COMMAND codesign --force --options runtime --timestamp --deep
                            --sign \${_id} \${_app}
                    RESULT_VARIABLE _sign_rc
                )
                if(NOT _sign_rc EQUAL 0)
                    message(FATAL_ERROR \"codesign failed (exit \${_sign_rc})\")
                endif()
                execute_process(
                    COMMAND codesign --verify --strict --deep --verbose=2 \${_app}
                    RESULT_VARIABLE _verify_rc
                )
                if(NOT _verify_rc EQUAL 0)
                    message(FATAL_ERROR \"codesign verify failed (exit \${_verify_rc})\")
                endif()
            else()
                message(STATUS \"INKTERNITY_CODESIGN_IDENTITY not set — skipping bundle codesign\")
            endif()
        ")
    endif()

    set(CPACK_PACKAGE_NAME "inkternity")
    # CPACK_PACKAGE_VERSION_* are set at the top of this file so the
    # workflow's -DCPACK_PACKAGE_VERSION_* overrides don't get clobbered.
    set(CPACK_PACKAGE_VENDOR "HEAVYMETA LCA")
    set(CPACK_PACKAGE_CONTACT "support@heavymeta.art")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
endif()

if(APPLE)
    # Nothing specific here for now
elseif(WIN32)
    install(DIRECTORY assets/data DESTINATION .)

    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
	include(InstallRequiredSystemLibraries)
	install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION .)

    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_DISPLAY_NAME "Inkternity")
    set(CPACK_NSIS_PACKAGE_NAME "Inkternity")
    set(CPACK_NSIS_UNINSTALL_NAME "Inkternity Uninstall")
    set(CPACK_NSIS_WELCOME_TITLE "Welcome to Inkternity Installer")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY .)
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/inviti8/inkternity")
    set(CPACK_NSIS_MENU_LINKS "inkternity.exe" "Inkternity")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "inkternity.exe")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/windowsinstall/icon.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/windowsinstall/icon.ico")
    # A backslash character uses 4 backslashes, since we have to escape twice
    # Double quote uses 3 backslashes, since we have to escape the backslash (two slashes), and escape the double quote (one slash)
	set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
	     WriteRegStr HKCR '.inkternity' '' 'InkternityCanvas'
         WriteRegStr HKCR 'InkternityCanvas' '' 'Inkternity Canvas File'
         WriteRegStr HKCR 'InkternityCanvas\\\\shell' '' 'open'
         WriteRegStr HKCR 'InkternityCanvas\\\\DefaultIcon' '' '\\\"$INSTDIR\\\\inkternity.exe\\\",0'
         WriteRegStr HKCR 'InkternityCanvas\\\\shell\\\\open\\\\command' '' '\\\"$INSTDIR\\\\inkternity.exe\\\" \\\"%1\\\"'
         WriteRegStr HKCR 'InkternityCanvas\\\\shell\\\\edit' '' 'Edit Inkternity Canvas File'
         WriteRegStr HKCR 'InkternityCanvas\\\\shell\\\\edit\\\\command' '' '\\\"$INSTDIR\\\\inkternity.exe\\\" \\\"%1\\\"'
         System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'
	")

	set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
	     DeleteRegKey HKCR '.inkternity'
         DeleteRegKey HKCR 'InkternityCanvas'
     ")

     set(CPACK_PACKAGE_INSTALL_DIRECTORY "inkternity")
else()
    install(DIRECTORY assets/data DESTINATION bin)
    install(TARGETS main DESTINATION bin)
    # Bundle libc2pa_c.so next to the binary inside the Flatpak's
    # /app/bin. $ORIGIN on main's runpath lets the ELF loader resolve
    # the sibling .so without depending on /app/lib being on the
    # runtime's default library search path.
    install(FILES "${C2PA_RS_SHLIB_PATH}" DESTINATION bin)
    set_property(TARGET main APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    install(FILES linuxinstall/com.inkternity.inkternity.desktop DESTINATION share/applications)
    install(FILES logo.svg DESTINATION share/icons/hicolor/scalable/apps RENAME com.inkternity.inkternity.svg)
    install(FILES logo.svg DESTINATION share/icons/hicolor/scalable/mimetypes RENAME com.inkternity.inkternity-application-inkternity.svg)
    install(FILES linuxinstall/com.inkternity.inkternity.metainfo.xml DESTINATION share/metainfo)
    install(FILES linuxinstall/com.inkternity.inkternity.xml DESTINATION share/mime/packages)
endif()

if(APPLE OR WIN32)
    include(CPack)
endif()
