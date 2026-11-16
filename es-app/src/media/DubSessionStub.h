//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  DubSessionStub.h
//
//  The video player without Dubx (cmake -DDUBX=OFF): the same interface as the dub session of
//  Dubx, which is never created, so the player has no dub, no Dubx button and no Dubx audio
//  streams.
//

#ifndef ES_APP_MEDIA_DUB_SESSION_STUB_H
#define ES_APP_MEDIA_DUB_SESSION_STUB_H

#include "media/MediaComponents.h"
#include "media/MpvPlayer.h"
#include "media/SubtitleDownloader.h"

#include <string>
#include <vector>

class Window;

class DubSession
{
public:
    // What the player gives the dub, the player implements it the same way with and without
    // Dubx.
    class Host
    {
    public:
        virtual ~Host() = default;
        virtual MediaDialog* pushDubDialog(const std::string& title, float widthFraction) = 0;
        virtual void addDubCloseOption(std::vector<MediaDialog::Option>& options,
                                       MediaDialog* dialog) = 0;
        virtual void refreshDubHelpPrompts() = 0;
        virtual const MpvPlayer::Track* selectedDubSubtitle() const = 0;
        virtual const MpvPlayer::Track* selectedDubAudio() const = 0;
        virtual std::string dubTitle() const = 0;
        virtual std::string dubVideoPath() const = 0;
        virtual bool isDubStream() const = 0;
        virtual SubtitleDownloader::Query dubSubtitleQuery() const = 0;
        virtual std::string dubSubtitleVideoPath() const = 0;
        virtual double dubStartPosition() const = 0;
        virtual void applyDubAudioFilters() = 0;
        virtual std::string dubOriginCountry() const { return ""; }
        virtual std::string dubOriginLanguage() const { return ""; }
        virtual std::string dubContext() const { return ""; }
    };

    enum class Mode {
        DUB,
        DUBX
    };

    DubSession(Host&, MpvPlayer&, Window*) {}

    void toggle(Mode = Mode::DUB) {}
    void toggleDubx() {}
    void onFileStarted() {}
    void onTracksKnown() {}
    void onUserPause() {}
    void update(int, bool) {}
    void addAudioOptions(std::vector<MediaDialog::Option>&) {}
    bool isWanted() const { return false; }
    bool isWanted(Mode) const { return false; }
    std::string getLabel() const { return ""; }
    int getAudioTrackCount() const { return 0; }
    std::string getAudioTrackLabel(int) const { return ""; }
    int getSelectedAudioTrack() const { return 0; }
    void selectAudioTrack(int) {}
    bool isActive() const { return false; }
    bool isPreparing() const { return false; }
    int getProgress() const { return 0; }
    std::string getStatus() const { return ""; }
    bool hidSubtitles() const { return false; }
    bool wantsDuckFilter() const { return false; }
    std::string getDuckFilterGraph() const { return ""; }
    void onAudioFiltersApplied(bool) {}
};

#endif // ES_APP_MEDIA_DUB_SESSION_STUB_H
