/*
 * Loudness normalizer based on the EBU R128 standard
 *
 * Copyright (c) 2014, Alessandro Ghedini
 * All rights reserved.
 *
 * 2019-06-30 - Matthias C. Hormann
 *  calculate correct album peak
 *  TODO: This still sucks because albums are handled track-by-track.
 * 2019-08-01 - Matthias C. Hormann
 *  - Move from deprecated libavresample library to libswresample (FFmpeg)
 * 2019-08-16 - Matthias C. Hormann
 *  - Rework to use the new FFmpeg API, no more deprecated calls
 *    (needed for FFmpeg 4.2+)
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
#include <omp.h>
#include <loudgain.hpp>
#include <scan.hpp>
#include <math.h>

#define LUFS_TO_RG(L) (-18 - L)
#define UNUSED(x) (void)x

int gain_to_q78num(double gain)
{
  // convert float to Q7.8 number: Q = round(f * 2^8)
  return int(round(gain * 256.0));    // 2^8 = 256
}

static void scan_av_log(void *avcl, int level, const char *fmt, va_list args)
{
    UNUSED(avcl); UNUSED(level); UNUSED(fmt); UNUSED(args);
}


AudioFile::AudioFile(const std::string &path)
{
    fs::path p(path);
    filePath = p.u8string();
    fileName = p.filename().u8string();
    directory = p.parent_path().u8string();
}

AudioFile::~AudioFile()
{
    destroyEbuR128State();
}

bool AudioFile::destroyEbuR128State()
{
    if (eburState != NULL)
    {
        ebur128_destroy(&eburState);
        free(eburState);
        eburState = NULL;
        return true;
    }
    return false;
}

bool AudioFile::scanFile(double pregain, bool loudness, bool verbose)
{
    scanStatus = SCANSTATUS::PROCESSING;

    AVFormatContext *container = NULL;
    int rc = avformat_open_input(&container, filePath.c_str(), NULL, NULL);
    if (rc < 0)
    {
        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Could not open input: " << errbuf << std::endl;
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }
    avFormat = std::string(container->iformat->name);

    if (verbose)
    {
        #pragma omp critical
        std::cout << "[" << fileName << "] " << "Container: " << container->iformat->long_name << " [" << avFormat  << "]" << std::endl;
    }

    rc = avformat_find_stream_info(container, NULL);
    if (rc < 0)
    {
        avformat_close_input(&container);

        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Could not find stream info: " << errbuf << std::endl;
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    /* select the audio stream */
    AVCodec *codec;
    int stream_id = av_find_best_stream(container, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);

    if (stream_id < 0)
    {
        avformat_close_input(&container);

        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Could not find audio stream!" << std::endl;
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    /* create decoding context */
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
    {
        avformat_close_input(&container);

        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Could not allocate audio codec context!" << std::endl;
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    avcodec_parameters_to_context(ctx, container->streams[stream_id]->codecpar);

    /* init the audio decoder */
    rc = avcodec_open2(ctx, codec, NULL);
    if (rc < 0)
    {
        avformat_close_input(&container);
        avcodec_close(ctx);


        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Could not open codec: " << errbuf << std::endl;
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    // try to get default channel layout (they aren’t specified in .wav files)
    if (!ctx->channel_layout)
        ctx->channel_layout = av_get_default_channel_layout(ctx->channels);

    // show some information about the file
    // only show bits/sample where it makes sense
    char infotext[20];
    infotext[0] = '\0';

    if (ctx->bits_per_raw_sample > 0 || ctx->bits_per_coded_sample > 0)
    {
        #pragma omp critical
        snprintf(infotext, sizeof(infotext), "%d bit, ", ctx->bits_per_raw_sample > 0 ? ctx->bits_per_raw_sample : ctx->bits_per_coded_sample);
    }

    char infobuf[512];
    av_get_channel_layout_string(infobuf, sizeof(infobuf), -1, ctx->channel_layout);

    if (verbose)
    {
        #pragma omp critical
        std::cout << "[" << fileName << "] " << "Stream #" << stream_id << ": " << codec->long_name << ", " << infotext << " " << ctx->sample_rate << " Hz, " << ctx->channels << " ch, " << infobuf << std::endl;
    }

    avCodecId = codec->id;

    if (!loudness)
    {
        scanStatus = SCANSTATUS::INIT;
        avcodec_close(ctx);
        avformat_close_input(&container);
        return true;
    }

    destroyEbuR128State();
    eburState = ebur128_init(ctx->channels, ctx->sample_rate, EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_SAMPLE_PEAK | EBUR128_MODE_TRUE_PEAK);

    if (eburState == NULL)
    {
        avformat_close_input(&container);
        avcodec_close(ctx);

        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Could not initialize EBU R128 scanner!" << std::endl;
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    AVFrame *frame = av_frame_alloc();

    if (frame == NULL)
    {
        avformat_close_input(&container);
        avcodec_close(ctx);

        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Could not allocate frame!" << std::endl;
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    SwrContext *swr = swr_alloc();
    AVPacket packet;
    while (av_read_frame(container, &packet) >= 0 && scanStatus != SCANSTATUS::FAIL)
    {
        if (packet.stream_index == stream_id)
        {
            rc = avcodec_send_packet(ctx, &packet);
            if (rc < 0)
            {
                #pragma omp critical
                std::cerr << "[" << fileName << "] " << "Error while sending a packet to the decoder!" << std::endl;
                scanStatus = SCANSTATUS::FAIL;
                break;
            }

            while (rc >= 0 && scanStatus != SCANSTATUS::FAIL)
            {
                rc = avcodec_receive_frame(ctx, frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                    break;
                else if (rc < 0)
                {
                    #pragma omp critical
                    std::cerr << "[" << fileName << "] " << "Error while receiving a frame from the decoder!" << std::endl;
                    scanStatus = SCANSTATUS::FAIL;
                    break;
                }

                if (!scanFrame(eburState, frame, swr))
                {
                    #pragma omp critical
                    std::cerr << "[" << fileName << "] " << "Error while scanning frame!" << std::endl;
                    scanStatus = SCANSTATUS::FAIL;
                    break;
                }
            }

            av_frame_unref(frame);
        }

        av_packet_unref(&packet);
    }

    /* Free */
    av_frame_free(&frame);
    swr_free(&swr);
    avcodec_close(ctx);
    avformat_close_input(&container);

    if (scanStatus == SCANSTATUS::FAIL)
        return false;

    /* Save results */
    double global_loudness;
    if (ebur128_loudness_global(eburState, &global_loudness) != EBUR128_SUCCESS)
    {
        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Error while calculating loudness!" << std::endl;
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    double loudness_range;
    if (ebur128_loudness_range(eburState, &loudness_range) != EBUR128_SUCCESS)
    {
        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Error while calculating loudness range!" << std::endl;
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    double peak = 0.0;
    for (unsigned ch = 0; ch < eburState->channels; ch++)
    {
        double tmp;

        if (ebur128_true_peak(eburState, ch, &tmp) != EBUR128_SUCCESS)
            continue;

        peak = std::max<double>(peak, tmp);
    }

    /* Opus is always based on -23 LUFS, we have to adapt */
    if (avCodecId == AV_CODEC_ID_OPUS)
        pregain -= 5.0;

    trackGain = LUFS_TO_RG(global_loudness) + pregain;
    trackPeak = peak;
    trackLoudness = global_loudness;
    trackLoudnessRange = loudness_range;
    loudnessReference = LUFS_TO_RG(-pregain);

    scanStatus = SCANSTATUS::SUCCESS;
    return true;
}

bool AudioFile::scanFrame(ebur128_state *ebur128, AVFrame *frame, SwrContext *swr)
{
    av_opt_set_channel_layout(swr, "in_channel_layout", frame->channel_layout, 0);
    av_opt_set_channel_layout(swr, "out_channel_layout", frame->channel_layout, 0);

    /* Add channel count to properly handle .wav reading */
    av_opt_set_int(swr, "in_channel_count",  frame -> channels, 0);
    av_opt_set_int(swr, "out_channel_count", frame -> channels, 0);

    av_opt_set_int(swr, "in_sample_rate", frame -> sample_rate, 0);
    av_opt_set_int(swr, "out_sample_rate", frame -> sample_rate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", (AVSampleFormat) frame -> format, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    int rc = swr_init(swr);
    if (rc < 0)
    {
        char errbuf[2048];
        av_strerror(rc, errbuf, 2048);

        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Could not open SWResample: " << errbuf << std::endl;
        return false;
    }

    int out_linesize;
    size_t out_size = av_samples_get_buffer_size(&out_linesize, frame -> channels, frame -> nb_samples, AV_SAMPLE_FMT_S16, 0);
    uint8_t *out_data = (uint8_t *) av_malloc(out_size);

    if (swr_convert(swr, (uint8_t**) &out_data, frame -> nb_samples, (const uint8_t**) frame -> data, frame -> nb_samples) < 0)
    {
        swr_close(swr);
        av_free(out_data);

        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Cannot convert" << std::endl;
        return false;
    }

    rc = ebur128_add_frames_short(ebur128, (short *) out_data, frame -> nb_samples);

    if (rc != EBUR128_SUCCESS)
    {
        swr_close(swr);
        av_free(out_data);

        #pragma omp critical
        std::cerr << "[" << fileName << "] " << "Error filtering" << std::endl;
        return false;
    }

    swr_close(swr);
    av_free(out_data);
    return true;
}



AudioFolder::AudioFolder(const std::vector<std::string> &files)
{
    size_t nb = files.size();
    audioFiles.reserve(nb);

    for (const std::string &file : files)
        audioFiles.push_back(std::shared_ptr<AudioFile>(new AudioFile(file)));

    if (audioFiles.size() > 0)
        directory = audioFiles[0]->directory;

    #if ( LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100) )
        av_register_all();
    #endif

    av_log_set_callback(scan_av_log);
}

AudioFolder::~AudioFolder()
{
    for (int i = 1; i < (int) audioFiles.size(); i++)
        audioFiles[i].reset();
}

int AudioFolder::count()
{
    return int(audioFiles.size());
}

std::shared_ptr<AudioFile> AudioFolder::getAudioFile(int i)
{
    return audioFiles[i];
}

bool AudioFolder::hasDifferentContainers()
{
    for (int i = 1; i < int(audioFiles.size()); i++)
        if (audioFiles[0]->avFormat.compare(audioFiles[i]->avFormat) != 0)
            return true;
    return false;
}

bool AudioFolder::hasDifferentCodecs()
{
    for (int i = 1; i < (int) audioFiles.size(); i++)
        if (audioFiles[0]->avCodecId != audioFiles[i]->avCodecId)
            return true;
    return false;
}

bool AudioFolder::hasOpus()
{
    for (int i = 0; i < (int) audioFiles.size(); i++) {
        if (audioFiles[i]->avCodecId == AV_CODEC_ID_OPUS)
            return true;
    }
    return false;
}

bool AudioFolder::scanFolder(double pregain, int threads, bool quiet)
{
    scanStatus = SCANSTATUS::PROCESSING;

    volatile bool ok = true;
    threads = std::max<int>(1, threads);

    /* Scan audio files */
    #pragma omp parallel for shared(ok) num_threads(threads)
    for (int i = 0; i < int(audioFiles.size()); i++)
        if (ok)
            if (!audioFiles[i]->scanFile(pregain, true, quiet))
                ok = false;

    /* Return if any file scans were not successful */
    if (!ok)
    {
        scanStatus = SCANSTATUS::FAIL;
        return false;
    }

    return processResults(pregain);
}

bool AudioFolder::canProcessResults()
{
    for (int i = 0; i < int(audioFiles.size()); i++)
        if (audioFiles[i]->scanStatus != AudioFile::SCANSTATUS::SUCCESS)
            return false;
    return true;
}

bool AudioFolder::processResults(double pregain)
{
    if (scanStatus == SCANSTATUS::FAIL)
        return false;
    else if (scanStatus == SCANSTATUS::SUCCESS)
        return true;
    else if ((hasDifferentContainers() || hasDifferentCodecs()) && hasOpus())
    {
        scanStatus = SCANSTATUS::FAIL;

        #pragma omp critical
        std::cerr << "Cannot calculate correct album gain when mixing Opus and non-Opus files!" << std::endl;
        return false;
    }

    scanStatus = SCANSTATUS::PROCESSING;

    /* Process folder */
    unsigned int nb = unsigned(audioFiles.size());
    ebur128_state **ebuR128States = (ebur128_state **) malloc(sizeof(ebur128_state *) * nb);

    for (int i = 0; i < int(audioFiles.size()); i++)
        ebuR128States[i] = audioFiles[i]->eburState;

    double global_loudness;
    if (ebur128_loudness_global_multiple(ebuR128States, nb, &global_loudness) != EBUR128_SUCCESS)
    {
        free(ebuR128States);
        scanStatus = SCANSTATUS::FAIL;
        std::cerr << "Album loudness fail!" << std::endl;
        return false;
    }

    double loudness_range;
    if (ebur128_loudness_range_multiple(ebuR128States, nb, &loudness_range) != EBUR128_SUCCESS)
    {
        free(ebuR128States);
        scanStatus = SCANSTATUS::FAIL;

        #pragma omp critical
        std::cerr << "Album loudness range fail!" << std::endl;
        return false;
    }

    free(ebuR128States);

    // Opus is always based on -23 LUFS, we have to adapt
    // When we arrive here, it’s already verified that the album
    // does NOT mix Opus and non-Opus tracks,
    // so we can safely reduce the pre-gain to arrive at -23 LUFS.
    if (hasOpus())
        pregain -= 5.0;

    double album_peak = 0.0;
    for (int i = 0; i < int(audioFiles.size()); i++)
        album_peak = std::max<double>(album_peak, audioFiles[i]->trackPeak);

    for (int i = 0; i < int(audioFiles.size()); i++)
    {
        AudioFile &audio = *audioFiles[i].get();
        audio.albumGain = LUFS_TO_RG(global_loudness) + pregain;
        audio.albumPeak = album_peak;
        audio.albumLoudness = global_loudness;
        audio.albumLoudnessRange = loudness_range;
    }

    scanStatus = SCANSTATUS::SUCCESS;
    return true;
}



AudioLibrary::AudioLibrary()
{
    userExtensions = supportedExtensions;

    #if ( LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100) )
        av_register_all();
    #endif

    av_log_set_callback(scan_av_log);
}

AudioLibrary::~AudioLibrary()
{

}

void AudioLibrary::setLibraryPaths(const std::vector<std::string> &paths)
{
    //for (const std::string &path : paths)
    //{
    //    std::string s = path;
    //    std::replace(s.begin(), s.end(), '/', '\\');
    //    libraryPaths.push_back(s);
    //}
    libraryPaths = paths;
}

void AudioLibrary::setRecursive(bool enable)
{
    recursive = enable;
}

void AudioLibrary::setUserExtensions(const std::string &extensions)
{
    std::vector<std::string> exts;
    std::istringstream f(extensions);
    std::string s;
    while (getline(f, s, ','))
    {
        if (s.length() >= 2 && s[0] != '.')
            exts.push_back("."+s);
        else if (s.length() >= 2 && s[0] == '.')
            exts.push_back(s);
    }

    setUserExtensions(exts);
}

void AudioLibrary::setUserExtensions(std::vector<std::string> &extensions)
{
    userExtensions.clear();

    for(const std::string& ext: extensions)
        if (std::find(supportedExtensions.begin(), supportedExtensions.end(), ext) != supportedExtensions.end())
            userExtensions.push_back(ext);

    userExtensions.shrink_to_fit();
}

bool AudioLibrary::removeReplayGainTags(LoudGain &lg)
{
    int nthreads = std::max<int>(1, lg.numberOfThreads);

    std::set<std::string> fset = getSupportedAudioFiles();
    std::vector<std::string> files{fset.begin(), fset.end()};
    fset.clear();

    #pragma omp parallel for schedule(dynamic, 1) num_threads(nthreads) if (nthreads > 1)
    for (int i = 0; i < int(files.size()); i++)
    {
        AudioFile audio_file = AudioFile(files[i]);
        if (audio_file.scanFile(0.0, false, (lg.verbosity >= 3)))
            lg.removeReplayGainTags(audio_file);
    }

    return true;
}

bool AudioLibrary::scanLibrary(LoudGain &lg)
{   
    if (lg.tabOutput)
        std::cout << "File\tLoudness\tRange\tTrue_Peak\tTrue_Peak_dBTP\tReference\tWill_clip\tClip_prevent\tGain\tNew_Peak\tNew_Peak_dBTP" << std::endl;

    int nthreads = std::max<int>(1, lg.numberOfThreads);

    if (lg.scanAlbum)
    {
        std::map<std::string, std::unique_ptr<std::vector<std::string>>> sorted_audio_files = getSupportedAudioFilesSortedByFolder();
        std::vector<std::pair<std::shared_ptr<AudioFolder>, std::shared_ptr<AudioFile>>> audio_files;

        std::map<std::string, std::unique_ptr<std::vector<std::string>>>::iterator it;
        for (it = sorted_audio_files.begin(); it != sorted_audio_files.end(); it++)
        {
            std::shared_ptr<AudioFolder> audio_folder = std::shared_ptr<AudioFolder>(new AudioFolder(*it->second));
            for (int i = 0; i < audio_folder->count(); i++)
                audio_files.push_back(std::pair<std::shared_ptr<AudioFolder>, std::shared_ptr<AudioFile>>(audio_folder, audio_folder->getAudioFile(i)));
        }
        sorted_audio_files.clear();

        #pragma omp parallel for schedule(dynamic, 1) num_threads(nthreads) if (nthreads > 1)
        for (int i = 0; i < int(audio_files.size()); i++)
        {
            audio_files[i].second->scanFile(lg.pregain, true, (lg.verbosity >= 3));

            if (audio_files[i].first.use_count() == 1)
            {
                if (audio_files[i].first->canProcessResults() && audio_files[i].first->scanStatus == AudioFolder::INIT)
                {
                    audio_files[i].first->processResults(lg.pregain);
                    lg.processFolderResults(*audio_files[i].first.get());
                }
            }
            audio_files[i].first.reset();
            audio_files[i].second.reset();
        }
    }
    else
    {
        std::set<std::string> fset = getSupportedAudioFiles();
        std::vector<std::string> files{fset.begin(), fset.end()};
        fset.clear();

        #pragma omp parallel for schedule(dynamic, 1) num_threads(nthreads) if (nthreads > 1)
        for (int i = 0; i < int(files.size()); i++)
        {           
            AudioFile audio_file = AudioFile(files[i]);
            audio_file.scanFile(lg.pregain, true, (lg.verbosity >= 3));
            lg.processFileResults(audio_file);
        }
    }

    return true;
}

bool AudioLibrary::isOnlyDirectories(const std::vector<std::string> &paths)
{
    for(const std::string& path: paths)
    {
        const fs::path p(path);
        std::error_code ec;
        if (!fs::is_directory(p, ec))
            return false;
    }
    return true;
}

bool AudioLibrary::isSupportedAudioFile(const std::string &path)
{
    const fs::path p(path);
    return (fs::is_regular_file(p) && (std::find(userExtensions.begin(), userExtensions.end(), p.extension()) != userExtensions.end()));
}

bool AudioLibrary::isSupportedAudioFile(const fs::path &path)
{
    return (fs::is_regular_file(path) && (std::find(userExtensions.begin(), userExtensions.end(), path.extension()) != userExtensions.end()));
}

std::set<std::string> AudioLibrary::getSupportedAudioFiles()
{
    std::set<std::string> audio_files;

    if (isOnlyDirectories(libraryPaths))
    {
        for (const std::string& path : libraryPaths)
        {
            if (recursive)
            {
                fs::recursive_directory_iterator it(fs::path(path), std::filesystem::directory_options::skip_permission_denied);
                for(const fs::directory_entry &entry : it)
                    if (isSupportedAudioFile(entry.path().u8string()))
                        audio_files.insert(entry.path().u8string());
            }
            else
            {
                fs::directory_iterator it(fs::path(path), std::filesystem::directory_options::skip_permission_denied);
                for(const fs::directory_entry &entry : it)
                    if (isSupportedAudioFile(entry.path().u8string()))
                        audio_files.insert(entry.path().u8string());
            }
        }
    }
    else
    {
        for (const std::string& path : libraryPaths)
            if (isSupportedAudioFile(path))
                audio_files.insert(path);
    }

    return audio_files;
}

std::map<std::string, std::unique_ptr<std::vector<std::string>>> AudioLibrary::getSupportedAudioFilesSortedByFolder()
{
    std::set<std::string> files = getSupportedAudioFiles();
    std::map<std::string, std::unique_ptr<std::vector<std::string>>> sorted;

    for (const std::string& file : files)
    {
        const std::string dir = fs::path(file).parent_path().string();

        if ((sorted.find(dir) == sorted.end()))
            sorted.insert(std::pair<std::string, std::unique_ptr<std::vector<std::string>>>(dir, std::unique_ptr<std::vector<std::string>>(new std::vector<std::string>)));

        sorted[dir]->push_back(file);
    }

    return sorted;
}

