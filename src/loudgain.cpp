/*
 * Loudness normalizer based on the EBU R128 standard
 *
 * Copyright (c) 2014, Alessandro Ghedini
 * All rights reserved.
 *
 * 2019-06-30 - v0.2.1 - Matthias C. Hormann
 *  - Added version
 *  - Added writing tags to Ogg Vorbis files (now supports MP3, FLAC, Ogg Vorbis)
 *  - Always remove REPLAYGAIN_REFERENCE_LOUDNESS, wrong value might confuse players
 *  - Added notice in help on which file types can be written
 *  - Added album summary
 * 2019-07-07 - v0.2.2 - Matthias C. Hormann
 *  - Fixed album peak calculation.
 *  - Write REPLAYGAIN_ALBUM_* tags only if in album mode
 *  - Better versioning (CMakeLists.txt → config.h)
 *  - TODO: clipping calculation still wrong
 * 2019-07-08 - v0.2.4 - Matthias C. Hormann
 *  - add "-s e" mode, writes extra tags (REPLAYGAIN_REFERENCE_LOUDNESS,
 *    REPLAYGAIN_TRACK_RANGE and REPLAYGAIN_ALBUM_RANGE)
 *  - add "-s l" mode (like "-s e" but uses LU/LUFS instead of dB)
 * 2019-07-08 - v0.2.5 - Matthias C. Hormann
 *  - Clipping warning & prevention (-k) now works correctly, both track & album
 * 2019-07-09 - v0.2.6 - Matthias C. Hormann
 *  - Add "-L" mode to force lowercase tags in MP3/ID3v2.
 * 2019-07-10 - v0.2.7 - Matthias C. Hormann
 *  - Add "-S" mode to strip ID3v1/APEv2 tags from MP3 files.
 *  - Add "-I 3"/"-I 4" modes to select ID3v2 version to write.
 *  - First step to implement a new tab-delimited list format: "-O" mode.
 * 2019-07-13 - v0.2.8 - Matthias C. Hormann
 *  - new -O output format: re-ordered, now shows peak before/after gain applied
 *  - -k now defaults to clipping prevention at -1 dBTP (as EBU recommends)
 *  - New -K: Allows clippping prevention with settable dBTP level,
 *     i.e. -K 0 (old-style) or -K -2 (to compensate for post-processing losses)
 * 2019-08-06 - v0.5.3 - Matthias C. Hormann
 *  - Add support for Opus (.opus) files.
 * 2019-08-16 - v0.6.0 - Matthias C. Hormann
 *  - Rework for new FFmpeg API (get rid of deprecated calls)
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
#include <iostream>
#include <filesystem>
#include <loudgain.hpp>
#include <tag.hpp>
#include <thread>
#include <algorithm>

namespace fs = std::filesystem;


LoudGain::LoudGain()
{ }

LoudGain::~LoudGain()
{
    closeCsvFile();
}

void LoudGain::setTagMode(const char tagmode)
{
    std::string valid_modes = "dies";
    if (valid_modes.find(tagmode) == std::string::npos)
    {
        std::cerr << "Invalid tag mode: " << tagmode  << std::endl;
        exit(EXIT_FAILURE);
    }
    tagMode = tagmode;
}

void LoudGain::setUnitToLUFS(bool enable)
{
    if (enable)
        snprintf(unit, sizeof(unit), "LU"); //strcpy(unit, "LU");
    else
        snprintf(unit, sizeof(unit), "dB"); //strcpy(unit, "dB");
}

void LoudGain::setVerbosity(int level)
{
    verbosity = level;
}

void LoudGain::setAlbumScanMode(bool enable)
{
    scanAlbum = enable;
}

void LoudGain::setPregain(double gain)
{
    this->pregain = std::clamp<double>(gain, -32.0, 32.0);
}

void LoudGain::setWarnClipping(bool enable)
{
    warnClipping = enable;
}

void LoudGain::setPreventClipping(bool enable)
{
    preventClipping = enable;
}

void LoudGain::setMaxTruePeakLevel(double mtpl)
{
    preventClipping = true;
    maxTruePeakLevel = std::clamp<double>(mtpl, -32.0, 3.0);
}

void LoudGain::setID3v2Version(int version)
{
    id3v2Version = std::clamp<int>(version, 3, 4);
}

void LoudGain::setForceLowerCaseTags(bool enable)
{
    lowerCaseTags = enable;
}

void LoudGain::setStripTags(bool enable)
{
    stripTags = enable;
}

void LoudGain::setTabOutput(bool enable)
{
    tabOutput = enable;
}

void LoudGain::openCsvFile(const std::string &file)
{
    fs::path csvpath = fs::path(file);

    if (!csvfile.is_open())
        csvfile.open(csvpath.string());

    if (!csvfile.is_open())
    {
        std::cerr << "Failed to open file: '" << csvpath.string() << "'" << std::endl;
        exit(EXIT_FAILURE);
    }

    /* Write headers */
    csvfile << "Type,Location,Loudness [LUFs],Range [" << unit << "],True Peak,True Peak [dBTP],Reference [LUFs],"
            << "Will clip,Clip prevent,Gain [" << unit << "],New Peak,New Peak [dBTP]" << std::endl;
}

void LoudGain::closeCsvFile()
{
    if (csvfile.is_open())
    {
        csvfile.flush();
        csvfile.close();
    }
}

void LoudGain::setNumberOfThreads(int n)
{
    int maxt = std::thread::hardware_concurrency();

    if (n <= 0)
        numberOfThreads = maxt - 1;
    else
        numberOfThreads = std::min<int>(n, maxt);
}

int LoudGain::avContainerNameToId(const std::string &str)
{
    if (str.length() == 0)
        return -1;

    for (int i = 0; i < (int) av_container_names.size(); i++)
        if (av_container_names[i].find(str) != std::string::npos)
            return i;

    return -1;
}

void LoudGain::removeReplayGainTags(AudioFile &audio_file)
{
    switch (avContainerNameToId(audio_file.avFormat))
    {
    case -1:
        #pragma omp critical
        std::cerr << "Couldn't determine file format: " << audio_file.filePath << std::endl;
        break;
    case AV_CONTAINER_ID_MP3:
        if (!tag_clear_mp3(&audio_file, stripTags, id3v2Version))
        {
            #pragma omp critical
            std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
        }
        break;

    case AV_CONTAINER_ID_FLAC:
        if (!tag_clear_flac(&audio_file))
        {
            #pragma omp critical
            std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
        }
        break;

    case AV_CONTAINER_ID_OGG:
        // must separate because TagLib uses fifferent File classes
        switch (audio_file.avCodecId)
        {
        // Opus needs special handling (different RG tags, -23 LUFS ref.)
        case AV_CODEC_ID_OPUS:
            if (!tag_clear_ogg_opus(&audio_file))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        case AV_CODEC_ID_VORBIS:
            if (!tag_clear_ogg_vorbis(&audio_file))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        case AV_CODEC_ID_FLAC:
            if (!tag_clear_ogg_flac(&audio_file))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        case AV_CODEC_ID_SPEEX:
            if (!tag_clear_ogg_speex(&audio_file))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        default:
            #pragma omp critical
            std::cerr << "Codec " << audio_file.avCodecId << " in " << audio_file.avFormat << " not supported" << std::endl;
            break;
        }
        break;

    case AV_CONTAINER_ID_MP4:
        if (!tag_clear_mp4(&audio_file))
        {
            #pragma omp critical
            std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
        }
        break;

    case AV_CONTAINER_ID_ASF:
        if (!tag_clear_asf(&audio_file))
        {
            #pragma omp critical
            std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
        }
        break;

    case AV_CONTAINER_ID_WAV:
        if (!tag_clear_wav(&audio_file, stripTags, id3v2Version))
        {
            #pragma omp critical
            std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
        }
        break;

    case AV_CONTAINER_ID_AIFF:
        if (!tag_clear_aiff(&audio_file, stripTags, id3v2Version))
        {
            #pragma omp critical
            std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
        }
        break;

    case AV_CONTAINER_ID_WV:
        if (!tag_clear_wavpack(&audio_file, stripTags))
        {
            #pragma omp critical
            std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
        }
        break;

    case AV_CONTAINER_ID_APE:
        if (!tag_clear_ape(&audio_file, stripTags))
        {
            #pragma omp critical
            std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
        }
        break;

    default:
        #pragma omp critical
        std::cerr << "File type not supported: " << audio_file.avFormat << std::endl;
        break;
    }
}

void LoudGain::processFileResults(AudioFile &audio_file)
{
    double tgain    = 1.0; // "gained" track peak
    double tpeak    = pow(10.0, maxTruePeakLevel / 20.0); // track peak limit
    double again    = 1.0; // "gained" album peak
    double apeak    = pow(10.0, maxTruePeakLevel / 20.0); // album peak limit

    // track peak after gain
    tgain = pow(10.0, audio_file.trackGain / 20.0) * audio_file.trackPeak;
    if (tgain > tpeak)
        audio_file.trackClips = true;

    // album peak after gain
    if (scanAlbum)
    {
        again = pow(10.0, audio_file.albumGain / 20.0) * audio_file.albumPeak;
        if (again > apeak)
            audio_file.albumClips = true;
    }

    // prevent clipping
    if ((audio_file.trackClips || audio_file.albumClips) && preventClipping)
    {
        if (audio_file.trackClips)
        {
            // set new track peak = minimum of peak after gain and peak limit
            audio_file.trackGain = audio_file.trackGain - (log10(tgain/tpeak) * 20.0);
            audio_file.trackClips = false;
        }

        if (scanAlbum && audio_file.albumClips)
        {
            audio_file.albumGain = audio_file.albumGain - (log10(again/apeak) * 20.0);
            audio_file.albumClips = false;
        }

        audio_file.clipPrevention = true;
    }

    audio_file.newTrackPeak = pow(10.0, audio_file.trackGain / 20.0) * audio_file.trackPeak;
    if (scanAlbum)
        audio_file.newAlbumPeak = pow(10.0, audio_file.albumGain / 20.0) * audio_file.albumPeak;

    switch (tagMode)
    {
    case 'i': /* ID3v2 tags */
    case 'e': /* same as 'i' plus extra tags */
        switch (avContainerNameToId(audio_file.avFormat))
        {
        case -1:
            #pragma omp critical
            std::cerr << "Couldn't determine file format: " << audio_file.filePath << std::endl;
            break;

        case AV_CONTAINER_ID_MP3:
            if (!tag_write_mp3(&audio_file, scanAlbum, tagMode, unit, lowerCaseTags, stripTags, id3v2Version))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        case AV_CONTAINER_ID_FLAC:
            if (!tag_write_flac(&audio_file, scanAlbum, tagMode, unit))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        case AV_CONTAINER_ID_OGG:
            // must separate because TagLib uses fifferent File classes
            switch (audio_file.avCodecId)
            {
            // Opus needs special handling (different RG tags, -23 LUFS ref.)
            case AV_CODEC_ID_OPUS:
                if (!tag_write_ogg_opus(&audio_file, scanAlbum, tagMode, unit))
                {
                    #pragma omp critical
                    std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
                }
                break;

            case AV_CODEC_ID_VORBIS:
                if (!tag_write_ogg_vorbis(&audio_file, scanAlbum, tagMode, unit))
                {
                    #pragma omp critical
                    std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
                }
                break;

            case AV_CODEC_ID_FLAC:
                if (!tag_write_ogg_flac(&audio_file, scanAlbum, tagMode, unit))
                {
                    #pragma omp critical
                    std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
                }
                break;

            case AV_CODEC_ID_SPEEX:
                if (!tag_write_ogg_speex(&audio_file, scanAlbum, tagMode, unit))
                {
                    #pragma omp critical
                    std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
                }
                break;

            default:
                #pragma omp critical
                std::cerr << "Codec " << audio_file.avCodecId << " in " << audio_file.avFormat << " not supported" << std::endl;
                break;
            }
            break;

        case AV_CONTAINER_ID_MP4:
            if (!tag_write_mp4(&audio_file, scanAlbum, tagMode, unit, lowerCaseTags))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        case AV_CONTAINER_ID_ASF:
            if (!tag_write_asf(&audio_file, scanAlbum, tagMode, unit, lowerCaseTags))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        case AV_CONTAINER_ID_WAV:
            if (!tag_write_wav(&audio_file, scanAlbum, tagMode, unit, lowerCaseTags, stripTags, id3v2Version))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        case AV_CONTAINER_ID_AIFF:
            if (!tag_write_aiff(&audio_file, scanAlbum, tagMode, unit, lowerCaseTags, stripTags, id3v2Version))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        case AV_CONTAINER_ID_WV:
            if (!tag_write_wavpack(&audio_file, scanAlbum, tagMode, unit, lowerCaseTags, stripTags))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        case AV_CONTAINER_ID_APE:
            if (!tag_write_ape(&audio_file, scanAlbum, tagMode, unit, lowerCaseTags, stripTags))
            {
                #pragma omp critical
                std::cerr << "Couldn't write to: " << audio_file.filePath << std::endl;
            }
            break;

        default:
            #pragma omp critical
            std::cerr << "File type not supported: " << audio_file.avFormat << std::endl;
            break;
        }
        break;

    case 's': /* skip tags */
        break;

    default:
        #pragma omp critical
        std::cerr << "Invalid tag mode" << std::endl;
        break;
    }

    if (csvfile.is_open())
    {
        #pragma omp critical
        csvfile << "File,\"" << audio_file.filePath << "\"" << ","
                << audio_file.trackLoudness << ","
                << audio_file.trackLoudnessRange << ","
                << audio_file.trackPeak << ","
                << 20.0 * log10(audio_file.trackPeak) << ","
                << audio_file.loudnessReference  << ","
                << (audio_file.trackClips || audio_file.albumClips) << ","
                << audio_file.clipPrevention << ","
                << audio_file.trackGain << ","
                << audio_file.newTrackPeak << ","
                << 20.0 * log10(audio_file.newTrackPeak) << std::endl;
    }

    if (tabOutput)
    {
        // output new style list: File;Loudness;Range;Gain;Reference;Peak;Peak dBTP;Clipping;Clip-prevent
        #pragma omp critical
        {
            printf("%s\t", audio_file.filePath.c_str());
            printf("%.2f LUFS\t", audio_file.trackLoudness);
            printf("%.2f %s\t", audio_file.trackLoudnessRange, unit);
            printf("%.6f\t", audio_file.trackPeak);
            printf("%.2f dBTP\t", 20.0 * log10(audio_file.trackPeak));
            printf("%.2f LUFS\t", audio_file.loudnessReference);
            printf("%s\t", (audio_file.trackClips || audio_file.albumClips) ? "Y" : "N");
            printf("%s\t", audio_file.clipPrevention ? "Y" : "N");
            printf("%.2f %s\t", audio_file.trackGain, unit);
            printf("%.6f\t", audio_file.newTrackPeak);
            printf("%.2f dBTP\n", 20.0 * log10(audio_file.newTrackPeak));
        }
    }
    else if (verbosity >= 2)
    {
        // output something human-readable
        #pragma omp critical
        {
            std::cout << "\nTrack: "   << audio_file.filePath << "\n"
                      << " Loudness: " << audio_file.trackLoudness << " LUFS\n"
                      << " Range:    " << audio_file.trackLoudnessRange << " dB\n"
                      << " Peak:     " << audio_file.trackPeak << " (" << 20.0 * log10(audio_file.trackPeak) << " dBTP)\n";

            if (audio_file.avCodecId == AV_CODEC_ID_OPUS)
                std::cout << " Gain:     " <<  audio_file.trackGain << " dB ("  << gain_to_q78num(audio_file.trackGain) << ")";
            else
                std::cout << " Gain:     " << audio_file.trackGain <<  " dB";

            if (audio_file.clipPrevention)
                std::cout << " (corrected to prevent clipping)";

            if (!scanAlbum)
                std::cout << "\n" << std::endl;
            else
                std::cout << std::endl;
        }
    }
}

void LoudGain::processFolderResults(AudioFolder &audio_album)
{
    if (audio_album.count() == 0)
    {
        #pragma omp critical
        std::cerr << "No files in album!" << std::endl;
        return;
    }

    if (audio_album.scanStatus != AudioFolder::SUCCESS)
    {
        #pragma omp critical
        std::cerr << "Album scan failed [" << audio_album.getAudioFile(0)->directory <<"]!" << std::endl;
        return;
    }

    // check for different file (codec) types in an album and warn(including Opus might mess up album gain)
    if (audio_album.hasDifferentContainers() || audio_album.hasDifferentCodecs())
    {
        #pragma omp critical
        std::cerr << "You have different file types in the same album [" << audio_album.getAudioFile(0)->directory <<"]!" << std::endl;

        if (audio_album.hasOpus())
        {
            #pragma omp critical
            std::cerr << "Cannot calculate correct album gain when mixing Opus and non-Opus files [" << audio_album.getAudioFile(0)->directory <<"]!" << std::endl;
            return;
        }
    }

    for (int i = 0; i < audio_album.count(); i++)
    {
        AudioFile &audio_file = *(audio_album.getAudioFile(i).get());
        processFileResults(audio_file);

        if (i == (audio_album.count() - 1) && scanAlbum)
        {
            #pragma omp critical
            {
                if (csvfile.is_open())
                {
                    csvfile << "Album,\"" << audio_file.directory << "\"" << ","
                            << audio_file.albumLoudness << ","
                            << audio_file.albumLoudnessRange << ","
                            << audio_file.albumPeak << ","
                            << 20.0 * log10(audio_file.albumPeak) << ","
                            << audio_file.loudnessReference  << ","
                            << audio_file.albumClips << ","
                            << audio_file.clipPrevention << ","
                            << audio_file.albumGain << ","
                            << audio_file.newAlbumPeak << ","
                            << 20.0 * log10(audio_file.newAlbumPeak) << std::endl;
                }

                if (tabOutput)
                {
                    printf("%s\t", "Album");
                    printf("%.2f LUFS\t", audio_file.albumLoudness);
                    printf("%.2f %s\t", audio_file.albumLoudnessRange, unit);
                    printf("%.6f\t", audio_file.albumPeak);
                    printf("%.2f dBTP\t", 20.0 * log10(audio_file.albumPeak));
                    printf("%.2f LUFS\t", audio_file.loudnessReference);
                    printf("%s\t", audio_file.albumClips ? "Y" : "N");
                    printf("%s\t", audio_file.clipPrevention ? "Y" : "N");
                    printf("%.2f %s\t", audio_file.albumGain, unit);
                    printf("%.6f\t", audio_file.newAlbumPeak);
                    printf("%.2f dBTP\n", 20.0 * log10(audio_file.newAlbumPeak));
                }
                else  if (verbosity >= 2)
                {
                    // output something human-readable
                    std::cout << "\nAlbum: "   << audio_file.directory << "\n"
                              << " Loudness: " << audio_file.albumLoudness << " LUFS\n"
                              << " Range:    " << audio_file.albumLoudnessRange << " dB\n"
                              << " Peak:     " << audio_file.albumPeak << " (" << 20.0 * log10(audio_file.albumPeak) << " dBTP)\n"
                              << " Gain:     " << audio_file.albumGain <<  " dB\n";

                    if (audio_file.clipPrevention)
                        std::cout << " (corrected to prevent clipping)" << std::endl;
                    else
                        std::cout << std::endl;
                }
            }
        }
    }
}
