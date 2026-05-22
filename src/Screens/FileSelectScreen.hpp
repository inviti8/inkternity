#pragma once
#include "Screen.hpp"
#include "nlohmann/json.hpp"
#include "../GUIStuff/GUIFloatAnimation.hpp"
#include "../GUIStuff/Elements/ScrollArea.hpp"

class FileSelectScreen : public Screen {
    public:
        FileSelectScreen(MainProgram& m);
        virtual void gui_layout_run() override;
        virtual void draw(SkCanvas* canvas) override;
        virtual void input_open_infinipaint_file_callback(const CustomEvents::OpenInfiniPaintFileEvent& openFile) override;
        virtual void input_global_back_button_callback() override;
        virtual void input_app_about_to_go_to_background_callback() override;

        struct TrashInfo {
            struct TrashFile {
                SDL_Time trashTime;
                NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TrashFile, trashTime)
            };
            std::unordered_map<std::string, TrashFile> files;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TrashInfo, files)
        };
        ~FileSelectScreen();

    private:
        std::filesystem::path savePath;
        std::filesystem::path trashPath;
        std::filesystem::path trashInfoPath;
        // Hidden subdir of savePath where Worlds drop periodic in-progress
        // autosaves (World::autosave_tick). Entries that survive a clean
        // save get deleted by World::delete_autosave_file; what remains is
        // work the artist hasn't yet committed to a real file. Rendered
        // inline at the top of the main file list with a "Recovered" badge.
        std::filesystem::path autosavePath;

        void save_files();

        struct FileInfo {
            std::string fileName;
            // Extension stored as found on disk (e.g. ".inkternity" or
            // legacy ".infpnt"); operations that touch the file must
            // append this rather than the canonical DOT_FILE_EXTENSION.
            std::string fileExtension;
            SDL_Time lastModifyTime;
            std::string lastModifyDate;
            bool selected = false;
            // Directory the entry lives in. Set per-entry so autosave
            // entries (under savePath/.autosave) and regular saves
            // (under savePath) can coexist in one list without the
            // open path being inferred from selectedMenu alone.
            std::filesystem::path basePath;
            // True for entries scanned out of autosavePath. Disables
            // edit-mode select / trash / duplicate (those operations
            // would silently fail since they assume savePath), and
            // triggers the "Recovered" badge in the file tile.
            bool isAutosave = false;
        };

        void update_file_list(std::vector<FileInfo>& fL, const std::filesystem::path& savePath, bool trashUpdate);
        // Repopulate fileList for the FILES tab — regular saves plus
        // any leftover autosave-recovery entries prepended. Replaces
        // bare update_file_list(fileList, savePath, false) calls so the
        // autosave breadcrumbs don't vanish on tab switch or edit-mode
        // refresh.
        void refresh_main_file_list();

        TrashInfo trashInfo;
        std::vector<FileInfo> fileList;

        GUIStuff::GUIFloatAnimation* mainMenuOpenAnim = nullptr;
        GUIStuff::GUIFloatAnimation* actionBarOpenAnim = nullptr;

        GUIStuff::ScrollArea* fileViewScrollArea = nullptr;

        void main_display();
        void main_menu();
        void file_view();
        // Lobby-side App Key surface (DISTRIBUTION-PHASE0.md): renders
        // the Inkternity app pubkey + Copy button when dev keys are
        // loaded; instructions to set up dev keys when not. Lives on
        // the Settings tab because the keypair is per-install (not
        // per-canvas) and persists across all canvases.
        //
        // DISTRIBUTION-PHASE1.md §3.3 added two on-demand sub-sections
        // gated by exportKeyOpen / restoreKeyOpen — Export App Key
        // (reveal S... + mnemonic) and Restore App Key (paste S... or
        // mnemonic, destructive overwrite). Both stay collapsed by
        // default per the crypto-averse UX direction.
        void settings_view();
        void export_app_key_section();
        void restore_app_key_section();
        // docs/design/C2PA.md §3.1 — the "Verifiable publishing" gateway
        // section. Visible only inside settings_view; toggle state lives
        // on GlobalConfig::verifiablePublishingEnabled. Future tasks
        // (I7 wallet panel, I10 walkthrough) attach below the toggle
        // when it's ON.
        void verifiable_publishing_section();
        bool exportKeyOpen = false;
        bool restoreKeyOpen = false;
        std::string restoreKeyInput;
        bool restoreConfirmStage = false;
        // Transient feedback after a Restore attempt — shown until
        // the artist closes/reopens the section.
        std::string restoreFeedback;
        void create_file_button();
        void file_view_edit();
        void menu_black_box();
        void edit_action_bar();
        void edit_title_bar();
        void title_bar();
        void text_transparent_option_button(const char* id, const char* text, const std::function<void()>& onClick);
        void icon_text_transparent_option_selected_button(const char* id, const std::string& svgPath, const char* text, bool isSelected, const std::function<void()>& onClick);
        void edit_action_bar_button(const char* id, const std::string& svgPath, const char* text, const std::function<void()>& onClick);

        enum class TrashMoveType {
            NONE,
            MOVE_TO_TRASH,
            MOVE_OUT_OF_TRASH
        };
        void move_selected_files(const std::filesystem::path& fromPath, const std::filesystem::path& toPath, TrashMoveType trashMoveType);
        void duplicate_selected_files(const std::filesystem::path& inPath);
        void delete_selected_files_in_trash();

        enum class FileViewType {
            LARGE_GRID,
            MEDIUM_GRID,
            SMALL_GRID,
            LIST
        } fileViewType = FileViewType::LARGE_GRID;

        enum class MoreOptionsMenu {
            CLOSED,
            MAIN,
            VIEW
        } moreOptionsMenu = MoreOptionsMenu::CLOSED;

        enum class SelectedMenu {
            FILES,
            TRASH,
            SETTINGS
        } selectedMenu = SelectedMenu::FILES;

        void start_edit_mode();
        size_t numberOfSelectedEntries;
        bool editMode = false;
};
