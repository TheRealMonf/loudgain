/*
 * Loudness normalizer based on the EBU R128 standard
 *
 * Copyright (c) 2014, Alessandro Ghedini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef SCAN_H
#define SCAN_H

#include <string>
#include <vector>
#include <set>
#include <map>
#include <memory>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

class LoudGain;

extern "C" {
    #include <ebur128.h>
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswresample/swresample.h>
    #include <libavutil/avutil.h>
    #include <libavutil/common.h>
    #include <libavutil/opt.h>
}

class AudioFile
{
public:
    enum SCANSTATUS
    {
        INIT,
        PROCESSING,
        FAIL,
        SUCCESS
    };

    enum SCANSTATUS scanStatus = SCANSTATUS::INIT;
    std::string filePath;
    std::string fileName;
    std::string directory;
    enum AVCodecID avCodecId;
    std::string avFormat = "";
    double trackGain = 0.0;
    double trackPeak = 0.0;
    double newTrackPeak = 0.0;
    double trackLoudness = 0.0;
    double trackLoudnessRange = 0.0;
    bool trackClips = false;
    double albumGain = 0.0;
    double albumPeak = 0.0;
    double newAlbumPeak = 0.0;
    double albumLoudness = 0.0;
    double albumLoudnessRange = 0.0;
    bool albumClips = false;
    double loudnessReference = 0.0;
    bool clipPrevention = false;
    ebur128_state *eburState = NULL;

    AudioFile(const std::string &path);
    ~AudioFile();

    bool destroyEbuR128State();
    bool scanFile(double pregain, bool loudness, bool verbose);

private:
    bool scanFrame(ebur128_state *ebur128, AVFrame *frame, SwrContext *swr);

};


class AudioFolder
{
public:
    enum SCANSTATUS
    {
        INIT,
        PROCESSING,
        FAIL,
        SUCCESS
    };

    enum SCANSTATUS scanStatus = SCANSTATUS::INIT;
    std::string directory;

private:
    std::vector<std::shared_ptr<AudioFile>> audioFiles;

public:

    AudioFolder(const std::vector<std::string> &files);
    ~AudioFolder();
    int count();
    std::shared_ptr<AudioFile> getAudioFile(int i);
    bool hasDifferentContainers();
    bool hasDifferentCodecs();
    bool hasOpus();
    bool scanFolder(double pregain, int threads, bool verbose);
    bool canProcessResults();
    bool processResults(double pregain);
};


class AudioLibrary
{
private:
    bool recursive = false;
    std::vector<std::string> libraryPaths;
    const std::vector<std::string> supportedExtensions = {".mp3", ".flac", ".ogg", ".mov", ".mp4", ".m4a", ".3gp", ".3g2", ".mj2", ".asf", ".wav", ".wv", ".aiff", ".ape"};
    std::vector<std::string> userExtensions;


public:
    AudioLibrary();
    ~AudioLibrary();

    void setLibraryPaths(const std::vector<std::string> &paths);
    void setRecursive(bool enable);
    void setUserExtensions(const std::string &extensions);
    void setUserExtensions(std::vector<std::string> &extensions);
    bool scanLibrary(LoudGain &lg);
    bool removeReplayGainTags(LoudGain &lg);
    bool isOnlyDirectories(const std::vector<std::string> &paths);
    bool isSupportedAudioFile(const std::string &path);
    bool isSupportedAudioFile(const fs::path &path);
    std::set<std::string> getSupportedAudioFiles();
    std::map<std::string, std::unique_ptr<std::vector<std::string>>> getSupportedAudioFilesSortedByFolder();

};

#endif

