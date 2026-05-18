#pragma once
#include "BrushCustomizationDrawer.hpp"
#include "SavedPresetsDrawer.hpp"
#include "HostMode.hpp"
#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>
#include "GUIStuff/Elements/ScrollArea.hpp"
#include "DrawData.hpp"
#include "TimePoint.hpp"
#include "Helpers/FileDownloader.hpp"
#include <filesystem>
#include <modules/skparagraph/include/Paragraph.h>
#include <nlohmann/json.hpp>
#include <Helpers/Serializers.hpp>
#include <SDL3/SDL_dialog.h>
#include <Helpers/VersionNumber.hpp>

class MainProgram;

class Toolbar {
    public:
        struct ChatMessage {
            static constexpr float DISPLAY_TIME = 8.0f;
            static constexpr float FADE_START_TIME = 7.0f;
            std::string name;
            std::string message;
            enum Type {
                NORMAL = 0,
                JOIN 
            } type;
            TimePoint time;
        };

        struct ExtensionFilter {
            std::string name;
            std::string extensions;
        };

        std::string chatMessageInput;

        Toolbar(MainProgram& initMain);
        void update();
        void layout_run();
        struct ColorSelectorData {
            std::function<void()> onChange;
            std::function<void()> onSelect;
            std::function<void()> onDeselect;
        };
        struct ColorSelectorButtonData {
            std::function<void()> onSelectorButtonClick;
            std::function<void()> onChange;
            std::function<void()> onSelect;
            std::function<void()> onDeselect;
        };
        void color_selector_left(GUIStuff::Element* button, Vector4f* color, const ColorSelectorData& colorSelectorData = {});
        void color_selector_right(GUIStuff::Element* button, Vector4f* color, const ColorSelectorData& colorSelectorData = {});
        void color_button_left(const char* id, Vector4f* color, const ColorSelectorButtonData& colorSelectorData = {});
        void color_button_right(const char* id, Vector4f* color, const ColorSelectorButtonData& colorSelectorData = {});

        void paint_popup(Vector2f popupPos);

        typedef std::function<void(const std::filesystem::path&, const ExtensionFilter& extensionSelected)> OpenFileSelectorCallback;
        void open_file_selector(const std::string& filePickerName, const std::vector<ExtensionFilter>& extensionFilters, OpenFileSelectorCallback postSelectionFunc, const std::string& fileName = "", bool isSaving = false);
        void save_func();
        void save_as_func();

        void open_chatbox();
        void close_chatbox();

        GUIStuff::Element* colorLeftButton; 
        Vector4f* colorLeft = nullptr;
        ColorSelectorData colorLeftData;

        GUIStuff::Element* colorRightButton; 
        Vector4f* colorRight = nullptr;
        ColorSelectorData colorRightData;

        bool drawGui = true;

        bool app_close_requested();

        // Accessor for nested-view components (e.g. BrushCustomizationDrawer)
        // that need to walk back to MainProgram. Returning the reference
        // keeps callers from having to re-plumb MainProgram everywhere.
        MainProgram& main_program() { return main; }

        // PHASE3 A1.M6 -- the drawer closes itself while capture-from-
        // canvas runs (so the user can drag a square on the canvas), then
        // re-opens itself when the capture callback fires. Setter flips
        // the bool + invalidates layout in one call. Body in .cpp so
        // this header doesn't need a full MainProgram include.
        void set_brush_customization_menu_open(bool open);
    private:
        static void sdl_open_file_dialog_callback(void* userData, const char * const * fileList, int filter);

        void text_button_wide(const char* id, const char* str, const std::function<void()>& onClick);
        void reload_theme_list();
        void player_list();
        void chat_box();
        void global_log();
        void top_toolbar();
        void grid_menu(GUIStuff::Element* gridMenuButton);
        void add_grid();
        void stop_displaying_grid_menu();
        void stop_displaying_bookmark_menu();
        void stop_displaying_layer_menu();
        void stop_displaying_brush_customization_menu();
        void stop_displaying_saved_presets_menu();
        void bookmark_menu(GUIStuff::Element* bookmarkMenuButton);
        void layer_menu(GUIStuff::Element* layerMenuButton);
        void brush_customization_menu(GUIStuff::Element* triggerButton);
        void saved_presets_menu(GUIStuff::Element* triggerButton);
        // PHASE3 §4 B.M2 -- avatar tile (clickable image) lives in the
        // top toolbar; popover is its on-click submenu with capture /
        // clear controls. tile() returns the trigger Element so the
        // popover can attach to it.
        GUIStuff::Element* avatar_tile();
        void avatar_popover(GUIStuff::Element* triggerTile);
        // Pulls the master avatar PNG off disk into avatarImage_. Called
        // lazily on first render and after every save (B.M3 capture
        // callback). No-op when no avatar file exists.
        void refresh_avatar_from_disk();
        // PHASE3 §4 B.M3 -- avatar capture entry point. Closes the
        // popover, switches the active tool to SquareCanvasCaptureTool
        // at 256x256, callback writes via AvatarStore and re-refreshes
        // the in-memory image.
        void start_avatar_capture();
        void drawing_program_gui();
        void options_menu();
        void file_picker_gui_refresh_entries();
        void file_picker_gui_done();
        void file_picker_gui();
        void performance_metrics();
        void color_picker_window(const char* id, Vector4f** color, GUIStuff::Element* b, const ColorSelectorData& colorSelectorData);
        void color_palette(const char* id, Vector4f* color, const std::function<void()>& onChange);
        void open_world_file(bool isClient, const std::string& netSource, const std::string& serverLocalID);
        void load_default_theme();
        void about_menu_inner_gui();
        void web_version_welcome();
        void still_connecting_center_message();
        void no_layers_being_edited_message();
        void close_popup_gui();
        void add_world_to_close_popup_data(const std::shared_ptr<World>& w);
        void general_settings_inner_gui();
        void center_obstructing_window_gui(const char* id, Clay_SizingAxis x, Clay_SizingAxis y, const std::function<void()>& innerContent);

        struct ClosePopupData {
            struct CloseWorldData {
                std::weak_ptr<World> w;
                bool setToSave = true;
            };
            std::vector<CloseWorldData> worldsToClose; // Using a vector to ensure that the worlds are in proper order
            bool closeAppWhenDone = false;
        } closePopupData;

        #ifndef __EMSCRIPTEN__
        void update_notification_gui();
        void update_notification_check();
        struct UpdateCheckerData {
            bool showGui = false;
            bool updateCheckDone = false;
            std::string newVersionStr;
            std::shared_ptr<FileDownloader::DownloadData> versionFile;
        } updateCheckerData;
        #endif

        int selectedLicense = -1;

        std::string downloadNameSet;

        // DISTRIBUTION-PHASE1.md §4 polish — inline rename. The text
        // input in the top toolbar binds to `canvasNameInput`; on
        // commit (Enter) we call World::rename_on_disk and the canvas
        // file + sidecars move. `canvasNameInputFocused` gates the
        // per-frame "sync from filePath" pass so we don't clobber the
        // user's in-progress typing.
        std::string canvasNameInput;
        bool canvasNameInputFocused = false;

        struct PaletteData {
            size_t selectedPalette = 0;
            bool addingPalette = false;
            std::string newPaletteStr;
        } paletteData;

        struct ThemeData {
            std::vector<std::string> themeDirList;
            std::optional<size_t> selectedThemeIndex;
            bool openedSaveAsMenu = false;
        } themeData;

        bool showPerformance = false;

        bool menuPopUpOpen = false;
        bool optionsMenuOpen = false;
        bool playerMenuOpen = false;

        float finalCalculatedGuiScale = 1.0f;

        bool chatboxOpen = false;

        enum OptionsMenuType {
            HOST_MENU,
            CONNECT_MENU,
            GENERAL_SETTINGS_MENU,
            LOBBY_INFO_MENU,
            CANVAS_SETTINGS_MENU,
            SET_DOWNLOAD_NAME,
            ABOUT_MENU
        } optionsMenuType;
        enum GeneralSettingsOptions {
            GSETTINGS_GENERAL = 0,
            GSETTINGS_TABLET,
            GSETTINGS_THEME,
            GSETTINGS_KEYBINDS,
            GSETTINGS_DEBUG
        } generalSettingsOptions = GSETTINGS_GENERAL;

        bool bookmarkMenuPopupOpen = false;
        bool layerMenuPopupOpen = false;
        // PHASE3.md §3 A1.M4 -- toolbar toggle state for the brush
        // customization popup. Button + popup only render when the
        // active tool is MyPaintBrushTool (the drawer is meaningless
        // for any other tool).
        bool brushCustomizationMenuPopupOpen = false;
        BrushCustomizationDrawer brushCustomizationDrawer{*this};
        // PHASE3.md §3 A2.M2 -- toolbar toggle state for the saved-
        // presets browser popup. Same MyPaintBrush gating as the
        // customization drawer.
        bool savedPresetsMenuPopupOpen = false;
        SavedPresetsDrawer savedPresetsDrawer{*this};

        // PHASE3 §4 B.M2 -- in-memory copy of the master avatar PNG.
        // Loaded from disk on first render (avatarLoaded_=false until
        // then) and refreshed after every B.M3 capture so the tile
        // updates without an app relaunch. nullptr when the artist has
        // never captured an avatar.
        sk_sp<SkImage> avatarImage;
        bool avatarLoaded = false;
        bool avatarPopoverOpen = false;

        struct GridMenu {
            bool popupOpen = false;
            std::string newName;
            uint32_t selectedGrid = std::numeric_limits<uint32_t>::max();
        } gridMenu;

        struct FilePicker {
            bool isOpen = false;
            std::string filePickerWindowName;
            std::vector<ExtensionFilter> extensionFiltersComplete;
            std::vector<std::string> extensionFilters;
            std::vector<std::filesystem::path> entries;
            std::filesystem::path currentSelectedPath;
            std::string fileName = "";
            bool isSaving = false;
            size_t extensionSelected;
            GUIStuff::ScrollArea* entriesScrollArea = nullptr;
            OpenFileSelectorCallback postSelectionFunc;
        } filePicker;

        struct NativeFilePicker {
            std::atomic<bool> isOpen = false;
            std::vector<ExtensionFilter> extensionFiltersComplete;
            std::vector<SDL_DialogFileFilter> sdlFileFilters;
            OpenFileSelectorCallback postSelectionFunc;
        };
        static NativeFilePicker nativeFilePicker;

        RichText::TextData build_paragraph_from_chat_message(const ChatMessage& message, float alpha);

        void load_icons_at(const std::filesystem::path& pathToLoad);

        std::string serverToConnectTo;
        std::string serverLocalID;
        // Selected hosting mode for the HOST_MENU dialog. Defaulted when
        // the menu opens (SUBSCRIPTION if the canvas has portal metadata,
        // COLLAB otherwise) and passed into World::start_hosting on Host.
        HostMode hostMenuMode = HostMode::COLLAB;

        MainProgram& main;
};
