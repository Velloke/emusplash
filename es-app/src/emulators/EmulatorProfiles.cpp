//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  EmulatorProfiles.cpp
//
//  The emulators whose own configuration files EmuSPLASH can change, see EmulatorProfiles.h.
//

#include "emulators/EmulatorProfiles.h"

#include "FileData.h"
#include "Log.h"
#include "Settings.h"
#include "SystemData.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <sstream>

#if defined(__linux__) && !defined(__ANDROID__)
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#if __has_include(<zlib.h>)
#include <zlib.h>
#define ES_HAS_ZLIB_HEADER 1
#endif

namespace EmulatorProfiles
{
    namespace
    {
        //------------------------------------------------------------------------------------
        // The settings of the emulators.
        //------------------------------------------------------------------------------------

        const std::vector<Choice> OFF_ON {{"OFF", "false"}, {"ON", "true"}};

        // yuzu and the emulators made from it have the same settings, but some of their
        // numbers differ: Eden added resolutions and filters and has two GPU accuracies,
        // citron four GPU accuracies, suyu the 32:9 aspect ratio.
        enum class SwitchEmulator {
            YUZU,
            SUYU,
            EDEN,
            CITRON
        };

        std::vector<Setting> switchSettings(SwitchEmulator emulator)
        {
            std::vector<Choice> resolutions {{"0.5X", "0"},  {"0.75X", "1"}, {"1X (NATIVE)", "2"},
                                             {"1.5X", "3"},  {"2X", "4"},    {"3X", "5"},
                                             {"4X", "6"}};
            std::vector<Choice> filters {{"NEAREST", "0"},  {"BILINEAR", "1"},
                                         {"BICUBIC", "2"},  {"GAUSSIAN", "3"},
                                         {"SCALEFORCE", "4"}, {"AMD FSR", "5"}};
            std::vector<Choice> gpuAccuracies {{"NORMAL", "0"}, {"HIGH", "1"}, {"EXTREME", "2"}};
            std::vector<Choice> aspectRatios {
                {"16:9", "0"}, {"4:3", "1"}, {"21:9", "2"}, {"16:10", "3"}, {"STRETCH", "4"}};
            switch (emulator) {
                case SwitchEmulator::YUZU:
                    break;
                case SwitchEmulator::SUYU:
                    aspectRatios = {{"16:9", "0"},  {"4:3", "1"},  {"21:9", "2"},
                                    {"16:10", "3"}, {"32:9", "4"}, {"STRETCH", "5"}};
                    break;
                case SwitchEmulator::EDEN:
                    // ResolutionSetup starts with Res1_4X and has Res5_4X, ScalingFilter has
                    // Lanczos before ScaleForce and Area after FSR (v0.2.1).
                    resolutions = {{"0.5X", "1"},  {"0.75X", "2"}, {"1X (NATIVE)", "3"},
                                   {"1.25X", "4"}, {"1.5X", "5"},  {"2X", "6"},
                                   {"3X", "7"},    {"4X", "8"}};
                    filters = {{"NEAREST", "0"},    {"BILINEAR", "1"}, {"BICUBIC", "2"},
                               {"GAUSSIAN", "3"},   {"LANCZOS", "4"},  {"SCALEFORCE", "5"},
                               {"AMD FSR", "6"},    {"AREA", "7"}};
                    gpuAccuracies = {{"LOW (FAST)", "0"}, {"HIGH (ACCURATE)", "1"}};
                    break;
                case SwitchEmulator::CITRON:
                    // The place of FSR in the filters of citron isn't known, only the first
                    // four are the same as in yuzu.
                    filters = {{"NEAREST", "0"}, {"BILINEAR", "1"}, {"BICUBIC", "2"},
                               {"GAUSSIAN", "3"}};
                    gpuAccuracies = {
                        {"LOW", "0"}, {"NORMAL", "1"}, {"HIGH", "2"}, {"EXTREME", "3"}};
                    break;
            }
            return {
                {"GRAPHICS API", "Renderer/backend", {{"OPENGL", "0"}, {"VULKAN", "1"}}},
                {"RESOLUTION", "Renderer/resolution_setup", resolutions},
                {"CPU ACCURACY",
                 "Cpu/cpu_accuracy",
                 {{"AUTO", "0"}, {"ACCURATE", "1"}, {"UNSAFE", "2"}, {"PARANOID", "3"}}},
                {"MULTICORE CPU EMULATION", "Core/use_multi_core", OFF_ON},
                {"GPU ACCURACY", "Renderer/gpu_accuracy", gpuAccuracies},
                {"ASPECT RATIO", "Renderer/aspect_ratio", aspectRatios},
                {"SCALING FILTER (FSR)", "Renderer/scaling_filter", filters},
                {"ANTI-ALIASING",
                 "Renderer/anti_aliasing",
                 {{"OFF", "0"}, {"FXAA", "1"}, {"SMAA", "2"}}},
                {"ANISOTROPIC FILTERING",
                 "Renderer/max_anisotropy",
                 {{"AUTO", "0"},
                  {"DEFAULT", "1"},
                  {"2X", "2"},
                  {"4X", "3"},
                  {"8X", "4"},
                  {"16X", "5"}}},
                {"VSYNC",
                 "Renderer/use_vsync",
                 {{"OFF (IMMEDIATE)", "0"},
                  {"MAILBOX", "1"},
                  {"ON (FIFO)", "2"},
                  {"FIFO RELAXED", "3"}}},
                {"ASTC RECOMPRESSION",
                 "Renderer/astc_recompression",
                 {{"UNCOMPRESSED", "0"}, {"BC1", "1"}, {"BC3", "2"}}},
                {"ASYNCHRONOUS SHADERS", "Renderer/use_asynchronous_shaders", OFF_ON},
                {"DISK SHADER CACHE", "Renderer/use_disk_shader_cache", OFF_ON},
                {"FORCE MAX CLOCK (VULKAN)", "Renderer/force_max_clock", OFF_ON},
            };
        }

        Profile switchProfile(const std::string& id,
                              const std::string& name,
                              SwitchEmulator emulator)
        {
            Profile profile;
            profile.id = id;
            profile.name = name;
            profile.match = {id};
            profile.format = Format::QT_INI;
            profile.configFiles = {"~/.config/" + id + "/qt-config.ini",
                                   "~/.var/app/*/config/" + id + "/qt-config.ini"};
            profile.gameFiles = GameFiles::QT_CUSTOM;
            profile.settings = switchSettings(emulator);
            // citron keeps its shortcuts in another form, which isn't written.
            profile.closesItself = emulator != SwitchEmulator::CITRON;
            return profile;
        }

        std::vector<Profile> makeProfiles()
        {
            std::vector<Profile> profiles;
            // Eden first: its release names may contain "yuzu".
            profiles.emplace_back(switchProfile("eden", "Eden (Switch)", SwitchEmulator::EDEN));
            profiles.emplace_back(
                switchProfile("citron", "citron (Switch)", SwitchEmulator::CITRON));
            profiles.emplace_back(switchProfile("suyu", "suyu (Switch)", SwitchEmulator::SUYU));
            profiles.emplace_back(switchProfile("yuzu", "yuzu (Switch)", SwitchEmulator::YUZU));

            Profile rpcs3;
            rpcs3.id = "rpcs3";
            rpcs3.name = "RPCS3 (PlayStation 3)";
            rpcs3.match = {"rpcs3"};
            rpcs3.format = Format::YAML;
            rpcs3.configFiles = {"~/.config/rpcs3/config.yml",
                                 "~/.var/app/net.rpcs3.RPCS3/config/rpcs3/config.yml"};
            rpcs3.gameFiles = GameFiles::RPCS3_CUSTOM;
            rpcs3.settings = {
                {"GRAPHICS API", "Video/Renderer", {{"VULKAN", "Vulkan"}, {"OPENGL", "OpenGL"}}},
                {"RESOLUTION SCALE",
                 "Video/Resolution Scale",
                 {{"50%", "50"},
                  {"100% (NATIVE)", "100"},
                  {"150%", "150"},
                  {"200%", "200"},
                  {"300%", "300"},
                  {"400%", "400"}}},
                {"ASPECT RATIO", "Video/Aspect ratio", {{"16:9", "16:9"}, {"4:3", "4:3"}}},
                {"VSYNC",
                 "Video/VSync Mode",
                 {{"OFF", "Disabled"}, {"ADAPTIVE", "Adaptive"}, {"ON", "Full"}}},
                {"FRAME LIMIT",
                 "Video/Frame limit",
                 {{"AUTO", "Auto"}, {"OFF", "Off"}, {"30", "30"}, {"50", "50"}, {"60", "60"}}},
                {"OUTPUT SCALING (FSR)",
                 "Video/Output Scaling Mode",
                 {{"NEAREST", "Nearest"},
                  {"BILINEAR", "Bilinear"},
                  {"AMD FSR", "FidelityFX Super Resolution"}}},
                {"ANISOTROPIC FILTERING",
                 "Video/Anisotropic Filter Override",
                 {{"AUTO", "0"}, {"2X", "2"}, {"4X", "4"}, {"8X", "8"}, {"16X", "16"}}},
                {"SHADER MODE",
                 "Video/Shader Mode",
                 {{"ASYNC RECOMPILER", "Async Shader Recompiler"},
                  {"ASYNC WITH INTERPRETER", "Async Recompiler with Shader Interpreter"}}},
                {"WRITE COLOR BUFFERS", "Video/Write Color Buffers", OFF_ON},
                {"MULTITHREADED RSX", "Video/Multithreaded RSX", OFF_ON},
                {"PPU DECODER",
                 "Core/PPU Decoder",
                 {{"RECOMPILER (LLVM)", "Recompiler (LLVM)"}, {"INTERPRETER", "Interpreter"}}},
                {"SPU DECODER",
                 "Core/SPU Decoder",
                 {{"RECOMPILER (LLVM)", "Recompiler (LLVM)"},
                  {"RECOMPILER (ASMJIT)", "Recompiler (ASMJIT)"}}},
                {"SPU BLOCK SIZE",
                 "Core/SPU Block Size",
                 {{"SAFE", "Safe"}, {"MEGA", "Mega"}, {"GIGA", "Giga"}}},
                {"SPU XFLOAT ACCURACY",
                 "Core/SPU XFloat Accuracy",
                 {{"ACCURATE", "Accurate"}, {"APPROXIMATE", "Approximate"}, {"RELAXED", "Relaxed"}}},
                {"PREFERRED SPU THREADS",
                 "Core/Preferred SPU Threads",
                 {{"AUTO", "0"},
                  {"1", "1"},
                  {"2", "2"},
                  {"3", "3"},
                  {"4", "4"},
                  {"5", "5"},
                  {"6", "6"}}},
                {"ACCURATE RSX RESERVATION ACCESS", "Core/Accurate RSX reservation access", OFF_ON},
                {"STRICT RENDERING MODE", "Video/Strict Rendering Mode", OFF_ON},
                {"RELAXED ZCULL SYNC", "Video/Relaxed ZCULL Sync", OFF_ON},
                {"ANTI-ALIASING (MSAA)", "Video/MSAA", {{"AUTO", "Auto"}, {"OFF", "Disabled"}}},
            };
            profiles.emplace_back(rpcs3);

            Profile pcsx2;
            pcsx2.id = "pcsx2";
            pcsx2.name = "PCSX2 (PlayStation 2)";
            pcsx2.match = {"pcsx2"};
            pcsx2.format = Format::INI;
            pcsx2.configFiles = {"~/.config/PCSX2/inis/PCSX2.ini",
                                 "~/.var/app/net.pcsx2.PCSX2/config/PCSX2/inis/PCSX2.ini"};
            pcsx2.gameFiles = GameFiles::PCSX2_GAMESETTINGS;
            pcsx2.closesItself = true;
            pcsx2.settings = {
                {"GRAPHICS API",
                 "EmuCore/GS/Renderer",
                 {{"AUTO", "-1"}, {"VULKAN", "14"}, {"OPENGL", "12"}, {"SOFTWARE", "13"}}},
                {"RESOLUTION",
                 "EmuCore/GS/upscale_multiplier",
                 {{"1X (NATIVE)", "1"},
                  {"2X", "2"},
                  {"3X", "3"},
                  {"4X", "4"},
                  {"5X", "5"},
                  {"6X", "6"},
                  {"8X", "8"}}},
                {"ASPECT RATIO",
                 "EmuCore/GS/AspectRatio",
                 {{"AUTO 4:3/3:2", "Auto 4:3/3:2"},
                  {"4:3", "4:3"},
                  {"16:9", "16:9"},
                  {"STRETCH", "Stretch"}}},
                {"VSYNC", "EmuCore/GS/VsyncEnable", OFF_ON},
                {"ANTI-ALIASING (FXAA)", "EmuCore/GS/fxaa", OFF_ON},
                {"ANISOTROPIC FILTERING",
                 "EmuCore/GS/MaxAnisotropy",
                 {{"OFF", "0"}, {"2X", "2"}, {"4X", "4"}, {"8X", "8"}, {"16X", "16"}}},
                {"SHARPENING (AMD CAS)",
                 "EmuCore/GS/CASMode",
                 {{"OFF", "0"}, {"SHARPEN ONLY", "1"}, {"SHARPEN AND RESIZE", "2"}}},
                {"BLENDING ACCURACY",
                 "EmuCore/GS/accurate_blending_unit",
                 {{"MINIMUM", "0"},
                  {"BASIC", "1"},
                  {"MEDIUM", "2"},
                  {"HIGH", "3"},
                  {"FULL", "4"},
                  {"MAXIMUM", "5"}}},
                {"MULTI-THREADED VU1 (MTVU)", "EmuCore/Speedhacks/vuThread", OFF_ON},
                {"EE CYCLE RATE",
                 "EmuCore/Speedhacks/EECycleRate",
                 {{"50%", "-3"},
                  {"60%", "-2"},
                  {"75%", "-1"},
                  {"100% (NORMAL)", "0"},
                  {"130%", "1"},
                  {"180%", "2"},
                  {"300%", "3"}}},
                {"FAST CDVD", "EmuCore/Speedhacks/fastCDVD", OFF_ON},
            };
            profiles.emplace_back(pcsx2);

            Profile ppsspp;
            ppsspp.id = "ppsspp";
            ppsspp.name = "PPSSPP (PSP)";
            ppsspp.match = {"ppsspp"};
            ppsspp.format = Format::INI;
            ppsspp.configFiles = {
                "~/.config/ppsspp/PSP/SYSTEM/ppsspp.ini",
                "~/.var/app/org.ppsspp.PPSSPP/config/ppsspp/PSP/SYSTEM/ppsspp.ini"};
            ppsspp.gameFiles = GameFiles::PPSSPP_GAME_INI;
            ppsspp.closesItself = true;
            const std::vector<Choice> trueFalse {{"OFF", "False"}, {"ON", "True"}};
            ppsspp.settings = {
                {"GRAPHICS API",
                 "Graphics/GraphicsBackend",
                 {{"VULKAN", "3 (VULKAN)"}, {"OPENGL", "0 (OPENGL)"}}},
                {"RESOLUTION",
                 "Graphics/InternalResolution",
                 {{"AUTO", "0"},
                  {"1X (NATIVE)", "1"},
                  {"2X", "2"},
                  {"3X", "3"},
                  {"4X", "4"},
                  {"5X", "5"},
                  {"6X", "6"},
                  {"8X", "8"},
                  {"10X", "10"}}},
                {"VSYNC", "Graphics/VerticalSync", trueFalse},
                {"ANTI-ALIASING (MSAA)",
                 "Graphics/MultiSampleLevel",
                 {{"OFF", "0"}, {"2X", "1"}, {"4X", "2"}, {"8X", "3"}}},
                {"ANISOTROPIC FILTERING",
                 "Graphics/AnisotropyLevel",
                 {{"OFF", "0"}, {"2X", "1"}, {"4X", "2"}, {"8X", "3"}, {"16X", "4"}}},
                {"TEXTURE SCALING",
                 "Graphics/TexScalingLevel",
                 {{"OFF", "1"}, {"2X", "2"}, {"3X", "3"}, {"4X", "4"}, {"5X", "5"}}},
                {"FRAME SKIPPING",
                 "Graphics/FrameSkip",
                 {{"OFF", "0"}, {"1", "1"}, {"2", "2"}, {"3", "3"}}},
                {"CPU CORE",
                 "CPU/CPUCore",
                 {{"INTERPRETER", "0"}, {"DYNAREC (JIT)", "1"}, {"IR INTERPRETER", "2"}}},
                {"FAST MEMORY", "CPU/FastMemoryAccess", trueFalse},
            };
            profiles.emplace_back(ppsspp);

            Profile cemu;
            cemu.id = "cemu";
            cemu.name = "Cemu (Wii U)";
            cemu.match = {"cemu"};
            cemu.format = Format::CEMU_XML;
            cemu.configFiles = {"~/.config/Cemu/settings.xml",
                                "~/.local/share/Cemu/settings.xml",
                                "~/.var/app/info.cemu.Cemu/config/Cemu/settings.xml"};
            cemu.gameFiles = GameFiles::CEMU_PROFILE;
            cemu.settings = {
                {"GRAPHICS API",
                 "Graphic/api",
                 {{"OPENGL", "0"}, {"VULKAN", "1"}},
                 true,
                 "Graphics/graphics_api"},
                {"VSYNC", "Graphic/VSync", {{"OFF", "0"}, {"ON", "1"}}, false},
                {"ASYNCHRONOUS SHADERS", "Graphic/AsyncCompile", OFF_ON, false},
                {"UPSCALE FILTER",
                 "Graphic/UpscaleFilter",
                 {{"BILINEAR", "0"}, {"BICUBIC", "1"}, {"HERMITE", "2"}, {"NEAREST", "3"}},
                 false},
                {"FULLSCREEN SCALING",
                 "Graphic/FullscreenScaling",
                 {{"KEEP ASPECT RATIO", "0"}, {"STRETCH", "1"}},
                 false},
                // Only in the profile of a game.
                {"CPU MODE",
                 "",
                 {{"AUTO", "Auto"},
                  {"SINGLE-CORE RECOMPILER", "Singlecore-Recompiler"},
                  {"MULTI-CORE RECOMPILER", "Multicore-Recompiler"}},
                 true,
                 "CPU/cpuMode"},
                {"ACCURATE SHADER MUL",
                 "",
                 {{"ON", "true"}, {"OFF", "false"}, {"MIN", "min"}},
                 true,
                 "Graphics/accurateShaderMul"},
            };
            profiles.emplace_back(cemu);

            Profile vita3k;
            vita3k.id = "vita3k";
            vita3k.name = "Vita3K (PS Vita)";
            vita3k.match = {"vita3k"};
            vita3k.format = Format::YAML;
            vita3k.configFiles = {"~/.config/Vita3K/config.yml"};
            vita3k.settings = {
                {"GRAPHICS API", "backend-renderer", {{"VULKAN", "Vulkan"}, {"OPENGL", "OpenGL"}},
                 false},
                {"RESOLUTION",
                 "resolution-multiplier",
                 {{"0.5X", "0.5"},
                  {"1X (NATIVE)", "1"},
                  {"1.5X", "1.5"},
                  {"2X", "2"},
                  {"3X", "3"},
                  {"4X", "4"}},
                 false},
                {"SCREEN FILTER (FSR)",
                 "screen-filter",
                 {{"NEAREST", "Nearest"},
                  {"BILINEAR", "Bilinear"},
                  {"BICUBIC", "Bicubic"},
                  {"FXAA", "FXAA"},
                  {"AMD FSR", "FSR"}},
                 false},
                {"VSYNC", "v-sync", OFF_ON, false},
                {"ANISOTROPIC FILTERING",
                 "anisotropic-filtering",
                 {{"1X", "1"}, {"2X", "2"}, {"4X", "4"}, {"8X", "8"}, {"16X", "16"}},
                 false},
                {"GPU HIGH ACCURACY", "high-accuracy", OFF_ON, false},
                {"SURFACE SYNC", "disable-surface-sync", {{"ON", "false"}, {"OFF", "true"}},
                 false},
                {"ASYNCHRONOUS SHADERS", "async-pipeline-compilation", OFF_ON, false},
            };
            profiles.emplace_back(vita3k);

            // RetroArch: retroarch.cfg stays as it is, the settings of EmuSPLASH are appended.
            Profile retroarch;
            retroarch.id = "retroarch";
            retroarch.name = "RetroArch";
            retroarch.match = {"retroarch"};
            retroarch.format = Format::RETROARCH_CFG;
            retroarch.configFiles = {
                "~/.config/retroarch/retroarch.cfg",
                "~/.var/app/org.libretro.RetroArch/config/retroarch/retroarch.cfg"};
            retroarch.gameFiles = GameFiles::RETROARCH_OVERRIDE;
            retroarch.ownSettingsFile = true;
            retroarch.closesItself = true;
            retroarch.settings = {
                {"GRAPHICS DRIVER",
                 "video_driver",
                 {{"VULKAN", "vulkan"}, {"OPENGL CORE", "glcore"}, {"OPENGL", "gl"}}},
                {"VSYNC", "video_vsync", OFF_ON},
                {"BILINEAR FILTERING", "video_smooth", OFF_ON},
                {"INTEGER SCALING", "video_scale_integer", OFF_ON},
                {"ASPECT RATIO",
                 "aspect_ratio_index",
                 {{"CORE PROVIDED", "22"}, {"4:3", "0"}, {"16:9", "1"}, {"FULL", "24"}}},
                {"SHADERS", "video_shader_enable", OFF_ON},
                {"THREADED VIDEO", "video_threaded", OFF_ON},
                {"RUN-AHEAD", "run_ahead_enabled", OFF_ON},
                {"RUN-AHEAD FRAMES",
                 "run_ahead_frames",
                 {{"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}}},
                {"REWIND", "rewind_enable", OFF_ON},
            };
            profiles.emplace_back(retroarch);
            return profiles;
        }

        //------------------------------------------------------------------------------------
        // Configuration files, changed line by line so everything else stays as it is.
        //------------------------------------------------------------------------------------

        std::string expandHome(const std::string& path)
        {
            if (path.rfind("~/", 0) == 0)
                return Utils::FileSystem::getHomePath() + path.substr(1);
            return path;
        }

        std::string trim(const std::string& text) { return Utils::String::trim(text); }

        // "Section/key" in two parts. The section of PCSX2 can contain a slash itself
        // ("EmuCore/GS/Renderer"), the key is after the last one.
        std::pair<std::string, std::string> splitKey(const std::string& key)
        {
            const size_t slash {key.rfind('/')};
            if (slash == std::string::npos)
                return {"", key};
            return {key.substr(0, slash), key.substr(slash + 1)};
        }

        class ConfigText
        {
        public:
            explicit ConfigText(Format format)
                : mFormat {format}
            {
            }

            bool load(const std::string& path)
            {
                mLines.clear();
                if (path.empty())
                    return false;
                std::ifstream file {path, std::ios::binary};
                if (!file.good())
                    return false;
                // Whether the last line ends with a newline, kept as it was.
                file.seekg(0, std::ios::end);
                if (file.tellg() > 0) {
                    file.seekg(-1, std::ios::end);
                    mFinalNewline = file.get() == '\n';
                }
                file.seekg(0, std::ios::beg);
                std::string line;
                while (std::getline(file, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                        mCrLf = true;
                    }
                    mLines.emplace_back(line);
                }
                if (!mLines.empty() && mLines.front().rfind("\xEF\xBB\xBF", 0) == 0) {
                    mBom = true;
                    mLines.front() = mLines.front().substr(3);
                }
                return true;
            }

            // backup: a copy of the file as it was before EmuSPLASH changed it the first time is
            // kept next to it (the configuration files of the emulators, not those of games).
            bool save(const std::string& path, std::string& error, bool backup = true)
            {
                const std::string backupFile {path + ".emusplash-backup"};
                // The copy made under the name of the earlier versions is the original file.
                const std::string earlierBackup {path + ".es-de-backup"};
                if (backup && !Utils::FileSystem::exists(backupFile) &&
                    Utils::FileSystem::isRegularFile(earlierBackup))
                    Utils::FileSystem::renameFile(earlierBackup, backupFile, false);
                if (backup && Utils::FileSystem::exists(path) &&
                    !Utils::FileSystem::exists(backupFile))
                    Utils::FileSystem::copyFile(path, backupFile, false);
                Utils::FileSystem::createDirectory(Utils::FileSystem::getParent(path));
                const std::string temporary {path + ".emusplash-new"};
                {
                    std::ofstream file {temporary, std::ios::binary | std::ios::trunc};
                    if (!file.good()) {
                        error = "Couldn't write " + path;
                        return false;
                    }
                    if (mBom)
                        file << "\xEF\xBB\xBF";
                    for (size_t index {0}; index < mLines.size(); ++index) {
                        file << mLines[index];
                        if (index + 1 < mLines.size() || mFinalNewline)
                            file << (mCrLf ? "\r\n" : "\n");
                    }
                }
                if (std::rename(temporary.c_str(), path.c_str()) != 0) {
                    error = "Couldn't replace " + path;
                    return false;
                }
                return true;
            }

            bool get(const std::string& key, std::string& value) const
            {
                const auto [section, name] = splitKey(key);
                const std::vector<std::string> sections {sectionOfLines()};
                for (size_t index {0}; index < mLines.size(); ++index) {
                    if (sections[index] == section && matches(mLines[index], name, value))
                        return true;
                }
                return false;
            }

            // All values of a key that is given more than once (the bindings of PCSX2).
            std::vector<std::string> getAll(const std::string& key) const
            {
                const auto [section, name] = splitKey(key);
                const std::vector<std::string> sections {sectionOfLines()};
                std::vector<std::string> values;
                std::string value;
                for (size_t index {0}; index < mLines.size(); ++index) {
                    if (sections[index] == section && matches(mLines[index], name, value))
                        values.emplace_back(value);
                }
                return values;
            }

            void set(const std::string& key, const std::string& value)
            {
                const auto [section, name] = splitKey(key);
                const std::vector<std::string> sections {sectionOfLines()};
                std::string old;
                for (size_t index {0}; index < mLines.size(); ++index) {
                    if (sections[index] == section && matches(mLines[index], name, old)) {
                        const std::string& line {mLines[index]};
                        mLines[index] = line.substr(0, line.find_first_not_of(" \t")) +
                                        makeLine(name, value);
                        return;
                    }
                }
                insert(section, makeLine(name, value));
            }

            // A key given more than once: the lines whose value is replaceable are removed and
            // the new value is added after the other lines of the key.
            void setOneOf(const std::string& key,
                          const std::string& value,
                          const std::function<bool(const std::string&)>& replaceable)
            {
                const auto [section, name] = splitKey(key);
                std::string old;
                for (size_t index {mLines.size()}; index > 0; --index) {
                    const std::vector<std::string> sections {sectionOfLines()};
                    if (sections[index - 1] == section && matches(mLines[index - 1], name, old) &&
                        replaceable(old))
                        mLines.erase(mLines.begin() + static_cast<std::ptrdiff_t>(index - 1));
                }
                const std::vector<std::string> sections {sectionOfLines()};
                for (size_t index {mLines.size()}; index > 0; --index) {
                    if (sections[index - 1] == section && matches(mLines[index - 1], name, old)) {
                        mLines.insert(mLines.begin() + static_cast<std::ptrdiff_t>(index),
                                      makeLine(name, value));
                        return;
                    }
                }
                insert(section, makeLine(name, value));
            }

            void remove(const std::string& key)
            {
                const auto [section, name] = splitKey(key);
                const std::vector<std::string> sections {sectionOfLines()};
                std::string old;
                for (size_t index {0}; index < mLines.size(); ++index) {
                    if (sections[index] == section && matches(mLines[index], name, old)) {
                        mLines.erase(mLines.begin() + static_cast<std::ptrdiff_t>(index));
                        return;
                    }
                }
            }

            // The keys of a section, in the order of the file.
            std::vector<std::string> keys(const std::string& section) const
            {
                const std::vector<std::string> sections {sectionOfLines()};
                std::vector<std::string> found;
                for (size_t index {0}; index < mLines.size(); ++index) {
                    if (sections[index] != section)
                        continue;
                    const std::string& line {mLines[index]};
                    const size_t equal {line.find('=')};
                    if (equal != std::string::npos)
                        found.emplace_back(trim(line.substr(0, equal)));
                }
                return found;
            }

            // The lines of a section without its header (to copy it into another file).
            std::vector<std::string> sectionLines(const std::string& section) const
            {
                const std::vector<std::string> sections {sectionOfLines()};
                std::vector<std::string> lines;
                for (size_t index {0}; index < mLines.size(); ++index) {
                    if (sections[index] == section && !trim(mLines[index]).empty())
                        lines.emplace_back(mLines[index]);
                }
                return lines;
            }

            // Replaces a section with the given lines, or adds it at the end.
            void replaceSection(const std::string& section, const std::vector<std::string>& lines)
            {
                for (size_t index {0}; index < mLines.size(); ++index) {
                    if (!isHeader(mLines[index], mFormat) || headerName(mLines[index]) != section)
                        continue;
                    size_t end {index + 1};
                    while (end < mLines.size() && !isHeader(mLines[end], mFormat))
                        ++end;
                    mLines.erase(mLines.begin() + static_cast<std::ptrdiff_t>(index + 1),
                                 mLines.begin() + static_cast<std::ptrdiff_t>(end));
                    mLines.insert(mLines.begin() + static_cast<std::ptrdiff_t>(index + 1),
                                  lines.begin(), lines.end());
                    return;
                }
                if (!mLines.empty() && !trim(mLines.back()).empty())
                    mLines.emplace_back("");
                mLines.emplace_back("[" + section + "]");
                mLines.insert(mLines.end(), lines.begin(), lines.end());
            }

            bool hasSection(const std::string& section) const
            {
                for (const std::string& line : mLines) {
                    if (isHeader(line, mFormat) && headerName(line) == section)
                        return true;
                }
                return false;
            }

            // Section headers without any setting under them are removed.
            void removeEmptySections()
            {
                if (mFormat != Format::INI && mFormat != Format::YAML &&
                    mFormat != Format::QT_INI)
                    return;
                for (size_t index {0}; index < mLines.size();) {
                    if (!isHeader(mLines[index], mFormat)) {
                        ++index;
                        continue;
                    }
                    size_t end {index + 1};
                    bool used {false};
                    while (end < mLines.size() && !isHeader(mLines[end], mFormat)) {
                        if (!trim(mLines[end]).empty())
                            used = true;
                        ++end;
                    }
                    if (used)
                        index = end;
                    else
                        mLines.erase(mLines.begin() + static_cast<std::ptrdiff_t>(index),
                                     mLines.begin() + static_cast<std::ptrdiff_t>(end));
                }
            }

            bool empty() const
            {
                return std::none_of(mLines.cbegin(), mLines.cend(), [](const std::string& line) {
                    return !trim(line).empty();
                });
            }

        private:
            static bool isHeader(const std::string& line, Format format)
            {
                if (format == Format::RETROARCH_CFG || format == Format::CEMU_XML)
                    return false;
                if (format == Format::YAML)
                    return !line.empty() && line.front() != ' ' && line.front() != '#' &&
                           line.back() == ':';
                const std::string text {trim(line)};
                return text.size() > 2 && text.front() == '[' && text.back() == ']';
            }

            std::string headerName(const std::string& line) const
            {
                const std::string text {trim(line)};
                if (mFormat == Format::YAML)
                    return text.substr(0, text.size() - 1);
                return text.substr(1, text.size() - 2);
            }

            // The section of every line, in one pass. A header line has none of its own
            // ("\n"), so it never matches a setting. For Cemu the section is the element the
            // line is directly in; for YAML only the lines indented by two spaces belong to
            // the section above them, unindented ones are in "" (files without sections).
            std::vector<std::string> sectionOfLines() const
            {
                std::vector<std::string> sections(mLines.size());
                if (mFormat == Format::RETROARCH_CFG)
                    return sections;
                if (mFormat == Format::CEMU_XML) {
                    std::vector<std::string> open;
                    for (size_t index {0}; index < mLines.size(); ++index) {
                        const std::string text {trim(mLines[index])};
                        sections[index] = open.empty() ? "" : open.back();
                        if (text.size() < 3 || text.front() != '<' || text[1] == '?' ||
                            text[1] == '!')
                            continue;
                        if (text[1] == '/') {
                            if (!open.empty())
                                open.pop_back();
                            sections[index] = "\n";
                        }
                        else if (text.find("</") == std::string::npos &&
                                 text.compare(text.size() - 2, 2, "/>") != 0) {
                            const size_t endName {text.find_first_of(" >")};
                            open.emplace_back(text.substr(1, endName - 1));
                            sections[index] = "\n";
                        }
                    }
                    return sections;
                }
                std::string current;
                for (size_t index {0}; index < mLines.size(); ++index) {
                    const std::string& line {mLines[index]};
                    if (isHeader(line, mFormat)) {
                        current = headerName(line);
                        sections[index] = "\n";
                        continue;
                    }
                    if (mFormat == Format::YAML) {
                        const bool child {line.size() > 2 && line[0] == ' ' && line[1] == ' ' &&
                                          line[2] != ' '};
                        const bool top {!line.empty() && line[0] != ' '};
                        sections[index] = child ? current : (top ? "" : "\n");
                        continue;
                    }
                    sections[index] = current;
                }
                return sections;
            }

            bool matches(const std::string& line, const std::string& name, std::string& value) const
            {
                switch (mFormat) {
                    case Format::QT_INI: {
                        if (line.size() > name.size() && line.compare(0, name.size(), name) == 0 &&
                            line[name.size()] == '=') {
                            value = line.substr(name.size() + 1);
                            return true;
                        }
                        return false;
                    }
                    case Format::INI:
                    case Format::RETROARCH_CFG: {
                        const size_t equal {line.find('=')};
                        if (equal == std::string::npos)
                            return false;
                        const std::string text {trim(line)};
                        if (text.front() == '#' || text.front() == ';')
                            return false;
                        if (trim(line.substr(0, equal)) != name)
                            return false;
                        value = trim(line.substr(equal + 1));
                        if (mFormat == Format::RETROARCH_CFG && value.size() >= 2 &&
                            value.front() == '"' && value.back() == '"')
                            value = value.substr(1, value.size() - 2);
                        return true;
                    }
                    case Format::YAML: {
                        const std::string text {trim(line)};
                        if (text.size() <= name.size() || text.compare(0, name.size(), name) != 0 ||
                            text[name.size()] != ':')
                            return false;
                        value = trim(text.substr(name.size() + 1));
                        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                            value = value.substr(1, value.size() - 2);
                        return true;
                    }
                    case Format::CEMU_XML: {
                        const std::string text {trim(line)};
                        const std::string open {"<" + name + ">"};
                        const std::string close {"</" + name + ">"};
                        if (text.rfind(open, 0) != 0 || text.size() < open.size() + close.size() ||
                            text.compare(text.size() - close.size(), close.size(), close) != 0)
                            return false;
                        value = text.substr(open.size(),
                                            text.size() - open.size() - close.size());
                        return true;
                    }
                }
                return false;
            }

            std::string makeLine(const std::string& name, const std::string& value) const
            {
                switch (mFormat) {
                    case Format::QT_INI:
                        return name + "=" + value;
                    case Format::INI:
                        return name + " = " + value;
                    case Format::RETROARCH_CFG:
                        return name + " = \"" + value + "\"";
                    case Format::YAML:
                        return name + ": " + value;
                    case Format::CEMU_XML:
                        return "<" + name + ">" + value + "</" + name + ">";
                }
                return "";
            }

            void insert(const std::string& section, const std::string& line)
            {
                if (mFormat == Format::RETROARCH_CFG) {
                    mLines.emplace_back(line);
                    return;
                }
                if (mFormat == Format::CEMU_XML) {
                    for (size_t index {0}; index < mLines.size(); ++index) {
                        if (trim(mLines[index]) == "</" + section + ">") {
                            const std::string& previous {mLines[index > 0 ? index - 1 : index]};
                            const std::string indent {
                                previous.substr(0, previous.find_first_not_of(" \t"))};
                            mLines.insert(mLines.begin() + static_cast<std::ptrdiff_t>(index),
                                          indent + line);
                            return;
                        }
                    }
                    return; // No such element: nothing is made up.
                }
                if (mFormat == Format::YAML && section.empty()) {
                    mLines.emplace_back(line);
                    return;
                }
                const std::string indent {mFormat == Format::YAML ? "  " : ""};
                for (size_t index {0}; index < mLines.size(); ++index) {
                    if (!isHeader(mLines[index], mFormat) || headerName(mLines[index]) != section)
                        continue;
                    // After the last line of the section that isn't empty.
                    size_t end {index + 1};
                    size_t last {index};
                    while (end < mLines.size() && !isHeader(mLines[end], mFormat)) {
                        if (!trim(mLines[end]).empty())
                            last = end;
                        ++end;
                    }
                    mLines.insert(mLines.begin() + static_cast<std::ptrdiff_t>(last + 1),
                                  indent + line);
                    return;
                }
                if (mFormat == Format::YAML) {
                    mLines.emplace_back(section + ":");
                }
                else {
                    if (!mLines.empty() && !trim(mLines.back()).empty())
                        mLines.emplace_back("");
                    mLines.emplace_back("[" + section + "]");
                }
                mLines.emplace_back(indent + line);
            }

            Format mFormat;
            std::vector<std::string> mLines;
            bool mBom {false};
            bool mCrLf {false};
            bool mFinalNewline {true};
        };

        // Values of RPCS3 and Vita3K that YAML would read as something else are quoted.
        std::string yamlValue(const std::string& value)
        {
            if (value.empty() || value.find(": ") != std::string::npos ||
                value.find(" #") != std::string::npos || value.front() == '#' ||
                value.front() == '"' || value.front() == '\'' || value.back() == ':')
                return "\"" + value + "\"";
            return value;
        }

        std::string fileValue(const Profile& profile, const std::string& value)
        {
            return profile.format == Format::YAML ? yamlValue(value) : value;
        }

        //------------------------------------------------------------------------------------
        // Reading the files of the games.
        //------------------------------------------------------------------------------------

        std::string readStart(const std::string& path, size_t bytes)
        {
            std::ifstream file {path, std::ios::binary};
            std::string data(bytes, '\0');
            file.read(&data[0], static_cast<std::streamsize>(bytes));
            data.resize(static_cast<size_t>(std::max<std::streamsize>(file.gcount(), 0)));
            return data;
        }

        std::string readWhole(const std::string& path, size_t maxBytes)
        {
            std::ifstream file {path, std::ios::binary};
            if (!file.good())
                return "";
            std::ostringstream data;
            data << file.rdbuf();
            std::string text {data.str()};
            if (text.size() > maxBytes)
                text.resize(maxBytes);
            return text;
        }

        uint32_t le32(const std::string& data, size_t offset)
        {
            if (offset + 4 > data.size())
                return 0;
            return static_cast<uint32_t>(static_cast<unsigned char>(data[offset])) |
                   static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 8 |
                   static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 16 |
                   static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 3])) << 24;
        }

        uint64_t le64(const std::string& data, size_t offset)
        {
            return static_cast<uint64_t>(le32(data, offset)) |
                   static_cast<uint64_t>(le32(data, offset + 4)) << 32;
        }

        // Reads bytes of a disc image at an offset of the uncompressed image.
        using DiscReader = std::function<std::string(uint64_t offset, size_t size)>;

        DiscReader plainReader(const std::string& image)
        {
            auto file = std::make_shared<std::ifstream>(image, std::ios::binary);
            return [file](uint64_t offset, size_t size) {
                std::string data(size, '\0');
                file->clear();
                file->seekg(static_cast<std::streamoff>(offset));
                file->read(&data[0], static_cast<std::streamsize>(size));
                data.resize(static_cast<size_t>(std::max<std::streamsize>(file->gcount(), 0)));
                return data;
            };
        }

#if defined(ES_HAS_ZLIB_HEADER) && defined(__linux__)
        // zlib is opened when first needed, so EmuSPLASH doesn't depend on it.
        struct Zlib {
            using InflateInit2 = int (*)(z_streamp, int, const char*, int);
            using Inflate = int (*)(z_streamp, int);
            using InflateEnd = int (*)(z_streamp);
            InflateInit2 init {nullptr};
            Inflate inflate {nullptr};
            InflateEnd end {nullptr};

            static const Zlib* get()
            {
                static const Zlib zlib {[] {
                    Zlib loaded;
                    void* library {dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL)};
                    if (library != nullptr) {
                        loaded.init = reinterpret_cast<InflateInit2>(dlsym(library, "inflateInit2_"));
                        loaded.inflate = reinterpret_cast<Inflate>(dlsym(library, "inflate"));
                        loaded.end = reinterpret_cast<InflateEnd>(dlsym(library, "inflateEnd"));
                    }
                    return loaded;
                }()};
                if (zlib.init == nullptr || zlib.inflate == nullptr || zlib.end == nullptr)
                    return nullptr;
                return &zlib;
            }
        };

        // A CSO image (PSP): blocks compressed with raw deflate and an index of their places.
        DiscReader csoReader(const std::string& image)
        {
            const Zlib* zlib {Zlib::get()};
            const std::string header {readStart(image, 24)};
            if (zlib == nullptr || header.size() < 24 || header.compare(0, 4, "CISO") != 0)
                return nullptr;
            const uint64_t totalBytes {le64(header, 8)};
            const uint32_t blockSize {le32(header, 16)};
            const unsigned int align {static_cast<unsigned char>(header[21])};
            if (blockSize == 0 || blockSize > 1024 * 1024 || align > 16)
                return nullptr;
            const uint64_t blocks {(totalBytes + blockSize - 1) / blockSize};
            DiscReader raw {plainReader(image)};
            const std::string index {raw(24, static_cast<size_t>((blocks + 1) * 4))};
            if (index.size() < (blocks + 1) * 4)
                return nullptr;
            return [=](uint64_t offset, size_t size) {
                std::string data;
                while (data.size() < size && offset < totalBytes) {
                    const uint64_t block {offset / blockSize};
                    const uint32_t entry {le32(index, block * 4)};
                    const uint32_t next {le32(index, (block + 1) * 4)};
                    const uint64_t start {static_cast<uint64_t>(entry & 0x7FFFFFFF) << align};
                    const uint64_t stop {static_cast<uint64_t>(next & 0x7FFFFFFF) << align};
                    std::string plain;
                    if (entry & 0x80000000) {
                        plain = raw(start, blockSize);
                    }
                    else {
                        std::string packed {raw(start, static_cast<size_t>(stop - start))};
                        plain.assign(blockSize, '\0');
                        z_stream stream {};
                        stream.next_in = reinterpret_cast<Bytef*>(&packed[0]);
                        stream.avail_in = static_cast<uInt>(packed.size());
                        stream.next_out = reinterpret_cast<Bytef*>(&plain[0]);
                        stream.avail_out = blockSize;
                        if (zlib->init(&stream, -15, ZLIB_VERSION,
                                       static_cast<int>(sizeof(z_stream))) != Z_OK)
                            break;
                        zlib->inflate(&stream, Z_FINISH);
                        plain.resize(blockSize - stream.avail_out);
                        zlib->end(&stream);
                    }
                    const size_t inBlock {static_cast<size_t>(offset % blockSize)};
                    if (plain.size() <= inBlock)
                        break;
                    const size_t take {std::min(size - data.size(), plain.size() - inBlock)};
                    data += plain.substr(inBlock, take);
                    offset += take;
                }
                return data;
            };
        }
#else
        DiscReader csoReader(const std::string&) { return nullptr; }
#endif

        DiscReader discReader(const std::string& image)
        {
            const std::string extension {
                Utils::String::toLower(Utils::FileSystem::getExtension(image))};
            if (extension == ".iso")
                return plainReader(image);
            if (extension == ".cso")
                return csoReader(image);
            return nullptr;
        }

        // A file in the root folder of an ISO 9660 image (a PS2 or PSP disc), empty if not
        // found. Images with 2048 byte sectors.
        std::string isoRootFile(const DiscReader& readAt,
                                const std::string& wanted,
                                size_t maxSize = 64 * 1024 * 1024)
        {
            if (!readAt)
                return "";
            const std::string descriptor {readAt(16 * 2048, 2048)};
            if (descriptor.size() < 190 || descriptor.compare(1, 5, "CD001") != 0)
                return "";
            const uint32_t rootSector {le32(descriptor, 156 + 2)};
            const uint32_t rootSize {std::min<uint32_t>(le32(descriptor, 156 + 10), 1024 * 1024)};
            const std::string directory {readAt(uint64_t {rootSector} * 2048, rootSize)};
            const std::string upperWanted {Utils::String::toUpper(wanted)};
            for (size_t offset {0}; offset < directory.size();) {
                const unsigned char length {static_cast<unsigned char>(directory[offset])};
                if (length == 0) {
                    // Records don't cross sectors, the next one starts at the next sector.
                    offset = (offset / 2048 + 1) * 2048;
                    continue;
                }
                if (offset + 33 > directory.size())
                    break;
                const unsigned char nameLength {static_cast<unsigned char>(directory[offset + 32])};
                std::string name {directory.substr(offset + 33, nameLength)};
                const size_t version {name.find(';')};
                if (version != std::string::npos)
                    name = name.substr(0, version);
                if (Utils::String::toUpper(name) == upperWanted) {
                    const uint32_t sector {le32(directory, offset + 2)};
                    const uint32_t size {le32(directory, offset + 10)};
                    if (size > maxSize)
                        return "";
                    return readAt(uint64_t {sector} * 2048, size);
                }
                offset += length;
            }
            return "";
        }

        // The title ID of a Switch game: [0100...] in the file name, or the ticket of an NSP
        // (its name starts with the title ID).
        std::string switchTitleId(FileData* game)
        {
            static const std::regex pattern {"\\[(01[0-9A-Fa-f]{14})\\]"};
            std::smatch match;
            const std::string path {game->getPath()};
            const std::string name {Utils::FileSystem::getFileName(path)};
            if (std::regex_search(name, match, pattern))
                return Utils::String::toUpper(match[1].str());
            if (Utils::String::toLower(Utils::FileSystem::getExtension(path)) != ".nsp")
                return "";
            // PFS0: "PFS0", file count, size of the name table, then 24 bytes for each file
            // and the names.
            const std::string header {readStart(path, 16)};
            if (header.size() < 16 || header.compare(0, 4, "PFS0") != 0)
                return "";
            const uint32_t files {le32(header, 4)};
            const uint32_t namesSize {le32(header, 8)};
            if (files == 0 || files > 4096 || namesSize > 1024 * 1024)
                return "";
            const std::string table {readStart(path, 16 + files * 24 + namesSize)};
            const std::string names {table.substr(std::min<size_t>(table.size(), 16 + files * 24))};
            static const std::regex ticket {"(01[0-9A-Fa-f]{14})[0-9A-Fa-f]{16}\\.tik"};
            if (std::regex_search(names, match, ticket))
                return Utils::String::toUpper(match[1].str());
            return "";
        }

        std::string ps3Serial(FileData* game)
        {
            static const std::regex pattern {"([A-Z]{4}[0-9]{5})"};
            std::smatch match;
            std::string path {game->getPath()};
            std::string text;
            if (Utils::String::toLower(Utils::FileSystem::getExtension(path)) == ".desktop") {
                // A shortcut of RPCS3: --no-gui "%%RPCS3_GAMEID%%:BLES01807".
                text = readStart(path, 16384);
            }
            else {
                // A game folder or a file inside one: PS3_GAME/PARAM.SFO, or PARAM.SFO above
                // USRDIR/EBOOT.BIN.
                for (std::string folder {path}; !folder.empty() && folder != "/";
                     folder = Utils::FileSystem::getParent(folder)) {
                    for (const std::string& sfo :
                         {folder + "/PS3_GAME/PARAM.SFO", folder + "/PARAM.SFO"}) {
                        if (Utils::FileSystem::isRegularFile(sfo)) {
                            text = readStart(sfo, 65536);
                            break;
                        }
                    }
                    if (!text.empty() ||
                        folder.size() <= game->getSystem()->getRootFolder()->getPath().size())
                        break;
                }
            }
            if (std::regex_search(text, match, pattern))
                return match[1].str();
            if (std::regex_search(path, match, pattern))
                return match[1].str();
            return "";
        }

        // The serial of a PS2 disc ("SLUS-20946") and the name of its program on the disc.
        bool ps2Serial(const DiscReader& readAt, std::string& serial, std::string& program)
        {
            const std::string config {isoRootFile(readAt, "SYSTEM.CNF", 4096)};
            static const std::regex boot {"BOOT2\\s*=\\s*cdrom0?:\\\\?([A-Za-z]{4})_([0-9]{3})\\."
                                          "([0-9]{2})"};
            std::smatch match;
            if (!std::regex_search(config, match, boot))
                return false;
            const std::string letters {Utils::String::toUpper(match[1].str())};
            serial = letters + "-" + match[2].str() + match[3].str();
            program = letters + "_" + match[2].str() + "." + match[3].str();
            return true;
        }

        // The CRC PCSX2 names the settings of a game with: the 32-bit words of its program
        // XORed together.
        std::string pcsx2Crc(const DiscReader& readAt, const std::string& program)
        {
            const std::string elf {isoRootFile(readAt, program)};
            if (elf.size() < 4)
                return "";
            uint32_t crc {0};
            for (size_t offset {0}; offset + 4 <= elf.size(); offset += 4)
                crc ^= le32(elf, offset);
            char text[16];
            std::snprintf(text, sizeof(text), "%08X", crc);
            return text;
        }

        // The serial and CRC of a game in the game list of PCSX2 (cache/gamelist.cache), which
        // PCSX2 makes of the folders of its game list, for every format it reads (.chd too).
        // An entry is the path, the serial and the titles as strings with their length in
        // front, then the type and region (a byte each), the size of the file and the time of
        // change (8 bytes each) and the CRC. The size of the file tells where the CRC is,
        // whatever number of titles the version of PCSX2 writes.
        bool pcsx2CacheEntry(const std::string& cacheFile,
                             const std::string& romPath,
                             std::string& serial,
                             std::string& crc)
        {
            const long fileSize {Utils::FileSystem::getFileSize(romPath)};
            if (fileSize <= 0)
                return false;
            const std::string data {readWhole(cacheFile, 64 * 1024 * 1024)};
            static const std::regex serialPattern {"^[A-Z]{4}-[0-9]{3,5}$"};
            for (size_t found {data.find(romPath)}; found != std::string::npos;
                 found = data.find(romPath, found + 1)) {
                if (found < 4 || le32(data, found - 4) != romPath.size())
                    continue;
                size_t position {found + romPath.size()};
                const uint32_t serialLength {le32(data, position)};
                if (serialLength == 0 || serialLength > 32)
                    continue;
                const std::string entrySerial {data.substr(position + 4, serialLength)};
                if (!std::regex_match(entrySerial, serialPattern))
                    continue;
                position += 4 + serialLength;
                for (int strings {0}; strings < 6 && position + 22 <= data.size(); ++strings) {
                    if (le64(data, position + 2) == static_cast<uint64_t>(fileSize)) {
                        char text[16];
                        std::snprintf(text, sizeof(text), "%08X", le32(data, position + 18));
                        serial = entrySerial;
                        crc = text;
                        return true;
                    }
                    const uint32_t length {le32(data, position)};
                    if (length > 4096)
                        break;
                    position += 4 + length;
                }
            }
            return false;
        }

        std::string pspGameId(FileData* game)
        {
            static const std::regex pattern {"([A-Z]{4})-?([0-9]{5})"};
            std::smatch match;
            const std::string path {game->getPath()};
            const std::string text {isoRootFile(discReader(path), "UMD_DATA.BIN", 4096)};
            if (std::regex_search(text, match, pattern))
                return match[1].str() + match[2].str();
            const std::string name {Utils::FileSystem::getFileName(path)};
            if (std::regex_search(name, match, pattern))
                return match[1].str() + match[2].str();
            return "";
        }

        // The title ID of a Wii U game: meta/meta.xml of a game folder, or the title list of
        // Cemu (title_list_cache.xml), which has the games of its game paths in any format.
        std::string wiiuTitleId(FileData* game)
        {
            static const std::regex metaPattern {"<title_id[^>]*>\\s*([0-9A-Fa-f]{16})\\s*<"};
            static const std::regex idPattern {"(0005000[0-9A-Fa-f]{9})"};
            std::smatch match;
            const std::string path {game->getPath()};
            const std::string root {game->getSystem()->getRootFolder()->getPath()};
            for (std::string folder {path}; folder.size() > root.size();
                 folder = Utils::FileSystem::getParent(folder)) {
                const std::string meta {folder + "/meta/meta.xml"};
                if (Utils::FileSystem::isRegularFile(meta)) {
                    const std::string text {readStart(meta, 65536)};
                    if (std::regex_search(text, match, metaPattern))
                        return Utils::String::toLower(match[1].str());
                    break;
                }
            }
            std::string escaped {Utils::String::replace(path, "&", "&amp;")};
            escaped = Utils::String::replace(escaped, "<", "&lt;");
            escaped = Utils::String::replace(escaped, ">", "&gt;");
            for (const std::string& candidate :
                 {std::string("~/.local/share/Cemu/title_list_cache.xml"),
                  std::string("~/.config/Cemu/title_list_cache.xml"),
                  std::string("~/.var/app/info.cemu.Cemu/data/Cemu/title_list_cache.xml")}) {
                const std::string text {readWhole(expandHome(candidate), 16 * 1024 * 1024)};
                for (const std::string& wanted : {escaped, path}) {
                    const size_t found {text.find(wanted)};
                    if (found == std::string::npos)
                        continue;
                    // The title element the path is in.
                    const size_t start {text.rfind("<title", found)};
                    const size_t end {text.find("</title>", found)};
                    if (start == std::string::npos)
                        continue;
                    const std::string element {
                        text.substr(start, end == std::string::npos ? found - start : end - start)};
                    if (std::regex_search(element, match, idPattern))
                        return Utils::String::toLower(match[1].str());
                }
            }
            return "";
        }

        // The folder of the game profiles of Cemu.
        std::string cemuProfileFolder()
        {
            const std::vector<std::string> folders {
                expandHome("~/.local/share/Cemu/gameProfiles"),
                expandHome("~/.config/Cemu/gameProfiles"),
                expandHome("~/.var/app/info.cemu.Cemu/data/Cemu/gameProfiles")};
            for (const std::string& folder : folders) {
                if (Utils::FileSystem::isDirectory(folder))
                    return folder;
            }
            if (Utils::FileSystem::isDirectory(expandHome("~/.var/app/info.cemu.Cemu")))
                return folders[2];
            return folders[0];
        }

        std::string retroArchFolder()
        {
            return Utils::FileSystem::getAppDataDirectory() + "/retroarch";
        }

        std::string retroArchSettingsFile()
        {
            const std::string file {retroArchFolder() + "/emusplash.cfg"};
            // The file of the earlier versions gets the new name.
            const std::string earlierFile {retroArchFolder() + "/es-de.cfg"};
            if (!Utils::FileSystem::exists(file) && Utils::FileSystem::isRegularFile(earlierFile))
                Utils::FileSystem::renameFile(earlierFile, file, false);
            return file;
        }

        //------------------------------------------------------------------------------------
        // Hotkeys.
        //------------------------------------------------------------------------------------

        std::string joinCombo(const std::vector<std::string>& buttons,
                              const std::map<std::string, std::string>& names,
                              const std::string& separator)
        {
            std::string text;
            for (const std::string& button : buttons) {
                const auto name = names.find(button);
                if (name == names.cend())
                    return "";
                text += (text.empty() ? "" : separator) + name->second;
            }
            return text;
        }

        void applySwitchHotkeys(ConfigText& config)
        {
            // yuzu names the buttons as on a Switch pad: the right one is A.
            static const std::map<std::string, std::string> names {
                {"south", "B"}, {"east", "A"},   {"west", "Y"},   {"north", "X"},
                {"back", "Minus"}, {"start", "Plus"}, {"l1", "L"}, {"r1", "R"},
                {"l2", "ZL"},   {"r2", "ZR"},    {"l3", "Left_Stick"}, {"r3", "Right_Stick"},
                {"guide", "Home"}};
            auto setShortcut = [&config](const std::string& action, const std::string& combo) {
                const std::string base {"UI/Shortcuts\\Main%20Window\\" + action +
                                        "\\Controller_KeySeq"};
                config.set(base + "\\default", "false");
                config.set(base, combo);
            };
            // The button the emulator gets written second ("A+Minus"), as in its own files.
            auto switchCombo = [](const std::vector<std::string>& buttons) {
                std::vector<std::string> reversed {buttons.rbegin(), buttons.rend()};
                return joinCombo(reversed, names, "+");
            };
            const std::string close {switchCombo(GameHotkeys::getCombination("HotkeyCloseGame"))};
            const std::string fast {switchCombo(GameHotkeys::getCombination("HotkeyFastForward"))};
            const std::vector<std::string> keys {config.keys("UI")};
            bool turbo {false};
            for (const std::string& key : keys) {
                if (key.find("\\Toggle%20Turbo%20Speed\\") != std::string::npos)
                    turbo = true;
            }
            for (const std::string& key : keys) {
                static const std::string window {"Shortcuts\\Main%20Window\\"};
                static const std::string suffix {"\\Controller_KeySeq"};
                if (key.rfind(window + "Exit%20", 0) != 0 || key.size() <= suffix.size() ||
                    key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0)
                    continue;
                const std::string action {
                    key.substr(window.size(), key.size() - suffix.size() - window.size())};
                if (action == "Exit%20Fullscreen" || close.empty())
                    continue;
                setShortcut(action, close);
            }
            if (!fast.empty())
                setShortcut(turbo ? "Toggle%20Turbo%20Speed" : "Toggle%20Framerate%20Limit", fast);
        }

        void applyPcsx2Hotkeys(ConfigText& config)
        {
            // The pad of the user's own bindings ("SDL-0/FaceSouth").
            std::string device {"SDL-0"};
            std::string cross;
            if (config.get("Pad1/Cross", cross) && cross.rfind("SDL-", 0) == 0 &&
                cross.find('/') != std::string::npos)
                device = cross.substr(0, cross.find('/'));
            const std::map<std::string, std::string> names {
                {"south", device + "/FaceSouth"}, {"east", device + "/FaceEast"},
                {"west", device + "/FaceWest"},   {"north", device + "/FaceNorth"},
                {"back", device + "/Back"},       {"start", device + "/Start"},
                {"guide", device + "/Guide"},     {"l1", device + "/LeftShoulder"},
                {"r1", device + "/RightShoulder"}, {"l2", device + "/+LeftTrigger"},
                {"r2", device + "/+RightTrigger"}, {"l3", device + "/LeftStick"},
                {"r3", device + "/RightStick"}};
            const std::vector<std::pair<std::string, std::string>> actions {
                {"HotkeyCloseGame", "Hotkeys/ShutdownVM"},
                {"HotkeySaveState", "Hotkeys/SaveStateToSlot"},
                {"HotkeyLoadState", "Hotkeys/LoadStateFromSlot"},
                {"HotkeyFastForward", "Hotkeys/ToggleTurbo"}};
            // A hotkey of PCSX2 can have several bindings, each on its own line: the keyboard
            // ones stay, the one of the pad is replaced.
            auto isPad = [](const std::string& value) {
                return value.find("SDL-") != std::string::npos ||
                       value.find("XInput-") != std::string::npos ||
                       value.find("DInput-") != std::string::npos;
            };
            for (const auto& [setting, key] : actions) {
                const std::string combo {
                    joinCombo(GameHotkeys::getCombination(setting), names, " & ")};
                if (combo.empty())
                    continue;
                const std::vector<std::string> current {config.getAll(key)};
                if (std::find(current.cbegin(), current.cend(), combo) != current.cend())
                    continue;
                config.setOneOf(key, combo, isPad);
            }
        }

        // The pad codes of PPSSPP: the ones the user mapped to the PSP buttons, otherwise the
        // ones PPSSPP gives the buttons of an SDL pad.
        std::map<std::string, std::string> ppssppButtonCodes(const ConfigText& controls)
        {
            std::map<std::string, std::string> names {
                {"south", "10-189"}, {"east", "10-190"},   {"west", "10-191"},
                {"north", "10-188"}, {"back", "10-196"},   {"start", "10-197"},
                {"l1", "10-194"},    {"r1", "10-195"},     {"l2", "10-4034"},
                {"r2", "10-4036"},   {"l3", "10-106"},     {"r3", "10-107"}};
            const std::vector<std::pair<std::string, std::string>> mapped {
                {"south", "Cross"}, {"east", "Circle"}, {"west", "Square"}, {"north", "Triangle"},
                {"back", "Select"}, {"start", "Start"}, {"l1", "L"},        {"r1", "R"}};
            for (const auto& [button, control] : mapped) {
                std::string value;
                if (!controls.get("ControlMapping/" + control, value))
                    continue;
                for (const std::string& entry : Utils::String::delimitedStringToVector(value, ",")) {
                    if (entry.rfind("10-", 0) == 0 && entry.find(':') == std::string::npos) {
                        names[button] = entry;
                        break;
                    }
                }
            }
            return names;
        }

        // The hotkeys in the [ControlMapping] of PPSSPP, true if something changed.
        bool setPpssppHotkeys(ConfigText& config, const std::map<std::string, std::string>& names)
        {
            const std::vector<std::pair<std::string, std::string>> actions {
                {"HotkeyCloseGame", "Exit App"},
                {"HotkeySaveState", "Save State"},
                {"HotkeyLoadState", "Load State"},
                {"HotkeyFastForward", "Fast-forward"}};
            bool changed {false};
            for (const auto& [setting, action] : actions) {
                const std::string combo {
                    joinCombo(GameHotkeys::getCombination(setting), names, ":")};
                if (combo.empty())
                    continue;
                // The keyboard keys of the action stay, the pad combinations are replaced.
                std::string value;
                config.get("ControlMapping/" + action, value);
                std::string updated;
                for (const std::string& entry : Utils::String::delimitedStringToVector(value, ",")) {
                    if (entry.rfind("10-", 0) != 0 && !entry.empty())
                        updated += (updated.empty() ? "" : ",") + entry;
                }
                updated += (updated.empty() ? "" : ",") + combo;
                if (updated != value) {
                    config.set("ControlMapping/" + action, updated);
                    changed = true;
                }
            }
            return changed;
        }

        void applyPpssppHotkeys(const std::string& configFile, const std::string& gameFile)
        {
            const std::string controlsFile {Utils::FileSystem::getParent(configFile) +
                                            "/controls.ini"};
            ConfigText controls {Format::INI};
            if (!controls.load(controlsFile))
                return;
            const std::map<std::string, std::string> names {ppssppButtonCodes(controls)};
            std::string error;
            if (setPpssppHotkeys(controls, names) && !controls.save(controlsFile, error)) {
                LOG(LogWarning) << "EmulatorProfiles: " << error;
            }
            // The file of a game can have its own [ControlMapping] (PPSSPP keeps the controls
            // of a game there too).
            if (gameFile.empty() || !Utils::FileSystem::isRegularFile(gameFile))
                return;
            ConfigText game {Format::INI};
            if (game.load(gameFile) && game.hasSection("ControlMapping") &&
                setPpssppHotkeys(game, names) && !game.save(gameFile, error, false)) {
                LOG(LogWarning) << "EmulatorProfiles: " << error;
            }
        }

        // The number RetroArch gives a button or axis of the first pad, as its udev or sdl2
        // joypad driver counts them: "6" for a button, "+2" for an axis, empty if unknown.
        std::map<std::string, std::string> retroArchPadCodes(const std::string& driver)
        {
            std::map<std::string, std::string> codes;
            if (driver == "sdl2") {
                // The buttons of an SDL game controller, the triggers are its axes 4 and 5.
                codes = {{"south", "0"}, {"east", "1"},  {"west", "2"},  {"north", "3"},
                         {"back", "4"},  {"guide", "5"}, {"start", "6"}, {"l3", "7"},
                         {"r3", "8"},    {"l1", "9"},    {"r1", "10"},   {"l2", "+4"},
                         {"r2", "+5"}};
                return codes;
            }
            if (driver != "udev")
                return codes;
            const std::vector<std::pair<std::string, SDL_GameControllerButton>> buttons {
                {"south", SDL_CONTROLLER_BUTTON_A},
                {"east", SDL_CONTROLLER_BUTTON_B},
                {"west", SDL_CONTROLLER_BUTTON_X},
                {"north", SDL_CONTROLLER_BUTTON_Y},
                {"back", SDL_CONTROLLER_BUTTON_BACK},
                {"guide", SDL_CONTROLLER_BUTTON_GUIDE},
                {"start", SDL_CONTROLLER_BUTTON_START},
                {"l3", SDL_CONTROLLER_BUTTON_LEFTSTICK},
                {"r3", SDL_CONTROLLER_BUTTON_RIGHTSTICK},
                {"l1", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
                {"r1", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}};
            // The udev driver counts the buttons and axes of the event device in the order of
            // their codes, as SDL does: the SDL mapping of the pad tells which one is which.
            for (int index {0}; index < SDL_NumJoysticks() && codes.empty(); ++index) {
                if (!SDL_IsGameController(index))
                    continue;
                SDL_GameController* controller {SDL_GameControllerOpen(index)};
                if (controller == nullptr)
                    continue;
                bool evdev {true};
#if SDL_VERSION_ATLEAST(2, 24, 0)
                const char* path {SDL_JoystickPath(SDL_GameControllerGetJoystick(controller))};
                evdev = path != nullptr && std::string(path).rfind("/dev/input/event", 0) == 0;
#endif
                if (evdev) {
                    for (const auto& [name, button] : buttons) {
                        const SDL_GameControllerButtonBind bind {
                            SDL_GameControllerGetBindForButton(controller, button)};
                        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON)
                            codes[name] = std::to_string(bind.value.button);
                    }
                    for (const auto& [name, axis] :
                         {std::make_pair(std::string("l2"), SDL_CONTROLLER_AXIS_TRIGGERLEFT),
                          std::make_pair(std::string("r2"), SDL_CONTROLLER_AXIS_TRIGGERRIGHT)}) {
                        const SDL_GameControllerButtonBind bind {
                            SDL_GameControllerGetBindForAxis(controller, axis)};
                        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_AXIS)
                            codes[name] = "+" + std::to_string(bind.value.axis);
                        else if (bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON)
                            codes[name] = std::to_string(bind.value.button);
                    }
                }
                SDL_GameControllerClose(controller);
            }
#if defined(__linux__) && !defined(__ANDROID__)
            if (!codes.empty())
                return codes;
            // Without SDL: the first event device with the buttons of a pad, counted as the
            // udev driver of RetroArch does.
            DIR* folder {opendir("/dev/input")};
            if (folder == nullptr)
                return codes;
            std::vector<std::string> devices;
            while (dirent* entry {readdir(folder)}) {
                if (std::string(entry->d_name).rfind("event", 0) == 0)
                    devices.emplace_back(std::string("/dev/input/") + entry->d_name);
            }
            closedir(folder);
            std::sort(devices.begin(), devices.end(), [](const std::string& a, const std::string& b) {
                return a.size() != b.size() ? a.size() < b.size() : a < b;
            });
            auto testBit = [](const unsigned char* bits, int bit) {
                return ((bits[bit / 8] >> (bit % 8)) & 1) != 0;
            };
            for (const std::string& device : devices) {
                const int fd {open(device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
                if (fd < 0)
                    continue;
                unsigned char keys[KEY_MAX / 8 + 1] {};
                unsigned char axes[ABS_MAX / 8 + 1] {};
                const bool read {ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 &&
                                 ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(axes)), axes) >= 0};
                close(fd);
                if (!read || !testBit(keys, BTN_SOUTH) || !testBit(keys, BTN_SELECT))
                    continue;
                std::map<int, int> buttonIndex;
                int count {0};
                for (int code {KEY_UP}; code <= KEY_DOWN; ++code) {
                    if (testBit(keys, code))
                        buttonIndex[code] = count++;
                }
                for (int code {BTN_MISC}; code < KEY_MAX; ++code) {
                    if (testBit(keys, code))
                        buttonIndex[code] = count++;
                }
                std::map<int, int> axisIndex;
                count = 0;
                for (int code {0}; code < ABS_MISC; ++code) {
                    if (code >= ABS_HAT0X && code <= ABS_HAT3Y)
                        continue;
                    if (testBit(axes, code))
                        axisIndex[code] = count++;
                }
                const std::vector<std::pair<std::string, int>> evdevButtons {
                    {"south", BTN_SOUTH}, {"east", BTN_EAST},   {"west", BTN_WEST},
                    {"north", BTN_NORTH}, {"back", BTN_SELECT}, {"start", BTN_START},
                    {"guide", BTN_MODE},  {"l1", BTN_TL},       {"r1", BTN_TR},
                    {"l3", BTN_THUMBL},   {"r3", BTN_THUMBR},   {"l2", BTN_TL2},
                    {"r2", BTN_TR2}};
                for (const auto& [name, code] : evdevButtons) {
                    if (buttonIndex.count(code))
                        codes[name] = std::to_string(buttonIndex[code]);
                }
                if (!codes.count("l2") && axisIndex.count(ABS_Z))
                    codes["l2"] = "+" + std::to_string(axisIndex[ABS_Z]);
                if (!codes.count("r2") && axisIndex.count(ABS_RZ))
                    codes["r2"] = "+" + std::to_string(axisIndex[ABS_RZ]);
                break;
            }
#endif
            return codes;
        }

        // The hotkeys of RetroArch, in a file of EmuSPLASH that is appended to its configuration.
        // RetroArch has one enable button for all its hotkeys, the first button of save state,
        // load state, fast forward or else close game; other combinations work only if they
        // start with the same button. closeGiven tells if RetroArch got the close hotkey (EmuSPLASH
        // ends the game itself otherwise).
        std::string writeRetroArchHotkeys(const std::string& configFile, bool& closeGiven)
        {
            closeGiven = false;
            ConfigText base {Format::RETROARCH_CFG};
            base.load(configFile);
            std::string driver;
            if (!base.get("input_joypad_driver", driver) || driver.empty())
                driver = "udev";
            const std::map<std::string, std::string> codes {retroArchPadCodes(driver)};
            if (codes.empty()) {
                LOG(LogWarning) << "EmulatorProfiles: The buttons of the pad for RetroArch (joypad "
                                   "driver \""
                                << driver << "\") couldn't be found, no hotkeys written";
                return "";
            }
            const std::vector<std::pair<std::string, std::string>> actions {
                {"HotkeyCloseGame", "input_exit_emulator"},
                {"HotkeySaveState", "input_save_state"},
                {"HotkeyLoadState", "input_load_state"},
                {"HotkeyFastForward", "input_toggle_fast_forward"}};
            std::string enable;
            for (const char* setting :
                 {"HotkeySaveState", "HotkeyLoadState", "HotkeyFastForward", "HotkeyCloseGame"}) {
                const std::vector<std::string> buttons {GameHotkeys::getCombination(setting)};
                if (buttons.size() == 2) {
                    enable = buttons.front();
                    break;
                }
            }
            if (enable.empty() || !codes.count(enable))
                return "";
            ConfigText hotkeys {Format::RETROARCH_CFG};
            auto bind = [&hotkeys](const std::string& key, const std::string& code) {
                const bool axis {!code.empty() && (code.front() == '+' || code.front() == '-')};
                hotkeys.set(key + "_btn", axis ? "nul" : code);
                hotkeys.set(key + "_axis", axis ? code : "nul");
            };
            bind("input_enable_hotkey", codes.at(enable));
            for (const auto& [setting, key] : actions) {
                const std::vector<std::string> buttons {GameHotkeys::getCombination(setting)};
                if (buttons.size() != 2 || buttons.front() != enable || !codes.count(buttons.back())) {
                    if (!buttons.empty()) {
                        LOG(LogInfo) << "EmulatorProfiles: " << setting
                                     << " can't be given to RetroArch (it has one enable button)";
                    }
                    continue;
                }
                bind(key, codes.at(buttons.back()));
                if (setting == "HotkeyCloseGame")
                    closeGiven = true;
            }
            // One press of the close hotkey is enough.
            hotkeys.set("quit_press_twice", "false");
            const std::string file {retroArchFolder() + "/hotkeys.cfg"};
            std::string error;
            if (!hotkeys.save(file, error, false)) {
                LOG(LogWarning) << "EmulatorProfiles: " << error;
                return "";
            }
            return file;
        }

        // The file of a value of RetroArch that applies: the game, EmuSPLASH or retroarch.cfg.
        std::string retroArchValue(const std::string& key,
                                   const std::string& gameFile,
                                   const std::string& configFile)
        {
            for (const std::string& file : {gameFile, retroArchSettingsFile(), configFile}) {
                ConfigText config {Format::RETROARCH_CFG};
                std::string value;
                if (config.load(file) && config.get(key, value))
                    return value;
            }
            return "";
        }

        // The end of the program (the first word) of a launch command, and the word itself.
        // Variables in front of it ("QT_QPA_PLATFORM=xcb", "%STARTDIR%=~/.mame", "env") and
        // "flatpak-spawn --host" are skipped, of "flatpak run <id>" the ID is the program. A
        // quoted path or one with escaped spaces can contain spaces.
        std::string programOf(const std::string& command, size_t* start, size_t* end)
        {
            size_t position {0};
            bool flatpakRun {false};
            while (position < command.size()) {
                while (position < command.size() && command[position] == ' ')
                    ++position;
                if (position >= command.size())
                    break;
                const size_t wordStart {position};
                std::string word;
                if (command[position] == '"') {
                    const size_t close {command.find('"', position + 1)};
                    word = command.substr(position + 1, close == std::string::npos ?
                                                            std::string::npos :
                                                            close - position - 1);
                    position = close == std::string::npos ? command.size() : close + 1;
                }
                else {
                    while (position < command.size() && command[position] != ' ') {
                        if (command[position] == '\\' && position + 1 < command.size()) {
                            word += command[position + 1];
                            position += 2;
                            continue;
                        }
                        word += command[position++];
                    }
                }
                if (word == "flatpak") {
                    flatpakRun = true;
                    continue;
                }
                // Flags of EmuSPLASH such as %ENABLESHORTCUTS% and %HIDEWINDOW%.
                const bool flag {word.size() > 2 && word.front() == '%' && word.back() == '%' &&
                                 word.rfind("%EMULATOR_", 0) != 0};
                if (flag || word.find('=') != std::string::npos || word == "flatpak-spawn" ||
                    word == "--host" || word == "env" || word == "run" ||
                    (flatpakRun && word.rfind("-", 0) == 0))
                    continue;
                if (start != nullptr)
                    *start = wordStart;
                if (end != nullptr)
                    *end = position;
                return word;
            }
            return "";
        }

        // True if RetroArch got the close hotkey.
        bool prepareRetroArch(const Profile& profile,
                              FileData* game,
                              std::string& command,
                              bool hotkeys)
        {
            bool closeGiven {false};
            const std::string configFile {getConfigFile(profile)};
            std::vector<std::string> files;
            if (Utils::FileSystem::isRegularFile(retroArchSettingsFile()))
                files.emplace_back(retroArchSettingsFile());
            std::string reason;
            const std::string gameFile {getGameFile(profile, game, reason)};
            if (!gameFile.empty() && Utils::FileSystem::isRegularFile(gameFile))
                files.emplace_back(gameFile);
            if (hotkeys) {
                const std::string hotkeyFile {writeRetroArchHotkeys(configFile, closeGiven)};
                if (!hotkeyFile.empty())
                    files.emplace_back(hotkeyFile);
            }
            size_t start {0};
            size_t end {0};
            const std::string program {programOf(command, &start, &end)};
            if (program.empty())
                return closeGiven;
            if (!files.empty()) {
                std::string list;
                for (const std::string& file : files)
                    list += (list.empty() ? "" : "|") + file;
                command.insert(end, " --appendconfig \"" + list + "\"");
                LOG(LogInfo) << "EmulatorProfiles: RetroArch gets the settings of EmuSPLASH: " << list;
            }
            // The AppImage of RetroArch has an old libwayland-client that the Vulkan drivers
            // of Mesa can't use (undefined symbol wl_fixes_interface): the one of the system
            // is loaded first.
            static const std::string wayland {"/usr/lib/x86_64-linux-gnu/libwayland-client.so.0"};
            if (Utils::String::toLower(program).find(".appimage") != std::string::npos &&
                retroArchValue("video_driver", gameFile, configFile) == "vulkan" &&
                Utils::FileSystem::isRegularFile(wayland) &&
                command.find("LD_PRELOAD=") == std::string::npos) {
                command.insert(start, "LD_PRELOAD=" + wayland + " ");
                LOG(LogInfo) << "EmulatorProfiles: Vulkan with the RetroArch AppImage, the "
                                "libwayland-client of the system is preloaded";
            }
            return closeGiven;
        }

        // The keyboard shortcuts of RPCS3 that the hotkeys press: its own setting if found in
        // GuiConfigs/CurrentSettings.ini, otherwise Ctrl+S for the save state. RPCS3 has no
        // load state while a game runs (a state is loaded by starting the game from it).
        std::vector<GameHotkeys::KeyAction> rpcs3KeyActions(const std::string& configFile)
        {
            std::string saveKeys {"Ctrl+S"};
            std::string speedKeys;
            ConfigText gui {Format::INI};
            if (gui.load(Utils::FileSystem::getParent(configFile) + "/GuiConfigs/CurrentSettings.ini")) {
                for (const std::string& key : gui.keys("Shortcuts")) {
                    std::string value;
                    if (!gui.get("Shortcuts/" + key, value) || value.empty())
                        continue;
                    const std::string lower {Utils::String::toLower(key)};
                    if (lower.find("savestate") != std::string::npos)
                        saveKeys = value;
                    else if (lower.find("frame_limit") != std::string::npos ||
                             lower.find("framelimit") != std::string::npos)
                        speedKeys = value;
                }
            }
            std::vector<GameHotkeys::KeyAction> actions;
            actions.push_back({"Save state", GameHotkeys::getCombination("HotkeySaveState"),
                               saveKeys});
            if (!speedKeys.empty())
                actions.push_back({"Fast forward", GameHotkeys::getCombination("HotkeyFastForward"),
                                   speedKeys});
            return actions;
        }
    } // namespace

    const std::vector<Profile>& getProfiles()
    {
        static const std::vector<Profile> profiles {makeProfiles()};
        return profiles;
    }

    const Profile* findByCommand(const std::string& command)
    {
        std::string program {Utils::String::toLower(
            Utils::FileSystem::getFileName(programOf(Utils::String::trim(command), nullptr, nullptr)))};
        if (program.empty())
            return nullptr;
        for (const Profile& profile : getProfiles()) {
            for (const std::string& word : profile.match) {
                if (program.find(word) != std::string::npos)
                    return &profile;
            }
        }
        return nullptr;
    }

    const Profile* findForGame(FileData* game)
    {
        if (game == nullptr)
            return nullptr;
        FileData* source {game->getSourceFileData()};
        SystemData* system {source->getSystem()};
        if (system == nullptr || system->getSystemEnvData()->mLaunchCommands.empty())
            return nullptr;
        // The same choice as when the game is launched: the emulator of the game, the one of
        // the system, or the first one.
        std::string command;
        const std::string gameLabel {source->metadata.get("altemulator")};
        if (!gameLabel.empty())
            command = system->getLaunchCommandFromLabel(gameLabel);
        if (command.empty() && !system->getAlternativeEmulator().empty())
            command = system->getLaunchCommandFromLabel(system->getAlternativeEmulator());
        if (command.empty())
            command = system->getSystemEnvData()->mLaunchCommands.front().first;
        const Profile* profile {findByCommand(command)};
        // A shortcut (.desktop) started as it is names the emulator in its Exec line.
        if (profile == nullptr && command.find("%ENABLESHORTCUTS%") != std::string::npos) {
            const std::string text {readStart(source->getPath(), 16384)};
            const size_t exec {text.find("\nExec=")};
            if (exec != std::string::npos) {
                const size_t end {text.find('\n', exec + 1)};
                profile = findByCommand(text.substr(exec + 6, end - exec - 6));
            }
        }
        return profile;
    }

    std::string getConfigFile(const Profile& profile)
    {
        for (const std::string& candidate : profile.configFiles) {
            const std::string path {expandHome(candidate)};
            if (path.find('*') == std::string::npos) {
                if (Utils::FileSystem::isRegularFile(path))
                    return path;
                continue;
            }
            // Flatpak: ~/.var/app/<id>/config/...
            const std::string base {expandHome("~/.var/app")};
            const std::string rest {path.substr(path.find('*') + 1)};
            if (!Utils::FileSystem::isDirectory(base))
                continue;
            for (const std::string& folder : Utils::FileSystem::getDirContent(base)) {
                if (Utils::FileSystem::isRegularFile(folder + rest))
                    return folder + rest;
            }
        }
        return "";
    }

    std::string getGameFile(const Profile& profile, FileData* game, std::string& reason)
    {
        const std::string config {getConfigFile(profile)};
        if (config.empty()) {
            reason = "THE SETTINGS FILE OF " + Utils::String::toUpper(profile.name) +
                     " WAS NOT FOUND, START THE EMULATOR ONCE";
            return "";
        }
        const std::string folder {Utils::FileSystem::getParent(config)};
        FileData* source {game->getSourceFileData()};
        const std::string path {source->getPath()};
        switch (profile.gameFiles) {
            case GameFiles::NONE:
                reason = Utils::String::toUpper(profile.name) +
                         " HAS NO SETTINGS FOR SINGLE GAMES, ONLY EMULATOR SETTINGS";
                return "";
            case GameFiles::QT_CUSTOM: {
                const std::string id {switchTitleId(source)};
                if (id.empty()) {
                    reason = "THE TITLE ID OF THIS GAME WASN'T FOUND (ADD IT TO THE FILE NAME, "
                             "E.G. [0100152000022000])";
                    return "";
                }
                return folder + "/custom/" + id + ".ini";
            }
            case GameFiles::RPCS3_CUSTOM: {
                const std::string serial {ps3Serial(source)};
                if (serial.empty())
                    break;
                return folder + "/custom_configs/config_" + serial + ".yml";
            }
            case GameFiles::PCSX2_GAMESETTINGS: {
                const std::string base {Utils::FileSystem::getParent(folder)};
                const std::string gameSettings {base + "/gamesettings"};
                std::string serial;
                std::string crc;
                std::string program;
                const DiscReader readAt {discReader(path)};
                if (readAt && ps2Serial(readAt, serial, program)) {
                    // A file PCSX2 already made for the game.
                    if (Utils::FileSystem::isDirectory(gameSettings)) {
                        for (const std::string& file :
                             Utils::FileSystem::getDirContent(gameSettings)) {
                            if (Utils::FileSystem::getFileName(file).rfind(serial + "_", 0) == 0)
                                return file;
                        }
                    }
                    crc = pcsx2Crc(readAt, program);
                }
                if (crc.empty())
                    pcsx2CacheEntry(base + "/cache/gamelist.cache", path, serial, crc);
                if (!serial.empty() && !crc.empty())
                    return gameSettings + "/" + serial + "_" + crc + ".ini";
                reason = "THE SERIAL OF THIS GAME COULDN'T BE READ: ADD ITS FOLDER TO THE GAME "
                         "LIST OF PCSX2 (OR USE AN .ISO FILE)";
                return "";
            }
            case GameFiles::PPSSPP_GAME_INI: {
                const std::string id {pspGameId(source)};
                if (id.empty())
                    break;
                return folder + "/" + id + "_ppsspp.ini";
            }
            case GameFiles::CEMU_PROFILE: {
                const std::string id {wiiuTitleId(source)};
                if (id.empty()) {
                    reason = "THE TITLE ID OF THIS GAME WASN'T FOUND: ADD ITS FOLDER TO THE GAME "
                             "PATHS OF CEMU";
                    return "";
                }
                return cemuProfileFolder() + "/" + id + ".ini";
            }
            case GameFiles::RETROARCH_OVERRIDE: {
                return retroArchFolder() + "/games/" + source->getSystem()->getName() + "/" +
                       Utils::FileSystem::getStem(path) + ".cfg";
            }
        }
        reason = "THE ID OF THIS GAME COULDN'T BE FOUND";
        return "";
    }

    std::vector<std::string> readValues(const Profile& profile)
    {
        std::vector<std::string> values(profile.settings.size());
        ConfigText config {profile.format};
        const std::string file {profile.ownSettingsFile ? retroArchSettingsFile() :
                                                          getConfigFile(profile)};
        if (!config.load(file))
            return values;
        for (size_t index {0}; index < profile.settings.size(); ++index) {
            if (!profile.settings[index].key.empty())
                config.get(profile.settings[index].key, values[index]);
        }
        return values;
    }

    std::vector<std::string> readBaseValues(const Profile& profile)
    {
        if (!profile.ownSettingsFile)
            return readValues(profile);
        std::vector<std::string> values(profile.settings.size());
        ConfigText config {profile.format};
        if (!config.load(getConfigFile(profile)))
            return values;
        for (size_t index {0}; index < profile.settings.size(); ++index) {
            if (!profile.settings[index].key.empty())
                config.get(profile.settings[index].key, values[index]);
        }
        return values;
    }

    bool writeValues(const Profile& profile,
                     const std::vector<std::pair<std::string, std::string>>& values,
                     std::string& error)
    {
        if (profile.ownSettingsFile) {
            const std::string path {retroArchSettingsFile()};
            ConfigText config {profile.format};
            config.load(path);
            for (const auto& [key, value] : values) {
                if (value.empty())
                    config.remove(key);
                else
                    config.set(key, value);
            }
            LOG(LogInfo) << "EmulatorProfiles: Changed " << values.size() << " settings of "
                         << profile.name << " in \"" << path << "\"";
            if (config.empty()) {
                Utils::FileSystem::removeFile(path);
                return true;
            }
            return config.save(path, error, false);
        }
        const std::string path {getConfigFile(profile)};
        ConfigText config {profile.format};
        if (path.empty() || !config.load(path)) {
            error = "The settings file of " + profile.name + " wasn't found";
            return false;
        }
        for (const auto& [key, value] : values) {
            if (value.empty())
                continue;
            config.set(key, fileValue(profile, value));
            // yuzu writes a setting only when it isn't the default one.
            if (profile.format == Format::QT_INI)
                config.set(key + "\\default", "false");
        }
        LOG(LogInfo) << "EmulatorProfiles: Changed " << values.size() << " settings of "
                     << profile.name << " in \"" << path << "\"";
        return config.save(path, error);
    }

    const std::string& gameKeyOf(const Setting& setting)
    {
        return setting.gameKey.empty() ? setting.key : setting.gameKey;
    }

    std::string labelOf(const Setting& setting, const std::string& value)
    {
        for (const Choice& choice : setting.choices) {
            if (choice.value == value)
                return choice.label;
        }
        return Utils::String::toUpper(value);
    }

    std::vector<std::string> readGameValues(const Profile& profile, const std::string& gameFile)
    {
        std::vector<std::string> values(profile.settings.size());
        if (gameFile.empty() || !Utils::FileSystem::isRegularFile(gameFile))
            return values;
        ConfigText config {profile.gameFiles == GameFiles::CEMU_PROFILE ? Format::INI :
                                                                            profile.format};
        if (!config.load(gameFile))
            return values;
        std::vector<std::string> global;
        if (profile.gameFiles == GameFiles::PPSSPP_GAME_INI)
            global = readValues(profile);
        for (size_t index {0}; index < profile.settings.size(); ++index) {
            const std::string& key {gameKeyOf(profile.settings[index])};
            if (key.empty() || !profile.settings[index].perGame)
                continue;
            std::string value;
            if (profile.gameFiles == GameFiles::QT_CUSTOM) {
                std::string useGlobal;
                if (config.get(key + "\\use_global", useGlobal) && useGlobal == "false")
                    config.get(key, value);
            }
            else if (config.get(key, value)) {
                // PPSSPP copies all settings: the same as the emulator is AUTO.
                if (!global.empty() && value == global[index])
                    value.clear();
            }
            values[index] = value;
        }
        return values;
    }

    bool writeGameValues(const Profile& profile,
                         const std::string& gameFile,
                         const std::vector<std::pair<std::string, std::string>>& values,
                         std::string& error)
    {
        const Format format {profile.gameFiles == GameFiles::CEMU_PROFILE ? Format::INI :
                                                                            profile.format};
        ConfigText config {format};
        const bool exists {Utils::FileSystem::isRegularFile(gameFile)};
        if (exists) {
            config.load(gameFile);
        }
        else if (profile.gameFiles == GameFiles::PPSSPP_GAME_INI) {
            // PPSSPP reads all settings of a game from its file, so it starts as a copy, with
            // the controls (PPSSPP keeps the controls of a game in its file too).
            config.load(getConfigFile(profile));
            ConfigText controls {Format::INI};
            if (controls.load(Utils::FileSystem::getParent(getConfigFile(profile)) +
                              "/controls.ini") &&
                controls.hasSection("ControlMapping"))
                config.replaceSection("ControlMapping", controls.sectionLines("ControlMapping"));
        }
        std::map<std::string, std::string> global;
        if (profile.gameFiles == GameFiles::PPSSPP_GAME_INI) {
            const std::vector<std::string> current {readValues(profile)};
            for (size_t index {0}; index < profile.settings.size(); ++index)
                global[profile.settings[index].key] = current[index];
        }
        for (const auto& [key, value] : values) {
            switch (profile.gameFiles) {
                case GameFiles::QT_CUSTOM:
                    if (value.empty()) {
                        config.set(key + "\\use_global", "true");
                        config.remove(key + "\\default");
                        config.remove(key);
                    }
                    else {
                        config.set(key + "\\use_global", "false");
                        config.set(key + "\\default", "false");
                        config.set(key, value);
                    }
                    break;
                case GameFiles::PPSSPP_GAME_INI:
                    if (!value.empty() || !global[key].empty())
                        config.set(key, value.empty() ? global[key] : value);
                    break;
                default:
                    if (value.empty())
                        config.remove(key);
                    else
                        config.set(key, fileValue(profile, value));
                    break;
            }
        }
        // A file without any setting left isn't needed.
        config.removeEmptySections();
        if (config.empty()) {
            if (exists)
                Utils::FileSystem::removeFile(gameFile);
            return true;
        }
        LOG(LogInfo) << "EmulatorProfiles: Changed " << values.size() << " settings of a game in \""
                     << gameFile << "\"";
        return config.save(gameFile, error, false);
    }

    GameHotkeys::Launch prepareLaunch(const Profile& profile, FileData* game, std::string& command)
    {
        GameHotkeys::Launch launch;
        // RPCS3 doesn't end when it's asked to (SIGTERM), it's killed almost at once.
        if (profile.id == "rpcs3")
            launch.killWaitMs = 300;
        const bool hotkeys {Settings::getInstance()->getBool("HotkeysToEmulators")};
        const std::string path {getConfigFile(profile)};
        // The time an emulator that got the close hotkey gets to close itself, before EmuSPLASH
        // ends it.
        const int closeWaitMs {
            GameHotkeys::getCombination("HotkeyCloseGame").empty() ? 0 : 2000};
        if (profile.id == "retroarch") {
            if (game != nullptr &&
                prepareRetroArch(profile, game, command, hotkeys && !path.empty()))
                launch.closeWaitMs = closeWaitMs;
            return launch;
        }
        if (!hotkeys || path.empty())
            return launch;
        launch.closeWaitMs = profile.closesItself ? closeWaitMs : 0;
        std::string error;
        if (profile.id == "ppsspp") {
            std::string reason;
            applyPpssppHotkeys(path, game != nullptr ? getGameFile(profile, game, reason) : "");
            return launch;
        }
        if (profile.id == "rpcs3") {
            launch.keyActions = rpcs3KeyActions(path);
            return launch;
        }
        if (profile.format != Format::QT_INI && profile.id != "pcsx2")
            return launch;
        if (profile.id == "citron")
            return launch;
        ConfigText config {profile.format};
        if (!config.load(path))
            return launch;
        if (profile.format == Format::QT_INI)
            applySwitchHotkeys(config);
        else
            applyPcsx2Hotkeys(config);
        if (!config.save(path, error)) {
            LOG(LogWarning) << "EmulatorProfiles: " << error;
        }
        return launch;
    }
} // namespace EmulatorProfiles
